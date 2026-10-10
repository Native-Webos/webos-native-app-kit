// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/render/font.hpp"
#include "wnk/render/renderer.hpp"

#include <string>

using namespace wnk;
using namespace wnk::render;

namespace {
const std::string kFontDir = WNK_TEST_FONT_DIR;
}

TEST("utf8 decodes ASCII, multibyte and emoji") {
    auto cps = decode_utf8("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");  // A é € 😀
    REQUIRE(cps.size() == 4u);
    CHECK(cps[0] == U'A');
    CHECK(cps[1] == U'é');
    CHECK(cps[2] == U'€');
    CHECK(cps[3] == U'\U0001F600');
}

TEST("utf8 replaces invalid, truncated, overlong and surrogate sequences") {
    auto a = decode_utf8("\xFF" "a");
    REQUIRE(a.size() == 2u);
    CHECK(a[0] == 0xFFFD);
    CHECK(a[1] == U'a');
    auto b = decode_utf8("\xE2\x82");  // truncated €
    REQUIRE(b.size() == 1u);
    CHECK(b[0] == 0xFFFD);
    auto c = decode_utf8("\xC0\xAF");  // overlong '/'
    REQUIRE(c.size() == 1u);
    CHECK(c[0] == 0xFFFD);
    auto d = decode_utf8("\xED\xA0\x80");  // UTF-16 surrogate
    REQUIRE(d.size() == 1u);
    CHECK(d[0] == 0xFFFD);
    auto e = decode_utf8("\xC3" "b");  // lead byte followed by ASCII: resync
    REQUIRE(e.size() == 2u);
    CHECK(e[1] == U'b');
}

TEST("bundled font loads and has sane metrics") {
    std::string err;
    auto face = FontFace::from_file(kFontDir + "/AtkinsonHyperlegible-Regular.ttf", &err);
    REQUIRE(face != nullptr);
    FontMetrics m = face->metrics(32);
    CHECK(m.ascent > 20 && m.ascent < 40);
    CHECK(m.descent > 2 && m.descent < 16);
    CHECK(face->glyph_index(U'A') != 0);
    CHECK(face->advance(face->glyph_index(U'W'), 32) > face->advance(face->glyph_index(U'i'), 32));
}

TEST("measure is additive with kerning and scales with size") {
    auto face = FontFace::from_file(kFontDir + "/AtkinsonHyperlegible-Regular.ttf");
    REQUIRE(face != nullptr);
    const float w32 = face->measure("Hello", 32);
    const float w64 = face->measure("Hello", 64);
    CHECK(w32 > 50);
    CHECK(std::abs(w64 - 2 * w32) < 1.0f);
    CHECK_EQ(face->measure("", 32), 0.0f);
}

TEST("rasterize produces coverage for visible glyphs and nothing for space") {
    auto face = FontFace::from_file(kFontDir + "/AtkinsonHyperlegible-Bold.ttf");
    REQUIRE(face != nullptr);
    GlyphBitmap h = face->rasterize(face->glyph_index(U'H'), 40);
    CHECK(h.width > 10 && h.height > 20);
    CHECK(h.y_offset < 0);  // above the baseline
    int lit = 0;
    for (uint8_t v : h.alpha) lit += v > 128;
    CHECK(lit > 50);
    GlyphBitmap sp = face->rasterize(face->glyph_index(U' '), 40);
    CHECK_EQ(sp.width, 0);
}

TEST("non-font data is rejected") {
    std::string err;
    CHECK(FontFace::from_bytes({1, 2, 3, 4}, &err) == nullptr);
    CHECK(!err.empty());
    CHECK(FontFace::from_file("/nonexistent/font.ttf", &err) == nullptr);
}

TEST("shelf packer fills rows and reports when full") {
    ShelfPacker p(100, 50, 0);
    auto a = p.insert(40, 20);
    auto b = p.insert(40, 20);
    auto c = p.insert(40, 20);  // does not fit in the first row: new shelf
    CHECK(a.ok && b.ok && c.ok);
    CHECK_EQ(a.y, 0);
    CHECK_EQ(b.x, 40);
    CHECK_EQ(c.y, 20);
    CHECK(!p.insert(200, 10).ok);  // wider than the atlas
    auto d = p.insert(40, 20);
    CHECK(d.ok);
    CHECK(!p.insert(40, 20).ok);  // third shelf would exceed the height
    p.reset();
    CHECK(p.insert(40, 20).ok);
}

TEST("canvas mapping letterboxes and round-trips") {
    CanvasMapping m = CanvasMapping::fit({1920, 1080}, 3840, 2160);
    CHECK_EQ(m.scale, 2.0f);
    CHECK_EQ(m.offset_x, 0.0f);
    CanvasMapping n = CanvasMapping::fit({1920, 1080}, 1920, 1200);  // 16:10: bars top and bottom
    CHECK_EQ(n.scale, 1.0f);
    CHECK_EQ(n.offset_y, 60.0f);
    Point p = n.to_logical(n.to_pixels({100, 200}));
    CHECK_EQ(p.x, 100.0f);
    CHECK_EQ(p.y, 200.0f);
    CanvasMapping z = CanvasMapping::fit({1920, 1080}, 0, 0);
    CHECK_EQ(z.scale, 1.0f);  // degenerate drawable keeps a usable default
}
