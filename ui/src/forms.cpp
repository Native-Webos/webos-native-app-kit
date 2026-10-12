// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/ui/forms.hpp"

#include "wnk/ui/navigation.hpp"

#include "qrcodegen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace wnk::ui {
namespace {

constexpr float kGap = 24;  // between the label and the value

render::TextStyle body(const Ui &ui, render::Color c) { return {ui.theme().regular, ui.theme().text_size, c}; }
render::TextStyle small(const Ui &ui, render::Color c) { return {ui.theme().regular, ui.theme().small_size, c}; }

bool pressed(const input::ActionEvent &e) { return e.phase != input::Phase::Release; }

// Text right-aligned against `right`, vertically centred in `area`; returns its width.
float text_right(Ui &ui, const render::TextStyle &s, const std::string &text, float right, const Rect &area) {
    render::Renderer &r = ui.renderer();
    const float w = r.measure_text(s, text);
    const float lh = r.line_height(s);
    r.draw_text(s, {right - w, area.y + (area.h - lh) * 0.5f + r.ascent(s)}, text);
    return w;
}

}  // namespace

// ---- FormRow --------------------------------------------------------------------------------------

FormRow::FormRow(std::string label) : label_(std::move(label)) {
    set_focusable(true);
    set_padding(Insets::xy(32, 20));
    radius_ = 14;
}

FormRow &FormRow::set_label(std::string label) {
    label_ = std::move(label);
    return *this;
}

FormRow &FormRow::set_description(std::string text) {
    description_ = std::move(text);
    invalidate();
    return *this;
}

Size FormRow::measure_content(Ui &ui, Size available) {
    render::Renderer &r = ui.renderer();
    float h = r.line_height(body(ui, ui.theme().text));
    if (!description_.empty()) h += 4 + r.line_height(small(ui, ui.theme().text_muted));
    const float w = std::isfinite(available.w) ? std::min(available.w, 1000.0f) : 1000.0f;
    return {w, h};
}

std::string FormRow::accessible_text() const {
    if (!accessible_label.empty()) return accessible_label;
    std::string t = label_;
    const std::string v = value_text_for_speech();
    if (!v.empty()) t += ", " + v;
    if (!enabled()) t += ", unavailable";
    if (!description_.empty()) t += ", " + description_;
    return t;
}

void FormRow::draw_value(Ui &, const Rect &) {}
float FormRow::value_width(Ui &) const { return 0; }

void FormRow::draw(Ui &ui) {
    render::Renderer &r = ui.renderer();
    const Theme &t = ui.theme();
    const bool focused = ui.is_focused(this);
    const Rect f = frame();
    r.fill_rect(f, ui.fade(!enabled() ? t.surface : focused ? t.raised_focused : t.raised), radius_);
    const Rect c = content_rect();
    const float vw = std::min(value_width(ui), c.w * 0.6f);
    const float label_w = std::max(0.0f, c.w - vw - (vw > 0 ? kGap : 0));
    const render::TextStyle ls = body(ui, ui.fade(enabled() ? t.text : t.text_disabled));
    const float lh = r.line_height(ls);
    const render::TextStyle ds = small(ui, ui.fade(t.text_muted));
    const float block = lh + (description_.empty() ? 0 : 4 + r.line_height(ds));
    const float top = c.y + (c.h - block) * 0.5f;
    const auto line = Label::wrap(r, ls, label_, label_w, 1);
    if (!line.empty()) r.draw_text(ls, {c.x, top + r.ascent(ls)}, line[0]);
    if (!description_.empty()) {
        const auto d = Label::wrap(r, ds, description_, label_w, 1);
        if (!d.empty()) r.draw_text(ds, {c.x, top + lh + 4 + r.ascent(ds)}, d[0]);
    }
    if (vw > 0) draw_value(ui, {c.right() - vw, c.y, vw, c.h});
}

// ---- Toggle ---------------------------------------------------------------------------------------

