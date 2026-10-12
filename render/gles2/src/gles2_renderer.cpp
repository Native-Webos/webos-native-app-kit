// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/render/gles2_renderer.hpp"

#include <GLES2/gl2.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <utility>

namespace wnk::render {
namespace {

constexpr int kMaxQuads = 4096;  // 16384 vertices: fits 16-bit indices

const char *kVertexShader = R"(
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec2 a_local;
attribute vec2 a_half;
attribute vec3 a_params;
attribute vec4 a_color;
uniform vec2 u_viewport;
varying vec2 v_uv;
varying vec2 v_local;
varying vec2 v_half;
varying vec3 v_params;
varying vec4 v_color;
void main() {
    v_uv = a_uv;
    v_local = a_local;
    v_half = a_half;
    v_params = a_params;
    v_color = a_color;
    gl_Position = vec4(a_pos.x / u_viewport.x * 2.0 - 1.0, 1.0 - a_pos.y / u_viewport.y * 2.0, 0.0, 1.0);
}
)";

// v_params = (corner radius, border width, mode). Mode 0: rounded rect / border by signed distance,
// 1: RGBA texture, 2: alpha-only glyph atlas. Distances are in drawable pixels.
const char *kFragmentShaderBody = R"(
uniform sampler2D u_tex;
varying vec2 v_uv;
varying vec2 v_local;
varying vec2 v_half;
varying vec3 v_params;
varying vec4 v_color;
float sd_round_rect(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
    float mode = v_params.z;
    if (mode < 0.5) {
        float d = sd_round_rect(v_local, v_half, v_params.x);
        float coverage = clamp(0.5 - d, 0.0, 1.0);
        if (v_params.y > 0.0) {
            coverage *= clamp(v_params.y + d + 0.5, 0.0, 1.0);
        }
        gl_FragColor = vec4(v_color.rgb, v_color.a * coverage);
    } else if (mode < 1.5) {
        gl_FragColor = texture2D(u_tex, v_uv) * v_color;
    } else {
        gl_FragColor = vec4(v_color.rgb, v_color.a * texture2D(u_tex, v_uv).a);
    }
}
)";

GLuint compile(GLenum type, const std::string &source, std::string *error) {
    GLuint shader = glCreateShader(type);
    const char *src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        if (error != nullptr) *error = std::string(type == GL_VERTEX_SHADER ? "vertex" : "fragment") + " shader: " + log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

uint8_t to_u8(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

std::string gl_string(GLenum name) {
    const GLubyte *s = glGetString(name);
    return s != nullptr ? reinterpret_cast<const char *>(s) : std::string();
}

}  // namespace

std::unique_ptr<Gles2Renderer> Gles2Renderer::create(std::string *error) {
    std::unique_ptr<Gles2Renderer> r(new Gles2Renderer());
    if (!r->init(error)) return nullptr;
    return r;
}

bool Gles2Renderer::init(std::string *error) {
    GLint range[2] = {0, 0}, precision = 0;
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
    highp_ = precision > 0;
    // Signed distances span up to ~1000 px on a 1080p rect; mediump (10-bit mantissa) is too coarse.
    const std::string fs = std::string(highp_ ? "precision highp float;\n" : "precision mediump float;\n") +
                           kFragmentShaderBody;
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader, error);
    if (vs == 0) return false;
    GLuint fsh = compile(GL_FRAGMENT_SHADER, fs, error);
    if (fsh == 0) {
        glDeleteShader(vs);
        return false;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fsh);
    glBindAttribLocation(program_, 0, "a_pos");
    glBindAttribLocation(program_, 1, "a_uv");
    glBindAttribLocation(program_, 2, "a_local");
    glBindAttribLocation(program_, 3, "a_half");
    glBindAttribLocation(program_, 4, "a_params");
    glBindAttribLocation(program_, 5, "a_color");
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fsh);
    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = {};
        glGetProgramInfoLog(program_, sizeof log, nullptr, log);
        if (error != nullptr) *error = std::string("link: ") + log;
        return false;
    }
    u_viewport_ = glGetUniformLocation(program_, "u_viewport");
    u_tex_ = glGetUniformLocation(program_, "u_tex");

    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ibo_);
    std::vector<uint16_t> indices(kMaxQuads * 6);
    for (int q = 0; q < kMaxQuads; ++q) {
        const auto v = static_cast<uint16_t>(q * 4);
        uint16_t *i = &indices[static_cast<size_t>(q) * 6];
        i[0] = v; i[1] = static_cast<uint16_t>(v + 1); i[2] = static_cast<uint16_t>(v + 2);
        i[3] = v; i[4] = static_cast<uint16_t>(v + 2); i[5] = static_cast<uint16_t>(v + 3);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(uint16_t)),
                 indices.data(), GL_STATIC_DRAW);

    // 1x1 white texture so shape-only batches always have a valid sampler bound.
    const uint8_t white[4] = {255, 255, 255, 255};
    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &atlas_);
    glBindTexture(GL_TEXTURE_2D, atlas_);
    std::vector<uint8_t> zeros(static_cast<size_t>(kAtlasSize) * kAtlasSize, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, kAtlasSize, kAtlasSize, 0, GL_ALPHA, GL_UNSIGNED_BYTE, zeros.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    vertices_.reserve(kMaxQuads * 4);
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        if (error != nullptr) *error = "GL error during renderer init: " + std::to_string(err);
        return false;
    }
    return true;
}

