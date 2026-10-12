// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/render/font.hpp"
#include "wnk/ui/widgets.hpp"

#include <algorithm>
#include <cmath>

namespace wnk::ui {
namespace {

constexpr float kCaretWidth = 3;
constexpr double kBlinkMs = 1060;  // on for the first 60 %
constexpr const char *kBullet = "\xE2\x80\xA2";  // U+2022, in the bundled Atkinson Hyperlegible

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

size_t next_boundary(const std::string &s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && continuation(s[i])) ++i;
    return i;
}

size_t prev_boundary(const std::string &s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && continuation(s[i])) --i;
    return i;
}

size_t code_points(const std::string &s) {
    return static_cast<size_t>(std::count_if(s.begin(), s.end(), [](char c) { return !continuation(c); }));
}

void append_utf8(std::string &out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// Valid UTF-8 without control characters or line breaks, at most `limit` code points (0: no limit).
std::string clean(const std::string &utf8, size_t limit) {
    std::string out;
    size_t n = 0;
    for (char32_t c : render::decode_utf8(utf8)) {
        if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || c == 0xFFFD || c == 0x2028 || c == 0x2029) continue;
        if (limit != 0 && n == limit) break;
        append_utf8(out, c);
        ++n;
    }
    return out;
}

std::string bullets(size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) s += kBullet;
    return s;
}

}  // namespace

TextField::TextField(std::string placeholder) : placeholder_(std::move(placeholder)) {
    set_focusable(true);
    set_padding(Insets::xy(28, 18));
    radius_ = 12;
}

TextField &TextField::set_text(std::string text) {
    text_ = clean(text, max_length_);
    cursor_ = text_.size();
    composition_.clear();
    return *this;
}

TextField &TextField::set_placeholder(std::string placeholder) {
    placeholder_ = std::move(placeholder);
    return *this;
}

TextField &TextField::set_password(bool on) {
    password_ = on;
    return *this;
}

TextField &TextField::set_max_length(size_t n) {
    max_length_ = n;
    if (n != 0 && code_points(text_) > n) set_text(text_);
    return *this;
}

TextField &TextField::set_on_change(TextHandler h) {
    on_change_ = std::move(h);
    return *this;
}

TextField &TextField::set_on_submit(TextHandler h) {
    on_submit_ = std::move(h);
    return *this;
}

std::string TextField::accessible_text() const {
    if (!accessible_label.empty()) return accessible_label;
    const std::string what = placeholder_.empty() ? "text field" : placeholder_ + ", text field";
    if (password_) return what + ", password, " + std::to_string(code_points(text_)) + " characters";
    return text_.empty() ? what + ", empty" : what + ", " + text_;
}

std::string TextField::display_text() const { return password_ ? bullets(code_points(text_)) : text_; }

void TextField::begin_editing(Ui &ui) {
    if (editing_ || !enabled()) return;
    if (!ui.is_focused(this)) ui.focus(this);
    editing_ = true;
    cursor_ = text_.size();
    caret_since_ = ui.now_ms();
    ui.begin_text_input(this);
}

void TextField::end_editing(Ui &ui) {
    if (!editing_) return;
    editing_ = false;
    composition_.clear();
    scroll_ = 0;
    ui.end_text_input(this);
}

void TextField::insert(Ui &ui, const std::string &utf8) {
    std::string add = clean(utf8, 0);
    if (max_length_ != 0) {
        const size_t have = code_points(text_);
        add = have >= max_length_ ? std::string() : clean(add, max_length_ - have);
    }
    if (add.empty()) return;
    text_.insert(cursor_, add);
    cursor_ += add.size();
    changed(ui);
}

void TextField::changed(Ui &ui) {
    caret_since_ = ui.now_ms();
    if (on_change_) {
        TextHandler h = on_change_;  // the handler may rebind or destroy this field
        h(ui, text_);
    }
}

void TextField::submit(Ui &ui) {
    end_editing(ui);
    if (on_submit_) {
        TextHandler h = on_submit_;
        const std::string text = text_;
        h(ui, text);
    }
}

Size TextField::measure_content(Ui &ui, Size available) {
    const render::TextStyle s{ui.theme().regular, ui.theme().text_size, ui.theme().text};
    return {std::min(available.w, 600.0f), ui.renderer().line_height(s)};
}

bool TextField::on_action(Ui &ui, const input::ActionEvent &e) {
    if (!enabled()) return false;
    const bool press = e.phase != input::Phase::Release;
    switch (e.action) {
        case input::Action::Confirm:
            if (e.phase != input::Phase::Press) return true;
            if (editing_) {
                submit(ui);
            } else {
                begin_editing(ui);
            }
            return true;
        case input::Action::Back:
            if (!editing_) return false;
            if (e.phase == input::Phase::Press) end_editing(ui);
            return true;
        // Arrows that reach the field as actions (no shell routing) still move the cursor.
        case input::Action::Left:
        case input::Action::Right:
            if (!editing_) return false;
            if (press) on_edit_key(ui, e.action == input::Action::Left ? input::EditKey::Left : input::EditKey::Right);
            return true;
        default: return false;
    }
}

