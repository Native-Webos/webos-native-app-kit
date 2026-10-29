// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/subtitles.hpp"
#include "wnk/media/ttml.hpp"
#include "wnk/media/webvtt.hpp"

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

TEST("webvtt: cues with and without identifiers, settings, tags and entities; NOTE and STYLE skipped") {
    const std::string vtt = "\xEF\xBB\xBFWEBVTT - a title\r\n\r\nSTYLE\r\n::cue { color: lime }\r\n\r\nNOTE a comment\r\nthat spans\r\n\r\n"
                            "1\r\n00:00:01.000 --> 00:00:02.500 align:start line:0\r\n<v Roger>Hello &amp; <i>welcome</i></v>\r\n"
                            "second &lt;line&gt;\r\n\r\n"
                            "01:02.250 --> 01:03.000\r\nno identifier, short timestamps\r\n\r\n"
                            "bad\r\n00:00:05.000 --> 00:00:04.000\r\nend before start: skipped\r\n\r\n"
                            "10:00:00.000 --> 10:00:01.000\r\n<c.yellow>long</c> &nbsp;hours\r\n";
    WebVttFile f;
    std::string err;
    REQUIRE(parse_webvtt(vtt, &f, &err));
    CHECK(!f.has_timestamp_map);
    REQUIRE(f.cues.size() == 3);
    CHECK_EQ(f.cues[0].start_us, int64_t{1000000});
    CHECK_EQ(f.cues[0].end_us, int64_t{2500000});
    CHECK_EQ(f.cues[0].text, std::string("Hello & welcome\nsecond <line>"));
    CHECK_EQ(f.cues[1].start_us, int64_t{62250000});
    CHECK_EQ(f.cues[2].start_us, int64_t{36000000000});
    CHECK_EQ(f.cues[2].text, std::string("long  hours"));
}

TEST("webvtt: HLS X-TIMESTAMP-MAP is reported; non-WebVTT is refused") {
    WebVttFile f;
    std::string err;
    REQUIRE(parse_webvtt("WEBVTT\nX-TIMESTAMP-MAP=MPEGTS:900000,LOCAL:00:00:00.000\n\n9\n00:00:08.000 --> 00:00:08.992\nBip!\n", &f, &err));
    CHECK(f.has_timestamp_map);
    CHECK_EQ(f.map_mpegts, int64_t{900000});
    CHECK_EQ(f.map_local_us, int64_t{0});
    REQUIRE(f.cues.size() == 1);
    CHECK_EQ(f.cues[0].start_us, int64_t{8000000});
    REQUIRE(parse_webvtt("WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00:01.000,MPEGTS:180000\n", &f, &err));
    CHECK_EQ(f.map_local_us, int64_t{1000000});
    CHECK_EQ(f.map_mpegts, int64_t{180000});
    CHECK(f.cues.empty());
    CHECK(!parse_webvtt("1\n00:00:01,000 --> 00:00:02,000\nSubRip\n", &f, &err));
    CHECK(!parse_webvtt("WEBVTTX\n", &f, &err));
    int64_t us = 0;
    CHECK(!parse_vtt_timestamp("00:61.000", &us));
    CHECK(!parse_vtt_timestamp("00:01.5", &us));
}

