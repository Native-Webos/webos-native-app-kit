// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/ass_renderer.hpp"

#include <memory>

using namespace wnk::media;

namespace {

const char *kHeader =
    "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\nWrapStyle: 0\n\n"
    "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, "
    "Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
    "MarginR, MarginV, Encoding\n"
    "Style: Default,Atkinson Hyperlegible,32,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,"
    "10,10,20,1\n";

std::shared_ptr<SubtitleCue> cue(int64_t start_ms, int64_t end_ms, int read_order, const std::string &text) {
    auto c = std::make_shared<SubtitleCue>();
    c->start_us = start_ms * 1000;
    c->end_us = end_ms * 1000;
    c->ass_events.push_back(std::to_string(read_order) + ",0,Default,,0,0,0,," + text);
    return c;
}

std::unique_ptr<AssRenderer> make() {
    std::string err;
    auto r = AssRenderer::create(std::string(WNK_TEST_FONT_DIR) + "/AtkinsonHyperlegible-Regular.ttf", &err);
    if (r == nullptr) std::printf("  no renderer: %s\n", err.c_str());
    return r;
}

// The colour of the most opaque pixel of the frame.
struct Pixel {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};
Pixel strongest(const std::vector<AssRenderer::Region> &regions) {
    Pixel best;
    for (const auto &g : regions) {
        for (size_t i = 0; i < g.rgba.size(); i += 4) {
            if (g.rgba[i + 3] > best.a) best = {g.rgba[i], g.rgba[i + 1], g.rgba[i + 2], g.rgba[i + 3]};
        }
    }
    return best;
}

#define SKIP_WITHOUT_LIBASS()                                   \
    if (!AssRenderer::available()) {                            \
        std::printf("  SKIP: built without libass\n");          \
        return;                                                 \
    }

}  // namespace

TEST("ass: an event shows only in its time, as white glyphs with a dark outline near the bottom") {
    SKIP_WITHOUT_LIBASS();
    auto r = make();
    REQUIRE(r != nullptr);
    REQUIRE(r->set_track(kHeader));
    r->add_events(*cue(1000, 2000, 0, "Hello there"));
    std::vector<AssRenderer::Region> regions;
    bool changed = false;
    REQUIRE(r->render(500000, 1280, 720, 1280, 720, &regions, &changed));
    CHECK(regions.empty());
    REQUIRE(r->render(1500000, 1280, 720, 1280, 720, &regions, &changed));
    CHECK(changed);
    REQUIRE(!regions.empty());
    int top = 720;
    for (const auto &g : regions) top = std::min(top, g.y);
    CHECK(top > 720 / 2);  // bottom-centre style (alignment 2)
    const Pixel p = strongest(regions);
    CHECK_EQ(p.a, 255);
    bool dark = false;  // the outline: opaque and black somewhere
    for (const auto &g : regions) {
        for (size_t i = 0; i < g.rgba.size() && !dark; i += 4) dark = g.rgba[i + 3] == 255 && g.rgba[i] < 40;
    }
    CHECK(dark);
    // The same frame again is reported unchanged; after the end it is empty again.
    REQUIRE(r->render(1500000, 1280, 720, 1280, 720, &regions, &changed));
    CHECK(!changed);
    REQUIRE(r->render(2500000, 1280, 720, 1280, 720, &regions, &changed));
    CHECK(changed);
    CHECK(regions.empty());
}

TEST("ass: override tags colour and place the text; a sign and a line stay separate regions") {
    SKIP_WITHOUT_LIBASS();
    auto r = make();
    REQUIRE(r != nullptr);
    REQUIRE(r->set_track(kHeader));
    r->add_events(*cue(0, 5000, 0, "{\\c&H0000FF&\\bord0}Red line"));  // ASS colours are BGR
    r->add_events(*cue(0, 5000, 1, "{\\pos(320,30)\\c&HFF0000&\\bord0}Sign"));
    std::vector<AssRenderer::Region> regions;
    bool changed = false;
    REQUIRE(r->render(1000000, 640, 360, 640, 360, &regions, &changed));
    REQUIRE(regions.size() == 2u);
    const auto &sign = regions[0].y < regions[1].y ? regions[0] : regions[1];
    const auto &line = regions[0].y < regions[1].y ? regions[1] : regions[0];
    CHECK(sign.y < 60);
    CHECK(line.y > 250);
    const Pixel s = strongest({sign}), l = strongest({line});
    CHECK(s.b > 200 && s.r < 60);  // blue sign
    CHECK(l.r > 200 && l.b < 60);  // red line
}

