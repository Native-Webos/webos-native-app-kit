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
    root_->set_background(theme_.background);
    overlay_->owner_ui_ = this;
    overlay_->name = "overlay";
    for (Smoothed *s : {&ring_dx_, &ring_dy_, &ring_dw_, &ring_dh_}) s->rate = 18;
    ring_alpha_.rate = 20;
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
                ring_dx_.snap(ring_rect_.x - target.x);
                ring_dy_.snap(ring_rect_.y - target.y);
                ring_dw_.snap(ring_rect_.w - target.w);
                ring_dh_.snap(ring_rect_.h - target.h);
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