namespace {

void be32(std::vector<uint8_t> *b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b->push_back(static_cast<uint8_t>(v >> s));
}

std::vector<uint8_t> box(const char *type, const std::vector<uint8_t> &body) {
    std::vector<uint8_t> b;
    be32(&b, static_cast<uint32_t>(body.size() + 8));
    b.insert(b.end(), type, type + 4);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto &p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

std::vector<uint8_t> text(const std::string &s) { return {s.begin(), s.end()}; }

}  // namespace

TEST("wvtt: timescale from mdhd; cues and gaps from a fragment, timed by tfdt and trun") {
    std::vector<uint8_t> mdhd(4, 0);  // version 0, flags
    be32(&mdhd, 0), be32(&mdhd, 0), be32(&mdhd, 1000), be32(&mdhd, 0), be32(&mdhd, 0x55c40000);  // language, pre_defined
    const auto init = box("moov", box("trak", box("mdia", box("mdhd", mdhd))));
    uint32_t ts = 0;
    std::string err;
    REQUIRE(parse_wvtt_init(init.data(), init.size(), &ts, &err));
    CHECK_EQ(ts, 1000u);

    // Three samples: a cue (2 s), a gap (1 s), two simultaneous cues (0.5 s); from t = 10 s.
    const auto s1 = box("vttc", cat({box("sttg", text("line:0")), box("payl", text("<i>Hello</i> &amp; bye\nline two"))}));
    const auto s2 = box("vtte", {});
    const auto s3 = cat({box("vttc", box("payl", text("left"))), box("vttc", box("payl", text("right")))});
    std::vector<uint8_t> tfhd = {0, 0, 0, 0};  // no optional fields
    be32(&tfhd, 1);
    std::vector<uint8_t> tfdt = {0, 0, 0, 0};
    be32(&tfdt, 10000);
    std::vector<uint8_t> trun = {0, 0, 0x03, 0x01};  // data offset, per-sample duration and size
    be32(&trun, 3);
    const size_t offset_at = trun.size();
    be32(&trun, 0);  // patched below
    for (auto [dur, size] : {std::pair<uint32_t, size_t>{2000, s1.size()}, {1000, s2.size()}, {500, s3.size()}}) {
        be32(&trun, dur);
        be32(&trun, static_cast<uint32_t>(size));
    }
    auto moof = box("moof", box("traf", cat({box("tfhd", tfhd), box("tfdt", tfdt), box("trun", trun)})));
    const uint32_t data_offset = static_cast<uint32_t>(moof.size() + 8);  // past moof and the mdat header
    const size_t at = 8 + 8 + 8 + tfhd.size() + 8 + tfdt.size() + 8 + offset_at;  // moof, traf, tfhd, tfdt, trun headers
    for (int i = 0; i < 4; ++i) moof[at + static_cast<size_t>(i)] = static_cast<uint8_t>(data_offset >> (24 - 8 * i));
    const auto frag = cat({moof, box("mdat", cat({s1, s2, s3}))});
    std::vector<SubtitleCue> cues;
    REQUIRE(parse_wvtt_fragment(frag.data(), frag.size(), ts, &cues, &err));
    REQUIRE(cues.size() == 3);
    CHECK_EQ(cues[0].start_us, int64_t{10000000});
    CHECK_EQ(cues[0].end_us, int64_t{12000000});
    CHECK_EQ(cues[0].text, std::string("Hello & bye\nline two"));
    CHECK_EQ(cues[1].start_us, int64_t{13000000});  // after the 1 s gap
    CHECK_EQ(cues[1].text, std::string("left"));
    CHECK_EQ(cues[2].text, std::string("right"));
    CHECK_EQ(cues[2].end_us, int64_t{13500000});
    CHECK(!parse_wvtt_fragment(frag.data(), frag.size() - 10, ts, &cues, &err));  // truncated mdat
    CHECK(!parse_wvtt_init(frag.data(), frag.size(), &ts, &err));
}

TEST("ttml time expressions: clock times, frames, offsets, ticks") {
    TtmlRates r;
    int64_t us = 0;
    REQUIRE(parse_ttml_time("01:02:03.5", r, &us));
    CHECK_EQ(us, int64_t{3723500000});
    REQUIRE(parse_ttml_time("100:00:00", r, &us));
    CHECK_EQ(us, int64_t{360000000000});
    r.frame_rate = 25;
    REQUIRE(parse_ttml_time("00:00:01:05", r, &us));  // 1 s + 5 frames at 25 fps
    CHECK_EQ(us, int64_t{1200000});
    REQUIRE(parse_ttml_time("50f", r, &us));
    CHECK_EQ(us, int64_t{2000000});
    REQUIRE(parse_ttml_time(" 1.5s ", r, &us));
    CHECK_EQ(us, int64_t{1500000});
    REQUIRE(parse_ttml_time("250ms", r, &us));
    CHECK_EQ(us, int64_t{250000});
    REQUIRE(parse_ttml_time("2m", r, &us));
    CHECK_EQ(us, int64_t{120000000});
    REQUIRE(parse_ttml_time("0.5h", r, &us));
    CHECK_EQ(us, int64_t{1800000000});
    r.tick_rate = 10000000;
    REQUIRE(parse_ttml_time("15000000t", r, &us));
    CHECK_EQ(us, int64_t{1500000});
    CHECK(!parse_ttml_time("1.5", r, &us));        // no metric
    CHECK(!parse_ttml_time("00:1:00", r, &us));    // minutes need two digits
    CHECK(!parse_ttml_time("00:00:00.5:01", r, &us));
    CHECK(!parse_ttml_time("-1s", r, &us));
    CHECK(!parse_ttml_time("1x", r, &us));
}

TEST("ttml: timing inherited from body and div, br, white space, entities, styling dropped") {
    const std::string doc =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:tts=\"http://www.w3.org/ns/ttml#styling\" xml:lang=\"en\">"
            "<head><styling><style xml:id=\"s\" tts:color=\"yellow\"/></styling><metadata>not shown</metadata></head>"
            "<body begin=\"10s\"><div begin=\"1s\">\n"
            "  <p begin=\"00:00:00.500\" end=\"00:00:02.000\" style=\"s\">\n    Hello   <span tts:fontStyle=\"italic\">big</span>\n"
            "    world<br/>  line &amp; two  </p>\n"
            "  <p begin=\"3s\" dur=\"1s\" xml:space=\"preserve\">a  b</p>\n"
            "  <p begin=\"5s\"><metadata>x</metadata></p>\n"
            "  <p begin=\"6s\">open</p>\n"
            "</div></body></tt>";
    TtmlFile f;
    std::string err;
    REQUIRE(parse_ttml(doc, &f, &err));
    REQUIRE(f.cues.size() == 3);
    CHECK_EQ(f.cues[0].start_us, int64_t{11500000});  // body 10 s + div 1 s + p 0.5 s
    CHECK_EQ(f.cues[0].end_us, int64_t{13000000});
    CHECK_EQ(f.cues[0].text, std::string("Hello big world\nline & two"));
    CHECK_EQ(f.cues[1].start_us, int64_t{14000000});
    CHECK_EQ(f.cues[1].end_us, int64_t{15000000});
    CHECK_EQ(f.cues[1].text, std::string("a  b"));
    CHECK_EQ(f.cues[2].start_us, int64_t{17000000});
    CHECK_EQ(f.cues[2].end_us, kNoTime);  // nothing ends it
    CHECK_EQ(f.cues[2].text, std::string("open"));
}

TEST("ttml: spans timed on their own become one cue per stretch; ticks; seq; refusals") {
    // Paint-on: "one" from 1 s, "two" joins at 2 s, both until 3 s.
    const std::string doc = "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttp=\"http://www.w3.org/ns/ttml#parameter\" "
                            "ttp:tickRate=\"10\"><body><div>"
                            "<p begin=\"10t\" end=\"30t\"><span>one</span> <span begin=\"10t\">two</span></p>"
                            "<p begin=\"40t\" end=\"50t\"><span end=\"5t\">gone</span><span begin=\"5t\">gone</span></p>"
                            "</div><div timeContainer=\"seq\" begin=\"10s\"><p dur=\"1s\">s1</p><p dur=\"2s\">s2</p></div>"
                            "</body></tt>";
    TtmlFile f;
    std::string err;
    REQUIRE(parse_ttml(doc, &f, &err));
    REQUIRE(f.cues.size() == 5);
    CHECK_EQ(f.cues[0].text, std::string("one"));
    CHECK_EQ(f.cues[0].start_us, int64_t{1000000});
    CHECK_EQ(f.cues[0].end_us, int64_t{2000000});
    CHECK_EQ(f.cues[1].text, std::string("one two"));
    CHECK_EQ(f.cues[1].start_us, int64_t{2000000});
    CHECK_EQ(f.cues[1].end_us, int64_t{3000000});
    CHECK_EQ(f.cues[2].text, std::string("gone"));  // the same text either side of a cut: one cue
    CHECK_EQ(f.cues[2].start_us, int64_t{4000000});
    CHECK_EQ(f.cues[2].end_us, int64_t{5000000});
    CHECK_EQ(f.cues[3].start_us, int64_t{10000000});  // seq: one after the other
    CHECK_EQ(f.cues[4].start_us, int64_t{11000000});
    CHECK_EQ(f.cues[4].end_us, int64_t{13000000});

    REQUIRE(parse_ttml("<tt xmlns=\"http://www.w3.org/ns/ttml\"><body/></tt>", &f, &err));  // a gap
    CHECK(f.cues.empty());
    CHECK(!parse_ttml("<html/>", &f, &err));
    CHECK(err.find("not TTML") != std::string::npos);
    CHECK(!parse_ttml("<tt><body><p begin=\"soon\">x</p></body></tt>", &f, &err));
    CHECK(!parse_ttml("<tt><body>", &f, &err));
}

namespace {

// A fragment of one text track: samples of the given durations (ms, timescale 1000) from `start_ms`.
std::vector<uint8_t> text_fragment(uint32_t start_ms, const std::vector<std::pair<uint32_t, std::vector<uint8_t>>> &samples) {
    std::vector<uint8_t> tfhd = {0, 0, 0, 0};
    be32(&tfhd, 1);
    std::vector<uint8_t> tfdt = {0, 0, 0, 0};
    be32(&tfdt, start_ms);
    std::vector<uint8_t> trun = {0, 0, 0x03, 0x01};
    be32(&trun, static_cast<uint32_t>(samples.size()));
    const size_t offset_at = trun.size();
    be32(&trun, 0);
    std::vector<uint8_t> mdat;
    for (const auto &[dur, body] : samples) {
        be32(&trun, dur);
        be32(&trun, static_cast<uint32_t>(body.size()));
        mdat.insert(mdat.end(), body.begin(), body.end());
    }
    auto moof = box("moof", box("traf", cat({box("tfhd", tfhd), box("tfdt", tfdt), box("trun", trun)})));
    const uint32_t data_offset = static_cast<uint32_t>(moof.size() + 8);
    const size_t at = 8 + 8 + 8 + tfhd.size() + 8 + tfdt.size() + 8 + offset_at;
    for (int i = 0; i < 4; ++i) moof[at + static_cast<size_t>(i)] = static_cast<uint8_t>(data_offset >> (24 - 8 * i));
    return cat({moof, box("mdat", mdat)});
}

}  // namespace

TEST("stpp: each sample a TTML document on the media timeline; sample-relative documents moved; gaps") {
    const std::string tt = "<tt xmlns=\"http://www.w3.org/ns/ttml\"><body><div>", end = "</div></body></tt>";
    // 20-22 s: media time. 22-24 s: timed from the sample start, padded. 24-25 s: an open end. 25-26 s: empty.
    const auto frag = text_fragment(20000, {{2000, text(tt + "<p begin=\"00:00:20.5\" end=\"00:00:21.5\">media</p>" + end)},
                                            {2000, text(tt + "<p begin=\"0.5s\" end=\"1s\">relative</p>" + end + std::string(2, '\0'))},
                                            {1000, text(tt + "<p begin=\"24.2s\">open</p>" + end)},
                                            {1000, {}}});
    std::vector<SubtitleCue> cues;
    std::string err;
    REQUIRE(parse_stpp_fragment(frag.data(), frag.size(), 1000, &cues, &err));
    REQUIRE(cues.size() == 3);
    CHECK_EQ(cues[0].text, std::string("media"));
    CHECK_EQ(cues[0].start_us, int64_t{20500000});
    CHECK_EQ(cues[0].end_us, int64_t{21500000});
    CHECK_EQ(cues[1].text, std::string("relative"));
    CHECK_EQ(cues[1].start_us, int64_t{22500000});
    CHECK_EQ(cues[1].end_us, int64_t{23000000});
    CHECK_EQ(cues[2].text, std::string("open"));
    CHECK_EQ(cues[2].start_us, int64_t{24200000});
    CHECK_EQ(cues[2].end_us, int64_t{25000000});  // closed at the sample's end
    const auto bad = text_fragment(0, {{1000, text("<tt><body><p begin=\"x\">no</p></body></tt>")}});
    CHECK(!parse_stpp_fragment(bad.data(), bad.size(), 1000, &cues, &err));
    CHECK(err.find("TTML sample") != std::string::npos);
}
