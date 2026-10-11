// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ui: the root of a widget tree. It owns the content tree, the overlay layer (dialogs), toasts, the
// animation clock and the FocusManager, and wires them to input and the renderer.
//
// Per frame, on the main thread:
//
//   for each ActionEvent:  ui.handle(e)   → focused view → its ancestors → geometric focus move
//   ui.update(dt)                          → clock, view updates (scrolling, loading), toasts
//   renderer.begin_frame(); ui.draw(); renderer.end_frame()   (draw() clears to theme().background)
//
// handle() returns false for events nobody consumed; Back at the root is the app's decision (e.g.
// "press Back again to exit").
#pragma once

#include "wnk/focus/focus.hpp"
#include "wnk/input/input.hpp"
#include "wnk/render/renderer.hpp"
#include "wnk/ui/anim.hpp"
#include "wnk/ui/view.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wnk::ui {

class Dialog;

struct Theme {
    render::FontId regular = render::kNoFont;
    render::FontId bold = render::kNoFont;
    render::Color background = render::Color::hex(0x0E1420FF);
    render::Color surface = render::Color::hex(0x172033FF);
    render::Color raised = render::Color::hex(0x24324DFF);
    render::Color raised_focused = render::Color::hex(0x3A5BA0FF);
    render::Color text = render::Color::hex(0xF2F5FAFF);
    render::Color text_muted = render::Color::hex(0x9AA7BDFF);
    render::Color text_disabled = render::Color::hex(0x5D6880FF);
    render::Color accent = render::Color::hex(0x7CC4FFFF);
    render::Color focus_ring = render::Color::hex(0xFFD54AFF);
    render::Color scrim = render::Color::hex(0x000000B0);
    render::Color placeholder = render::Color::hex(0x2B3956FF);
    float radius = 16;
    float ring_width = 6;
    float ring_gap = 8;  // ring drawn this far outside the focused view
    float text_size = 32;
    float small_size = 24;
    float title_size = 52;
};

// Where image widgets get pictures from. The kit's implementation (ServiceImageSource) sits on
// image::ImageService + ImageTextures; tests use a fake.
class ImageSource {
 public:
    using Ticket = uint64_t;
    struct Loaded {
        bool ok = false;
        std::string key;  // pass to texture()
        int width = 0, height = 0;
        std::string error;
    };
    virtual ~ImageSource() = default;
    // Starts loading; `done` runs later on the main thread, never inside this call and never after
    // cancel(ticket). max_w/max_h bound the decoded size (0 = original).
    virtual Ticket load(const std::string &url, int max_w, int max_h, std::function<void(Loaded)> done) = 0;
    virtual void cancel(Ticket ticket) = 0;
    // GPU texture for a loaded key, uploading if needed. kNoTexture if the pixels were evicted, in
    // which case the caller loads again.
    virtual render::TextureId texture(const std::string &key) = 0;
};

// What moved focus last; scrolling containers follow keyboard and programmatic focus but not hover,
// which would slide the hovered item away from under a stationary pointer.
enum class FocusSource : uint8_t { Program, Key, Pointer };

struct UiStats {
    size_t views = 0;        // in the content and overlay trees, last sync
    size_t focus_nodes = 0;  // registered with the FocusManager
    uint64_t layouts = 0;    // full layout passes since creation
    size_t retired = 0;      // views waiting to be destroyed
};

class Ui {
 public:
    Ui(render::Renderer &renderer, Theme theme, Size canvas = {1920, 1080});
    ~Ui();
    Ui(const Ui &) = delete;
    Ui &operator=(const Ui &) = delete;

    render::Renderer &renderer() { return r_; }
    const Theme &theme() const { return theme_; }
    Size canvas() const { return canvas_; }
    void set_image_source(ImageSource *source) { images_ = source; }
    ImageSource *images() const { return images_; }

    // Milliseconds of animation time (sum of update() dt), and the dt of the current update.
    double now_ms() const { return now_ms_; }

    // Content tree root: a full-canvas view that stacks its children. Add screens or a Navigator here.
    View &root() { return *root_; }

    // ---- frame -------------------------------------------------------------------------------------
    bool handle(const input::ActionEvent &e);
    // Magic Remote / mouse pointer. Returns true if the event hit something. Positions in the event
    // are drawable pixels; they are mapped to logical units with the renderer's current mapping, so
    // call this after the first frame has been drawn.
    bool handle_pointer(const input::PointerEvent &e);
    // Same, with the position already in logical units (tests, synthetic input).
    bool handle_pointer_logical(const input::PointerEvent &e, Point logical);
    // True from pointer movement until an arrow key, a Hidden event or the pointer leaving.
    bool pointer_active() const { return pointer_active_; }
    Point pointer_position() const { return pointer_; }
    FocusSource focus_source() const { return focus_source_; }
    // Topmost focusable, enabled view under `p` (logical units), honouring clipping and modal dialogs.
    View *hit_focusable(Point p);
    void update(float dt);
    void draw();
    // Runs layout now if anything invalidated it, and re-syncs focus. Entry points call this already.
    void sync();
    void invalidate_layout() { layout_dirty_ = true; }

