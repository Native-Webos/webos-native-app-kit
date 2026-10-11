// SPDX-License-Identifier: GPL-3.0-or-later
//
// AdaptiveSource against DASH and HLS presentations written by the system ffmpeg CLI into the
// directory tests/integration/http_server.py serves (WNK_TEST_FILES_DIR at WNK_TEST_HTTP_BASE/files/).
// 10 s of 25 fps H.264 + AAC, keyframe every second, 2 s segments. SKIPs without the CLI or server.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/adaptive_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace wnk;
using namespace wnk::media;

namespace {

std::string env(const char *name) {
    const char *v = std::getenv(name);
    return v != nullptr ? v : "";
}

bool have_server() { return !env("WNK_TEST_HTTP_BASE").empty() && !env("WNK_TEST_FILES_DIR").empty(); }

bool have_ffmpeg() {
    static const bool ok = std::system("ffmpeg -hide_banner -version >/dev/null 2>&1") == 0;
    return ok;
}

// Writes the presentations once. Names are flat because the server serves one directory.
bool make_presentations() {
    static const bool ok = [] {
        const std::string d = env("WNK_TEST_FILES_DIR");
        const std::string in = "cd '" + d + "' && ffmpeg -hide_banner -loglevel error -y "
                               "-f lavfi -i testsrc2=size=320x240:rate=25:duration=10 "
                               "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=10 -map 0:v -map 1:a "
                               "-c:v libx264 -g 25 -keyint_min 25 -sc_threshold 0 -c:a aac ";
        const std::string cmds[] = {
            in + "-f dash -seg_duration 2 -use_template 1 -use_timeline 0 -init_seg_name 'd-init-$RepresentationID$.m4s' "
                 "-media_seg_name 'd-$RepresentationID$-$Number%05d$.m4s' d.mpd",
            // ffmpeg's dash muxer lists the first AAC segment at t="0" but names its file after the
            // primed time (-1024); rename it so the manifest is self-consistent.
            in + "-f dash -seg_duration 2 -use_template 1 -use_timeline 1 -init_seg_name 't-init-$RepresentationID$.m4s' "
                 "-media_seg_name 't-$RepresentationID$-$Time$.m4s' t.mpd && for f in t-1--*.m4s; do mv \"$f\" t-1-0.m4s; done",
            in + "-f hls -hls_time 2 -hls_playlist_type vod -hls_segment_filename 'ts_%03d.ts' ts.m3u8",
            in + "-f hls -hls_time 2 -hls_playlist_type vod -hls_segment_type fmp4 -hls_fmp4_init_filename 'f_init_%v.mp4' "
                 "-hls_segment_filename 'f_%v_%03d.m4s' -master_pl_name f.m3u8 "
                 "-var_stream_map 'v:0,agroup:aud a:0,agroup:aud,language:de,name:Deutsch' 'f_%v.m3u8'",
        };
        // Video keyframes every 3 s against 2 s audio segments: the renditions' boundaries differ.
        std::string misaligned = in + "-f hls -hls_time 2 -hls_playlist_type vod -hls_segment_type fmp4 "
                                      "-hls_fmp4_init_filename 'g_init_%v.mp4' -hls_segment_filename 'g_%v_%03d.m4s' "
                                      "-master_pl_name g.m3u8 -var_stream_map 'v:0,agroup:aud a:0,agroup:aud' 'g_%v.m3u8'";
        const size_t gop = misaligned.find("-g 25 -keyint_min 25");
        misaligned.replace(gop, 20, "-g 75 -keyint_min 75");
        for (const std::string &c : cmds) {
            if (std::system(c.c_str()) != 0) return false;
        }
        if (std::system(misaligned.c_str()) != 0) return false;
        // A three-rung ladder (same keyframes), 20 s, for bitrate switching.
        const std::string ladder =
            "cd '" + d + "' && ffmpeg -hide_banner -loglevel error -y "
            "-f lavfi -i testsrc2=size=640x480:rate=25:duration=20 "
            "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=20 -map 0:v -map 0:v -map 0:v -map 1:a "
            "-c:v libx264 -g 25 -keyint_min 25 -sc_threshold 0 -c:a aac "
            "-s:v:0 160x120 -b:v:0 100k -s:v:1 320x240 -b:v:1 400k -s:v:2 640x480 -b:v:2 1500k "
            "-f dash -seg_duration 2 -adaptation_sets 'id=0,streams=v id=1,streams=a' "
            "-init_seg_name 'l-init-$RepresentationID$.m4s' -media_seg_name 'l-$RepresentationID$-$Number%05d$.m4s' l.mpd";
        if (std::system(ladder.c_str()) != 0) return false;
        // Subtitles: ten cues "Cue N" at N.2-N.8 s, as HLS WebVTT segments (no X-TIMESTAMP-MAP from
        // ffmpeg) and as a DASH text/vtt file added to a copy of d.mpd.
        std::string srt, vtt = "WEBVTT\n\n";
        for (int i = 0; i < 10; ++i) {
            char line[128];
            std::snprintf(line, sizeof line, "%d\n00:00:%02d,200 --> 00:00:%02d,800\nCue %d\n\n", i + 1, i, i, i);
            srt += line;
            std::snprintf(line, sizeof line, "00:00:%02d.200 --> 00:00:%02d.800\n<i>Cue</i> %d\n\n", i, i, i);
            vtt += line;
        }
        if (FILE *f = std::fopen((d + "/cues.srt").c_str(), "w")) {
            std::fputs(srt.c_str(), f);
            std::fclose(f);
        }
        if (FILE *f = std::fopen((d + "/de.vtt").c_str(), "w")) {
            std::fputs(vtt.c_str(), f);
            std::fclose(f);
        }
        const std::string subs =
            "cd '" + d + "' && ffmpeg -hide_banner -loglevel error -y "
            "-f lavfi -i testsrc2=size=320x240:rate=25:duration=10 "
            "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=10 -i cues.srt -map 0:v -map 1:a -map 2:s "
            "-c:v libx264 -g 25 -keyint_min 25 -sc_threshold 0 -c:a aac -c:s webvtt -f hls -hls_time 2 "
            "-hls_playlist_type vod -hls_segment_filename 's_%v_%03d.ts' -master_pl_name s.m3u8 "
            "-var_stream_map 'v:0,a:0,s:0,sgroup:subs,language:en' 's_%v.m3u8'";
        if (std::system(subs.c_str()) != 0) return false;
        // d.mpd with a German text/vtt AdaptationSet and a TTML one this build cannot read.
        FILE *src = std::fopen((d + "/d.mpd").c_str(), "r");
        if (src == nullptr) return false;
        std::string mpd;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, src)) > 0) mpd.append(buf, n);
        std::fclose(src);
        const size_t end = mpd.rfind("</Period>");
        if (end == std::string::npos) return false;
        mpd.insert(end, "<AdaptationSet id=\"9\" contentType=\"text\" mimeType=\"text/vtt\" lang=\"de\">"
                        "<Role schemeIdUri=\"urn:mpeg:dash:role:2011\" value=\"main\"/>"
                        "<Representation id=\"de\" bandwidth=\"100\"><BaseURL>de.vtt</BaseURL></Representation>"
                        "</AdaptationSet><AdaptationSet id=\"10\" contentType=\"text\" mimeType=\"application/mp4\" "
                        "codecs=\"stpp\" lang=\"fr\"><Representation id=\"fr\" bandwidth=\"100\">"
                        "<BaseURL>fr.mp4</BaseURL></Representation></AdaptationSet>");
        if (FILE *out = std::fopen((d + "/ds.mpd").c_str(), "w")) {
            std::fputs(mpd.c_str(), out);
            std::fclose(out);
        }
        return true;
    }();
    return ok;
}

