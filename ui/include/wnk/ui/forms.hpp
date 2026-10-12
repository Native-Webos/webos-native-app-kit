// SPDX-License-Identifier: GPL-3.0-or-later
//
// Settings and form controls, built for the remote: every control is one focusable row operated with
// OK and Left/Right, so a column of them navigates with Up/Down like any list.
//
//   Toggle      on/off (a switch, or a check box)           OK flips
//   Slider      a number in a range                         Left/Right step, held keys repeat
//   Choice      one of a few options (radio-button role)    Left/Right step through, OK opens the list
//   ValueRow    a label and a value that opens something    OK runs the handler (a sub-page, a dialog)
//   Tabs        a row of tabs; the page follows focus        with the pointer, a click selects
//   TextView    long text that scrolls with Up/Down          then focus moves on at either end
//   QrCode      a QR code (for "continue on your phone")     not focusable
//
// Handlers run on the main thread after the value changed; set_*() calls never run them.
#pragma once

#include "wnk/ui/anim.hpp"
#include "wnk/ui/widgets.hpp"

#include <functional>
#include <string>
#include <vector>

namespace wnk::ui {

// A full-width focusable row: the label (and an optional muted description under it) on the left,
// the control's value on the right.
class FormRow : public View {
 public:
    explicit FormRow(std::string label);
    FormRow &set_label(std::string label);
    FormRow &set_description(std::string text);
    const std::string &label() const { return label_; }
    std::string accessible_text() const override;  // label, value (value_text), description

 protected:
    // What the value says aloud ("on", "120 %", "1080p").
    virtual std::string value_text_for_speech() const { return {}; }
    Size measure_content(Ui &ui, Size available) override;
    void draw(Ui &ui) override;
    // The value part, drawn right-aligned inside `area` (the content rectangle's right side).
    virtual void draw_value(Ui &ui, const Rect &area);
    // Width the value part needs, so the label does not run under it.
    virtual float value_width(Ui &ui) const;

 private:
    std::string label_, description_;
};

class Toggle : public FormRow {
 public:
    enum class Style : uint8_t { Switch, Check };
    using Handler = std::function<void(Ui &, bool)>;
    Toggle(std::string label, bool on = false, Handler on_change = {});
    Toggle &set_on(bool on);
    Toggle &set_style(Style s);
    Toggle &set_on_change(Handler h);
    bool on() const { return on_; }

 protected:
    bool on_action(Ui &ui, const input::ActionEvent &e) override;
    void update(Ui &ui, float dt) override;
    void draw_value(Ui &ui, const Rect &area) override;
    float value_width(Ui &ui) const override;
    std::string value_text_for_speech() const override { return on_ ? "on" : "off"; }

 private:
    bool on_;
    Style style_ = Style::Switch;
    Handler on_change_;
    Smoothed knob_;  // 0 off, 1 on
};

class Slider : public FormRow {
 public:
    using Handler = std::function<void(Ui &, float)>;
    using Format = std::function<std::string(float)>;
    Slider(std::string label, float min, float max, float step, float value, Handler on_change = {});
    Slider &set_value(float v);  // clamped and snapped to the step
    Slider &set_format(Format f);  // the value text; default: the number with the step's decimals
    Slider &set_on_change(Handler h);
    float value() const { return value_; }
    float min() const { return min_; }
    float max() const { return max_; }
    std::string value_text() const;

 protected:
    bool on_action(Ui &ui, const input::ActionEvent &e) override;
    bool on_wheel(Ui &ui, float notches) override;
    void update(Ui &ui, float dt) override;
    void draw_value(Ui &ui, const Rect &area) override;
    float value_width(Ui &ui) const override;
    std::string value_text_for_speech() const override { return value_text(); }

 private:
    float snap(float v) const;
    void change(Ui &ui, float v);
    float min_, max_, step_, value_;
    Handler on_change_;
    Format format_;
    Smoothed shown_;  // fraction drawn, follows the value
};

class Choice : public FormRow {
 public:
    using Handler = std::function<void(Ui &, int)>;
    Choice(std::string label, std::vector<std::string> options, int selected = 0, Handler on_change = {});
    Choice &set_selected(int index);
    Choice &set_options(std::vector<std::string> options, int selected = 0);
    Choice &set_on_change(Handler h);
    int selected() const { return selected_; }
    const std::vector<std::string> &options() const { return options_; }
    // Opens the dialog listing every option, the current one focused (what OK does).
    void open_list(Ui &ui);

