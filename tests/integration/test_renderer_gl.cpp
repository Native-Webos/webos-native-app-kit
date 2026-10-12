// SPDX-License-Identifier: GPL-3.0-or-later
//
// Renderer pixel tests on a real GLES2 context (host: Mesa via SDL's offscreen video driver).
// They check geometry, blending, clipping, text and textures by reading back pixels. They prove the
// GLES2 code is correct on Mesa; they do NOT prove identical output on the TV's GPU.
// If no GLES2 context can be created (headless CI without EGL), the suite reports SKIP and exits 0.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/render/gles2_renderer.hpp"

#include <GLES2/gl2.h>
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace wnk;
using namespace wnk::render;

namespace {

constexpr int kW = 640, kH = 360;
const std::string kFontDir = WNK_TEST_FONT_DIR;

struct GlEnv {
    SDL_Window *window = nullptr;
    SDL_GLContext ctx = nullptr;
    std::unique_ptr<Gles2Renderer> r;
    bool ok = false;
    std::string why;

    GlEnv() {
        setenv("SDL_VIDEODRIVER", "offscreen", 0);
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            why = SDL_GetError();
            return;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        window = SDL_CreateWindow("t", 0, 0, kW, kH, SDL_WINDOW_OPENGL);
        if (window == nullptr) {
            why = SDL_GetError();
            return;
        }
        ctx = SDL_GL_CreateContext(window);
        if (ctx == nullptr) {
            why = SDL_GetError();
            return;
        }
        std::string err;
        r = Gles2Renderer::create(&err);
        if (!r) {
            why = err;
            return;
        }
        r->set_logical_size({static_cast<float>(kW), static_cast<float>(kH)});  // 1 logical unit = 1 pixel
        ok = true;
    }
    ~GlEnv() {
        r.reset();
        if (ctx != nullptr) SDL_GL_DeleteContext(ctx);
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

GlEnv &env() {
    static GlEnv e;
    return e;
}

struct Px {
    int r, g, b, a;
};

Px pixel(int x, int y) {
    auto v = env().r->read_pixels(x, y, 1, 1);
    return {v[0], v[1], v[2], v[3]};
}

bool near(Px p, int r, int g, int b, int a, int tol = 2) {
    return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol && std::abs(p.a - a) <= tol;
}

#define SKIP_WITHOUT_GL()                                                   \
    do {                                                                    \
        if (!env().ok) {                                                    \
            std::printf("  SKIP (no GLES2 context: %s)\n", env().why.c_str()); \
            return;                                                         \
        }                                                                   \
    } while (0)

void frame() { env().r->begin_frame(kW, kH); }

}  // namespace

TEST("clear fills the drawable, transparent clear has alpha 0") {
    SKIP_WITHOUT_GL();
    frame();
    env().r->clear(Color::hex(0x204060FF));
    CHECK(near(pixel(5, 5), 0x20, 0x40, 0x60, 255));
    env().r->clear(kTransparent);
    CHECK(near(pixel(5, 5), 0, 0, 0, 0));
    env().r->end_frame();
}

TEST("solid rect covers exactly its pixels") {
    SKIP_WITHOUT_GL();
    frame();
    env().r->clear(kBlack);
    env().r->fill_rect({100, 50, 40, 30}, Color::hex(0xFF0000FF));
    env().r->end_frame();
    CHECK(near(pixel(100, 50), 255, 0, 0, 255));
    CHECK(near(pixel(139, 79), 255, 0, 0, 255));
    CHECK(near(pixel(99, 50), 0, 0, 0, 255));
    CHECK(near(pixel(140, 50), 0, 0, 0, 255));
    CHECK(near(pixel(100, 80), 0, 0, 0, 255));
}

TEST("rounded rect leaves corners empty and fills the middle") {
    SKIP_WITHOUT_GL();
    frame();
    env().r->clear(kBlack);
    env().r->fill_rect({200, 100, 100, 100}, kWhite, 30);
    env().r->end_frame();
    CHECK(near(pixel(201, 101), 0, 0, 0, 255));
    CHECK(near(pixel(250, 150), 255, 255, 255, 255));
    CHECK(near(pixel(250, 101), 255, 255, 255, 255));  // straight top edge is filled
}

TEST("stroke draws a border and keeps the inside empty") {
    SKIP_WITHOUT_GL();
    frame();
    env().r->clear(kBlack);
    env().r->stroke_rect({50, 50, 100, 60}, Color::hex(0x00FF00FF), 4);
    env().r->end_frame();
    CHECK(near(pixel(51, 80), 0, 255, 0, 255));
    CHECK(near(pixel(100, 80), 0, 0, 0, 255));
    CHECK(near(pixel(100, 52), 0, 255, 0, 255));
}

TEST("blending over a transparent clear accumulates alpha") {
    SKIP_WITHOUT_GL();
    frame();
    env().r->clear(kTransparent);
    env().r->fill_rect({10, 10, 20, 20}, Color{1, 1, 1, 0.5f});
    env().r->end_frame();
    Px p = pixel(15, 15);
    CHECK(std::abs(p.a - 128) <= 2);
    CHECK(near(pixel(40, 40), 0, 0, 0, 0));
}

TEST("clip restricts drawing and nests by intersection") {
    SKIP_WITHOUT_GL();
    frame();
    env().r->clear(kBlack);
    env().r->push_clip({0, 0, 120, 120});
    env().r->push_clip({100, 100, 100, 100});  // effective clip 100..120
    env().r->fill_rect({0, 0, 300, 300}, kWhite);
    env().r->pop_clip();
    env().r->pop_clip();
    env().r->fill_rect({300, 300, 10, 10}, kWhite);  // after pop: unclipped
    env().r->end_frame();
    CHECK(near(pixel(110, 110), 255, 255, 255, 255));
    CHECK(near(pixel(125, 110), 0, 0, 0, 255));
    CHECK(near(pixel(90, 90), 0, 0, 0, 255));
    CHECK(near(pixel(305, 305), 255, 255, 255, 255));
}

TEST("translate offsets subsequent drawing") {
    SKIP_WITHOUT_GL();
    frame();
    env().r->clear(kBlack);
    env().r->push_translate(200, 100);
    env().r->fill_rect({0, 0, 10, 10}, kWhite);
    env().r->pop_translate();
    env().r->end_frame();
    CHECK(near(pixel(205, 105), 255, 255, 255, 255));
    CHECK(near(pixel(5, 5), 0, 0, 0, 255));
}

TEST("image texture is sampled and tinted") {
    SKIP_WITHOUT_GL();
    const uint8_t rgba[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    TextureId t = env().r->create_texture(2, 2, rgba);
    REQUIRE(t != kNoTexture);
    frame();
    env().r->clear(kBlack);
    env().r->draw_image(t, {0, 0, 100, 100});
    env().r->draw_image(t, {200, 0, 100, 100}, Color{1, 1, 1, 1}, {0.75f, 0.75f, 0.0f, 0.0f});
    env().r->end_frame();
    CHECK(near(pixel(10, 10), 255, 0, 0, 255, 4));    // top-left texel: red
    CHECK(near(pixel(90, 90), 255, 255, 255, 255, 4)); // bottom-right texel: white
    CHECK(env().r->destroy_texture(t));
    CHECK(!env().r->destroy_texture(t));
    CHECK_EQ(env().r->stats().textures, 0u);
}

TEST("text renders glyph coverage and advance matches measurement") {
    SKIP_WITHOUT_GL();
    std::string err;
    FontId f = env().r->load_font_file(kFontDir + "/AtkinsonHyperlegible-Bold.ttf", &err);
    REQUIRE(f != kNoFont);
    TextStyle style{f, 48, kWhite};
    frame();
    env().r->clear(kBlack);
    const float adv = env().r->draw_text(style, {20, 100}, "HHHH");
    env().r->end_frame();
    CHECK(std::abs(adv - env().r->measure_text(style, "HHHH")) < 0.5f);
    CHECK(adv > 80);
    auto px = env().r->read_pixels(20, 50, static_cast<int>(adv), 50);
    int lit = 0;
    for (size_t i = 0; i < px.size(); i += 4) lit += px[i] > 200;
    CHECK(lit > 200);
    auto below = env().r->read_pixels(20, 110, static_cast<int>(adv), 20);  // 'H' has no descender
    int lit_below = 0;
    for (size_t i = 0; i < below.size(); i += 4) lit_below += below[i] > 200;
    CHECK_EQ(lit_below, 0);
    CHECK(env().r->stats().glyphs_cached >= 1u);
}

TEST("many rects in one batch use a single draw call") {
    SKIP_WITHOUT_GL();
    frame();
    env().r->clear(kBlack);
    for (int i = 0; i < 500; ++i) env().r->fill_rect({static_cast<float>(i % 600), 10, 1, 1}, kWhite);
    env().r->end_frame();
    CHECK_EQ(env().r->stats().draw_calls, 1u);
    CHECK_EQ(env().r->stats().quads, 500u);
}

TEST("glyph atlas resets when full and text still renders") {
    SKIP_WITHOUT_GL();
    FontId f = env().r->load_font_file(kFontDir + "/AtkinsonHyperlegible-Regular.ttf");
    REQUIRE(f != kNoFont);
    const uint32_t before = env().r->stats().atlas_resets;
    frame();
    env().r->clear(kBlack);
    for (int size = 60; size < 200; size += 2) {
        env().r->draw_text({f, static_cast<float>(size), kWhite}, {0, 300}, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    }
    env().r->clear(kBlack);
    env().r->draw_text({f, 40, kWhite}, {10, 60}, "OK");
    env().r->end_frame();
    CHECK(env().r->stats().atlas_resets > before);
    auto px = env().r->read_pixels(10, 25, 60, 40);
    int lit = 0;
    for (size_t i = 0; i < px.size(); i += 4) lit += px[i] > 200;
    CHECK(lit > 50);
}

TEST("frame fingerprint: equal for identical frames, different for any visible change") {
    SKIP_WITHOUT_GL();
    auto &r = *env().r;
    FontId font = r.load_font_file(kFontDir + "/AtkinsonHyperlegible-Regular.ttf");
    auto draw = [&](Color fill, float x, const char *text, Rect clip, TextureId tex) {
        frame();
        r.clear(Color::hex(0x101010FF));
        r.push_clip(clip);
        r.fill_rect({x, 4, 20, 10}, fill, 3);
        r.pop_clip();
        r.draw_text({font, 14, kWhite}, {4, 40}, text);
        if (tex != kNoTexture) r.draw_image(tex, {30, 30, 8, 8});
        r.end_frame();
        return r.frame_fingerprint();
    };
    const Rect clip{0, 0, 60, 30};
    draw(kWhite, 2, "Hello", clip, kNoTexture);  // rasterises the glyphs once
    const uint64_t a = draw(kWhite, 2, "Hello", clip, kNoTexture);
    CHECK_EQ(draw(kWhite, 2, "Hello", clip, kNoTexture), a);
    CHECK(draw(Color::hex(0xFF0000FF), 2, "Hello", clip, kNoTexture) != a);  // colour
    CHECK(draw(kWhite, 3, "Hello", clip, kNoTexture) != a);                   // one pixel to the right
    CHECK(draw(kWhite, 2, "Hellp", clip, kNoTexture) != a);                   // text
    draw(kWhite, 2, "Hello", clip, kNoTexture);
    CHECK(draw(kWhite, 2, "Hello", {0, 0, 61, 30}, kNoTexture) != a);         // clip
    // A texture deleted and another created may reuse the GL name with other pixels: still a change.
    const uint8_t red[4] = {255, 0, 0, 255}, blue[4] = {0, 0, 255, 255};
    TextureId t1 = r.create_texture(1, 1, red);
    const uint64_t with_red = draw(kWhite, 2, "Hello", clip, t1);
    CHECK_EQ(draw(kWhite, 2, "Hello", clip, t1), with_red);
    r.destroy_texture(t1);
    TextureId t2 = r.create_texture(1, 1, blue);
    CHECK(draw(kWhite, 2, "Hello", clip, t2) != with_red);
    r.destroy_texture(t2);
}

TEST("fallback fonts: a character the font lacks comes from the chain, else it is reported once") {
    SKIP_WITHOUT_GL();
    std::string cjk;
    for (const char *p : {"/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
                          "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"}) {
        if (wnk::render::FontFace::from_file(p)) {
            cjk = p;
            break;
        }
    }
    if (cjk.empty()) {
        std::printf("  SKIP (no CJK font on this host)\n");
        return;
    }
    auto &r = *env().r;
    const FontId latin = r.load_font_file(kFontDir + "/AtkinsonHyperlegible-Regular.ttf");
    const TextStyle s{latin, 20, kWhite};
    r.set_fallback_fonts({});
    r.take_missing_codepoints();
    const float without = r.measure_text(s, "a\xE4\xB8\xAD");  // a中
    auto missing = r.take_missing_codepoints();
    REQUIRE(missing.size() == 1u);
    CHECK_EQ(static_cast<unsigned>(missing[0]), 0x4E2Du);
    r.measure_text(s, "\xE4\xB8\xAD");
    CHECK(r.take_missing_codepoints().empty());  // reported once only
    const FontId han = r.load_font_file(cjk);
    REQUIRE(han != kNoFont);
    r.set_fallback_fonts({han});
    const float with = r.measure_text(s, "a\xE4\xB8\xAD");
    CHECK(with > without);  // a full-width ideograph, wider than .notdef
    frame();
    r.clear(kTransparent);
    r.draw_text(s, {4, 40}, "a\xE4\xB8\xAD");
    r.end_frame();
    CHECK(r.take_missing_codepoints().empty());
    // The ideograph's pixels are drawn to the right of the "a".
    int lit = 0;
    for (int x = 4 + static_cast<int>(with * 0.5f); x < 4 + static_cast<int>(with); ++x) {
        for (int y = 22; y < 42; ++y) lit += pixel(x, y).a > 128 ? 1 : 0;
    }
    CHECK(lit > 10);
    r.set_fallback_fonts({});
}

TEST("custom GL draw: only inside its rectangle, renderer state restored, version drives the fingerprint") {
    SKIP_WITHOUT_GL();
    auto &r = *env().r;
    auto frame_with = [&](uint64_t version, bool fill_after) {
        frame();
        r.clear(Color::hex(0x000000FF));
        r.draw_custom({10, 10, 20, 20}, version, [](const CustomTarget &t) {
            // What an app might do: its own clear colour, blending off, a texture unit changed.
            glDisable(GL_BLEND);
            glActiveTexture(GL_TEXTURE1);
            glClearColor(1, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);  // the scissor keeps it inside the view
            (void)t;
        });
        if (fill_after) r.fill_rect({40, 10, 10, 10}, Color::hex(0x00FF0080));  // half-transparent green over black
        r.end_frame();
        return r.frame_fingerprint();
    };
    const uint64_t a = frame_with(1, true);
    CHECK(near(pixel(15, 15), 255, 0, 0, 255));  // inside the custom rectangle
    CHECK(near(pixel(5, 5), 0, 0, 0, 255));      // outside: untouched
    CHECK(near(pixel(32, 15), 0, 0, 0, 255));
    CHECK(near(pixel(45, 15), 0, 128, 0, 255, 3));  // blending and texture unit restored for the UI after it
    CHECK_EQ(frame_with(1, true), a);
    CHECK(frame_with(2, true) != a);
    // The rectangle in GL's bottom-left coordinates.
    frame();
    CustomTarget seen;
    r.draw_custom({10, 5, 20, 8}, 1, [&seen](const CustomTarget &t) { seen = t; });
    r.end_frame();
    CHECK_EQ(seen.x, 10);
    CHECK_EQ(seen.width, 20);
    CHECK_EQ(seen.height, 8);
    CHECK_EQ(seen.y, seen.drawable_height - 13);
}