std::shared_ptr<net::HttpTransport> transport() {
    static std::shared_ptr<net::HttpTransport> t = [] {
        net::CurlOptions o;
        o.ca_bundle = env("WNK_TEST_CA");
        return std::shared_ptr<net::HttpTransport>(net::make_curl_transport(o));
    }();
    return t;
}

std::string url(const std::string &name) { return env("WNK_TEST_HTTP_BASE") + "/files/" + name; }

AdaptiveSourceOptions options() {
    AdaptiveSourceOptions o;
    o.http = transport();
    o.video_codecs = {CodecId::H264};
    return o;
}

BackendCaps starfish_like() {
    BackendCaps c;
    c.video_codecs = {CodecId::H264, CodecId::Hevc};
    c.audio_codecs = {CodecId::Aac, CodecId::Ac3, CodecId::Eac3};
    c.aac_adts = true;
    c.adaptive_resolution = true;
    return c;
}

BackendCaps ndl_like() {
    BackendCaps c;
    c.video_codecs = {CodecId::H264, CodecId::Hevc};
    c.audio_codecs = {CodecId::PcmS16};
    return c;
}

struct Drained {
    std::vector<Packet> video, audio;
    bool ordered = true;  // the merged stream never goes back in time by more than a frame
    bool timed = true;    // every packet has a timestamp (a backend cannot place one without)
};