 protected:
    bool on_action(Ui &ui, const input::ActionEvent &e) override;
    void draw_value(Ui &ui, const Rect &area) override;
    float value_width(Ui &ui) const override;
    std::string value_text_for_speech() const override {
        return selected_ >= 0 ? options_[static_cast<size_t>(selected_)] : std::string();
    }

 private:
    void change(Ui &ui, int index);
    std::vector<std::string> options_;
    int selected_;
    Handler on_change_;
};

class ValueRow : public FormRow {
 public:
    ValueRow(std::string label, std::string value = {}, ClickHandler on_click = {});
    ValueRow &set_value(std::string value);
    ValueRow &set_on_click(ClickHandler h);
    const std::string &value() const { return value_; }

 protected:
    bool on_action(Ui &ui, const input::ActionEvent &e) override;
    void draw_value(Ui &ui, const Rect &area) override;
    float value_width(Ui &ui) const override;
    std::string value_text_for_speech() const override { return value_; }

 private:
    std::string value_;
    ClickHandler on_click_;
};

// A row of tabs. Moving focus onto a tab with the D-pad selects it; the pointer selects on click, so
// sweeping across the row does not flip pages. The row is a focus scope that remembers its tab.
class Tabs : public Row {
 public:
    using Handler = std::function<void(Ui &, int)>;
    explicit Tabs(std::vector<std::string> titles, int selected = 0, Handler on_select = {});
    Tabs &set_selected(int index);  // no handler call
    Tabs &set_on_select(Handler h);
    int selected() const { return selected_; }
    size_t count() const { return tabs_.size(); }
    View &tab(size_t i);  // e.g. to name it for logs and tests

 private:
    class Tab;
    void select(Ui &ui, int index);
    std::vector<Tab *> tabs_;
    int selected_;
    Handler on_select_;
};

// Text longer than its frame. Focusable when it overflows: Up/Down scroll it a few lines at a time,
// and at either end the arrow moves focus on as usual. The pointer wheel scrolls it too.
class TextView : public View {
 public:
    explicit TextView(std::string text = {}, TextRole role = TextRole::Body);
    TextView &set_text(std::string text);
    const std::string &text() const;
    std::string accessible_text() const override;
    float max_scroll() const { return max_scroll_; }
    float scroll() const { return scroll_.target; }
    Point scroll_offset() const override { return {0, scroll_.value}; }
    bool clips_children() const override { return true; }

 protected:
    Size measure_content(Ui &ui, Size available) override;
    void layout_content(Ui &ui, const Rect &content) override;
    bool on_action(Ui &ui, const input::ActionEvent &e) override;
    bool on_wheel(Ui &ui, float notches) override;
    void update(Ui &ui, float dt) override;
    void draw_over(Ui &ui) override;

 private:
    bool scroll_by(float dy);
    Label *label_;
    Smoothed scroll_;
    float max_scroll_ = 0;
    float page_ = 0;
};

// A QR code drawn as dark modules on a light quiet zone, square, as large as its frame allows.
// Byte mode, error correction Medium by default (about 15% of the code may be damaged).
class QrCode : public View {
 public:
    enum class Ecc : uint8_t { Low, Medium, Quartile, High };
    explicit QrCode(std::string text = {}, Ecc ecc = Ecc::Medium);
    QrCode &set_text(std::string text);
    const std::string &text() const { return text_; }
    int modules() const { return size_; }       // modules per side, 0 if nothing could be encoded
    const std::string &error() const { return error_; }
    bool dark(int x, int y) const;              // for tests

 protected:
    Size measure_content(Ui &ui, Size available) override;
    void draw(Ui &ui) override;

 private:
    void encode();
    std::string text_;
    Ecc ecc_;
    int size_ = 0;
    std::vector<bool> modules_;
    std::string error_;
};

}  // namespace wnk::ui
