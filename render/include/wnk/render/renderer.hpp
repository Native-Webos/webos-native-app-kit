// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kit's 2D renderer interface. UI code draws in LOGICAL units on a fixed canvas (1920x1080 by
// default); the renderer maps that to the drawable with uniform scaling, centred (letterboxed).
//
// Ownership and threading: every method must be called on the thread that owns the GL context
// (the main thread). Textures and fonts are owned by the renderer and referenced by id; ids are
// never reused within one renderer, so a stale id is detectable rather than aliasing a new texture.
#pragma once

#include "wnk/geometry.hpp"
#include "wnk/render/color.hpp"

#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace wnk::render {

using TextureId = uint32_t;
using FontId = uint32_t;
inline constexpr TextureId kNoTexture = 0;
inline constexpr FontId kNoFont = 0;

struct CanvasMapping {
    Size logical{1920, 1080};
    float scale = 1;     // drawable pixels per logical unit
    float offset_x = 0;  // letterbox offsets in drawable pixels
    float offset_y = 0;
    int drawable_w = 0, drawable_h = 0;

    static CanvasMapping fit(Size logical, int drawable_w, int drawable_h);
    Point to_pixels(Point p) const { return {offset_x + p.x * scale, offset_y + p.y * scale}; }
    Point to_logical(Point px) const { return {(px.x - offset_x) / scale, (px.y - offset_y) / scale}; }
};

struct TextStyle {
    FontId font = kNoFont;
    float size = 32;  // logical units (pixel height of the font's em at scale 1)
    Color color = kWhite;
};

struct RenderStats {
    uint32_t draw_calls = 0;     // last frame
    uint32_t quads = 0;          // last frame
    uint32_t textures = 0;       // live user textures
    uint32_t glyphs_cached = 0;  // glyphs currently in the atlas
    uint32_t atlas_resets = 0;   // since creation
};

// Where a custom draw may render, in drawable pixels with GL's bottom-left origin (glViewport order).
struct CustomTarget {
    int x = 0, y = 0, width = 0, height = 0;  // the view's rectangle, clipped to the current clip
    int drawable_width = 0, drawable_height = 0;
};
using CustomDraw = std::function<void(const CustomTarget &)>;

struct GpuInfo {
    std::string vendor, renderer, version, shading_language, extensions;
    int max_texture_size = 0;
    bool fragment_highp = false;
};

class Renderer {
 public:
    virtual ~Renderer() = default;

    virtual void set_logical_size(Size logical) = 0;
    virtual void begin_frame(int drawable_w, int drawable_h) = 0;
    virtual const CanvasMapping &mapping() const = 0;
    // Clears the whole drawable, letterbox included. Color{0,0,0,0} lets a video plane show through.
    virtual void clear(Color color) = 0;

    virtual void push_clip(const Rect &logical) = 0;  // intersected with the current clip
    virtual void pop_clip() = 0;
    virtual void push_translate(float dx, float dy) = 0;
    virtual void pop_translate() = 0;

    virtual void fill_rect(const Rect &rect, Color color, float radius = 0) = 0;
    virtual void stroke_rect(const Rect &rect, Color color, float width, float radius = 0) = 0;
    virtual void draw_image(TextureId texture, const Rect &dst, Color tint = kWhite,
                            const Rect &uv = {0, 0, 1, 1}) = 0;
    // Draws one line of text with its baseline at `baseline`. Returns the advance in logical units.
    virtual float draw_text(const TextStyle &style, Point baseline, std::string_view utf8) = 0;
    virtual float measure_text(const TextStyle &style, std::string_view utf8) const = 0;
    // Ascent/descent/line height of a style, in logical units.
    virtual float line_height(const TextStyle &style) const = 0;
    virtual float ascent(const TextStyle &style) const = 0;

    // Lets the app draw with GLES2 itself inside `logical`: everything queued so far is drawn first,
    // the viewport and scissor are set to the rectangle, `draw` runs, and the renderer's own GL state
    // is restored afterwards (any program, buffers, textures, blending the callback changed).
    // `version` stands for the content: the same version on two frames means the same picture, so a
    // still frame need not be presented; change it whenever the content may have changed.
    virtual void draw_custom(const Rect &logical, uint64_t version, const CustomDraw &draw);

    virtual void end_frame() = 0;  // flushes pending geometry; the caller swaps buffers

    // Resources.
    virtual TextureId create_texture(int width, int height, const uint8_t *rgba) = 0;
    virtual bool destroy_texture(TextureId id) = 0;
    // Replaces a texture's pixels in place (same size), for animations. False if it cannot: the
    // caller then creates a new texture instead.
    virtual bool update_texture(TextureId id, int width, int height, const uint8_t *rgba) {
        (void)id, (void)width, (void)height, (void)rgba;
        return false;
    }
    virtual FontId load_font(std::vector<uint8_t> ttf, std::string *error = nullptr) = 0;
    virtual FontId load_font_file(const std::string &path, std::string *error = nullptr) = 0;

    virtual RenderStats stats() const = 0;
    virtual GpuInfo gpu_info() const = 0;
};

}  // namespace wnk::render