Drained drain(AdaptiveSource &s) {
    Drained d;
    Packet p;
    std::string err;
    int64_t last = INT64_MIN;
    MediaSource::Read r;
    while ((r = s.read(&p, &err)) == MediaSource::Read::Packet) {
        d.timed = d.timed && p.pts_us != kNoTime;
        if (p.pts_us != kNoTime) {
            const int64_t t = p.dts_us != kNoTime ? p.dts_us : p.pts_us;
            if (last != INT64_MIN && t < last) d.ordered = false;
            last = std::max(last, t);
        }
        (p.type == TrackType::Video ? d.video : d.audio).push_back(p);
    }
    if (r != MediaSource::Read::EndOfStream) std::printf("  read ended with %d: %s\n", static_cast<int>(r), err.c_str());
    return d;
}

#define SKIP_UNLESS_READY()                                                   \
    if (!have_server() || !have_ffmpeg()) {                                   \
        std::printf("  SKIP: run through http_server.py with the ffmpeg CLI\n"); \
        return;                                                               \
    }                                                                         \
    REQUIRE(make_presentations());

void check_full_play(const char *name, const BackendCaps &caps, CodecId audio_codec) {
    AdaptiveSource s(options());
    std::string err;
    REQUIRE(s.open(url(name), &err));
    const MediaInfo &info = s.info();
    CHECK(std::abs(info.duration_us - 10000000) < 100000);
    REQUIRE(info.tracks.size() >= 2);
    CHECK(info.tracks[0].type == TrackType::Video && info.tracks[0].codec == CodecId::H264);
    const TrackInfo *a = info.best(TrackType::Audio);
    REQUIRE(a != nullptr);
    CHECK(a->codec == CodecId::Aac);
    StreamFormat f;
    REQUIRE(s.select(0, a->index, caps, &f, &err));
    REQUIRE(f.video && f.audio);
    CHECK_EQ(f.video->width, 320);
    CHECK(f.audio->codec == audio_codec);
    const Drained d = drain(s);
    CHECK_EQ(d.video.size(), size_t{250});
    REQUIRE(!d.video.empty() && !d.audio.empty());
    CHECK(d.video[0].key);
    // 0, or the AAC priming when the container's start is the audio's (MPEG-TS, as for local files).
    CHECK(d.video[0].pts_us >= 0 && d.video[0].pts_us < 50000);
    CHECK(std::llabs(d.audio[0].pts_us) < 50000);
    CHECK(d.audio.back().pts_us > 9800000);
    CHECK(d.timed);
    if (s.audio_rendition() >= 0) CHECK(d.ordered);  // our merge; one lane keeps the container order
    size_t keys = 0;
    for (const Packet &p : d.video) keys += p.key ? 1 : 0;
    CHECK_EQ(keys, size_t{10});
}