    // ---- focus -------------------------------------------------------------------------------------
    View *focused() const;
    bool is_focused(const View *v) const { return v != nullptr && focused() == v; }
    bool has_focus_within(const View *v) const;
    // Focus a view now (registering it first if it was just created). If the view is not yet in an
    // attached tree the request is kept and applied on the next sync.
    bool focus(View *v);
    // Focus inside `subtree`: its remembered node if it is a focus scope, else its first focusable
    // descendant in reading order. Deferred to the next sync if nothing there is registered yet.
    bool focus_within(View *subtree);
    // Jump the ring to the focused view without animating (after a screen change).
    void snap_focus_ring() { ring_snap_ = true; }
    focus::FocusManager &focus_manager() { return fm_; }

    // ---- overlays ----------------------------------------------------------------------------------
    // Shows a modal dialog: focus is trapped in it until it is dismissed, then restored.
    Dialog &show_dialog(std::unique_ptr<Dialog> dialog);
    void dismiss_dialog(Dialog *dialog);
    size_t dialog_count() const;
    Dialog *top_dialog() const;
    // A transient message at the bottom of the screen; queued if one is already showing.
    void toast(std::string text, double duration_ms = 2500);
    bool toast_visible() const { return !toasts_.empty(); }

    // ---- helpers for widgets -----------------------------------------------------------------------
    // Destroys `v` at the start of the next entry point (handle/update/draw/sync).
    void retire(std::unique_ptr<View> v);
    // Colour multiplied by the current subtree opacity.
    render::Color fade(render::Color c) const { return c.with_alpha(c.a * alpha_); }
    float alpha() const { return alpha_; }
    View *find(ViewId id) const;

    UiStats stats() const;

 private:
    friend class View;
    void refresh();  // layout if dirty + focus sync, without destroying retired views
    bool route(const input::ActionEvent &e);  // focused view → ancestors
    void walk(View &v, focus::ScopeId scope, Point offset, bool shown, std::vector<focus::NodeSpec> &on,
              std::vector<focus::NodeSpec> &off);
    bool attached(const View &v) const;
    void apply_pending_focus();
    bool focus_within_now(View &subtree);
    void dispatch_focus_events();
    View *first_focusable(View &subtree) const;
    void step_ring(float dt);
    void draw_ring();
    void draw_toasts();
    void hover();
    View *hit(View &v, Point p, Point offset, Rect clip, bool focusable_only);

    render::Renderer &r_;
    Theme theme_;
    Size canvas_;
    ImageSource *images_ = nullptr;
    focus::FocusManager fm_;
    std::vector<std::unique_ptr<View>> graveyard_;  // destroyed by the next outermost entry point
    std::unique_ptr<View> root_;
    std::unique_ptr<View> overlay_;
    std::unordered_map<ViewId, View *> views_;                    // rebuilt by every refresh
    std::unordered_map<focus::ScopeId, focus::ScopeId> scopes_;  // registered scope → parent scope
    std::unordered_set<focus::ScopeId> seen_scopes_;
    std::unordered_set<focus::NodeId> registered_;
    size_t node_count_ = 0;
    int depth_ = 0;
    double now_ms_ = 0;
    float alpha_ = 1;
    bool layout_dirty_ = true;
    uint64_t layouts_ = 0;
    ViewId pending_focus_ = 0;
    ViewId pending_within_ = 0;
    focus::NodeId notified_focus_ = focus::kNoNode;
    // Focus ring: follows the focused view; a focus change animates as a decaying offset.
    ViewId ring_view_ = 0;
    Rect ring_rect_;
    float ring_radius_ = 0;
    Smoothed ring_dx_, ring_dy_, ring_dw_, ring_dh_, ring_alpha_;
    bool ring_snap_ = true;
    struct Toast {
        std::string text;
        double duration_ms;
        double shown_at = -1;
    };
    std::deque<Toast> toasts_;
    // Pointer.
    bool pointer_active_ = false;
    Point pointer_;
    ViewId pressed_ = 0;
    FocusSource focus_source_ = FocusSource::Program;
};

}  // namespace wnk::ui
