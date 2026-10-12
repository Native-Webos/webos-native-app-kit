// SPDX-License-Identifier: GPL-3.0-or-later
//
// The view tree: the base class every widget derives from.
//
// A View has a frame in canvas coordinates (logical units, before any scrolling), layout hints
// (width/height, padding, alignment), an optional background, and children it owns. Layout is a
// two-pass measure/layout walk that runs only when something called invalidate(). Scrolling and
// slide animations do not relayout: a view's translation and scroll offset shift what it draws and
// where its focusable descendants are reported to the focus manager.
//
// Focus: a view marked focusable becomes a FocusManager node (node id = view id); a view marked as a
// focus scope becomes a scope. The Ui keeps both in sync with the tree every frame, so widgets never
// talk to the FocusManager directly. Hidden views stay registered but unfocusable, which keeps each
// scope's remembered focus while a screen is covered by another.
//
// Input: Ui::handle() offers each ActionEvent to the focused view, then to its ancestors, until one
// returns true from on_action(). Unhandled arrows fall back to geometric focus movement. With the
// Magic Remote pointer (Ui::handle_pointer) hovering a focusable view focuses it, a click is OK on it,
// and the wheel goes to on_wheel() of the views under the pointer.
//
// Ownership and threading: parents own children through unique_ptr. Everything here runs on the
// main thread. Views removed from a tree attached to a Ui are destroyed at the start of the next
// Ui entry point, never in the middle of the call that removed them, so a button's click handler
// may close the screen the button lives on.
#pragma once

#include "wnk/geometry.hpp"
#include "wnk/input/input.hpp"
#include "wnk/render/color.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wnk::ui {

class Ui;

using ViewId = uint32_t;

struct Insets {
    float left = 0, top = 0, right = 0, bottom = 0;
    static constexpr Insets all(float v) { return {v, v, v, v}; }
    static constexpr Insets xy(float x, float y) { return {x, y, x, y}; }
    float horizontal() const { return left + right; }
    float vertical() const { return top + bottom; }
};

// Size along one axis. Auto: the content's size (or the parent's full extent when aligned Stretch).
// Fixed: `value` logical units. Fill: a share of the space left in a Row/Column, weighted by `value`;
// elsewhere the parent's full extent.
struct Dim {
    enum Kind : uint8_t { Auto, Fixed, Fill };
    Kind kind = Auto;
    float value = 0;
    static constexpr Dim automatic() { return {Auto, 0}; }
    static constexpr Dim fixed(float v) { return {Fixed, v}; }
    static constexpr Dim fill(float weight = 1) { return {Fill, weight}; }
};

enum class Align : uint8_t { Start, Center, End, Stretch };

class View {
 public:
    View();
    virtual ~View();
    View(const View &) = delete;
    View &operator=(const View &) = delete;

    ViewId id() const { return id_; }
    View *parent() const { return parent_; }
    const std::vector<std::unique_ptr<View>> &children() const { return children_; }