void check_seek(const char *name, int64_t target, int64_t expect_video) {
    AdaptiveSource s(options());
    std::string err;
    REQUIRE(s.open(url(name), &err));
    StreamFormat f;
    REQUIRE(s.select(0, s.info().best(TrackType::Audio)->index, starfish_like(), &f, &err));
    Packet p;
    for (int i = 0; i < 30; ++i) REQUIRE(s.read(&p, &err) == MediaSource::Read::Packet);
    REQUIRE(s.seek(target, &err));
    const Drained d = drain(s);
    REQUIRE(!d.video.empty() && !d.audio.empty());
    CHECK(d.video[0].key);
    CHECK(d.video[0].pts_us >= expect_video && d.video[0].pts_us < expect_video + 50000);
    CHECK(std::llabs(d.audio[0].pts_us - expect_video) < 100000);
    CHECK(d.timed);
    CHECK_EQ(d.video.size(), static_cast<size_t>((10000000 - expect_video) / 40000));
}

}  // namespace

TEST("adaptive source: DASH SegmentTemplate by number, AAC passed through (Starfish-like)") {
    SKIP_UNLESS_READY();
    check_full_play("d.mpd", starfish_like(), CodecId::Aac);
}

TEST("adaptive source: DASH SegmentTimeline with $Time$ (audio starts at a negative time), AAC to PCM") {
    SKIP_UNLESS_READY();
    check_full_play("t.mpd", ndl_like(), CodecId::PcmS16);
}

TEST("adaptive source: HLS MPEG-TS media playlist, audio muxed, time from 0") {
    SKIP_UNLESS_READY();
    AdaptiveSource s(options());
    std::string err;
    REQUIRE(s.open(url("ts.m3u8"), &err));
    CHECK_EQ(s.info().tracks.size(), size_t{2});  // video + the muxed audio
    check_full_play("ts.m3u8", starfish_like(), CodecId::Aac);
}

TEST("adaptive source: HLS fMP4 master with a separate German audio rendition") {
    SKIP_UNLESS_READY();
    AdaptiveSource s(options());
    std::string err;
    REQUIRE(s.open(url("f.m3u8"), &err));
    REQUIRE(s.info().tracks.size() == 2);
    CHECK_EQ(s.info().tracks[1].language, std::string("de"));
    check_full_play("f.m3u8", starfish_like(), CodecId::Aac);
    StreamFormat f;
    REQUIRE(s.select(0, 1, starfish_like(), &f, &err));
    CHECK(s.audio_rendition() >= 0);  // a lane of its own
}

TEST("adaptive source: seeks land on the segment start in DASH, HLS-TS and HLS-fMP4") {
    SKIP_UNLESS_READY();
    check_seek("d.mpd", 5500000, 4000000);
    check_seek("t.mpd", 7999000, 6000000);
    check_seek("ts.m3u8", 4000000, 4000000);
    check_seek("f.m3u8", 9000000, 8000000);
}

TEST("adaptive source: audio reopened at an earlier segment boundary is trimmed to the video's keyframe") {
    SKIP_UNLESS_READY();
    AdaptiveSource s(options());
    std::string err;
    REQUIRE(s.open(url("g.m3u8"), &err));
    StreamFormat f;
    REQUIRE(s.select(0, 1, starfish_like(), &f, &err));
    REQUIRE(s.audio_rendition() >= 0);
    REQUIRE(s.seek(4500000, &err));
    const Drained d = drain(s);
    REQUIRE(!d.video.empty() && !d.audio.empty());
    CHECK(d.video[0].key);
    CHECK(d.video[0].pts_us >= 3000000 && d.video[0].pts_us < 3050000);  // the 3 s keyframe
    // The audio segment starts at 2 s; what ends before the keyframe is not delivered.
    CHECK(d.audio[0].pts_us + d.audio[0].duration_us > d.video[0].pts_us);
    CHECK(d.audio[0].pts_us <= d.video[0].pts_us);
    CHECK(d.ordered);
}