void TextField::on_focus_changed(Ui &ui, bool focused) {
    if (!focused) end_editing(ui);
}

bool TextField::on_text(Ui &ui, const std::string &utf8) {
    if (!editing_) return false;
    composition_.clear();
    insert(ui, utf8);
    return true;
}

bool TextField::on_edit_key(Ui &ui, input::EditKey key) {
    if (!editing_) return false;
    caret_since_ = ui.now_ms();
    switch (key) {
        case input::EditKey::Backspace:
            if (cursor_ > 0) {
                const size_t from = prev_boundary(text_, cursor_);
                text_.erase(from, cursor_ - from);
                cursor_ = from;
                changed(ui);
            }
            break;
        case input::EditKey::Delete:
            if (cursor_ < text_.size()) {
                text_.erase(cursor_, next_boundary(text_, cursor_) - cursor_);
                changed(ui);
            }
            break;
        case input::EditKey::Left: cursor_ = prev_boundary(text_, cursor_); break;
        case input::EditKey::Right: cursor_ = next_boundary(text_, cursor_); break;
        case input::EditKey::Home: cursor_ = 0; break;
        case input::EditKey::End: cursor_ = text_.size(); break;
        case input::EditKey::Enter: submit(ui); break;
        case input::EditKey::Clear:
            if (!text_.empty()) {
                text_.clear();
                cursor_ = 0;
                changed(ui);
            }
            break;
    }
    return true;
}

void TextField::on_composition(Ui &ui, const std::string &utf8) {
    if (!editing_) return;
    composition_ = clean(utf8, 0);
    caret_since_ = ui.now_ms();
}

void TextField::on_text_input_closed(Ui &ui) {
    // Text input has already ended in the Ui; keep the text, as Back would.
    editing_ = false;
    composition_.clear();
    scroll_ = 0;
    (void)ui;
}

void TextField::draw(Ui &ui) {
    render::Renderer &r = ui.renderer();
    const Theme &t = ui.theme();
    const Rect f = frame();
    const bool focused = ui.is_focused(this);
    r.fill_rect(f, ui.fade(!enabled() || editing_ ? t.surface : focused ? t.raised_focused : t.raised), radius_);
    if (editing_) r.stroke_rect(f, ui.fade(t.accent), 3, radius_);

    const Rect c = content_rect();
    const render::TextStyle s{t.regular, t.text_size, ui.fade(enabled() ? t.text : t.text_disabled)};
    const float lh = r.line_height(s);
    const float top = c.y + (c.h - lh) * 0.5f;
    const float baseline = top + r.ascent(s);
    const std::string shown = display_text();
    const bool caret = editing_ && std::fmod(ui.now_ms() - caret_since_, kBlinkMs) < kBlinkMs * 0.6;

    r.push_clip(c);
    if (shown.empty() && composition_.empty()) {
        if (!placeholder_.empty()) r.draw_text({t.regular, t.text_size, ui.fade(t.text_muted)}, {c.x, baseline}, placeholder_);
        if (caret) r.fill_rect({c.x, top, kCaretWidth, lh}, ui.fade(t.text));
        r.pop_clip();
        return;
    }
    const std::string before = password_ ? bullets(code_points(text_.substr(0, cursor_))) : text_.substr(0, cursor_);
    const std::string after = password_ ? bullets(code_points(text_.substr(cursor_))) : text_.substr(cursor_);
    const render::TextStyle cs{t.regular, t.text_size, ui.fade(t.accent)};
    const float caret_x = r.measure_text(s, before) + r.measure_text(cs, composition_);
    const float total = caret_x + r.measure_text(s, after);
    if (editing_) {
        // Keep the caret inside, and do not leave empty space on the right after deleting.
        const float room = c.w - kCaretWidth;
        if (caret_x - scroll_ > room) scroll_ = caret_x - room;
        if (caret_x < scroll_) scroll_ = caret_x;
        if (total - scroll_ < room) scroll_ = std::max(0.0f, total - room);
    } else {
        scroll_ = 0;
    }
    float x = c.x - scroll_;
    x += r.draw_text(s, {x, baseline}, before);
    if (!composition_.empty()) {
        const float w = r.draw_text(cs, {x, baseline}, composition_);
        r.fill_rect({x, top + lh - 2, w, 2}, ui.fade(t.accent));  // underlined while it is being composed
        x += w;
    }
    r.draw_text(s, {x, baseline}, after);
    if (caret) r.fill_rect({c.x - scroll_ + caret_x, top, kCaretWidth, lh}, ui.fade(t.text));
    r.pop_clip();
}

}  // namespace wnk::ui
