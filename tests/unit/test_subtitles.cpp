// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/subtitles.hpp"

using namespace wnk::media;

namespace {

std::shared_ptr<const SubtitleCue> text_cue(int track, int64_t start, int64_t end, std::string text) {
    auto c = std::make_shared<SubtitleCue>();
    c->track = track;
    c->start_us = start;
    c->end_us = end;
    c->text = std::move(text);
    return c;
}

std::shared_ptr<const SubtitleCue> bitmap_cue(int track, int64_t start, int w, int h, uint8_t fill) {
    auto c = std::make_shared<SubtitleCue>();
    c->track = track;
    c->start_us = start;
    c->canvas_w = 1920;
    c->canvas_h = 1080;
    SubtitleBitmap b;
    b.width = w;
    b.height = h;
    b.rgba.assign(static_cast<size_t>(w) * h * 4, fill);
    c->bitmaps.push_back(std::move(b));
    return c;
}

std::shared_ptr<const SubtitleCue> clear_cue(int track, int64_t start) {
    auto c = std::make_shared<SubtitleCue>();
    c->track = track;
    c->start_us = start;
    return c;
}

std::string texts(const std::vector<std::shared_ptr<const SubtitleCue>> &v) {
    std::string s;
    for (const auto &c : v) s += (s.empty() ? "" : "|") + c->text;
    return s;
}

}  // namespace

TEST("ass event text: fields, override tags, line breaks, drawings") {
    CHECK_EQ(ass_event_text("0,0,Default,,0,0,0,,Hello world"), std::string("Hello world"));
    CHECK_EQ(ass_event_text("1,0,Default,,0,0,0,,Hello, world, with commas"), std::string("Hello, world, with commas"));
    CHECK_EQ(ass_event_text("2,0,Default,,0,0,0,,{\\i1}Italic{\\i0} and\\Nsecond line"),
             std::string("Italic and\nsecond line"));
    CHECK_EQ(ass_event_text("3,0,Default,,0,0,0,,{\\an8\\pos(960,50)}Sign\\nText"), std::string("Sign\nText"));
    CHECK_EQ(ass_event_text("4,0,Default,,0,0,0,,a\\hb"), std::string("a b"));
    CHECK_EQ(ass_event_text("5,0,Default,,0,0,0,,{\\p1}m 0 0 l 100 0 100 100{\\p0}After"), std::string("After"));
    CHECK_EQ(ass_event_text("6,0,Default,,0,0,0,,  padded  \\N\\N  "), std::string("padded"));
    CHECK_EQ(ass_event_text("7,0,Default,,0,0,0,,{unclosed"), std::string("{unclosed"));
    CHECK_EQ(ass_event_text("too,few,fields"), std::string());
}

TEST("subtitle store: cues show between start and end, per track, overlapping in start order") {
    SubtitleStore s;
    CHECK(s.add(text_cue(2, 1000000, 3000000, "one")));
    CHECK(s.add(text_cue(2, 2000000, 4000000, "two")));
    CHECK(s.add(text_cue(5, 1000000, 9000000, "other track")));
    CHECK_EQ(texts(s.active(2, 500000)), std::string());
    CHECK_EQ(texts(s.active(2, 1000000)), std::string("one"));
    CHECK_EQ(texts(s.active(2, 2500000)), std::string("one|two"));
    CHECK_EQ(texts(s.active(2, 3000000)), std::string("two"));
    CHECK_EQ(texts(s.active(2, 4000000)), std::string());
    CHECK_EQ(texts(s.active(5, 2500000)), std::string("other track"));
    CHECK(s.active(7, 2500000).empty());
    CHECK(s.active(2, kNoTime).empty());
}

TEST("subtitle store: the same cue read again after a seek is dropped; a different one at the same time is kept") {
    SubtitleStore s;
    CHECK(s.add(text_cue(2, 1000000, 2000000, "a")));
    CHECK(!s.add(text_cue(2, 1000000, 2000000, "a")));
    CHECK(s.add(text_cue(2, 1000000, 2000000, "b")));
    CHECK_EQ(s.count(2), 2u);
    // Added out of order (a backward seek): still sorted.
    CHECK(s.add(text_cue(2, 500000, 800000, "early")));
    CHECK_EQ(texts(s.active(2, 600000)), std::string("early"));
    s.clear();
    CHECK_EQ(s.count(2), 0u);
}

TEST("subtitle store: open-ended bitmap cues end at the next cue or clear, capped") {
    SubtitleStore::Limits l;
    l.open_end_us = 5000000;
    SubtitleStore s(l);
    CHECK(s.add(bitmap_cue(3, 1000000, 10, 10, 1)));
    CHECK(s.add(clear_cue(3, 2000000)));
    CHECK(s.add(bitmap_cue(3, 4000000, 10, 10, 2)));
    CHECK_EQ(s.active(3, 1500000).size(), 1u);
    CHECK(s.active(3, 2500000).empty());  // cleared
    CHECK_EQ(s.active(3, 8000000).size(), 1u);
    CHECK(s.active(3, 9000000).empty());  // no successor: capped at 5 s
}

TEST("subtitle store: bitmaps over the byte budget are evicted farthest from the playhead") {
    SubtitleStore::Limits l;
    l.max_bitmap_bytes = 3 * 100 * 100 * 4;
    SubtitleStore s(l);
    s.active(3, 10000000);  // the playhead is at 10 s
    CHECK(s.add(bitmap_cue(3, 1000000, 100, 100, 1)));
    CHECK(s.add(bitmap_cue(3, 9000000, 100, 100, 2)));
    CHECK(s.add(bitmap_cue(3, 10000000, 100, 100, 3)));
    CHECK(s.add(bitmap_cue(3, 11000000, 100, 100, 4)));
    CHECK_EQ(s.count(3), 3u);
    CHECK(s.bitmap_bytes() <= l.max_bitmap_bytes);
    CHECK(s.active(3, 1500000).empty());     // the cue at 1 s went
    CHECK_EQ(s.active(3, 9500000).size(), 1u);
}