TEST("adaptive source: missing and malformed manifests, live, interrupt") {
    SKIP_UNLESS_READY();
    std::string err;
    {
        AdaptiveSource s(options());
        CHECK(!s.open(url("nope.mpd"), &err));
        CHECK(err.find("404") != std::string::npos);
    }
    {
        AdaptiveSource s(options());
        CHECK(!s.open(url("ts_000.ts"), &err));  // not a manifest
    }
    CHECK(AdaptiveSource::handles("https://h/a/b.MPD?x=1"));
    CHECK(AdaptiveSource::handles("http://h/master.m3u8"));
    CHECK(!AdaptiveSource::handles("https://h/a.mp4"));
    CHECK(!AdaptiveSource::handles("file:///a.mpd"));
    AdaptiveSource s(options());
    REQUIRE(s.open(url("d.mpd"), &err));
    StreamFormat f;
    REQUIRE(s.select(0, 1, starfish_like(), &f, &err));
    s.interrupt();
    Packet p;
    CHECK(s.read(&p, &err) == MediaSource::Read::Interrupted);
    s.reset_interrupt();
    CHECK(s.read(&p, &err) == MediaSource::Read::Packet);

    // A seek cut short by the next command (held arrow keys) succeeds; reading on reopens there.
    s.interrupt();
    CHECK(s.seek(4000000, &err));
    CHECK(s.read(&p, &err) == MediaSource::Read::Interrupted);
    s.reset_interrupt();
    const Drained d = drain(s);
    REQUIRE(!d.video.empty());
    CHECK(d.video[0].key);
    CHECK_EQ(d.video[0].pts_us, int64_t{4000000});
}

namespace {

// Reads until `want` cues of `track` have arrived (or the stream ends), keeping them.
std::vector<std::shared_ptr<const SubtitleCue>> read_cues(AdaptiveSource &s, int track, size_t want) {
    std::vector<std::shared_ptr<const SubtitleCue>> cues;
    Packet p;
    std::string err;
    while (cues.size() < want && s.read(&p, &err) == MediaSource::Read::Packet) {
        if (p.type == TrackType::Subtitle && p.subtitle && p.subtitle->track == track) cues.push_back(p.subtitle);
    }
    std::sort(cues.begin(), cues.end(), [](const auto &a, const auto &b) { return a->start_us < b->start_us; });
    return cues;
}

}  // namespace

TEST("adaptive source: HLS WebVTT subtitles arrive as cues on the presentation timeline") {
    SKIP_UNLESS_READY();
    AdaptiveSource s(options());
    std::string err;
    REQUIRE(s.open(url("s.m3u8"), &err));
    const MediaInfo &info = s.info();
    REQUIRE(info.tracks.size() == 3);  // video, muxed audio, subtitles
    const TrackInfo &t = info.tracks[2];
    CHECK(t.type == TrackType::Subtitle);
    CHECK(t.subtitle == SubtitleKind::Text);
    CHECK_EQ(t.language, std::string("en"));
    CHECK(t.is_default);
    StreamFormat f;
    REQUIRE(s.select(0, 1, starfish_like(), &f, &err));
    const auto cues = read_cues(s, 2, 10);
    REQUIRE(cues.size() == 10);
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(cues[static_cast<size_t>(i)]->text, "Cue " + std::to_string(i));
        // ffmpeg writes MPEG-TS from 1.4 s and cue times on the same clock without a timestamp
        // map; within a frame or two of N.2 s is what a viewer needs.
        CHECK(std::llabs(cues[static_cast<size_t>(i)]->start_us - (i * 1000000 + 200000)) < 150000);
    }
}

TEST("adaptive source: DASH text/vtt subtitles; TTML listed but not readable; a seek does not stop loading") {
    SKIP_UNLESS_READY();
    AdaptiveSource s(options());
    std::string err;
    REQUIRE(s.open(url("ds.mpd"), &err));
    const MediaInfo &info = s.info();
    REQUIRE(info.tracks.size() == 4);
    CHECK_EQ(info.tracks[2].language, std::string("de"));
    CHECK(info.tracks[2].subtitle == SubtitleKind::Text);
    CHECK(info.tracks[2].is_default);  // Role main
    CHECK_EQ(info.tracks[3].language, std::string("fr"));
    CHECK(info.tracks[3].subtitle == SubtitleKind::None);
    StreamFormat f;
    REQUIRE(s.select(0, 1, starfish_like(), &f, &err));
    s.interrupt();  // a seek's interrupt must not reach the subtitle loader
    REQUIRE(s.seek(4000000, &err));
    s.reset_interrupt();
    const auto cues = read_cues(s, 2, 10);
    REQUIRE(cues.size() == 10);
    CHECK_EQ(cues[3]->text, std::string("Cue 3"));  // tags removed
    CHECK_EQ(cues[3]->start_us, int64_t{3200000});
    CHECK_EQ(cues[3]->end_us, int64_t{3800000});
}

