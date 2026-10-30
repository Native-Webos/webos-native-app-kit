// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layouts and widgets: Row/Column (Box), Grid, ScrollView, Label, ImageView, Button, Card, TextField,
// Spinner, ProgressBar. Virtualised lists and grids are in collection.hpp; dialogs and the navigation stack in
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
    // Animated GIF and WebP play (their own frame times and loop count) unless this is off; off
    // shows the first frame.
    ImageView &set_animate(bool on);
    // Show only the region (x, y, w, h) of the image, in the image's own pixels (a tile of a sprite
    // sheet: seek previews). The image is then loaded at its original size. w 0: the whole image.
    ImageView &set_region(int x, int y, int w, int h);
    const std::string &url() const { return url_; }
    enum class State : uint8_t { Empty, Loading, Ready, Failed };
    State state() const { return state_; }
    size_t frame_index() const { return frame_; }  // the animation frame showing
    size_t frame_count() const { return delays_.empty() ? 1 : delays_.size(); }

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
    bool animate_ = true;
    int region_x_ = 0, region_y_ = 0, region_w_ = 0, region_h_ = 0;
    std::vector<int> delays_;  // of the loaded animation
    int loops_ = 0, played_ = 0;
    size_t frame_ = 0;
    float frame_ms_ = 0;  // time the current frame has shown
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
    std::string accessible_text() const override;

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
    std::string accessible_text() const override;
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

// A single-line text input. OK or a click starts editing: the shell turns text input on, which opens
// LG's on-screen keyboard on the TV; a USB or Bluetooth keyboard types directly. While editing:
// Left/Right/Home/End move the cursor, Backspace/Delete erase, OK/Enter submits and ends editing,
// Back ends editing and keeps the text, and moving focus away ends it too. Text is UTF-8; the
// cursor never splits a code point. No selection or clipboard.
class TextField : public View {
 public:
    using TextHandler = std::function<void(Ui &, const std::string &)>;
    explicit TextField(std::string placeholder = {});
    TextField &set_text(std::string text);  // replaces the text (no change callback), cursor at the end
    TextField &set_placeholder(std::string placeholder);
    TextField &set_password(bool on);        // shows a bullet per character
    TextField &set_max_length(size_t code_points);  // 0: unlimited; longer input is cut
    TextField &set_on_change(TextHandler h);  // after every edit
    TextField &set_on_submit(TextHandler h);  // OK/Enter while editing
    const std::string &text() const { return text_; }
    const std::string &placeholder() const { return placeholder_; }
    size_t cursor() const { return cursor_; }  // byte offset into text()
    bool editing() const { return editing_; }
    bool password() const { return password_; }
    const std::string &composition() const { return composition_; }
    void begin_editing(Ui &ui);
    void end_editing(Ui &ui);
    // What draw() shows for the text (bullets in password mode).
    std::string display_text() const;
    std::string accessible_text() const override;  // never the content of a password field

 protected:
    Size measure_content(Ui &ui, Size available) override;
    bool on_action(Ui &ui, const input::ActionEvent &e) override;
    void on_focus_changed(Ui &ui, bool focused) override;
    bool on_text(Ui &ui, const std::string &utf8) override;
    bool on_edit_key(Ui &ui, input::EditKey key) override;
    void on_composition(Ui &ui, const std::string &utf8) override;
    void on_text_input_closed(Ui &ui) override;
    void draw(Ui &ui) override;

 private:
    void insert(Ui &ui, const std::string &utf8);
    void changed(Ui &ui);
    void submit(Ui &ui);
    std::string text_, placeholder_, composition_;
    size_t cursor_ = 0;
    size_t max_length_ = 0;
    bool password_ = false;
    bool editing_ = false;
    float scroll_ = 0;          // horizontal, keeps the cursor in view
    double caret_since_ = 0;    // the caret blinks from here (reset by every edit)
    TextHandler on_change_, on_submit_;
};

// A rectangle the app draws into with OpenGL ES 2 itself: a game's world, a visualiser, a map. Views
// may sit around or over it as usual. The callback runs inside Ui::draw with the GL context current,
// the viewport and scissor set to the view (Renderer::draw_custom); the renderer restores its own
// state afterwards. Animated by default (redrawn and presented every frame while visible); a static
// view is presented again only after invalidate_content().
class GlView : public View {
 public:
    using DrawFn = std::function<void(const render::CustomTarget &target, double now_ms)>;
    explicit GlView(DrawFn on_draw = {});
    GlView &set_on_draw(DrawFn fn);
    GlView &set_animated(bool animated);
    void invalidate_content() { ++version_; }
    bool animated() const { return animated_; }

 protected:
    void draw(Ui &ui) override;
    // Subclasses may draw here instead of passing a callback.
    virtual void render_gl(const render::CustomTarget &target, double now_ms);

 private:
    DrawFn on_draw_;
    bool animated_ = true;
    uint64_t version_ = 1;
    uint64_t frames_ = 0;
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