Toggle::Toggle(std::string label, bool on, Handler on_change)
    : FormRow(std::move(label)), on_(on), on_change_(std::move(on_change)) {
    knob_.rate = 22;
    knob_.snap(on ? 1 : 0);
}

Toggle &Toggle::set_on(bool on) {
    on_ = on;
    knob_.target = on ? 1 : 0;
    return *this;
}

Toggle &Toggle::set_style(Style s) {
    style_ = s;
    return *this;
}

Toggle &Toggle::set_on_change(Handler h) {
    on_change_ = std::move(h);
    return *this;
}

bool Toggle::on_action(Ui &ui, const input::ActionEvent &e) {
    if (e.action != input::Action::Confirm || !enabled()) return false;
    if (e.phase == input::Phase::Press) {
        set_on(!on_);
        if (on_change_) {
            Handler h = on_change_;  // may rebind or destroy this row
            h(ui, on_);
        }
    }
    return true;
}

void Toggle::update(Ui &, float dt) { knob_.step(dt); }

float Toggle::value_width(Ui &) const { return style_ == Style::Switch ? 88 : 44; }

void Toggle::draw_value(Ui &ui, const Rect &area) {
    render::Renderer &r = ui.renderer();
    const Theme &t = ui.theme();
    const float k = std::clamp(knob_.value, 0.0f, 1.0f);
    const auto mix = [k](render::Color a, render::Color b) {
        return render::Color{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k};
    };
    if (style_ == Style::Switch) {
        const Rect track{area.right() - 88, area.cy() - 22, 88, 44};
        r.fill_rect(track, ui.fade(mix(t.placeholder, t.accent)), 22);
        const float x = track.x + 4 + k * (track.w - 44);
        r.fill_rect({x, track.y + 4, 36, 36}, ui.fade(enabled() ? t.text : t.text_disabled), 18);
    } else {
        const Rect box{area.right() - 40, area.cy() - 20, 40, 40};
        r.stroke_rect(box, ui.fade(mix(t.text_muted, t.accent)), 3, 8);
        if (k > 0.01f) r.fill_rect({box.x + 9, box.y + 9, 22, 22}, ui.fade(t.accent.with_alpha(k)), 4);
    }
}

// ---- Slider ---------------------------------------------------------------------------------------

Slider::Slider(std::string label, float min, float max, float step, float value, Handler on_change)
    : FormRow(std::move(label)), min_(std::min(min, max)), max_(std::max(min, max)), step_(step > 0 ? step : 1),
      value_(0), on_change_(std::move(on_change)) {
    value_ = snap(value);
    shown_.rate = 24;
    shown_.snap(max_ > min_ ? (value_ - min_) / (max_ - min_) : 0);
}

float Slider::snap(float v) const {
    const float steps = std::round((std::clamp(v, min_, max_) - min_) / step_);
    return std::clamp(min_ + steps * step_, min_, max_);
}

Slider &Slider::set_value(float v) {
    value_ = snap(v);
    shown_.target = max_ > min_ ? (value_ - min_) / (max_ - min_) : 0;
    return *this;
}

Slider &Slider::set_format(Format f) {
    format_ = std::move(f);
    return *this;
}

Slider &Slider::set_on_change(Handler h) {
    on_change_ = std::move(h);
    return *this;
}

std::string Slider::value_text() const {
    if (format_) return format_(value_);
    int decimals = 0;
    for (float s = step_; decimals < 4 && std::fabs(s - std::round(s)) > 1e-4f; s *= 10) ++decimals;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, static_cast<double>(value_));
    return buf;
}

void Slider::change(Ui &ui, float v) {
    const float before = value_;
    set_value(v);
    if (value_ != before && on_change_) {
        Handler h = on_change_;
        h(ui, value_);
    }
}

bool Slider::on_action(Ui &ui, const input::ActionEvent &e) {
    if (!enabled()) return false;
    if (e.action != input::Action::Left && e.action != input::Action::Right) return false;
    // Left/Right always belong to the slider, even at either end: in a column of rows they would
    // otherwise jump to whatever lies beside the list.
    if (pressed(e)) change(ui, value_ + (e.action == input::Action::Right ? step_ : -step_));
    return true;
}