Gles2Renderer::~Gles2Renderer() {
    for (auto &[id, t] : textures_) glDeleteTextures(1, &t.gl);
    if (atlas_ != 0) glDeleteTextures(1, &atlas_);
    if (white_ != 0) glDeleteTextures(1, &white_);
    if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
    if (ibo_ != 0) glDeleteBuffers(1, &ibo_);
    if (program_ != 0) glDeleteProgram(program_);
}

void Gles2Renderer::set_logical_size(Size logical) {
    if (logical.w > 0 && logical.h > 0) logical_ = logical;
}

void Gles2Renderer::begin_frame(int drawable_w, int drawable_h) {
    mapping_ = CanvasMapping::fit(logical_, drawable_w, drawable_h);
    vertices_.clear();
    batch_texture_ = 0;
    clips_.clear();
    translates_.clear();
    translate_ = {0, 0};
    frame_stats_ = {};
    fingerprint_ = 0xCBF29CE484222325ull;
    mix(static_cast<uint64_t>(drawable_w) << 32 | static_cast<uint32_t>(drawable_h));

    apply_state();
}

void Gles2Renderer::apply_state() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);  // a custom draw may have rendered to a texture
    GLint attribs = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attribs);
    for (GLint i = 6; i < attribs; ++i) glDisableVertexAttribArray(static_cast<GLuint>(i));
    glViewport(0, 0, mapping_.drawable_w, mapping_.drawable_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    // Straight alpha for colour; destination alpha accumulates coverage so a transparent clear lets
    // the video plane show through exactly where nothing opaque was drawn.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(program_);
    glUniform2f(u_viewport_, static_cast<float>(mapping_.drawable_w), static_cast<float>(mapping_.drawable_h));
    glUniform1i(u_tex_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    const auto stride = static_cast<GLsizei>(sizeof(Vertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(Vertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(Vertex, lx)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(Vertex, hw)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(Vertex, radius)));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void *>(offsetof(Vertex, r)));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, white_);
    apply_scissor();
}

