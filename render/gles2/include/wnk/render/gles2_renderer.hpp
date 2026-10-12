// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenGL ES 2.0 implementation of wnk::render::Renderer (ADR-004). The same code runs on the host
// (Mesa EGL/GLES2) and on the TV (firmware EGL/GLES2); only the GL context comes from the platform.
//
// Batching: every primitive is one quad; consecutive quads share a draw call until the texture,
// clip or batch capacity changes. One shader draws rounded rectangles and borders (signed distance),
// RGBA images and alpha-only glyphs. Blending writes correct destination alpha so the UI can be
// composited over a hardware video plane.
#pragma once

#include "wnk/render/font.hpp"
#include "wnk/render/renderer.hpp"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wnk::render {

class Gles2Renderer final : public Renderer {
 public:
    // Requires a current GLES2 context. Returns nullptr (and sets *error) if shaders fail to build.
    static std::unique_ptr<Gles2Renderer> create(std::string *error = nullptr);
    ~Gles2Renderer() override;
    Gles2Renderer(const Gles2Renderer &) = delete;
    Gles2Renderer &operator=(const Gles2Renderer &) = delete;

    void set_logical_size(Size logical) override;
    void begin_frame(int drawable_w, int drawable_h) override;
    const CanvasMapping &mapping() const override { return mapping_; }
    void clear(Color color) override;

    void push_clip(const Rect &logical) override;
    void pop_clip() override;
    void push_translate(float dx, float dy) override;
    void pop_translate() override;

    void fill_rect(const Rect &rect, Color color, float radius = 0) override;
    void stroke_rect(const Rect &rect, Color color, float width, float radius = 0) override;
    void draw_image(TextureId texture, const Rect &dst, Color tint = kWhite, const Rect &uv = {0, 0, 1, 1}) override;
    float draw_text(const TextStyle &style, Point baseline, std::string_view utf8) override;
    float measure_text(const TextStyle &style, std::string_view utf8) const override;
    float line_height(const TextStyle &style) const override;
    float ascent(const TextStyle &style) const override;
    void end_frame() override;

    TextureId create_texture(int width, int height, const uint8_t *rgba) override;
    bool destroy_texture(TextureId id) override;
    FontId load_font(std::vector<uint8_t> ttf, std::string *error = nullptr) override;
    FontId load_font_file(const std::string &path, std::string *error = nullptr) override;

    RenderStats stats() const override;
    GpuInfo gpu_info() const override;

    // A hash of everything the last frame drew (valid after end_frame): drawable size, clears, scissor
    // rectangles, every batch's texture and vertices, and the texture and glyph uploads since the
    // previous frame. Equal fingerprints mean the frames look the same, so the shell need not present.
    uint64_t frame_fingerprint() const { return last_fingerprint_; }
    // Read back drawable pixels as RGBA, top row first (tests and diagnostics; flushes first).
    std::vector<uint8_t> read_pixels(int x, int y, int w, int h);

 private:
    Gles2Renderer() = default;
    bool init(std::string *error);

    struct Vertex {
        float x, y;        // drawable pixels
        float u, v;        // texture coordinates
        float lx, ly;      // position relative to the quad centre, pixels
        float hw, hh;      // quad half size, pixels
        float radius, border, mode;
        uint8_t r, g, b, a;
    };
    enum Mode { kShape = 0, kImage = 1, kGlyph = 2 };

    struct GlyphKey {
        FontId font;
        int glyph;
        int pixel_size;
        bool operator==(const GlyphKey &o) const {
            return font == o.font && glyph == o.glyph && pixel_size == o.pixel_size;
        }
    };
    struct GlyphKeyHash {
        size_t operator()(const GlyphKey &k) const {
            return (static_cast<size_t>(k.font) * 73856093u) ^ (static_cast<size_t>(k.glyph) * 19349663u) ^
                   (static_cast<size_t>(k.pixel_size) * 83492791u);
        }
    };
    struct GlyphEntry {
        float u0, v0, u1, v1;
        int width, height, x_offset, y_offset;
    };
    struct Texture {
        unsigned gl = 0;
        int width = 0, height = 0;
    };
    struct PixelRect {
        int x, y, w, h;
    };

    void quad(const Rect &px, float u0, float v0, float u1, float v1, Color c, float radius, float border, Mode mode,
              unsigned texture);
    void flush();
    void mix(uint64_t v) { fingerprint_ = (fingerprint_ ^ v) * 0x100000001B3ull; }
    void apply_scissor();
    Rect to_pixels(const Rect &logical) const;
    const GlyphEntry *glyph(FontId font, int glyph_index, int pixel_size);
    void reset_atlas();

    CanvasMapping mapping_;
    Size logical_{1920, 1080};
    std::vector<Vertex> vertices_;
    unsigned batch_texture_ = 0;
    unsigned program_ = 0, vbo_ = 0, ibo_ = 0, white_ = 0, atlas_ = 0;
    int u_viewport_ = -1, u_tex_ = -1;
    std::vector<PixelRect> clips_;
    std::vector<Point> translates_;
    Point translate_{0, 0};

    std::map<TextureId, Texture> textures_;
    TextureId next_texture_ = 1;
    std::map<FontId, std::unique_ptr<FontFace>> fonts_;
    FontId next_font_ = 1;

    static constexpr int kAtlasSize = 1024;
    ShelfPacker packer_{kAtlasSize, kAtlasSize, 1};
    std::unordered_map<GlyphKey, GlyphEntry, GlyphKeyHash> glyphs_;

    RenderStats frame_stats_;
    RenderStats last_stats_;
    uint32_t atlas_resets_ = 0;
    uint64_t fingerprint_ = 0, last_fingerprint_ = 0;
    uint64_t uploads_ = 0;  // texture creations, deletions and glyph uploads, ever
    bool highp_ = false;
};

}  // namespace wnk::render