bool Slider::on_wheel(Ui &ui, float notches) {
    if (!enabled() || notches == 0) return false;
    change(ui, value_ + (notches > 0 ? step_ : -step_));
    return true;
}

void Slider::update(Ui &, float dt) { shown_.step(dt); }

float Slider::value_width(Ui &) const { return 440; }

void Slider::draw_value(Ui &ui, const Rect &area) {
    render::Renderer &r = ui.renderer();
    const Theme &t = ui.theme();
    const render::TextStyle s = body(ui, ui.fade(enabled() ? t.text : t.text_disabled));
    text_right(ui, s, value_text(), area.right(), area);
    const Rect track{area.x, area.cy() - 4, area.w - 130, 8};
    r.fill_rect(track, ui.fade(t.placeholder), 4);
    const float frac = std::clamp(shown_.value, 0.0f, 1.0f);
    r.fill_rect({track.x, track.y, track.w * frac, track.h}, ui.fade(t.accent), 4);
    const float d = ui.is_focused(this) ? 28 : 20;
    r.fill_rect({track.x + track.w * frac - d * 0.5f, track.cy() - d * 0.5f, d, d}, ui.fade(t.text), d * 0.5f);
}

// ---- Choice ---------------------------------------------------------------------------------------

Choice::Choice(std::string label, std::vector<std::string> options, int selected, Handler on_change)
    : FormRow(std::move(label)), options_(std::move(options)), selected_(0), on_change_(std::move(on_change)) {
    set_selected(selected);
}

Choice &Choice::set_selected(int index) {
    selected_ = options_.empty() ? -1 : std::clamp(index, 0, static_cast<int>(options_.size()) - 1);
    return *this;
}

Choice &Choice::set_options(std::vector<std::string> options, int selected) {
    options_ = std::move(options);
    return set_selected(selected);
}

Choice &Choice::set_on_change(Handler h) {
    on_change_ = std::move(h);
    return *this;
}

void Choice::change(Ui &ui, int index) {
    const int before = selected_;
    set_selected(index);
    if (selected_ != before && on_change_) {
        Handler h = on_change_;
        h(ui, selected_);
    }
}

void Choice::open_list(Ui &ui) {
    if (options_.empty()) return;
    auto d = std::make_unique<Dialog>(label());
    Dialog *dialog = d.get();
    Button *current = nullptr;
    for (size_t i = 0; i < options_.size(); ++i) {
        const int index = static_cast<int>(i);
        const bool on = index == selected_;
        auto &b = d->body().add<Button>(options_[i], [this, dialog, index](Ui &u) {
            change(u, index);
            dialog->dismiss(u);
        });
        b.name = (name.empty() ? std::string("choice") : name) + ".option" + std::to_string(i);
        b.set_width(Dim::fill());
        if (on) current = &b;
    }
    ui.show_dialog(std::move(d));
    if (current != nullptr) ui.focus(current);
}

bool Choice::on_action(Ui &ui, const input::ActionEvent &e) {
    if (!enabled() || options_.empty()) return false;
    switch (e.action) {
        case input::Action::Left:
        case input::Action::Right:
            if (pressed(e)) change(ui, selected_ + (e.action == input::Action::Right ? 1 : -1));
            return true;
        case input::Action::Confirm:
            if (e.phase == input::Phase::Press) open_list(ui);
            return true;
        default: return false;
    }
}

float Choice::value_width(Ui &ui) const {
    const render::TextStyle s = body(ui, ui.theme().text);
    float w = 0;
    for (const auto &o : options_) w = std::max(w, ui.renderer().measure_text(s, o));
    return w + 2 * (ui.renderer().measure_text(s, "\xE2\x80\xB9") + 20);
}

