// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/ass_renderer.hpp"
#include "wnk/media/subtitle_view.hpp"
#include "wnk/render/font.hpp"
#include "wnk/ui/ui.hpp"

#include <cmath>

using namespace wnk;
using namespace wnk::media;

namespace {

// Records what is drawn; text is 0.5 em per code point.
class FakeRenderer : public render::Renderer {
 public:
    struct Text {
        std::string text;
        Point baseline;
        render::Color color;
    };
    std::vector<Rect> fills, images;
    std::vector<Text> texts;
    int created = 0, destroyed = 0;
    render::CanvasMapping map;

    void set_logical_size(Size) override {}
    void begin_frame(int, int) override {
        fills.clear();
        images.clear();
        texts.clear();
    }
    const render::CanvasMapping &mapping() const override { return map; }
    void clear(render::Color) override {}
    void push_clip(const Rect &) override {}
    void pop_clip() override {}
    void push_translate(float, float) override {}
    void pop_translate() override {}
    void fill_rect(const Rect &r, render::Color, float) override { fills.push_back(r); }
    void stroke_rect(const Rect &, render::Color, float, float) override {}
    void draw_image(render::TextureId, const Rect &dst, render::Color, const Rect &) override { images.push_back(dst); }
    float draw_text(const render::TextStyle &s, Point b, std::string_view t) override {
        texts.push_back({std::string(t), b, s.color});
        return measure_text(s, t);
    }
    float measure_text(const render::TextStyle &s, std::string_view t) const override {
        return static_cast<float>(render::decode_utf8(t).size()) * s.size * 0.5f;
    }
    float line_height(const render::TextStyle &s) const override { return s.size * 1.25f; }
    float ascent(const render::TextStyle &s) const override { return s.size; }
    void end_frame() override {}
    render::TextureId create_texture(int, int, const uint8_t *) override { return static_cast<render::TextureId>(++created); }
    bool destroy_texture(render::TextureId) override {
        ++destroyed;
        return true;
    }
    render::FontId load_font(std::vector<uint8_t>, std::string *) override { return 1; }
    render::FontId load_font_file(const std::string &, std::string *) override { return 1; }
    render::RenderStats stats() const override { return {}; }
    render::GpuInfo gpu_info() const override { return {}; }

    // Text drawn in white (the visible copy, not the outline).
    std::vector<Text> white() const {
        std::vector<Text> out;
        for (const auto &t : texts) {
            if (t.color.r == 1 && t.color.a == 1) out.push_back(t);
        }
        return out;
    }
};

struct Env {
    FakeRenderer r;
    ui::Ui ui{r, ui::Theme{}};
    SubtitleView *view = nullptr;
    SubtitleView::Cues cues;
    Env() {
        view = &ui.root().add<SubtitleView>();
        view->set_provider([this] { return cues; });
    }
    void frame() {
        ui.update(1.0f / 60);
        r.begin_frame(1920, 1080);
        ui.draw();
    }
};

std::shared_ptr<const SubtitleCue> text(std::string s) {
    auto c = std::make_shared<SubtitleCue>();
    c->start_us = 0;
    c->text = std::move(s);
    return c;
}

std::shared_ptr<const SubtitleCue> bitmap(int canvas_w, int canvas_h, SubtitleBitmap b) {
    auto c = std::make_shared<SubtitleCue>();
    c->start_us = 0;
    c->canvas_w = canvas_w;
    c->canvas_h = canvas_h;
    b.rgba.assign(static_cast<size_t>(b.width) * b.height * 4, 255);
    c->bitmaps.push_back(std::move(b));
    return c;
}

}  // namespace

TEST("subtitle view: text is centred above the bottom margin, white over an eight-copy outline") {
    Env e;
    e.cues = {text("Hello world")};
    e.frame();
    REQUIRE(e.view->lines().size() == 1u);
    CHECK_EQ(e.view->lines()[0], std::string("Hello world"));
    const auto white = e.r.white();
    REQUIRE(white.size() == 1u);
    CHECK_EQ(e.r.texts.size(), 9u);
    const float size = 1080 * 0.055f;
    const float width = 11 * size * 0.5f;
    CHECK(std::abs(white[0].baseline.x - (1920 - width) / 2) < 0.5f);
    // One line: its box ends at the margin, 7% above the bottom.
    CHECK(std::abs(white[0].baseline.y - (1080 * 0.93f - size * 1.25f + size)) < 0.5f);
    e.cues.clear();
    e.frame();
    CHECK(e.r.texts.empty());
}

