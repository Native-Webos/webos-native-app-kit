// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layouts and widgets: Row/Column (Box), Grid, ScrollView, Label, ImageView, Button, Card, Spinner,
// ProgressBar. Virtualised lists and grids are in collection.hpp; dialogs and the navigation stack in
// navigation.hpp.
#pragma once

#include "wnk/render/renderer.hpp"
#include "wnk/ui/anim.hpp"
#include "wnk/ui/ui.hpp"
#include "wnk/ui/view.hpp"

#include <functional>
#include <string>
#include <vector>

namespace wnk::ui {

// ---- layouts ---------------------------------------------------------------------------------------

enum class Axis : uint8_t { Horizontal, Vertical };

// Lays children out in a line. Fill children share the space left after the others; `justify`
// positions the children along the axis when none of them fills.
class Box : public View {
 public:
    explicit Box(Axis axis, float gap = 0) : axis_(axis), gap_(gap) {}
    Box &set_gap(float g);
    Box &set_justify(Align a);
    Axis axis() const { return axis_; }

 protected:
    Size measure_content(Ui &ui, Size available) override;
    void layout_content(Ui &ui, const Rect &content) override;

 private:
    Axis axis_;
    float gap_;
    Align justify_ = Align::Start;
};

class Row : public Box {
 public:
    explicit Row(float gap = 0) : Box(Axis::Horizontal, gap) {}
};
class Column : public Box {
 public:
    explicit Column(float gap = 0) : Box(Axis::Vertical, gap) {}
};

// Fixed number of equal-width columns; each row is as tall as its tallest child. Not virtualised:
// use CollectionView for long or data-driven grids.
class Grid : public View {
 public:
    explicit Grid(int columns, float gap = 0) : columns_(columns < 1 ? 1 : columns), gap_x_(gap), gap_y_(gap) {}
    Grid &set_gaps(float x, float y);

 protected:
    Size measure_content(Ui &ui, Size available) override;
    void layout_content(Ui &ui, const Rect &content) override;

 private:
    int columns_;
    float gap_x_, gap_y_;
};

// Vertical scrolling for content that is not a list (a details page). The single child is measured
// with unbounded height; the view scrolls so the focused descendant stays visible.
class ScrollView : public View {
 public:
    ScrollView();
    Point scroll_offset() const override { return {0, scroll_.value}; }
    bool clips_children() const override { return true; }
    float max_scroll() const { return max_scroll_; }
    void scroll_to(float y, bool animate = true);
    float margin = 60;  // keep the focused view this far from the edges where possible

 protected:
    Size measure_content(Ui &ui, Size available) override;
    void layout_content(Ui &ui, const Rect &content) override;
    void on_descendant_focused(Ui &ui, View *child, View *focused) override;
    void update(Ui &ui, float dt) override;
    bool on_wheel(Ui &ui, float notches) override;

 private:
    Smoothed scroll_;
    float max_scroll_ = 0;
};

// ---- text ------------------------------------------------------------------------------------------

enum class TextRole : uint8_t { Body, Small, Title };

// Text with word wrapping up to `max_lines` and an ellipsis when it does not fit. Single-line
// labels shorter than the available width are measured at their natural width.
class Label : public View {
 public:
    explicit Label(std::string text = {}, TextRole role = TextRole::Body);
    Label &set_text(std::string text);
    Label &set_role(TextRole role);
    Label &set_bold(bool bold);
    Label &set_color(render::Color c);
    Label &set_muted(bool muted);
    Label &set_max_lines(int n);
    Label &set_text_align(Align a);  // Start/Center/End within the frame
    Label &set_font_size(float size);
    const std::string &text() const { return text_; }
    const std::vector<std::string> &lines() const { return lines_; }  // after layout
    render::TextStyle style(const Ui &ui) const;