void Choice::draw_value(Ui &ui, const Rect &area) {
    if (selected_ < 0) return;
    render::Renderer &r = ui.renderer();
    const Theme &t = ui.theme();
    const render::TextStyle s = body(ui, ui.fade(enabled() ? t.text : t.text_disabled));
    const render::TextStyle can = body(ui, ui.fade(t.text));
    const render::TextStyle cannot = body(ui, ui.fade(t.text_disabled));
    const char *left = "\xE2\x80\xB9", *right = "\xE2\x80\xBA";  // ‹ ›
    const float aw = r.measure_text(s, left);
    const float lh = r.line_height(s);
    const float baseline = area.y + (area.h - lh) * 0.5f + r.ascent(s);
    r.draw_text(selected_ > 0 ? can : cannot, {area.x, baseline}, left);
    r.draw_text(selected_ + 1 < static_cast<int>(options_.size()) ? can : cannot, {area.right() - aw, baseline}, right);
    const std::string &v = options_[static_cast<size_t>(selected_)];
    const float w = r.measure_text(s, v);
    r.draw_text(s, {area.cx() - w * 0.5f, baseline}, v);
}

// ---- ValueRow -------------------------------------------------------------------------------------

ValueRow::ValueRow(std::string label, std::string value, ClickHandler on_click)
    : FormRow(std::move(label)), value_(std::move(value)), on_click_(std::move(on_click)) {}

ValueRow &ValueRow::set_value(std::string value) {
    value_ = std::move(value);
    return *this;
}

ValueRow &ValueRow::set_on_click(ClickHandler h) {
    on_click_ = std::move(h);
    return *this;
}

bool ValueRow::on_action(Ui &ui, const input::ActionEvent &e) {
    if (e.action != input::Action::Confirm || !enabled() || !on_click_) return false;
    if (e.phase == input::Phase::Press) {
        ClickHandler h = on_click_;
        h(ui);
    }
    return true;
}

float ValueRow::value_width(Ui &ui) const {
    const render::TextStyle s = body(ui, ui.theme().text);
    return ui.renderer().measure_text(s, value_ + "  \xE2\x80\xBA");
}

void ValueRow::draw_value(Ui &ui, const Rect &area) {
    const Theme &t = ui.theme();
    const float chevron = on_click_ ? text_right(ui, body(ui, ui.fade(t.text)), "\xE2\x80\xBA", area.right(), area) : 0;
    text_right(ui, body(ui, ui.fade(t.text_muted)), value_, area.right() - (chevron > 0 ? chevron + 16 : 0), area);
}

// ---- Tabs -----------------------------------------------------------------------------------------

class Tabs::Tab : public Button {
 public:
    Tab(Tabs &owner, int index, std::string title) : Button(std::move(title)), owner_(owner), index_(index) {
        set_padding(Insets::xy(28, 14));
        set_on_click([this](Ui &ui) { owner_.select(ui, index_); });
    }
    std::string accessible_text() const override {
        return text() + ", tab " + std::to_string(index_ + 1) + " of " + std::to_string(owner_.count()) +
               (owner_.selected() == index_ ? ", selected" : "");
    }
    void on_focus_changed(Ui &ui, bool focused) override {
        if (!focused || ui.focus_source() == FocusSource::Pointer) return;
        // Arriving from outside the row (Up from the page below) lands on the selected tab rather
        // than on whichever tab happens to be nearest, which would switch the page.
        View *from = ui.previous_focus();
        const bool from_inside = from != nullptr && owner_.is_ancestor_of(from);
        if (!from_inside && index_ != owner_.selected() && owner_.selected() >= 0) {
            ui.focus(owner_.tabs_[static_cast<size_t>(owner_.selected())]);
            return;
        }
        owner_.select(ui, index_);
    }