TEST("subtitle view: long and multi-line cues wrap within the video, overlapping cues stack in order") {
    Env e;
    e.view->set_video_rect({0, 0, 1280, 720});
    e.cues = {text("one two three four five six seven eight nine ten eleven twelve thirteen"), text("second cue")};
    e.frame();
    const auto &lines = e.view->lines();
    REQUIRE(lines.size() >= 3u);
    CHECK_EQ(lines.back(), std::string("second cue"));
    const float size = 720 * 0.055f;
    for (const auto &l : lines) CHECK(static_cast<float>(l.size()) * size * 0.5f <= 1280 * 0.84f);
    const auto white = e.r.white();
    for (size_t i = 1; i < white.size(); ++i) CHECK(white[i].baseline.y > white[i - 1].baseline.y);
}

TEST("subtitle view: box style draws a dark box per line and no outline") {
    Env e;
    e.view->set_text_style(SubtitleView::TextStyle::Box);
    e.cues = {text("a\nbb")};
    e.frame();
    CHECK_EQ(e.r.texts.size(), 2u);
    CHECK_EQ(e.r.fills.size(), 2u);
}

TEST("subtitle view: bitmaps map from their canvas onto the video; one upload while shown, released after") {
    Env e;
    SubtitleBitmap b;
    b.x = 360;
    b.y = 480;
    b.width = 100;
    b.height = 40;
    e.view->set_video_rect({0, 0, 1920, 1080});
    e.cues = {bitmap(720, 576, b)};
    for (int i = 0; i < 3; ++i) e.frame();
    REQUIRE(e.r.images.size() == 1u);
    const Rect r = e.r.images[0];
    CHECK(std::abs(r.x - 960) < 0.01f);
    CHECK(std::abs(r.y - 900) < 0.01f);
    CHECK(std::abs(r.w - 100 * 1920 / 720.0f) < 0.01f);
    CHECK(std::abs(r.h - 40 * 1080 / 576.0f) < 0.01f);
    CHECK_EQ(e.r.created, 1);
    CHECK_EQ(e.view->textures(), 1u);
    e.cues.clear();
    e.frame();
    CHECK_EQ(e.r.destroyed, 1);
    CHECK_EQ(e.view->textures(), 0u);
}

TEST("subtitle view: an ASS track draws styled images at the clock's time instead of plain text") {
    if (!AssRenderer::available()) {
        std::printf("  SKIP: built without libass\n");
        return;
    }
    Env e;
    e.view->set_video_rect({0, 0, 1920, 1080});
    int64_t now = 1500000;
    e.view->set_clock([&now] { return now; });
    const std::string header =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n\n[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, Bold\nStyle: Default,Atkinson Hyperlegible,32,&H00FFFFFF,0\n";
    REQUIRE(e.view->set_ass_track(header, {}, WNK_TEST_FONT_DIR "/AtkinsonHyperlegible-Regular.ttf", 1920, 1080));
    auto c = std::make_shared<SubtitleCue>();
    c->start_us = 1000000;
    c->end_us = 2000000;
    c->text = "Hello";
    c->ass_events.push_back("0,0,Default,,0,0,0,,{\\an8}Hello");  // top centre
    e.cues = {c};
    e.frame();
    CHECK(e.r.white().empty());  // no plain text
    REQUIRE(!e.r.images.empty());
    CHECK(e.r.images[0].y < 200);  // where the tags put it
    const int uploads = e.r.created;
    e.frame();  // same time: drawn again from the same textures
    CHECK_EQ(e.r.created, uploads);
    CHECK(!e.r.images.empty());
    CHECK_EQ(e.view->ass_renderer()->stats().events, size_t{1});  // fed once
    now = 2500000;  // past the end
    e.frame();
    CHECK(e.r.images.empty());
    // Back to plain text when the track is not ASS.
    e.view->clear_ass_track();
    e.frame();
    CHECK_EQ(e.r.white().size(), size_t{1});
}