void Gles2Renderer::draw_custom(const Rect &logical, uint64_t version, const CustomDraw &draw) {
    flush();
    mix(0xC057ull);
    mix(version);
    const Rect px = to_pixels(logical);
    int x0 = static_cast<int>(std::floor(px.x)), y0 = static_cast<int>(std::floor(px.y));
    int x1 = static_cast<int>(std::ceil(px.right())), y1 = static_cast<int>(std::ceil(px.bottom()));
    const PixelRect clip = clips_.empty() ? PixelRect{0, 0, mapping_.drawable_w, mapping_.drawable_h} : clips_.back();
    x0 = std::max(x0, clip.x);
    y0 = std::max(y0, clip.y);
    x1 = std::min(x1, clip.x + clip.w);
    y1 = std::min(y1, clip.y + clip.h);
    if (x1 <= x0 || y1 <= y0 || !draw) return;
    CustomTarget t;
    t.x = x0;
    t.y = mapping_.drawable_h - y1;  // GL counts rows from the bottom
    t.width = x1 - x0;
    t.height = y1 - y0;
    t.drawable_width = mapping_.drawable_w;
    t.drawable_height = mapping_.drawable_h;
    glViewport(t.x, t.y, t.width, t.height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(t.x, t.y, t.width, t.height);
    draw(t);
    apply_state();
}

void Gles2Renderer::clear(Color color) {
    flush();
    mix(0xC1EA2ull);
    mix(static_cast<uint64_t>(to_u8(color.r)) << 24 | to_u8(color.g) << 16 | to_u8(color.b) << 8 | to_u8(color.a));
    glDisable(GL_SCISSOR_TEST);
    glClearColor(color.r, color.g, color.b, color.a);
    glClear(GL_COLOR_BUFFER_BIT);
    apply_scissor();
}

Rect Gles2Renderer::to_pixels(const Rect &l) const {
    const Point p = mapping_.to_pixels({l.x + translate_.x, l.y + translate_.y});
    return {p.x, p.y, l.w * mapping_.scale, l.h * mapping_.scale};
}

void Gles2Renderer::apply_scissor() {
    if (clips_.empty()) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    const PixelRect &c = clips_.back();
    glEnable(GL_SCISSOR_TEST);
    glScissor(c.x, mapping_.drawable_h - (c.y + c.h), c.w, c.h);
}

void Gles2Renderer::push_clip(const Rect &logical) {
    flush();
    const Rect px = to_pixels(logical);
    int x0 = static_cast<int>(std::floor(px.x)), y0 = static_cast<int>(std::floor(px.y));
    int x1 = static_cast<int>(std::ceil(px.right())), y1 = static_cast<int>(std::ceil(px.bottom()));
    PixelRect parent = clips_.empty() ? PixelRect{0, 0, mapping_.drawable_w, mapping_.drawable_h} : clips_.back();
    x0 = std::max(x0, parent.x);
    y0 = std::max(y0, parent.y);
    x1 = std::min(x1, parent.x + parent.w);
    y1 = std::min(y1, parent.y + parent.h);
    clips_.push_back({x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)});
    apply_scissor();
}

void Gles2Renderer::pop_clip() {
    if (clips_.empty()) return;
    flush();
    clips_.pop_back();
    apply_scissor();
}

void Gles2Renderer::push_translate(float dx, float dy) {
    translates_.push_back(translate_);
    translate_.x += dx;
    translate_.y += dy;
}

void Gles2Renderer::pop_translate() {
    if (translates_.empty()) return;
    translate_ = translates_.back();
    translates_.pop_back();
}