 protected:
    void draw(Ui &ui) override {
        render::Renderer &r = ui.renderer();
        const Theme &t = ui.theme();
        const Rect f = frame();
        const bool focused = ui.is_focused(this);
        const bool selected = owner_.selected() == index_;
        if (focused) r.fill_rect(f, ui.fade(t.raised_focused), radius_);
        const render::TextStyle s{t.bold, t.text_size, ui.fade(selected || focused ? t.text : t.text_muted)};
        const float w = r.measure_text(s, text());
        const float lh = r.line_height(s);
        r.draw_text(s, {f.cx() - w * 0.5f, f.y + (f.h - lh) * 0.5f + r.ascent(s)}, text());
        if (selected) r.fill_rect({f.cx() - w * 0.5f, f.bottom() - 6, w, 4}, ui.fade(t.accent), 2);
    }

 private:
    Tabs &owner_;
    int index_;
};

Tabs::Tabs(std::vector<std::string> titles, int selected, Handler on_select) : Row(8), on_select_(std::move(on_select)) {
    set_focus_scope(false, true);
    set_align(Align::Start, Align::Start);
    for (size_t i = 0; i < titles.size(); ++i) {
        auto &tab = add<Tab>(*this, static_cast<int>(i), std::move(titles[i]));
        tab.name = "tab" + std::to_string(i);
        tabs_.push_back(&tab);
    }
    selected_ = tabs_.empty() ? -1 : std::clamp(selected, 0, static_cast<int>(tabs_.size()) - 1);
}

Tabs &Tabs::set_selected(int index) {
    if (!tabs_.empty()) selected_ = std::clamp(index, 0, static_cast<int>(tabs_.size()) - 1);
    return *this;
}

View &Tabs::tab(size_t i) { return *tabs_.at(i); }

Tabs &Tabs::set_on_select(Handler h) {
    on_select_ = std::move(h);
    return *this;
}

void Tabs::select(Ui &ui, int index) {
    if (index == selected_) return;
    selected_ = index;
    if (on_select_) {
        Handler h = on_select_;
        h(ui, index);
    }
}

// ---- TextView -------------------------------------------------------------------------------------

TextView::TextView(std::string text, TextRole role) {
    label_ = &add<Label>(std::move(text), role);
    label_->set_max_lines(std::numeric_limits<int>::max());
    scroll_.rate = 18;
    radius_ = 12;
}

TextView &TextView::set_text(std::string text) {
    label_->set_text(std::move(text));
    scroll_.snap(0);
    return *this;
}

const std::string &TextView::text() const { return label_->text(); }

std::string TextView::accessible_text() const { return accessible_label.empty() ? text() : accessible_label; }

Size TextView::measure_content(Ui &ui, Size available) {
    const Size s = label_->measure(ui, {available.w, std::numeric_limits<float>::infinity()});
    return {s.w, std::isfinite(available.h) ? std::min(s.h, available.h) : s.h};
}

void TextView::layout_content(Ui &ui, const Rect &content) {
    const Size s = label_->measure(ui, {content.w, std::numeric_limits<float>::infinity()});
    label_->layout(ui, {content.x, content.y, content.w, s.h});
    max_scroll_ = std::max(0.0f, s.h - content.h);
    page_ = content.h;
    set_focusable(max_scroll_ > 0);  // nothing to scroll: the remote passes over it
    scroll_.target = std::clamp(scroll_.target, 0.0f, max_scroll_);
}

bool TextView::scroll_by(float dy) {
    const float to = std::clamp(scroll_.target + dy, 0.0f, max_scroll_);
    if (std::fabs(to - scroll_.target) < 0.5f) return false;
    scroll_.target = to;
    return true;
}

bool TextView::on_action(Ui &, const input::ActionEvent &e) {
    if (!pressed(e) || (e.action != input::Action::Up && e.action != input::Action::Down)) return false;
    // A third of the frame per press: enough to move on, little enough to keep the reader's place.
    return scroll_by((e.action == input::Action::Down ? 1 : -1) * std::max(40.0f, page_ / 3));
}

bool TextView::on_wheel(Ui &, float notches) { return scroll_by(-notches * std::max(40.0f, page_ / 4)); }

void TextView::update(Ui &, float dt) { scroll_.step(dt); }