TEST("throughput estimate: needs 128 KiB, ignores small transfers, falls fast and rises slowly") {
    ThroughputEstimator e;
    CHECK_EQ(e.estimate_bps(), int64_t{0});
    e.add(8 << 10, 0.001);  // too small to count
    CHECK_EQ(e.samples(), 0);
    e.add(100 << 10, 0.1);  // 8.2 Mb/s, not enough data yet
    CHECK_EQ(e.estimate_bps(), int64_t{0});
    e.add(100 << 10, 0.1);
    const int64_t steady = e.estimate_bps();
    CHECK(std::llabs(steady - 8192000) < 100000);  // bias-corrected: the true rate, not a ramp from 0
    e.add(1 << 20, 4.0);  // a slow patch at 2 Mb/s for 4 s
    const int64_t after_drop = e.estimate_bps();
    CHECK(after_drop < 5000000);  // the fast average pulls it down at once
    for (int i = 0; i < 3; ++i) e.add(1 << 20, 0.5);  // 16.8 Mb/s bursts for 1.5 s
    CHECK(e.estimate_bps() < 12000000);  // the slow average does not trust them yet
}

TEST("rung choice: hysteresis between going up and staying; limits always apply") {
    const std::vector<int64_t> bw = {500000, 1500000, 3000000, 6000000};
    CHECK_EQ(choose_rung(bw, 1, 0, 0), 1);                  // no estimate: stay
    CHECK_EQ(choose_rung(bw, 0, 10000000, 0), 3);           // 6 <= 0.7 * 10: top
    CHECK_EQ(choose_rung(bw, 1, 4000000, 0), 1);            // up needs 3 <= 2.8: no; 1.5 <= 3.4: stay
    CHECK_EQ(choose_rung(bw, 2, 3600000, 0), 2);            // 3 <= 0.85 * 3.6 = 3.06: stay
    CHECK_EQ(choose_rung(bw, 2, 3000000, 0), 1);            // 3 > 2.55: down to 1.5
    CHECK_EQ(choose_rung(bw, 3, 100000, 0), 0);             // nothing fits: the lowest
    CHECK_EQ(choose_rung(bw, 3, 0, 2000000), 1);            // a limit without an estimate
    CHECK_EQ(choose_rung(bw, 0, 100000000, 3000000), 2);    // a limit caps going up
    CHECK_EQ(choose_rung({}, 0, 1, 0), -1);
}

TEST("adaptive source: bitrate switches up on a fast link and down under a limit, seamlessly") {
    SKIP_UNLESS_READY();
    AdaptiveSourceOptions o = options();
    o.initial_bandwidth = 500000;  // starts on the 400 kb/s rung
    o.max_ahead_us = 4000000;      // two segments: the switches land soon after they are decided
    AdaptiveSource s(o);
    std::string err;
    REQUIRE(s.open(url("l.mpd"), &err));
    const Presentation &pres = s.presentation();
    auto height = [&](int r) { return pres.renditions[static_cast<size_t>(r)].height; };
    CHECK_EQ(s.info().tracks[0].height, 240);
    StreamFormat f;
    REQUIRE(s.select(0, 1, starfish_like(), &f, &err));  // adaptive_resolution set below
    Packet p;
    std::vector<Packet> video;
    int64_t last_dts = INT64_MIN;
    bool monotonic = true;
    int keys = 0;
    while (s.read(&p, &err) == MediaSource::Read::Packet) {
        if (p.type != TrackType::Video) continue;
        monotonic = monotonic && p.dts_us > last_dts;
        last_dts = p.dts_us;
        keys += p.key ? 1 : 0;
        video.push_back(p);
        if (video.size() == 300) s.set_bandwidth_limit(150000);  // halfway: only the lowest rung fits
    }
    CHECK_EQ(video.size(), size_t{500});  // every frame once: no gap or overlap at the switches
    CHECK(monotonic);
    CHECK_EQ(keys, 20);
    CHECK(s.switches() >= 2);
    CHECK_EQ(height(s.video_rendition()), 120);  // ended on the lowest rung
    CHECK(s.throughput_estimate() > 1500000);   // localhost is fast
}