void Gles2Renderer::quad(const Rect &px, float u0, float v0, float u1, float v1, Color c, float radius, float border,
                         Mode mode, unsigned texture) {
    if (texture != 0 && batch_texture_ != 0 && texture != batch_texture_) flush();
    if (vertices_.size() + 4 > static_cast<size_t>(kMaxQuads) * 4) flush();
    if (texture != 0) batch_texture_ = texture;

    const float hw = px.w * 0.5f, hh = px.h * 0.5f;
    // Shapes get one pixel of margin so the anti-aliased edge has room; images and glyphs do not.
    const float pad = mode == kShape ? 1.0f : 0.0f;
    const float x0 = px.x - pad, y0 = px.y - pad, x1 = px.right() + pad, y1 = px.bottom() + pad;
    const float r = std::min(radius, std::min(hw, hh));
    const uint8_t cr = to_u8(c.r), cg = to_u8(c.g), cb = to_u8(c.b), ca = to_u8(c.a);
    const auto m = static_cast<float>(mode);
    vertices_.push_back({x0, y0, u0, v0, -hw - pad, -hh - pad, hw, hh, r, border, m, cr, cg, cb, ca});
    vertices_.push_back({x1, y0, u1, v0, hw + pad, -hh - pad, hw, hh, r, border, m, cr, cg, cb, ca});
    vertices_.push_back({x1, y1, u1, v1, hw + pad, hh + pad, hw, hh, r, border, m, cr, cg, cb, ca});
    vertices_.push_back({x0, y1, u0, v1, -hw - pad, hh + pad, hw, hh, r, border, m, cr, cg, cb, ca});
}

void Gles2Renderer::flush() {
    if (vertices_.empty()) return;
    // The batch's state and vertices, eight bytes at a time (a Vertex is 64 bytes, no padding).
    static_assert(sizeof(Vertex) % 8 == 0, "fingerprint reads whole words");
    mix(batch_texture_);
    if (!clips_.empty()) {
        const PixelRect &c = clips_.back();
        mix(static_cast<uint64_t>(static_cast<uint32_t>(c.x)) << 32 | static_cast<uint32_t>(c.y));
        mix(static_cast<uint64_t>(static_cast<uint32_t>(c.w)) << 32 | static_cast<uint32_t>(c.h));
    }
    const auto *bytes = reinterpret_cast<const unsigned char *>(vertices_.data());
    const size_t words = vertices_.size() * sizeof(Vertex) / 8;
    for (size_t i = 0; i < words; ++i) {
        uint64_t w;
        std::memcpy(&w, bytes + i * 8, 8);
        mix(w);
    }
    glBindTexture(GL_TEXTURE_2D, batch_texture_ != 0 ? batch_texture_ : white_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices_.size() * sizeof(Vertex)), vertices_.data(),
                 GL_STREAM_DRAW);
    const auto quads = static_cast<GLsizei>(vertices_.size() / 4);
    glDrawElements(GL_TRIANGLES, quads * 6, GL_UNSIGNED_SHORT, nullptr);
    frame_stats_.draw_calls++;
    frame_stats_.quads += static_cast<uint32_t>(quads);
    vertices_.clear();
    batch_texture_ = 0;
}

void Gles2Renderer::fill_rect(const Rect &rect, Color color, float radius) {
    if (rect.empty() || color.a <= 0) return;
    quad(to_pixels(rect), 0, 0, 1, 1, color, radius * mapping_.scale, 0, kShape, 0);
}

void Gles2Renderer::stroke_rect(const Rect &rect, Color color, float width, float radius) {
    if (rect.empty() || color.a <= 0 || width <= 0) return;
    quad(to_pixels(rect), 0, 0, 1, 1, color, radius * mapping_.scale, std::max(1.0f, width * mapping_.scale), kShape, 0);
}

void Gles2Renderer::draw_image(TextureId texture, const Rect &dst, Color tint, const Rect &uv) {
    auto it = textures_.find(texture);
    if (it == textures_.end() || dst.empty()) return;
    quad(to_pixels(dst), uv.x, uv.y, uv.right(), uv.bottom(), tint, 0, 0, kImage, it->second.gl);
}

void Gles2Renderer::reset_atlas() {
    glyphs_.clear();
    packer_.reset();
    ++atlas_resets_;
}