void TextView::draw_over(Ui &ui) {
    if (max_scroll_ <= 0) return;
    // A thin scroll bar on the right: where in the text the frame is.
    const Rect f = frame();
    const float track_h = f.h - 16;
    const float thumb = std::max(40.0f, track_h * f.h / (f.h + max_scroll_));
    const float y = f.y + 8 + (track_h - thumb) * std::clamp(scroll_.value / max_scroll_, 0.0f, 1.0f);
    ui.renderer().fill_rect({f.right() - 6, y, 4, thumb}, ui.fade(ui.theme().text_muted.with_alpha(0.6f)), 2);
}

// ---- QrCode ---------------------------------------------------------------------------------------

QrCode::QrCode(std::string text, Ecc ecc) : text_(std::move(text)), ecc_(ecc) { encode(); }

QrCode &QrCode::set_text(std::string text) {
    text_ = std::move(text);
    encode();
    return *this;
}

void QrCode::encode() {
    size_ = 0;
    modules_.clear();
    error_.clear();
    if (text_.empty()) return;
    const qrcodegen::QrCode::Ecc levels[] = {qrcodegen::QrCode::Ecc::LOW, qrcodegen::QrCode::Ecc::MEDIUM,
                                             qrcodegen::QrCode::Ecc::QUARTILE, qrcodegen::QrCode::Ecc::HIGH};
    try {
        const auto qr = qrcodegen::QrCode::encodeText(text_.c_str(), levels[static_cast<int>(ecc_)]);
        size_ = qr.getSize();
        modules_.resize(static_cast<size_t>(size_) * static_cast<size_t>(size_));
        for (int y = 0; y < size_; ++y) {
            for (int x = 0; x < size_; ++x) modules_[static_cast<size_t>(y * size_ + x)] = qr.getModule(x, y);
        }
    } catch (const std::exception &e) {  // longer than a version 40 code holds
        error_ = e.what();
        size_ = 0;
    }
}

bool QrCode::dark(int x, int y) const {
    if (x < 0 || y < 0 || x >= size_ || y >= size_) return false;
    return modules_[static_cast<size_t>(y * size_ + x)];
}

Size QrCode::measure_content(Ui &, Size available) {
    const float side = std::min({360.0f, std::isfinite(available.w) ? available.w : 360.0f,
                                 std::isfinite(available.h) ? available.h : 360.0f});
    return {side, side};
}

void QrCode::draw(Ui &ui) {
    render::Renderer &r = ui.renderer();
    const Rect c = content_rect();
    const float side = std::min(c.w, c.h);
    const Rect box{c.cx() - side * 0.5f, c.cy() - side * 0.5f, side, side};
    if (size_ == 0) {
        r.fill_rect(box, ui.fade(ui.theme().placeholder), 12);
        return;
    }
    // Whole drawable pixels per module keep the edges sharp for a phone camera; four modules of
    // quiet zone around the code, as the standard asks.
    const float scale = std::max(0.01f, r.mapping().scale);
    const int total = size_ + 8;
    const float module = std::max(1.0f, std::floor(side * scale / static_cast<float>(total))) / scale;
    const float drawn = module * static_cast<float>(total);
    const float ox = std::round((box.x + (side - drawn) * 0.5f) * scale) / scale;
    const float oy = std::round((box.y + (side - drawn) * 0.5f) * scale) / scale;
    r.fill_rect({ox, oy, drawn, drawn}, ui.fade(render::kWhite), 8);
    const render::Color ink = ui.fade(render::Color::hex(0x000000FF));
    for (int y = 0; y < size_; ++y) {
        int x = 0;
        while (x < size_) {  // one quad per horizontal run of dark modules
            if (!dark(x, y)) {
                ++x;
                continue;
            }
            int end = x;
            while (end < size_ && dark(end, y)) ++end;
            r.fill_rect({ox + (x + 4) * module, oy + (y + 4) * module, (end - x) * module, module}, ink);
            x = end;
        }
    }
}

}  // namespace wnk::ui
