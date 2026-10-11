// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/ui/ui.hpp"

#include "wnk/ui/navigation.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_set>

namespace wnk::ui {
namespace {

constexpr double kToastFadeIn = 150, kToastFadeOut = 300;

// Sets the focus source for the duration of a call.
struct SourceScope {
    SourceScope(FocusSource &slot, FocusSource value) : slot_(slot), saved_(slot) { slot_ = value; }
    ~SourceScope() { slot_ = saved_; }
    FocusSource &slot_;
    FocusSource saved_;
};

// Counts nested entry points so the graveyard is only emptied by the outermost one.
struct Entry {
    explicit Entry(int &depth) : depth_(depth) { ++depth_; }
    ~Entry() { --depth_; }
    bool outermost() const { return depth_ == 1; }
    int &depth_;
};

}  // namespace

Ui::Ui(render::Renderer &renderer, Theme theme, Size canvas)
    : r_(renderer), theme_(theme), canvas_(canvas), root_(std::make_unique<View>()), overlay_(std::make_unique<View>()) {
    root_->owner_ui_ = this;
    root_->name = "root";
    overlay_->owner_ui_ = this;
    overlay_->name = "overlay";
    for (Smoothed *s : {&ring_dx_, &ring_dy_, &ring_dw_, &ring_dh_}) s->rate = 22;  // ~0.2 s
    ring_alpha_.rate = 24;
}

Ui::~Ui() {
    // Destroy the trees while the Ui (and the image source widgets may call) is still valid.
    overlay_.reset();
    root_.reset();
    graveyard_.clear();
}

// ---- sync -----------------------------------------------------------------------------------------

void Ui::sync() {
    Entry e(depth_);
    if (e.outermost()) {
        views_.clear();  // may point into the graveyard
        graveyard_.clear();
    }
    refresh();
}

void Ui::refresh() {
    if (layout_dirty_) {
        layout_dirty_ = false;
        ++layouts_;
        root_->layout(*this, {0, 0, canvas_.w, canvas_.h});
        overlay_->layout(*this, {0, 0, canvas_.w, canvas_.h});
    }

    views_.clear();
    seen_scopes_.clear();
    std::vector<focus::NodeSpec> on, off;
    walk(*root_, focus::kRootScope, {0, 0}, true, on, off);
    walk(*overlay_, focus::kRootScope, {0, 0}, true, on, off);

    // Focusable nodes first, then pending focus requests, then the unfocusable ones: a screen being
    // covered loses focusability only after focus has moved to the screen covering it.
    std::unordered_set<focus::NodeId> live;
    for (const auto &n : on) {
        fm_.upsert(n);
        live.insert(n.id);
    }
    apply_pending_focus();
    for (const auto &n : off) {
        fm_.upsert(n);
        live.insert(n.id);
    }
    for (auto it = registered_.begin(); it != registered_.end();) {
        if (!live.count(*it)) {
            fm_.remove(*it);
            it = registered_.erase(it);
        } else {
            ++it;
        }
    }
    registered_.insert(live.begin(), live.end());
    for (auto it = scopes_.begin(); it != scopes_.end();) {
        if (!seen_scopes_.count(it->first)) {
            fm_.remove_scope(it->first);
            it = scopes_.erase(it);
        } else {
            ++it;
        }
    }
    node_count_ = registered_.size();
    dispatch_focus_events();
}

void Ui::walk(View &v, focus::ScopeId scope, Point offset, bool shown, std::vector<focus::NodeSpec> &on,
              std::vector<focus::NodeSpec> &off) {
    views_[v.id_] = &v;
    shown = shown && v.visible_;
    const Point self{offset.x + v.translation_.x, offset.y + v.translation_.y};
    if (v.focusable_) {
        focus::NodeSpec n;
        n.id = v.id_;
        n.scope = scope;
        n.rect = v.frame_;
        n.rect.x += self.x;
        n.rect.y += self.y;
        n.focusable = shown && v.enabled_;
        (n.focusable ? on : off).push_back(n);
    }
    if (v.scope_) {
        auto it = scopes_.find(v.id_);
        if (it != scopes_.end() && it->second != scope) {
            fm_.remove_scope(v.id_);  // moved to another parent scope
            scopes_.erase(it);
            it = scopes_.end();
        }
        if (it == scopes_.end()) {
            fm_.add_scope({v.id_, scope, v.scope_contain_, v.scope_restore_});
            scopes_[v.id_] = scope;
        }
        seen_scopes_.insert(v.id_);
        scope = v.id_;
    }
    const Point s = v.scroll_offset();
    const Point child{self.x - s.x, self.y - s.y};
    for (const auto &c : v.children_) walk(*c, scope, child, shown, on, off);
}

void Ui::apply_pending_focus() {
    if (pending_focus_ != 0) {
        auto it = views_.find(pending_focus_);
        if (it != views_.end()) {
            fm_.focus(pending_focus_);  // applied once the view is attached, whether or not it succeeds
            pending_focus_ = 0;
        }
    }
    if (pending_within_ != 0) {
        auto it = views_.find(pending_within_);
        if (it != views_.end() && focus_within_now(*it->second)) pending_within_ = 0;
    }
}

bool Ui::focus_within_now(View &subtree) {
    if (subtree.scope_ && scopes_.count(subtree.id_) && fm_.enter_scope(subtree.id_)) return true;
    View *first = first_focusable(subtree);
    return first != nullptr && fm_.focus(first->id_);
}

View *Ui::first_focusable(View &subtree) const {
    View *best = nullptr;
    std::tuple<float, float> best_key{};
    std::vector<View *> stack{&subtree};
    while (!stack.empty()) {
        View *v = stack.back();
        stack.pop_back();
        if (v->focusable_) {
            const focus::NodeSpec *n = fm_.node(v->id_);
            if (n != nullptr && n->focusable && !n->rect.empty()) {
                auto key = std::make_tuple(n->rect.y, n->rect.x);
                if (best == nullptr || key < best_key) {
                    best = v;
                    best_key = key;
                }
            }
        }
        for (const auto &c : v->children_) stack.push_back(c.get());
    }
    return best;
}

void Ui::dispatch_focus_events() {
    // A focus listener may move focus again; bound the loop instead of recursing.
    for (int guard = 0; guard < 8; ++guard) {
        const focus::NodeId now_id = fm_.focused();
        if (now_id == notified_focus_) return;
        View *old = find(notified_focus_);
        View *now = find(now_id);
        notified_focus_ = now_id;
        if (old != nullptr) old->on_focus_changed(*this, false);
        if (now != nullptr) {
            now->on_focus_changed(*this, true);
            for (View *c = now, *p = now->parent_; p != nullptr; c = p, p = p->parent_) {
                p->on_descendant_focused(*this, c, now);
            }
        }
    }
}

// ---- focus ----------------------------------------------------------------------------------------

View *Ui::find(ViewId id) const {
    auto it = views_.find(id);
    return it == views_.end() ? nullptr : it->second;
}

View *Ui::focused() const { return find(fm_.focused()); }

bool Ui::has_focus_within(const View *v) const {
    const View *f = focused();
    return f != nullptr && v != nullptr && (f == v || v->is_ancestor_of(f));
}

bool Ui::attached(const View &v) const { return v.ui() == this; }

bool Ui::focus(View *v) {
    if (v == nullptr) return false;
    if (!attached(*v)) {
        pending_focus_ = v->id_;
        pending_within_ = 0;
        return false;
    }
    Entry e(depth_);
    refresh();
    pending_focus_ = 0;
    pending_within_ = 0;
    const bool ok = fm_.focus(v->id_);
    dispatch_focus_events();
    return ok;
}

bool Ui::focus_within(View *subtree) {
    if (subtree == nullptr) return false;
    pending_focus_ = 0;
    pending_within_ = 0;
    if (!attached(*subtree)) {
        pending_within_ = subtree->id_;
        return false;
    }
    Entry e(depth_);
    refresh();
    const bool ok = focus_within_now(*subtree);
    if (!ok) pending_within_ = subtree->id_;
    dispatch_focus_events();
    return ok;
}

// ---- input ----------------------------------------------------------------------------------------

bool Ui::handle(const input::ActionEvent &ev) {
    Entry e(depth_);
    if (e.outermost()) {
        views_.clear();
        graveyard_.clear();
    }
    refresh();
    SourceScope source(focus_source_, FocusSource::Key);
    const bool arrow = ev.action == input::Action::Up || ev.action == input::Action::Down ||
                       ev.action == input::Action::Left || ev.action == input::Action::Right;
    if (arrow && ev.phase != input::Phase::Release) pointer_active_ = false;  // the D-pad takes over
    if (route(ev)) return true;
    if (ev.phase == input::Phase::Release) return false;
    focus::Direction dir;
    switch (ev.action) {
        case input::Action::Up: dir = focus::Direction::Up; break;
        case input::Action::Down: dir = focus::Direction::Down; break;
        case input::Action::Left: dir = focus::Direction::Left; break;
        case input::Action::Right: dir = focus::Direction::Right; break;
        default: return false;
    }
    const bool moved = fm_.move(dir);
    dispatch_focus_events();
    return moved;
}

bool Ui::route(const input::ActionEvent &ev) {
    View *target = focused();
    if (target == nullptr) {
        // Nothing focused: offer the event to the top dialog, else down the visible content.
        if (Dialog *d = top_dialog()) {
            target = d;
        } else {
            target = root_.get();
            for (bool descended = true; descended;) {
                descended = false;
                for (auto it = target->children_.rbegin(); it != target->children_.rend(); ++it) {
                    if ((*it)->visible_) {
                        target = it->get();
                        descended = true;
                        break;
                    }
                }
            }
        }
    }
    for (View *v = target; v != nullptr; v = v->parent_) {
        if (v->visible_ && v->on_action(*this, ev)) {
            refresh();
            return true;
        }
    }
    return false;
}

// ---- pointer --------------------------------------------------------------------------------------

View *Ui::hit(View &v, Point p, Point offset, Rect clip, bool focusable_only) {
    if (!v.visible_ || v.opacity_ <= 0.001f) return nullptr;
    const Point self{offset.x + v.translation_.x, offset.y + v.translation_.y};
    const Rect r{v.frame_.x + self.x, v.frame_.y + self.y, v.frame_.w, v.frame_.h};
    const Rect child_clip = v.clips_children() ? clip.intersect(r) : clip;
    const Point s = v.scroll_offset();
    const Point child{self.x - s.x, self.y - s.y};
    // Last child first: it is drawn on top.
    for (auto it = v.children_.rbegin(); it != v.children_.rend(); ++it) {
        if (View *h = hit(**it, p, child, child_clip, focusable_only)) return h;
    }
    if (!r.contains(p) || !clip.contains(p)) return nullptr;
    if (!focusable_only) return &v;
    return v.focusable_ && v.enabled_ ? &v : nullptr;
}

View *Ui::hit_focusable(Point p) {
    const Rect canvas{0, 0, canvas_.w, canvas_.h};
    if (Dialog *d = top_dialog()) return hit(*d, p, {0, 0}, canvas, true);  // modal: nothing else is live
    return hit(*root_, p, {0, 0}, canvas, true);
}

void Ui::hover() {
    View *v = hit_focusable(pointer_);
    if (v == nullptr || v == focused()) return;
    SourceScope source(focus_source_, FocusSource::Pointer);
    fm_.focus(v->id_);
    dispatch_focus_events();
}

bool Ui::handle_pointer(const input::PointerEvent &e) {
    return handle_pointer_logical(e, r_.mapping().to_logical({e.x, e.y}));
}

bool Ui::handle_pointer_logical(const input::PointerEvent &e, Point p) {
    Entry entry(depth_);
    if (entry.outermost()) {
        views_.clear();
        graveyard_.clear();
    }
    refresh();
    using T = input::PointerEvent::Type;
    switch (e.type) {
        case T::Move:
            pointer_active_ = true;
            pointer_ = p;
            hover();
            return hit_focusable(p) != nullptr;
        case T::Press: {
            pointer_active_ = true;
            pointer_ = p;
            hover();
            if (e.button != 1) return false;
            View *v = hit_focusable(p);
            pressed_ = v != nullptr ? v->id_ : 0;
            return v != nullptr;
        }
        case T::Release: {
            pointer_ = p;
            if (e.button != 1) return false;
            View *v = hit_focusable(p);
            const bool same = v != nullptr && v->id_ == pressed_;
            pressed_ = 0;
            if (!same) return false;  // released somewhere else: no click
            SourceScope source(focus_source_, FocusSource::Pointer);
            if (!is_focused(v)) {
                fm_.focus(v->id_);
                dispatch_focus_events();
            }
            // A click is OK on the clicked view: the same path as the remote's OK key.
            input::ActionEvent ok;
            ok.action = input::Action::Confirm;
            ok.source.time_ms = e.time_ms;
            const bool handled = route(ok);
            ok.phase = input::Phase::Release;
            route(ok);
            return handled;
        }
        case T::Wheel: {
            const Rect canvas{0, 0, canvas_.w, canvas_.h};
            Dialog *d = top_dialog();
            View *target = d != nullptr ? hit(*d, pointer_, {0, 0}, canvas, false)
                                        : hit(*root_, pointer_, {0, 0}, canvas, false);
            for (View *v = target; v != nullptr; v = v->parent_) {
                if (v->on_wheel(*this, e.wheel)) {
                    refresh();
                    return true;  // what is under the pointer changes as it scrolls: update() re-hovers
                }
            }
            return false;
        }
        case T::Shown:
            pointer_active_ = true;  // position unknown until the next Move
            return false;
        case T::Hidden:
        case T::Left:
            pointer_active_ = false;
            pressed_ = 0;
            return false;
    }
    return false;
}

// ---- frame ----------------------------------------------------------------------------------------

namespace {
void update_tree(Ui &ui, View &v, float dt) {
    if (!v.visible()) return;
    v.update(ui, dt);
    // Snapshot: an update may add or remove children (collections realise cells here).
    std::vector<View *> kids;
    kids.reserve(v.children().size());
    for (const auto &c : v.children()) kids.push_back(c.get());
    for (View *c : kids) {
        if (c->parent() == &v) update_tree(ui, *c, dt);
    }
}
}  // namespace

void Ui::update(float dt) {
    Entry e(depth_);
    if (e.outermost()) {
        views_.clear();
        graveyard_.clear();
    }
    refresh();
    now_ms_ += static_cast<double>(dt) * 1000.0;
    update_tree(*this, *root_, dt);
    update_tree(*this, *overlay_, dt);
    refresh();
    // Content may have moved under a stationary pointer (wheel scrolling, screen changes).
    if (pointer_active_) hover();

    if (!toasts_.empty()) {
        Toast &t = toasts_.front();
        if (t.shown_at < 0) t.shown_at = now_ms_;
        if (now_ms_ - t.shown_at >= t.duration_ms) toasts_.pop_front();
    }
    step_ring(dt);
}

void Ui::step_ring(float dt) {
    View *f = focused();
    const bool want = f != nullptr && f->focus_ring_;
    ring_alpha_.target = want ? 1.0f : 0.0f;
    if (f != nullptr) {
        const Rect target = f->to_screen(f->focus_bounds()).inset(-theme_.ring_gap);
        if (f->id_ != ring_view_) {
            // Keep the ring attached to the focused view while it scrolls; animate only the jump
            // from the old rectangle, as an offset that decays to zero.
            if (ring_view_ != 0 && !ring_snap_ && ring_alpha_.value > 0.01f) {
                // Keep each spring's velocity: a second press mid-flight continues the motion.
                ring_dx_.value = ring_rect_.x - target.x;
                ring_dy_.value = ring_rect_.y - target.y;
                ring_dw_.value = ring_rect_.w - target.w;
                ring_dh_.value = ring_rect_.h - target.h;
                ring_dx_.target = ring_dy_.target = ring_dw_.target = ring_dh_.target = 0;
            } else {
                ring_dx_.snap(0);
                ring_dy_.snap(0);
                ring_dw_.snap(0);
                ring_dh_.snap(0);
                if (ring_alpha_.value <= 0.01f || ring_snap_) ring_alpha_.snap(want ? 1.0f : 0.0f);
            }
            ring_view_ = f->id_;
        }
        if (ring_snap_) {
            ring_dx_.snap(0);
            ring_dy_.snap(0);
            ring_dw_.snap(0);
            ring_dh_.snap(0);
        }
        for (Smoothed *s : {&ring_dx_, &ring_dy_, &ring_dw_, &ring_dh_}) s->step(dt);
        ring_rect_ = {target.x + ring_dx_.value, target.y + ring_dy_.value, target.w + ring_dw_.value,
                      target.h + ring_dh_.value};
        ring_radius_ = f->focus_ring_radius() + theme_.ring_gap;
    } else {
        ring_view_ = 0;
    }
    ring_snap_ = false;
    ring_alpha_.step(dt);
}

void Ui::draw_ring() {
    if (ring_alpha_.value <= 0.01f || ring_rect_.empty()) return;
    r_.stroke_rect(ring_rect_, theme_.focus_ring.with_alpha(theme_.focus_ring.a * ring_alpha_.value), theme_.ring_width,
                   ring_radius_);
}

void Ui::draw() {
    Entry e(depth_);
    if (e.outermost()) {
        views_.clear();
        graveyard_.clear();
    }
    refresh();
    alpha_ = 1;
    // A clear, not a full-screen quad: one less full-screen fragment pass per frame on the TV's GPU.
    // A transparent theme background lets a video plane show through.
    r_.clear(clear_override_ ? *clear_override_ : theme_.background);
    View *f = focused();
    const bool focus_in_overlay = f != nullptr && overlay_->is_ancestor_of(f);
    root_->draw_tree(*this);
    if (!focus_in_overlay) draw_ring();
    overlay_->draw_tree(*this);
    if (focus_in_overlay) draw_ring();
    draw_toasts();
}

void Ui::draw_toasts() {
    if (toasts_.empty() || toasts_.front().shown_at < 0) return;
    const Toast &t = toasts_.front();
    const double age = now_ms_ - t.shown_at;
    float a = 1;
    if (age < kToastFadeIn) a = static_cast<float>(age / kToastFadeIn);
    if (t.duration_ms - age < kToastFadeOut) a = std::min(a, static_cast<float>((t.duration_ms - age) / kToastFadeOut));
    a = std::clamp(a, 0.0f, 1.0f);
    render::TextStyle s{theme_.bold, theme_.text_size, theme_.text.with_alpha(a)};
    const float w = r_.measure_text(s, t.text);
    const float h = r_.line_height(s) + 36;
    const Rect box{(canvas_.w - w) * 0.5f - 44, canvas_.h - 190 + (1 - a) * 16, w + 88, h};
    r_.fill_rect(box, render::Color::hex(0x000000E0).with_alpha(0.88f * a), h * 0.5f);
    r_.draw_text(s, {box.x + 44, box.y + 18 + r_.ascent(s)}, t.text);
}

// ---- overlays -------------------------------------------------------------------------------------

Dialog &Ui::show_dialog(std::unique_ptr<Dialog> dialog) {
    Entry e(depth_);
    Dialog &d = *dialog;
    overlay_->add_child(std::move(dialog));
    refresh();
    fm_.push_modal(d.id_);
    dispatch_focus_events();
    return d;
}

void Ui::dismiss_dialog(Dialog *dialog) {
    if (dialog == nullptr || dialog->parent_ != overlay_.get()) return;
    Entry e(depth_);
    if (fm_.active_modal() == dialog->id_) fm_.pop_modal();
    overlay_->remove_child(dialog);
    refresh();
}

size_t Ui::dialog_count() const { return overlay_->children_.size(); }

Dialog *Ui::top_dialog() const {
    return overlay_->children_.empty() ? nullptr : static_cast<Dialog *>(overlay_->children_.back().get());
}

void Ui::toast(std::string text, double duration_ms) { toasts_.push_back({std::move(text), duration_ms}); }

void Ui::retire(std::unique_ptr<View> v) {
    if (!v) return;
    v->parent_ = nullptr;
    graveyard_.push_back(std::move(v));
}

UiStats Ui::stats() const {
    UiStats s;
    s.views = views_.size();
    s.focus_nodes = node_count_;
    s.layouts = layouts_;
    s.retired = graveyard_.size();
    return s;
}

}  // namespace wnk::ui