const Gles2Renderer::GlyphEntry *Gles2Renderer::glyph(FontId font, int glyph_index, int pixel_size) {
    const GlyphKey key{font, glyph_index, pixel_size};
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) return &it->second;
    auto f = fonts_.find(font);
    if (f == fonts_.end()) return nullptr;
    GlyphBitmap bmp = f->second->rasterize(glyph_index, static_cast<float>(pixel_size));
    GlyphEntry e{0, 0, 0, 0, bmp.width, bmp.height, bmp.x_offset, bmp.y_offset};
    if (bmp.width > 0) {
        ShelfPacker::Slot slot = packer_.insert(bmp.width, bmp.height);
        if (!slot.ok) {
            // Full: draw what is queued (it references the current atlas), then start over.
            flush();
            reset_atlas();
            slot = packer_.insert(bmp.width, bmp.height);
            if (!slot.ok) return nullptr;  // glyph larger than the atlas
        }
        ++uploads_;
        glBindTexture(GL_TEXTURE_2D, atlas_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, slot.x, slot.y, bmp.width, bmp.height, GL_ALPHA, GL_UNSIGNED_BYTE,
                        bmp.alpha.data());
        const float inv = 1.0f / kAtlasSize;
        e.u0 = slot.x * inv;
        e.v0 = slot.y * inv;
        e.u1 = (slot.x + bmp.width) * inv;
        e.v1 = (slot.y + bmp.height) * inv;
    }
    return &glyphs_.emplace(key, e).first->second;
}

Gles2Renderer::Resolved Gles2Renderer::resolve(FontId font, const FontFace &face, char32_t cp) const {
    const int g = face.glyph_index(cp);
    if (g != 0 || cp < 0x20 || cp == 0xFFFD) return {font, &face, g};
    // Joiners and selectors are invisible: not worth a fallback search.
    const bool invisible = (cp >= 0x200B && cp <= 0x200F) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x2060 || cp == 0xFEFF;
    if (!invisible) {
        for (FontId fb : fallbacks_) {
            if (fb == font) continue;
            auto it = fonts_.find(fb);
            if (it == fonts_.end()) continue;
            if (const int h = it->second->glyph_index(cp); h != 0) return {fb, it->second.get(), h};
        }
        if (reported_.insert(cp).second) missing_.push_back(cp);
    }
    return {font, &face, 0};
}

std::vector<char32_t> Gles2Renderer::take_missing_codepoints() { return std::exchange(missing_, {}); }

float Gles2Renderer::draw_text(const TextStyle &style, Point baseline, std::string_view utf8) {
    auto f = fonts_.find(style.font);
    if (f == fonts_.end() || style.size <= 0) return 0;
    const FontFace &face = *f->second;
    const int px_size = std::max(1, static_cast<int>(std::lround(style.size * mapping_.scale)));
    const auto px = static_cast<float>(px_size);
    const Point origin = mapping_.to_pixels({baseline.x + translate_.x, baseline.y + translate_.y});
    const float base_y = std::round(origin.y);
    float pen = origin.x;
    Resolved prev{kNoFont, nullptr, 0};
    for (char32_t cp : decode_utf8(utf8)) {
        const Resolved r = resolve(style.font, face, cp);
        if (prev.glyph != 0 && prev.font == r.font) pen += r.face->kerning(prev.glyph, r.glyph, px);
        const GlyphEntry *e = glyph(r.font, r.glyph, px_size);
        if (e != nullptr && e->width > 0) {
            const Rect dst{std::round(pen) + e->x_offset, base_y + e->y_offset, static_cast<float>(e->width),
                           static_cast<float>(e->height)};
            quad(dst, e->u0, e->v0, e->u1, e->v1, style.color, 0, 0, kGlyph, atlas_);
        }
        pen += r.face->advance(r.glyph, px);
        prev = r;
    }
    return (pen - origin.x) / mapping_.scale;
}