    // Greedy word wrap (breaks long words) with an ellipsis on the last line. Exposed for tests.
    static std::vector<std::string> wrap(const render::Renderer &r, const render::TextStyle &style,
                                         const std::string &text, float width, int max_lines);

 protected:
    Size measure_content(Ui &ui, Size available) override;
    void layout_content(Ui &ui, const Rect &content) override;
    void draw(Ui &ui) override;

 private:
    std::string text_;
    TextRole role_;
    bool bold_ = false;
    bool muted_ = false;
    std::optional<render::Color> color_;
    float size_override_ = 0;
    int max_lines_ = 1;
    Align text_align_ = Align::Start;
    std::vector<std::string> lines_;
};

// ---- images ----------------------------------------------------------------------------------------

enum class ImageFit : uint8_t { Cover, Contain };

// Loads `url` through Ui::images() once it has a frame, decoded at about the frame's size. Shows a
// pulsing placeholder while loading and a muted panel when loading fails.
class ImageView : public View {
 public:
    explicit ImageView(std::string url = {});
    ~ImageView() override;
    ImageView &set_url(std::string url);
    ImageView &set_fit(ImageFit fit);
    const std::string &url() const { return url_; }
    enum class State : uint8_t { Empty, Loading, Ready, Failed };
    State state() const { return state_; }

 protected:
    void update(Ui &ui, float dt) override;
    void draw(Ui &ui) override;

 private:
    void start(Ui &ui);
    void cancel();
    std::string url_;
    ImageFit fit_ = ImageFit::Cover;
    State state_ = State::Empty;
    ImageSource *source_ = nullptr;
    ImageSource::Ticket ticket_ = 0;
    std::string key_;
    int img_w_ = 0, img_h_ = 0;
    int reloads_ = 0;
};

// ---- controls --------------------------------------------------------------------------------------

using ClickHandler = std::function<void(Ui &)>;

class Button : public View {
 public:
    explicit Button(std::string text = {}, ClickHandler on_click = {});
    Button &set_text(std::string text);
    Button &set_on_click(ClickHandler h);
    const std::string &text() const { return text_; }
    bool click(Ui &ui);  // as if OK was pressed; false when disabled or without a handler

 protected:
    Size measure_content(Ui &ui, Size available) override;
    bool on_action(Ui &ui, const input::ActionEvent &e) override;
    void draw(Ui &ui) override;

 private:
    std::string text_;
    ClickHandler on_click_;
    Tween press_;
};

// A focusable poster: image on top, title (and optional subtitle) underneath.
class Card : public View {
 public:
    Card(std::string title, std::string image_url, std::string subtitle = {});
    Card &set_on_click(ClickHandler h);
    ImageView &image() { return *image_; }
    Label &title() { return *title_; }
    float image_aspect = 1.5f;  // height / width of the image area

 protected:
    void layout_content(Ui &ui, const Rect &content) override;
    Size measure_content(Ui &ui, Size available) override;
    bool on_action(Ui &ui, const input::ActionEvent &e) override;
    void on_focus_changed(Ui &ui, bool focused) override;

 public:
    Rect focus_bounds() const override;  // the image, not the title

 private:
    ImageView *image_;
    Label *title_;
    Label *subtitle_;
    ClickHandler on_click_;
};

class Spinner : public View {
 public:
    explicit Spinner(float size = 64);

 protected:
    void draw(Ui &ui) override;
};

// Determinate (value 0..1, animated towards the target) or indeterminate (a sliding segment).
class ProgressBar : public View {
 public:
    ProgressBar();
    ProgressBar &set_value(float v);  // clears indeterminate
    ProgressBar &set_indeterminate(bool on);
    float value() const { return value_.target; }
    float shown_value() const { return value_.value; }

 protected:
    Size measure_content(Ui &ui, Size available) override;
    void update(Ui &ui, float dt) override;
    void draw(Ui &ui) override;

 private:
    Smoothed value_;
    bool indeterminate_ = false;
};

}  // namespace wnk::ui