    // ---- tree --------------------------------------------------------------------------------------
    template <typename T, typename... Args>
    T &add(Args &&...args) {
        return static_cast<T &>(add_child(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    View &add_child(std::unique_ptr<View> child);
    // Detaches `child`. If this tree belongs to a Ui the child is destroyed later (see header comment).
    void remove_child(View *child);
    void clear_children();
    // Detaches `child` and hands ownership to the caller (nullptr if it is not a child).
    std::unique_ptr<View> take_child(View *child);
    // The Ui this view's tree is attached to, or nullptr.
    Ui *ui() const;
    bool is_ancestor_of(const View *v) const;

    // ---- layout hints (setters invalidate layout) ----------------------------------------------------
    View &set_width(Dim d);
    View &set_height(Dim d);
    View &set_size(float w, float h) { return set_width(Dim::fixed(w)).set_height(Dim::fixed(h)); }
    View &set_padding(Insets p);
    View &set_align(Align x, Align y);
    View &set_visible(bool v);
    View &set_background(std::optional<render::Color> c, float radius = 0);
    Dim width() const { return width_; }
    Dim height() const { return height_; }
    const Insets &padding() const { return padding_; }
    Align align_x() const { return align_x_; }
    Align align_y() const { return align_y_; }
    bool visible() const { return visible_; }

    // Draw-time only (no relayout): opacity multiplies this subtree's colours; translation shifts it.
    void set_opacity(float a) { opacity_ = a; }
    float opacity() const { return opacity_; }
    void set_translation(Point t) { translation_ = t; }
    Point translation() const { return translation_; }

    // Marks layout dirty for the whole Ui. Call after changing anything that affects size.
    void invalidate();

    // Preferred outer size (padding included) given the space available. Do not override; override
    // measure_content instead.
    Size measure(Ui &ui, Size available);
    // Places this view at `frame` and lays out its children. Override layout_content, not this.
    void layout(Ui &ui, const Rect &frame);
    const Rect &frame() const { return frame_; }
    Rect content_rect() const;
    // The frame as it appears on screen: shifted by every ancestor's scroll offset and translation.
    Rect screen_rect() const;
    // A rectangle in this view's layout coordinates mapped to the screen like screen_rect().
    Rect to_screen(Rect r) const;

    // ---- focus -------------------------------------------------------------------------------------
    View &set_focusable(bool f);
    bool focusable() const { return focusable_; }
    View &set_enabled(bool e);
    bool enabled() const { return enabled_; }
    // A scope groups focusable descendants: `contain` keeps arrows inside it, `restore` makes arrow
    // entry go back to the node focused there last (row/column memory).
    View &set_focus_scope(bool contain = false, bool restore = true);
    bool is_focus_scope() const { return scope_; }
    bool scope_contains() const { return scope_contain_; }
    bool scope_restores() const { return scope_restore_; }
    // Ui draws an animated ring around the focused view unless the view draws its own focus state.
    View &set_focus_ring(bool on, float radius = -1);
    bool wants_focus_ring() const { return focus_ring_; }
    float focus_ring_radius() const { return ring_radius_ >= 0 ? ring_radius_ : radius_; }
    // The part of the frame the focus ring surrounds (layout coordinates); the whole frame by default.
    virtual Rect focus_bounds() const { return frame_; }

    // ---- hooks -------------------------------------------------------------------------------------
    virtual bool on_action(Ui &ui, const input::ActionEvent &e);
    virtual void on_focus_changed(Ui &ui, bool focused);
    // Called on every ancestor of a newly focused view; `child` is this view's child on the path.
    virtual void on_descendant_focused(Ui &ui, View *child, View *focused);
    virtual void update(Ui &ui, float dt);
    // Pointer wheel over this view or a descendant (bubbles like actions). `notches` > 0 means the
    // wheel turned away from the user: scroll towards the start.
    virtual bool on_wheel(Ui &ui, float notches);

    // Text input, delivered only to the view that called Ui::begin_text_input (see Ui). `utf8` is
    // committed text; a composition is text an input method is still building (empty when it ends).
    virtual bool on_text(Ui &ui, const std::string &utf8);
    virtual bool on_edit_key(Ui &ui, input::EditKey key);
    virtual void on_composition(Ui &ui, const std::string &utf8);
    // The system closed the keyboard (the viewer dismissed it); text input has already ended.
    virtual void on_text_input_closed(Ui &ui);

    // What a screen reader says when the view gets focus (Audio Guidance): its label and state, never
    // a secret. Empty: nothing to say. The default reads `accessible_label` if set, else nothing.
    virtual std::string accessible_text() const;
    std::string accessible_label;  // overrides what a widget would say about itself

    // Draws this view and its subtree (background, draw(), children, draw_over()).
    void draw_tree(Ui &ui);

    // Offset applied to children (scroll position). Positive y scrolls content up.
    virtual Point scroll_offset() const { return {0, 0}; }
    virtual bool clips_children() const { return false; }

    std::string name;  // optional, for logs and tests

 protected:
    virtual Size measure_content(Ui &ui, Size available);  // default: overlap children (stack)
    virtual void layout_content(Ui &ui, const Rect &content);
    virtual void draw(Ui &ui);       // own content, under the children
    virtual void draw_over(Ui &ui);  // above the children

    // Frame for a child aligned within `area` along both axes (used by stack-like layouts).
    static Rect place(Ui &ui, View &child, const Rect &area);
    static float extent(Dim d, Align a, float measured, float available);
    // Like add_child/remove_child but without invalidating layout: for containers that lay out the
    // child themselves (virtualised collections create and drop cells while scrolling).
    View &add_child_silent(std::unique_ptr<View> child);
    void remove_child_silent(View *child);

    float radius_ = 0;

 private:
    friend class Ui;

    ViewId id_;
    View *parent_ = nullptr;
    Ui *owner_ui_ = nullptr;  // set only on roots adopted by a Ui
    std::vector<std::unique_ptr<View>> children_;
    Dim width_, height_;
    Insets padding_;
    Align align_x_ = Align::Stretch, align_y_ = Align::Stretch;
    bool visible_ = true;
    bool focusable_ = false, enabled_ = true;
    bool scope_ = false, scope_contain_ = false, scope_restore_ = true;
    bool focus_ring_ = true;
    float ring_radius_ = -1;
    std::optional<render::Color> background_;
    float opacity_ = 1;
    Point translation_;
    Rect frame_;
};

}  // namespace wnk::ui