float Gles2Renderer::measure_text(const TextStyle &style, std::string_view utf8) const {
    auto f = fonts_.find(style.font);
    if (f == fonts_.end() || style.size <= 0) return 0;
    const auto px = static_cast<float>(std::max(1, static_cast<int>(std::lround(style.size * mapping_.scale))));
    const FontFace &face = *f->second;
    float width = 0;
    Resolved prev{kNoFont, nullptr, 0};
    for (char32_t cp : decode_utf8(utf8)) {
        const Resolved r = resolve(style.font, face, cp);
        if (prev.glyph != 0 && prev.font == r.font) width += r.face->kerning(prev.glyph, r.glyph, px);
        width += r.face->advance(r.glyph, px);
        prev = r;
    }
    return width / mapping_.scale;
}

float Gles2Renderer::line_height(const TextStyle &style) const {
    auto f = fonts_.find(style.font);
    return f == fonts_.end() ? 0 : f->second->metrics(style.size).line_height();
}

float Gles2Renderer::ascent(const TextStyle &style) const {
    auto f = fonts_.find(style.font);
    return f == fonts_.end() ? 0 : f->second->metrics(style.size).ascent;
}

void Gles2Renderer::end_frame() {
    flush();
    last_stats_ = frame_stats_;
    mix(uploads_);
    last_fingerprint_ = fingerprint_;
}

TextureId Gles2Renderer::create_texture(int width, int height, const uint8_t *rgba) {
    if (width <= 0 || height <= 0 || rgba == nullptr) return kNoTexture;
    Texture t;
    t.width = width;
    t.height = height;
    glGenTextures(1, &t.gl);
    glBindTexture(GL_TEXTURE_2D, t.gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &t.gl);
        return kNoTexture;
    }
    const TextureId id = next_texture_++;
    textures_[id] = t;
    ++uploads_;  // a recycled GL name may now hold other pixels
    return id;
}

bool Gles2Renderer::destroy_texture(TextureId id) {
    auto it = textures_.find(id);
    if (it == textures_.end()) return false;
    flush();  // queued quads may still reference it
    glDeleteTextures(1, &it->second.gl);
    textures_.erase(it);
    ++uploads_;
    return true;
}

FontId Gles2Renderer::load_font(std::vector<uint8_t> ttf, std::string *error) {
    auto face = FontFace::from_bytes(std::move(ttf), error);
    if (face == nullptr) return kNoFont;
    const FontId id = next_font_++;
    fonts_[id] = std::move(face);
    return id;
}

FontId Gles2Renderer::load_font_file(const std::string &path, std::string *error) {
    auto face = FontFace::from_file(path, error);
    if (face == nullptr) return kNoFont;
    const FontId id = next_font_++;
    fonts_[id] = std::move(face);
    return id;
}

RenderStats Gles2Renderer::stats() const {
    RenderStats s = last_stats_;
    s.textures = static_cast<uint32_t>(textures_.size());
    s.glyphs_cached = static_cast<uint32_t>(glyphs_.size());
    s.atlas_resets = atlas_resets_;
    return s;
}

GpuInfo Gles2Renderer::gpu_info() const {
    GpuInfo info;
    info.vendor = gl_string(GL_VENDOR);
    info.renderer = gl_string(GL_RENDERER);
    info.version = gl_string(GL_VERSION);
    info.shading_language = gl_string(GL_SHADING_LANGUAGE_VERSION);
    info.extensions = gl_string(GL_EXTENSIONS);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &info.max_texture_size);
    info.fragment_highp = highp_;
    return info;
}

std::vector<uint8_t> Gles2Renderer::read_pixels(int x, int y, int w, int h) {
    flush();
    std::vector<uint8_t> rows(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    glReadPixels(x, mapping_.drawable_h - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    // GL returns the bottom row first; flip to top-first.
    std::vector<uint8_t> out(rows.size());
    const size_t stride = static_cast<size_t>(w) * 4;
    for (int r = 0; r < h; ++r) {
        std::copy_n(&rows[static_cast<size_t>(h - 1 - r) * stride], stride, &out[static_cast<size_t>(r) * stride]);
    }
    return out;
}

}  // namespace wnk::render