TEST("ass: an event offered again (same ReadOrder, after a seek) is kept once; prune drops ended ones") {
    SKIP_WITHOUT_LIBASS();
    auto r = make();
    REQUIRE(r != nullptr);
    REQUIRE(r->set_track(kHeader));
    r->add_events(*cue(1000, 2000, 0, "Once"));
    r->add_events(*cue(1000, 2000, 0, "Once"));
    r->add_events(*cue(3000, 4000, 1, "Later"));
    CHECK_EQ(r->stats().events, size_t{2});
    r->prune(2500000);
    CHECK_EQ(r->stats().events, size_t{1});
    // A new track starts empty.
    REQUIRE(r->set_track(kHeader));
    CHECK_EQ(r->stats().events, size_t{0});
}

TEST("ass: a font the media carries is used by its family name") {
    SKIP_WITHOUT_LIBASS();
    // The bundled bold face added under its own name, asked for by family with \b1 off: the
    // rendering must differ from the default font's (bold glyphs cover more pixels).
    auto coverage = [](bool with_font) {
        std::string err;
        auto r = AssRenderer::create("", &err);
        if (with_font) {
            FILE *f = std::fopen(WNK_TEST_FONT_DIR "/AtkinsonHyperlegible-Bold.ttf", "rb");
            std::vector<uint8_t> data;
            if (f != nullptr) {
                uint8_t buf[65536];
                for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
                std::fclose(f);
            }
            r->add_font("AtkinsonHyperlegible-Bold.ttf", data);
        }
        r->set_track(kHeader);
        r->add_events(*cue(0, 1000, 0, "{\\fnAtkinson Hyperlegible\\bord0}Wide words here"));
        std::vector<AssRenderer::Region> regions;
        bool changed = false;
        r->render(500000, 640, 360, 640, 360, &regions, &changed);
        size_t n = 0;
        for (const auto &g : regions) {
            for (size_t i = 3; i < g.rgba.size(); i += 4) n += g.rgba[i] > 128;
        }
        return n;
    };
    const size_t none = coverage(false), bold = coverage(true);
    CHECK_EQ(none, size_t{0});  // no default font and no matching font: nothing to draw with
    CHECK(bold > 0);
}

TEST("ass: still text at a later time is reported unchanged (no re-upload while it shows)") {
    SKIP_WITHOUT_LIBASS();
    auto r = make();
    REQUIRE(r != nullptr);
    REQUIRE(r->set_track(kHeader));
    r->add_events(*cue(0, 5000, 0, "Still"));
    r->add_events(*cue(0, 5000, 1, "{\\pos(320,30)\\frz-5}Sign"));
    std::vector<AssRenderer::Region> regions;
    bool changed = false;
    REQUIRE(r->render(1000000, 640, 360, 640, 360, &regions, &changed));
    CHECK(changed);
    int later_changes = 0;
    for (int64_t t = 1016000; t < 2000000; t += 16000) {
        REQUIRE(r->render(t, 640, 360, 640, 360, &regions, &changed));
        later_changes += changed;
    }
    CHECK_EQ(later_changes, 0);
    // After the events: one change to empty, then empty frames are unchanged too.
    later_changes = 0;
    for (int64_t t = 5100000; t < 6000000; t += 16000) {
        REQUIRE(r->render(t, 640, 360, 640, 360, &regions, &changed));
        later_changes += changed;
    }
    CHECK_EQ(later_changes, 1);
    CHECK(regions.empty());
}
