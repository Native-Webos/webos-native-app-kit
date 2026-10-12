// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/render/font.hpp"
#include "wnk/render/renderer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <unistd.h>

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

namespace {
// A CJK face the host may have (Debian/Ubuntu packages); tests that need one skip without it.
std::string host_cjk_font() {
    for (const char *p : {"/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
                          "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"}) {
        if (FontFace::from_file(p)) return p;
    }
    return {};
}

struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/wnk-fonts-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() { std::system(("rm -rf '" + path + "'").c_str()); }
};
}  // namespace

TEST("font files are memory-mapped and directories are rejected") {
    std::string err;
    auto face = FontFace::from_file(kFontDir + "/AtkinsonHyperlegible-Regular.ttf", &err);
    REQUIRE(face != nullptr);
    CHECK(face->covers(U'A'));
    CHECK(!face->covers(U'中'));
    CHECK(FontFace::from_file(kFontDir, &err) == nullptr);
    CHECK(err.find("regular file") != std::string::npos);
}

TEST("font finder: picks a face covering the missing characters, regular and tagged faces first") {
    const std::string cjk = host_cjk_font();
    if (cjk.empty()) {
        std::printf("  SKIP (no CJK font on this host)\n");
        return;
    }
    TempDir dir;
    REQUIRE(!dir.path.empty());
    std::system(("mkdir -p '" + dir.path + "/a/b'").c_str());
    // The same CJK face under three names: the finder ranks by name, then checks coverage.
    for (const char *name : {"/a/Zed-Bold.ttf", "/a/b/Zed-Regular.ttf", "/a/b/Zed-JP-Regular.ttf"}) {
        CHECK(symlink(cjk.c_str(), (dir.path + name).c_str()) == 0);
    }
    CHECK(symlink((kFontDir + "/AtkinsonHyperlegible-Regular.ttf").c_str(), (dir.path + "/Atkinson.ttf").c_str()) == 0);
    CHECK(symlink("/nonexistent", (dir.path + "/broken.ttf").c_str()) == 0);
    std::FILE *junk = std::fopen((dir.path + "/junk.otf").c_str(), "w");
    std::fputs("not a font", junk);
    std::fclose(junk);
    FontFinder finder({dir.path, "/nonexistent-dir"});
    CHECK_EQ(finder.files().size(), size_t{5});  // the broken link is skipped; junk is listed, then refused
    auto found = finder.find({U'中', U'あ'});
    REQUIRE(found.size() == 1u);
    CHECK(found[0] != dir.path + "/a/Zed-Bold.ttf");  // one face covers both; a regular one before bold
    found = finder.find({U'中'}, {"Zed-Regular"});
    REQUIRE(found.size() == 1u);
    CHECK_EQ(found[0], dir.path + "/a/b/Zed-Regular.ttf");
    found = finder.find({U'中'}, {"JP"});
    REQUIRE(found.size() == 1u);
    CHECK_EQ(found[0], dir.path + "/a/b/Zed-JP-Regular.ttf");
    CHECK(finder.find({}).empty());
}