TEST("adaptive source: a backend without adaptive resolution keeps the picture size") {
    SKIP_UNLESS_READY();
    AdaptiveSourceOptions o = options();
    o.initial_bandwidth = 500000;
    AdaptiveSource s(o);
    std::string err;
    REQUIRE(s.open(url("l.mpd"), &err));
    BackendCaps caps = starfish_like();
    caps.adaptive_resolution = false;
    StreamFormat f;
    REQUIRE(s.select(0, 1, caps, &f, &err));
    Packet p;
    size_t n = 0;
    while (s.read(&p, &err) == MediaSource::Read::Packet) n += p.type == TrackType::Video ? 1 : 0;
    CHECK_EQ(n, size_t{500});
    CHECK_EQ(s.switches(), 0);
    CHECK_EQ(s.presentation().renditions[static_cast<size_t>(s.video_rendition())].height, 240);
}

TEST("adaptive source: the video rendition follows codec preference, size and bandwidth limits") {
    Presentation p;
    std::string err;
    REQUIRE(parse_mpd(R"(<MPD type="static" mediaPresentationDuration="PT4S"><Period>
      <AdaptationSet contentType="video"><SegmentTemplate media="$Number$" duration="2"/>
        <Representation id="avc1080" codecs="avc1.640028" height="1080" bandwidth="5000000"/>
        <Representation id="hevc1080" codecs="hvc1.2.4.L120" height="1080" bandwidth="4000000"/>
        <Representation id="hevc2160" codecs="hvc1.2.4.L153" height="2160" bandwidth="15000000"/>
        <Representation id="av1" codecs="av01.0.12M.10" height="2160" bandwidth="9000000"/>
      </AdaptationSet>
      <AdaptationSet contentType="video"><SegmentTemplate media="$Number$" duration="2"/>
        <Representation id="dv" codecs="dvh1.05.06" height="2160" bandwidth="15000000"/>
      </AdaptationSet></Period></MPD>)", "http://h/m.mpd", &p, &err));
    AdaptiveSourceOptions o;
    auto id = [&](int i) { return i < 0 ? std::string("none") : p.renditions[static_cast<size_t>(i)].id; };
    CHECK_EQ(id(choose_video(p, o)), std::string("hevc2160"));
    o.max_height = 1080;
    CHECK_EQ(id(choose_video(p, o)), std::string("hevc1080"));
    o.video_codecs = {CodecId::H264, CodecId::Hevc};
    CHECK_EQ(id(choose_video(p, o)), std::string("avc1080"));
    o.max_bandwidth = 4500000;
    CHECK_EQ(id(choose_video(p, o)), std::string("hevc1080"));
    o = AdaptiveSourceOptions{};
    o.dolby_vision = true;
    CHECK_EQ(id(choose_video(p, o)), std::string("dv"));
    o.video_codecs = {CodecId::Av1};
    o.dolby_vision = false;
    CHECK_EQ(id(choose_video(p, o)), std::string("av1"));
    o.video_codecs = {CodecId::Vp9};
    CHECK_EQ(id(choose_video(p, o)), std::string("none"));

    // A Profile 5 ladder alone: only for a Dolby Vision display (no base layer anyone else can show).
    Presentation p5;
    REQUIRE(parse_mpd(R"(<MPD type="static" mediaPresentationDuration="PT4S"><Period>
      <AdaptationSet contentType="video"><SegmentTemplate media="$Number$" duration="2"/>
        <Representation id="p5" codecs="dvh1.05.06" height="2160" bandwidth="15000000"/>
      </AdaptationSet></Period></MPD>)", "http://h/m.mpd", &p5, &err));
    o = AdaptiveSourceOptions{};
    CHECK_EQ(choose_video(p5, o), -1);
    o.dolby_vision = true;
    CHECK_EQ(choose_video(p5, o), 0);
}
