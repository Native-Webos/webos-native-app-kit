// SPDX-License-Identifier: GPL-3.0-or-later
//
// AdaptiveSource against DASH and HLS presentations written by the system ffmpeg CLI into the
// directory tests/integration/http_server.py serves (WNK_TEST_FILES_DIR at WNK_TEST_HTTP_BASE/files/).
// 10 s of 25 fps H.264 + AAC, keyframe every second, 2 s segments. SKIPs without the CLI or server.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/adaptive_source.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <atomic>
#include <chrono>
#include <ctime>
#include <map>
#include <thread>
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

// ISO BMFF boxes for a TTML-in-MP4 (stpp) track, which the ffmpeg CLI cannot fragment.
void put32(std::string *b, uint32_t v) {
    for (int sh = 24; sh >= 0; sh -= 8) *b += static_cast<char>(v >> sh);
}

std::string box(const char *type, const std::string &body) {
    std::string b;
    put32(&b, static_cast<uint32_t>(body.size() + 8));
    return b + type + body;
}

std::string stpp_init() {
    std::string mdhd(4, '\0');
    put32(&mdhd, 0), put32(&mdhd, 0), put32(&mdhd, 1000), put32(&mdhd, 0), put32(&mdhd, 0x55c40000);
    return box("ftyp", "iso6" + std::string(4, '\0')) + box("moov", box("trak", box("mdia", box("mdhd", mdhd))));
}

// One 2 s sample from `start_ms` holding `doc`.
std::string stpp_segment(uint32_t start_ms, const std::string &doc) {
    std::string tfhd(4, '\0'), tfdt(4, '\0'), trun = std::string("\0\0\x03\x01", 4);
    put32(&tfhd, 1);
    put32(&tfdt, start_ms);
    put32(&trun, 1);
    const std::string traf_head = box("tfhd", tfhd) + box("tfdt", tfdt);
    const uint32_t moof_size = 8 + 8 + static_cast<uint32_t>(traf_head.size()) + 8 + static_cast<uint32_t>(trun.size()) + 12;
    put32(&trun, moof_size + 8);  // data offset: past the moof and the mdat header
    put32(&trun, 2000);
    put32(&trun, static_cast<uint32_t>(doc.size()));
    return box("moof", box("traf", traf_head + box("trun", trun))) + box("mdat", doc);
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
        // An audio ladder: two AAC Representations (48 and 160 kb/s) in one AdaptationSet, 20 s.
        const std::string audio_ladder =
            "cd '" + d + "' && ffmpeg -hide_banner -loglevel error -y "
            "-f lavfi -i testsrc2=size=320x240:rate=25:duration=20 "
            "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=20 -map 0:v -map 1:a -map 1:a "
            "-c:v libx264 -g 25 -keyint_min 25 -sc_threshold 0 -b:v 300k -c:a aac -b:a:0 48k -b:a:1 160k "
            "-f dash -seg_duration 2 -adaptation_sets 'id=0,streams=v id=1,streams=a' "
            "-init_seg_name 'la-init-$RepresentationID$.m4s' -media_seg_name 'la-$RepresentationID$-$Number%05d$.m4s' la.mpd";
        if (std::system(audio_ladder.c_str()) != 0) return false;
        // HLS bitrate tiers: 160x120 with 48 kb/s audio (group_alo), 640x480 with 160 kb/s audio
        // (group_ahi), 20 s. ffmpeg names the two audio renditions differently ("audio_2", "audio_3").
        const std::string hls_tiers =
            "cd '" + d + "' && ffmpeg -hide_banner -loglevel error -y "
            "-f lavfi -i testsrc2=size=640x480:rate=25:duration=20 "
            "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=20 -map 0:v -map 0:v -map 1:a -map 1:a "
            "-c:v libx264 -g 25 -keyint_min 25 -sc_threshold 0 -c:a aac -s:v:0 160x120 -b:v:0 100k "
            "-s:v:1 640x480 -b:v:1 1500k -b:a:0 48k -b:a:1 160k -f hls -hls_time 2 -hls_playlist_type vod "
            "-hls_segment_type fmp4 -hls_fmp4_init_filename 'ha_init_%v.mp4' -hls_segment_filename 'ha_%v_%03d.m4s' "
            "-master_pl_name ha.m3u8 -var_stream_map 'v:0,agroup:alo v:1,agroup:ahi "
            "a:0,agroup:alo,language:en,name:alo a:1,agroup:ahi,language:en,name:ahi' 'ha_%v.m3u8'";
        if (std::system(hls_tiers.c_str()) != 0) return false;
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
        if (FILE *f = std::fopen((d + "/channels.m3u").c_str(), "w")) {
            std::fputs("#EXTM3U\n#EXTINF:-1,One\nhttp://h/one.m3u8\n", f);
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
        // TTML: "Fr N" in 2 s stpp segments (media time), "En N" in one file (ticks), at N.2-N.8 s.
        const std::string tt = "<?xml version=\"1.0\"?><tt xmlns=\"http://www.w3.org/ns/ttml\" "
                               "xmlns:ttp=\"http://www.w3.org/ns/ttml#parameter\" ttp:tickRate=\"10000\"><body><div>";
        if (FILE *f = std::fopen((d + "/fr-init.mp4").c_str(), "wb")) {
            const std::string b = stpp_init();
            std::fwrite(b.data(), 1, b.size(), f);
            std::fclose(f);
        }
        std::string en = tt;
        for (int seg = 0; seg < 5; ++seg) {
            std::string doc = tt;
            for (int i = seg * 2; i < seg * 2 + 2; ++i) {
                char line[160];
                std::snprintf(line, sizeof line, "<p begin=\"00:00:%02d.200\" end=\"00:00:%02d.800\">Fr <span>%d</span></p>", i, i, i);
                doc += line;
                std::snprintf(line, sizeof line, "<p begin=\"%dt\" end=\"%dt\">En<br/>%d</p>", i * 10000 + 2000, i * 10000 + 8000, i);
                en += line;
            }
            const std::string b = stpp_segment(static_cast<uint32_t>(seg) * 2000, doc + "</div></body></tt>");
            if (FILE *f = std::fopen((d + "/fr-" + std::to_string(seg + 1) + ".m4s").c_str(), "wb")) {
                std::fwrite(b.data(), 1, b.size(), f);
                std::fclose(f);
            }
        }
        if (FILE *f = std::fopen((d + "/en.ttml").c_str(), "w")) {
            std::fputs((en + "</div></body></tt>").c_str(), f);
            std::fclose(f);
        }
        // d.mpd with a German text/vtt AdaptationSet, French stpp segments, an English TTML file and
        // a format this build cannot read.
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
                        "codecs=\"stpp.ttml.im1t\" lang=\"fr\"><SegmentTemplate timescale=\"1000\" duration=\"2000\" "
                        "startNumber=\"1\" initialization=\"fr-init.mp4\" media=\"fr-$Number$.m4s\"/>"
                        "<Representation id=\"fr\" bandwidth=\"100\"/></AdaptationSet>"
                        "<AdaptationSet id=\"11\" contentType=\"text\" mimeType=\"application/ttml+xml\" lang=\"en\">"
                        "<Representation id=\"en\" bandwidth=\"100\"><BaseURL>en.ttml</BaseURL></Representation>"
                        "</AdaptationSet><AdaptationSet id=\"12\" contentType=\"text\" mimeType=\"text/x-unknown\" "
                        "lang=\"es\"><Representation id=\"es\" bandwidth=\"100\"><BaseURL>es.txt</BaseURL>"
                        "</Representation></AdaptationSet>");
        if (FILE *out = std::fopen((d + "/ds.mpd").c_str(), "w")) {
            std::fputs(mpd.c_str(), out);
            std::fclose(out);
        }
        // d.mpd's segments as two Periods: 0-4 s (segments 1-2), then 4-10 s (3-5) with its own
        // presentationTimeOffset, as ad-inserted and chaptered VOD is published.
        auto period = [](const char *id, const char *start, const char *dur, int first, int pto_ms) {
            std::string out = std::string("<Period id=\"") + id + "\" start=\"" + start + "\" duration=\"" + dur + "\">";
            for (int rep = 0; rep < 2; ++rep) {
                const std::string r = std::to_string(rep);
                out += std::string("<AdaptationSet id=\"") + r + "\" contentType=\"" + (rep == 0 ? "video" : "audio") +
                       "\"><Representation id=\"" + r + "\" mimeType=\"" + (rep == 0 ? "video/mp4" : "audio/mp4") +
                       "\" codecs=\"" + (rep == 0 ? "avc1.640016" : "mp4a.40.2") + "\" bandwidth=\"" +
                       (rep == 0 ? "300000" : "128000") + "\"" + (rep == 0 ? " width=\"320\" height=\"240\"" : "") +
                       "><SegmentTemplate timescale=\"1000\" duration=\"2000\" startNumber=\"" + std::to_string(first) +
                       "\" presentationTimeOffset=\"" + std::to_string(pto_ms) + "\" initialization=\"d-init-" + r +
                       ".m4s\" media=\"d-" + r + "-$Number%05d$.m4s\"/></Representation></AdaptationSet>";
            }
            return out + "</Period>";
        };
        const std::string mp = "<?xml version=\"1.0\"?><MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
                               "mediaPresentationDuration=\"PT10S\">" +
                               period("a", "PT0S", "PT4S", 1, 0) + period("b", "PT4S", "PT6S", 3, 4000) + "</MPD>";
        if (FILE *out = std::fopen((d + "/mp.mpd").c_str(), "w")) {
            std::fputs(mp.c_str(), out);
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

TEST("adaptive source: missing and malformed manifests, interrupt") {
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
    {
        AdaptiveSource s(options());
        CHECK(!s.open(url("channels.m3u"), &err));  // a channel list is not a playlist
        CHECK(err.find("parse_iptv_playlist") != std::string::npos);
    }
    CHECK(AdaptiveSource::handles("https://h/a/b.MPD?x=1"));
    CHECK(AdaptiveSource::handles("http://h/master.m3u8"));
    CHECK(!AdaptiveSource::handles("https://h/a.mp4"));
    CHECK(!AdaptiveSource::handles("file:///a.mpd"));
    CHECK(AdaptiveSource::handles("http://h/live/x.m3u"));
    CHECK(AdaptiveSource::handles("http://h/play.php?id=7"));
    CHECK(AdaptiveSource::handles("http://h/get.php?username=u&password=p"));
    CHECK(AdaptiveSource::handles("http://h/live?id=7&e=.m3u8"));
    CHECK(AdaptiveSource::handles("http://h/s?output=M3U8"));
    CHECK(AdaptiveSource::handles("http://h/s?type=m3u8#t"));
    CHECK(!AdaptiveSource::handles("http://h/stream.php?id=7"));
    CHECK(!AdaptiveSource::handles("http://h/a.ts#type=m3u8"));
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

TEST("adaptive source: DASH with two Periods plays through the boundary and seeks into the second") {
    SKIP_UNLESS_READY();
    {
        AdaptiveSource s(options());
        std::string err;
        REQUIRE(s.open(url("mp.mpd"), &err));
        CHECK_EQ(s.info().duration_us, int64_t{10000000});
        CHECK_EQ(s.info().tracks.size(), size_t{2});  // the second Period adds no tracks
        StreamFormat f;
        REQUIRE(s.select(0, 1, starfish_like(), &f, &err));
        const Drained d = drain(s);
        CHECK_EQ(d.video.size(), size_t{250});
        CHECK(d.ordered);
        CHECK(d.timed);
        REQUIRE(!d.audio.empty());
        CHECK(d.audio.back().pts_us > 9800000);
        for (size_t i = 1; i < d.video.size(); ++i) {
            if (d.video[i].key) CHECK_EQ(d.video[i].pts_us % 1000000, int64_t{0});  // nothing repeated or lost
        }
        // Audio across the boundary: nothing twice (never back in time) and no real gap. ffmpeg's AAC
        // segments are a few frames shorter than the 2 s this MPD claims, so the second Period's first
        // segment holds audio from before 4 s that the clip drops: up to ~100 ms is the fixture's.
        int64_t worst = 0;
        bool forward = true;
        for (size_t i = 1; i < d.audio.size(); ++i) {
            const int64_t step = d.audio[i].pts_us - d.audio[i - 1].pts_us;
            forward = forward && step > 0;
            worst = std::max(worst, step);
        }
        CHECK(forward);
        CHECK(worst < 100000);
    }
    check_seek("mp.mpd", 7000000, 6000000);  // in the second Period
    check_seek("mp.mpd", 3000000, 2000000);  // back in the first
}

TEST("adaptive source: WebM DASH (VP9, Opus) indexed by its Cues") {
    SKIP_UNLESS_READY();
    const std::string d = env("WNK_TEST_FILES_DIR");
    const std::string make =
        "cd '" + d + "' && ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x240:rate=25:duration=10 "
        "-c:v libvpx-vp9 -b:v 300k -g 25 -keyint_min 25 -deadline realtime -cpu-used 8 -an -f webm -dash 1 "
        "-cluster_time_limit 2000 wv.webm && ffmpeg -hide_banner -loglevel error -y -f lavfi "
        "-i sine=frequency=440:sample_rate=48000:duration=10 -c:a libopus -b:a 64k -vn -f webm -dash 1 "
        "-cluster_time_limit 2000 wa.webm && ffmpeg -hide_banner -loglevel error -y -f webm_dash_manifest -i wv.webm "
        "-f webm_dash_manifest -i wa.webm -c copy -map 0 -map 1 -f webm_dash_manifest "
        "-adaptation_sets 'id=0,streams=0 id=1,streams=1' w.mpd";
    if (std::system(make.c_str()) != 0) {
        std::printf("  SKIP: the ffmpeg CLI has no libvpx-vp9 or libopus\n");
        return;
    }
    AdaptiveSourceOptions o = options();
    o.video_codecs = {CodecId::Vp9};
    AdaptiveSource s(o);
    std::string err;
    REQUIRE(s.open(url("w.mpd"), &err));
    REQUIRE(s.info().tracks.size() == 2);
    CHECK(s.info().tracks[0].codec == CodecId::Vp9);
    CHECK(s.info().tracks[1].codec == CodecId::Opus);
    BackendCaps caps = ndl_like();
    caps.video_codecs = {CodecId::Vp9};
    StreamFormat f;
    REQUIRE(s.select(0, 1, caps, &f, &err));
    REQUIRE(f.video && f.audio);
    CHECK(f.video->codec == CodecId::Vp9);
    CHECK(f.audio->codec == CodecId::PcmS16);
    const Drained all = drain(s);
    CHECK_EQ(all.video.size(), size_t{250});
    REQUIRE(!all.audio.empty());
    CHECK(all.audio.back().pts_us > 9800000);
    REQUIRE(s.seek(5000000, &err));
    const Drained after = drain(s);
    REQUIRE(!after.video.empty());
    CHECK(after.video[0].key);
    CHECK_EQ(after.video[0].pts_us, int64_t{5000000});  // ffmpeg starts a Cluster at every keyframe
}

TEST("adaptive source: DASH text/vtt, TTML file and stpp subtitles; unknown listed; a seek does not stop loading") {
    SKIP_UNLESS_READY();
    AdaptiveSource s(options());
    std::string err;
    REQUIRE(s.open(url("ds.mpd"), &err));
    const MediaInfo &info = s.info();
    REQUIRE(info.tracks.size() == 6);
    CHECK_EQ(info.tracks[2].language, std::string("de"));
    CHECK(info.tracks[2].subtitle == SubtitleKind::Text);
    CHECK(info.tracks[2].is_default);  // Role main
    CHECK_EQ(info.tracks[3].language, std::string("fr"));
    CHECK(info.tracks[3].subtitle == SubtitleKind::Text);
    CHECK_EQ(info.tracks[4].language, std::string("en"));
    CHECK(info.tracks[4].subtitle == SubtitleKind::Text);
    CHECK_EQ(info.tracks[5].language, std::string("es"));
    CHECK(info.tracks[5].subtitle == SubtitleKind::None);
    StreamFormat f;
    REQUIRE(s.select(0, 1, starfish_like(), &f, &err));
    s.interrupt();  // a seek's interrupt must not reach the subtitle loader
    REQUIRE(s.seek(4000000, &err));
    s.reset_interrupt();
    std::map<int, std::vector<std::shared_ptr<const SubtitleCue>>> cues;
    Packet p;
    while (s.read(&p, &err) == MediaSource::Read::Packet &&
           (cues[2].size() < 10 || cues[3].size() < 10 || cues[4].size() < 10)) {
        if (p.type == TrackType::Subtitle && p.subtitle) cues[p.subtitle->track].push_back(p.subtitle);
    }
    for (auto &[track, list] : cues) {
        std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a->start_us < b->start_us; });
    }
    REQUIRE(cues[2].size() == 10);
    CHECK_EQ(cues[2][3]->text, std::string("Cue 3"));  // tags removed
    CHECK_EQ(cues[2][3]->start_us, int64_t{3200000});
    CHECK_EQ(cues[2][3]->end_us, int64_t{3800000});
    REQUIRE(cues[3].size() == 10);
    CHECK_EQ(cues[3][7]->text, std::string("Fr 7"));
    CHECK_EQ(cues[3][7]->start_us, int64_t{7200000});
    CHECK_EQ(cues[3][7]->end_us, int64_t{7800000});
    REQUIRE(cues[4].size() == 10);
    CHECK_EQ(cues[4][9]->text, std::string("En\n9"));
    CHECK_EQ(cues[4][9]->start_us, int64_t{9200000});
    CHECK(cues[5].empty());
}

namespace {

void write_file(const std::string &path, const std::string &text) {
    const std::string tmp = path + ".tmp";
    if (FILE *f = std::fopen(tmp.c_str(), "w")) {
        std::fputs(text.c_str(), f);
        std::fclose(f);
        std::rename(tmp.c_str(), path.c_str());  // the server never sees half a playlist
    }
}

std::string iso_utc(std::chrono::system_clock::time_point t) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    const std::time_t sec = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&sec, &tm);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return buf;
}

struct LiveRun {
    size_t video = 0;
    bool ordered = true, timed = true;
    MediaSource::Read end = MediaSource::Read::Error;
    double seconds = 0;
    double max_gap = 0;  // longest wait between two video packets, seconds
};

LiveRun play_live(AdaptiveSource &s) {
    LiveRun run;
    std::string err;
    StreamFormat f;
    if (!s.select(0, 1, starfish_like(), &f, &err)) return run;
    const auto t0 = std::chrono::steady_clock::now();
    Packet p;
    int64_t last = INT64_MIN;
    auto prev = t0;
    while ((run.end = s.read(&p, &err)) == MediaSource::Read::Packet) {
        if (p.type != TrackType::Video) continue;
        const auto now = std::chrono::steady_clock::now();
        if (run.video > 0) run.max_gap = std::max(run.max_gap, std::chrono::duration<double>(now - prev).count());
        prev = now;
        ++run.video;
        run.timed = run.timed && p.pts_us != kNoTime;
        run.ordered = run.ordered && p.dts_us > last;
        last = p.dts_us;
    }
    if (run.end != MediaSource::Read::EndOfStream) std::printf("  live read ended: %s\n", err.c_str());
    run.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return run;
}

}  // namespace

TEST("adaptive source live: HLS sliding window followed by sequence number until EXT-X-ENDLIST") {
    SKIP_UNLESS_READY();
    const std::string d = env("WNK_TEST_FILES_DIR");
    // The server: a three-segment window over ts_000..ts_004 that moves on every second.
    auto playlist = [&](int first, int count, bool end) {
        std::string m = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:" + std::to_string(first) + "\n";
        for (int i = first; i < first + count; ++i) {
            char line[64];
            std::snprintf(line, sizeof line, "#EXTINF:2.000000,\nts_%03d.ts\n", i);
            m += line;
        }
        if (end) m += "#EXT-X-ENDLIST\n";
        write_file(d + "/live.m3u8", m);
    };
    playlist(0, 3, false);
    std::atomic<bool> stop{false};
    std::thread server([&] {
        for (int first = 1; first <= 2 && !stop; ++first) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            playlist(first, 3, first == 2);  // 1..3, then 2..4 with ENDLIST
        }
    });
    AdaptiveSource s(options());
    std::string err;
    const bool opened = s.open(url("live.m3u8"), &err);
    LiveRun run;
    if (opened) {
        CHECK_EQ(s.info().duration_us, kNoTime);
        run = play_live(s);
    }
    stop = true;
    server.join();
    REQUIRE(opened);
    CHECK(run.end == MediaSource::Read::EndOfStream);
    CHECK_EQ(run.video, size_t{250});  // every segment once, though two fell out of the window
    CHECK(run.ordered && run.timed);
    CHECK(run.seconds > 1.0);  // it had to wait for the playlist to grow
}

TEST("adaptive source live: HLS WebVTT subtitles follow their growing playlist") {
    SKIP_UNLESS_READY();
    const std::string d = env("WNK_TEST_FILES_DIR");
    // s.m3u8's renditions (s_0_00N.ts, s_0N.vtt) published as a live event that grows every second.
    write_file(d + "/ls.m3u8", "#EXTM3U\n#EXT-X-VERSION:3\n"
                               "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"English\",DEFAULT=YES,LANGUAGE=\"en\",URI=\"ls_vtt.m3u8\"\n"
                               "#EXT-X-STREAM-INF:BANDWIDTH=75900,RESOLUTION=320x240,CODECS=\"avc1.64000d,mp4a.40.2\",SUBTITLES=\"subs\"\n"
                               "ls_v.m3u8\n");
    auto publish = [&](int count, bool end) {
        std::string v = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n", t = v;
        for (int i = 0; i < count; ++i) {
            char line[64];
            std::snprintf(line, sizeof line, "#EXTINF:2.000000,\ns_0_%03d.ts\n", i);
            v += line;
            std::snprintf(line, sizeof line, "#EXTINF:2.000000,\ns_0%d.vtt\n", i);
            t += line;
        }
        if (end) v += "#EXT-X-ENDLIST\n", t += "#EXT-X-ENDLIST\n";
        write_file(d + "/ls_v.m3u8", v);
        write_file(d + "/ls_vtt.m3u8", t);
    };
    publish(2, false);
    std::atomic<bool> stop{false};
    std::thread server([&] {
        for (int n = 3; n <= 5 && !stop; ++n) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            publish(n, n == 5);
        }
    });
    AdaptiveSource s(options());
    std::string err;
    const bool opened = s.open(url("ls.m3u8"), &err);
    std::vector<std::shared_ptr<const SubtitleCue>> cues;
    MediaSource::Read end = MediaSource::Read::Error;
    if (opened) {
        CHECK(s.presentation().live);
        CHECK(s.info().tracks[2].subtitle == SubtitleKind::Text);
        StreamFormat f;
        if (s.select(0, 1, starfish_like(), &f, &err)) {
            Packet p;
            while ((end = s.read(&p, &err)) == MediaSource::Read::Packet) {
                if (p.type == TrackType::Subtitle && p.subtitle) cues.push_back(p.subtitle);
            }
        }
    }
    stop = true;
    server.join();
    REQUIRE(opened);
    CHECK(end == MediaSource::Read::EndOfStream);
    std::sort(cues.begin(), cues.end(), [](const auto &a, const auto &b) { return a->start_us < b->start_us; });
    REQUIRE(cues.size() == 10);  // the last three files were listed only after playback had started
    CHECK_EQ(cues[9]->text, std::string("Cue 9"));
}

TEST("adaptive source live: dynamic DASH segments become available by the clock; static ends it") {
    SKIP_UNLESS_READY();
    const std::string d = env("WNK_TEST_FILES_DIR");
    const auto ast = std::chrono::system_clock::now() - std::chrono::milliseconds(4500);
    auto mpd = [&](bool dynamic) {
        const std::string tmpl = "<SegmentTemplate timescale=\"1000000\" duration=\"2000000\" initialization=\"d-init-$RepresentationID$.m4s\" "
                                 "media=\"d-$RepresentationID$-$Number%05d$.m4s\" startNumber=\"1\"/>";
        std::string m = "<?xml version=\"1.0\"?><MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" ";
        m += dynamic ? "type=\"dynamic\" availabilityStartTime=\"" + iso_utc(ast) + "\" minimumUpdatePeriod=\"PT1S\" timeShiftBufferDepth=\"PT30S\""
                     : "type=\"static\" mediaPresentationDuration=\"PT10S\"";
        m += "><Period id=\"0\" start=\"PT0S\"><AdaptationSet id=\"0\" contentType=\"video\">" + tmpl +
             "<Representation id=\"0\" mimeType=\"video/mp4\" codecs=\"avc1.64000d\" bandwidth=\"312164\" width=\"320\" height=\"240\"/>"
             "</AdaptationSet><AdaptationSet id=\"1\" contentType=\"audio\">" + tmpl +
             "<Representation id=\"1\" mimeType=\"audio/mp4\" codecs=\"mp4a.40.2\" bandwidth=\"69000\" audioSamplingRate=\"48000\"/>"
             "</AdaptationSet></Period></MPD>";
        write_file(d + "/dl.mpd", m);
    };
    mpd(true);
    std::atomic<bool> stop{false};
    std::thread server([&] {
        // All five segments have arrived 10 s after the availability start; then the event is over.
        while (!stop && std::chrono::system_clock::now() < ast + std::chrono::milliseconds(10600)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!stop) mpd(false);
    });
    AdaptiveSource s(options());
    std::string err;
    const bool opened = s.open(url("dl.mpd"), &err);
    LiveRun run;
    if (opened) {
        CHECK(s.presentation().live);
        CHECK_EQ(s.presentation().renditions[0].segments.size(), size_t{2});  // 0-2 s and 2-4 s by 4.5 s
        run = play_live(s);
    }
    stop = true;
    server.join();
    REQUIRE(opened);
    CHECK(run.end == MediaSource::Read::EndOfStream);
    CHECK_EQ(run.video, size_t{250});
    CHECK(run.ordered && run.timed);
    CHECK(run.seconds > 4.0);
}

TEST("adaptive source live: DASH segments are followed by the clock between MPD updates") {
    SKIP_UNLESS_READY();
    // minimumUpdatePeriod 8 s over 2 s segments (live services often publish 10-60 s): the MPD
    // describes the next segments already, so each must be read when it becomes available, not with
    // the next MPD. Waiting for the MPD starved playback for 8 s here (and the TV dropped every late frame).
    const std::string d = env("WNK_TEST_FILES_DIR");
    const auto ast = std::chrono::system_clock::now() - std::chrono::milliseconds(4500);
    auto mpd = [&](bool dynamic) {
        const std::string tmpl = "<SegmentTemplate timescale=\"1000000\" duration=\"2000000\" initialization=\"d-init-$RepresentationID$.m4s\" "
                                 "media=\"d-$RepresentationID$-$Number%05d$.m4s\" startNumber=\"1\"/>";
        std::string m = "<?xml version=\"1.0\"?><MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" ";
        m += dynamic ? "type=\"dynamic\" availabilityStartTime=\"" + iso_utc(ast) + "\" minimumUpdatePeriod=\"PT8S\" timeShiftBufferDepth=\"PT30S\""
                     : "type=\"static\" mediaPresentationDuration=\"PT10S\"";
        m += "><Period id=\"0\" start=\"PT0S\"><AdaptationSet id=\"0\" contentType=\"video\">" + tmpl +
             "<Representation id=\"0\" mimeType=\"video/mp4\" codecs=\"avc1.64000d\" bandwidth=\"312164\" width=\"320\" height=\"240\"/>"
             "</AdaptationSet><AdaptationSet id=\"1\" contentType=\"audio\">" + tmpl +
             "<Representation id=\"1\" mimeType=\"audio/mp4\" codecs=\"mp4a.40.2\" bandwidth=\"69000\" audioSamplingRate=\"48000\"/>"
             "</AdaptationSet></Period></MPD>";
        write_file(d + "/du.mpd", m);
    };
    mpd(true);
    std::atomic<bool> stop{false};
    std::thread server([&] {
        while (!stop && std::chrono::system_clock::now() < ast + std::chrono::milliseconds(10600)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!stop) mpd(false);  // the MPD fetched 8 s after opening ends the event
    });
    AdaptiveSource s(options());
    std::string err;
    const bool opened = s.open(url("du.mpd"), &err);
    LiveRun run;
    if (opened) {
        CHECK_EQ(s.presentation().renditions[0].segments.size(), size_t{2});
        run = play_live(s);
    }
    stop = true;
    server.join();
    REQUIRE(opened);
    CHECK(run.end == MediaSource::Read::EndOfStream);
    CHECK_EQ(run.video, size_t{250});
    CHECK(run.ordered && run.timed);
    std::printf("  longest wait between video packets %.2f s\n", run.max_gap);
    CHECK(run.max_gap < 3.5);  // a segment every 2 s, not one MPD's worth every 8 s
}

TEST("adaptive source: HLS behind Referer, Origin and a cookie the playlist sets") {
    SKIP_UNLESS_READY();
    // ts.m3u8 at /guard/: every request needs the site's Referer and Origin, every segment the
    // cookie set with the playlist (as ExoPlayer's Cronet source keeps it).
    auto run = [](bool cookies, bool headers, std::string *err) {
        net::CurlOptions co;
        co.cookies = cookies;
        AdaptiveSourceOptions o = options();
        o.http = std::shared_ptr<net::HttpTransport>(net::make_curl_transport(co));
        if (headers) o.headers = {{"Referer", "https://site.example/"}, {"Origin", "https://site.example"}};
        AdaptiveSource s(o);
        if (!s.open(env("WNK_TEST_HTTP_BASE") + "/guard/ts.m3u8", err)) return size_t{0};
        StreamFormat f;
        if (!s.select(0, s.info().best(TrackType::Audio) ? s.info().best(TrackType::Audio)->index : -1, starfish_like(),
                      &f, err)) {
            return size_t{0};
        }
        return drain(s).video.size();
    };
    std::string err;
    CHECK_EQ(run(true, true, &err), size_t{250});
    CHECK_EQ(run(false, true, &err), size_t{0});  // the segments are refused without the cookie
    CHECK(err.find("403") != std::string::npos);
    err.clear();
    CHECK_EQ(run(true, false, &err), size_t{0});  // and the playlist without the headers
    CHECK(err.find("403") != std::string::npos);
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

TEST("adaptive source: DASH audio switches bitrate within its AdaptationSet, beside the video, seamlessly") {
    SKIP_UNLESS_READY();
    AdaptiveSourceOptions o = options();
    o.max_ahead_us = 4000000;
    AdaptiveSource probe(o);
    std::string err;
    REQUIRE(probe.open(url("la.mpd"), &err));
    const Presentation &pp = probe.presentation();
    int64_t video_bw = 0;
    std::vector<int64_t> audio_bw;
    for (const Rendition &r : pp.renditions) {
        if (r.type == TrackType::Video) video_bw = r.bandwidth;
        if (r.type == TrackType::Audio) audio_bw.push_back(r.bandwidth);
    }
    REQUIRE(audio_bw.size() == 2);
    std::sort(audio_bw.begin(), audio_bw.end());
    // Room beside the video for the low rung only: it starts there, then the fast link takes it up.
    o.initial_bandwidth = video_bw + (audio_bw[0] + audio_bw[1]) / 2;
    AdaptiveSource s(o);
    REQUIRE(s.open(url("la.mpd"), &err));
    const Presentation &pres = s.presentation();
    REQUIRE(s.info().tracks.size() == 2);  // one audio track: the rungs are one choice
    StreamFormat f;
    REQUIRE(s.select(0, 1, starfish_like(), &f, &err));
    auto bw = [&](int r) { return pres.renditions[static_cast<size_t>(r)].bandwidth; };
    CHECK_EQ(bw(s.audio_rendition()), audio_bw[0]);
    Packet p;
    std::vector<int64_t> audio;
    size_t video = 0;
    bool limited = false;
    int64_t peak = 0;
    while (s.read(&p, &err) == MediaSource::Read::Packet) {
        if (p.type == TrackType::Video) ++video;
        if (p.type != TrackType::Audio) continue;
        audio.push_back(p.pts_us);
        peak = std::max(peak, bw(s.audio_rendition()));
        if (!limited && p.pts_us > 10000000) {
            limited = true;
            s.set_bandwidth_limit(video_bw + (audio_bw[0] + audio_bw[1]) / 2);  // the high rung no longer fits
        }
    }
    CHECK_EQ(video, size_t{500});
    CHECK_EQ(peak, audio_bw[1]);                   // went up
    CHECK_EQ(bw(s.audio_rendition()), audio_bw[0]);  // and came down under the limit
    CHECK(s.audio_switches() >= 2);
    CHECK_EQ(s.video_rendition() >= 0, true);
    // Every AAC frame once, in order: 20 s at 1024 samples / 48 kHz, no gap or overlap at a switch.
    CHECK(audio.size() >= 935 && audio.size() <= 940);
    bool even = true;
    for (size_t i = 1; i < audio.size(); ++i) {
        const int64_t step = audio[i] - audio[i - 1];
        even = even && step > 20000 && step < 22700;
    }
    CHECK(even);
}

TEST("adaptive source: seek previews from a DASH image AdaptationSet and from an HLS image playlist") {
    SKIP_UNLESS_READY();
    const std::string d = env("WNK_TEST_FILES_DIR");
    // l.mpd with a thumbnail_tile AdaptationSet: one 4x3 sheet for its 20 s.
    std::string mpd;
    {
        std::ifstream f(d + "/l.mpd");
        std::stringstream ss;
        ss << f.rdbuf();
        mpd = ss.str();
    }
    const size_t end = mpd.rfind("</Period>");
    REQUIRE(end != std::string::npos);
    mpd.insert(end, "<AdaptationSet id=\"9\" contentType=\"image\" mimeType=\"image/jpeg\">"
                    "<SegmentTemplate media=\"sheet-$Number$.jpg\" duration=\"20\" startNumber=\"1\"/>"
                    "<Representation id=\"t\" bandwidth=\"1000\" width=\"640\" height=\"270\">"
                    "<EssentialProperty schemeIdUri=\"http://dashif.org/thumbnail_tile\" value=\"4x3\"/>"
                    "</Representation></AdaptationSet>");
    write_file(d + "/lt.mpd", mpd);
    AdaptiveSource dash(options());
    std::string err;
    REQUIRE(dash.open(url("lt.mpd"), &err));
    ThumbnailTrack t;
    REQUIRE(dash.thumbnails(320, &t, {}, &err));
    CHECK_EQ(t.tile_width, 160);
    REQUIRE(t.sheets.size() == 1);
    CHECK_EQ(t.sheets[0].url, url("sheet-1.jpg"));
    auto tile = t.at(11000000);  // 20 s / 12 tiles: tile 6 → column 2, row 1
    REQUIRE(tile.has_value());
    CHECK_EQ(tile->x, 320);
    CHECK_EQ(tile->y, 90);
    // The image AdaptationSet is no track: one video, one audio.
    CHECK_EQ(dash.info().tracks.size(), size_t{2});

    // ts.m3u8 behind a master with an image playlist, which thumbnails() fetches.
    write_file(d + "/ti.m3u8", "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=320x240\nts.m3u8\n"
                               "#EXT-X-IMAGE-STREAM-INF:BANDWIDTH=2000,RESOLUTION=160x90,URI=\"tiles.m3u8\"\n");
    write_file(d + "/tiles.m3u8", "#EXTM3U\n#EXT-X-IMAGES-ONLY\n#EXTINF:10.0,\n#EXT-X-TILES:RESOLUTION=160x90,LAYOUT=2x2,DURATION=2.5\n"
                                  "tiles.jpg\n#EXT-X-ENDLIST\n");
    AdaptiveSource hls(options());
    REQUIRE(hls.open(url("ti.m3u8"), &err));
    REQUIRE(hls.thumbnails(320, &t, {}, &err));
    CHECK_EQ(t.id, url("tiles.m3u8"));
    auto h = t.at(8000000);  // tile 3 → column 1, row 1
    REQUIRE(h.has_value());
    CHECK_EQ(h->url, url("tiles.jpg"));
    CHECK_EQ(h->x, 160);
    CHECK_EQ(h->y, 90);
    // Without any: a reason.
    AdaptiveSource plain(options());
    REQUIRE(plain.open(url("d.mpd"), &err));
    CHECK(!plain.thumbnails(320, &t, {}, &err));
    CHECK(err.find("no thumbnails") != std::string::npos);
}

TEST("adaptive source: HLS audio follows the video's variant into its audio group, seamlessly") {
    SKIP_UNLESS_READY();
    AdaptiveSourceOptions o = options();
    o.initial_bandwidth = 3000000;  // starts on the high tier
    o.max_ahead_us = 4000000;
    AdaptiveSource s(o);
    std::string err;
    REQUIRE(s.open(url("ha.m3u8"), &err));
    const Presentation &pres = s.presentation();
    REQUIRE(s.info().tracks.size() == 2);  // one audio track: the tiers' copies are one choice
    auto rend = [&](int r) -> const Rendition & { return pres.renditions[static_cast<size_t>(r)]; };
    int64_t low = 0, high = 0;
    for (const Rendition &r : pres.renditions) {
        if (r.type != TrackType::Audio || r.group.empty()) continue;
        CHECK(r.bandwidth > 0);  // from the audio-only variant on the same playlist
        (r.group == "group_alo" ? low : high) = r.bandwidth;
    }
    REQUIRE(low > 0 && high > low);
    StreamFormat f;
    REQUIRE(s.select(0, 1, starfish_like(), &f, &err));
    CHECK_EQ(rend(s.video_rendition()).height, 480);
    CHECK_EQ(rend(s.audio_rendition()).group, std::string("group_ahi"));
    // Down under a limit at 2 s, up again when it is lifted at 8 s.
    Packet p;
    std::vector<std::pair<int64_t, bool>> video_high, audio_high;  // (pts, on the high tier)
    int limit = 0;
    while (s.read(&p, &err) == MediaSource::Read::Packet) {
        if (p.type == TrackType::Video) video_high.emplace_back(p.pts_us, rend(s.video_rendition()).height == 480);
        if (p.type != TrackType::Audio) continue;
        audio_high.emplace_back(p.pts_us, rend(s.audio_rendition()).group == "group_ahi");
        if (limit == 0 && p.pts_us > 2000000) {
            limit = 1;
            s.set_bandwidth_limit(500000);  // only the low tier fits
        } else if (limit == 1 && p.pts_us > 8000000) {
            limit = 2;
            s.set_bandwidth_limit(0);
        }
    }
    CHECK_EQ(video_high.size(), size_t{500});
    // The audio changes tier with the picture, at the video's switch, however far its own download
    // has got: a few AAC frames apart at most (the picture's tier is sampled per video frame).
    std::sort(video_high.begin(), video_high.end());
    int64_t apart_us = 0;
    bool low_seen = false;
    for (const auto &[pts, hi] : audio_high) {
        auto it = std::upper_bound(video_high.begin(), video_high.end(), std::make_pair(pts, true));
        if (it != video_high.begin() && std::prev(it)->second != hi) apart_us += 21333;
        low_seen = low_seen || !hi;
    }
    CHECK(low_seen);
    CHECK(apart_us <= 200000);
    CHECK(audio_high.back().second);  // back on the high tier, with the picture
    CHECK(video_high.back().second);
    CHECK(s.audio_switches() >= 2);
    CHECK(audio_high.size() >= 935 && audio_high.size() <= 940);
    bool even = true;
    for (size_t i = 1; i < audio_high.size(); ++i) {
        const int64_t step = audio_high[i].first - audio_high[i - 1].first;
        even = even && step > 20000 && step < 22700;
    }
    CHECK(even);
}

TEST("adaptive source live: a DASH bitrate switch lands on the current segment of the new rendition") {
    SKIP_UNLESS_READY();
    // The 20 s ladder as a live event already 12.5 s in. The renditions not playing must be followed
    // too: a switch to one whose list stayed as the MPD had it at open went back to its last listed
    // segment, and the picture jumped back in time (on the TV: seconds of dropped frames).
    const std::string d = env("WNK_TEST_FILES_DIR");
    const auto ast = std::chrono::system_clock::now() - std::chrono::milliseconds(12500);
    auto mpd = [&](bool dynamic) {
        const std::string tmpl = "<SegmentTemplate timescale=\"1000000\" duration=\"2000000\" initialization=\"l-init-$RepresentationID$.m4s\" "
                                 "media=\"l-$RepresentationID$-$Number%05d$.m4s\" startNumber=\"1\"/>";
        std::string m = "<?xml version=\"1.0\"?><MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" ";
        m += dynamic ? "type=\"dynamic\" availabilityStartTime=\"" + iso_utc(ast) + "\" minimumUpdatePeriod=\"PT4S\" timeShiftBufferDepth=\"PT30S\""
                     : "type=\"static\" mediaPresentationDuration=\"PT20S\"";
        m += "><Period id=\"0\" start=\"PT0S\"><AdaptationSet id=\"0\" contentType=\"video\">" + tmpl +
             "<Representation id=\"0\" mimeType=\"video/mp4\" codecs=\"avc1.64000c\" bandwidth=\"100000\" width=\"160\" height=\"120\"/>"
             "<Representation id=\"1\" mimeType=\"video/mp4\" codecs=\"avc1.64000d\" bandwidth=\"400000\" width=\"320\" height=\"240\"/>"
             "<Representation id=\"2\" mimeType=\"video/mp4\" codecs=\"avc1.64001e\" bandwidth=\"1500000\" width=\"640\" height=\"480\"/>"
             "</AdaptationSet><AdaptationSet id=\"1\" contentType=\"audio\">" + tmpl +
             "<Representation id=\"3\" mimeType=\"audio/mp4\" codecs=\"mp4a.40.2\" bandwidth=\"69000\" audioSamplingRate=\"48000\"/>"
             "</AdaptationSet></Period></MPD>";
        write_file(d + "/ll.mpd", m);
    };
    mpd(true);
    std::atomic<bool> stop{false};
    std::thread server([&] {
        // Static (all ten segments) before the MPD fetch 8 s after opening, which ends the event
        // before the clock lists an eleventh segment.
        while (!stop && std::chrono::system_clock::now() < ast + std::chrono::milliseconds(19000)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!stop) mpd(false);
    });
    AdaptiveSourceOptions o = options();
    o.initial_bandwidth = 2000000;  // starts on the top rung
    o.max_ahead_us = 4000000;
    AdaptiveSource s(o);
    std::string err;
    const bool opened = s.open(url("ll.mpd"), &err);
    std::vector<int64_t> pts;
    bool monotonic = true;
    int64_t last_dts = INT64_MIN;
    MediaSource::Read end = MediaSource::Read::Error;
    StreamFormat f;
    if (opened && s.select(0, 1, starfish_like(), &f, &err)) {
        const auto t0 = std::chrono::steady_clock::now();
        bool capped = false;
        Packet p;
        while ((end = s.read(&p, &err)) == MediaSource::Read::Packet) {
            if (p.type != TrackType::Video) continue;
            monotonic = monotonic && p.dts_us > last_dts;
            last_dts = p.dts_us;
            pts.push_back(p.pts_us);
            // 4 s after opening: the segments since then are ones the MPD did not list at open.
            if (!capped && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(4)) {
                s.set_bandwidth_limit(150000);
                capped = true;
            }
        }
    }
    stop = true;
    server.join();
    REQUIRE(opened);
    if (end != MediaSource::Read::EndOfStream) std::printf("  live read ended: %s\n", err.c_str());
    CHECK(end == MediaSource::Read::EndOfStream);
    CHECK(monotonic);  // no jump back in time at the switch
    std::sort(pts.begin(), pts.end());
    CHECK(std::adjacent_find(pts.begin(), pts.end()) == pts.end());  // every frame once
    REQUIRE(!pts.empty());
    CHECK(pts.back() - pts.front() == static_cast<int64_t>(pts.size() - 1) * 40000);  // and none missing
    CHECK(s.switches() >= 1);
    CHECK_EQ(s.presentation().renditions[static_cast<size_t>(s.video_rendition())].height, 120);
}

TEST("adaptive source live: the join point leaves room for the longest segments (6 s audio, 2 s video)") {
    SKIP_UNLESS_READY();
    // 20.5 s into the event: video (2 s) is listed to 20 s, audio (6 s) only to 18 s. Three video
    // segments back from the video's edge (14 s) is inside the audio's newest segment, which is
    // not published until 24 s: playback would stop for audio at once. Three of the longest
    // segments back from the earliest edge: 0 s. Only where playback starts is checked here.
    const std::string d = env("WNK_TEST_FILES_DIR");
    const auto ast = std::chrono::system_clock::now() - std::chrono::milliseconds(20500);
    auto tmpl = [](int64_t dur_us) {
        return "<SegmentTemplate timescale=\"1000000\" duration=\"" + std::to_string(dur_us) +
               "\" initialization=\"l-init-$RepresentationID$.m4s\" media=\"l-$RepresentationID$-$Number%05d$.m4s\" startNumber=\"1\"/>";
    };
    const std::string m = "<?xml version=\"1.0\"?><MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"dynamic\" availabilityStartTime=\"" +
                          iso_utc(ast) + "\" minimumUpdatePeriod=\"PT30S\" timeShiftBufferDepth=\"PT60S\"><Period id=\"0\" start=\"PT0S\">"
                          "<AdaptationSet id=\"0\" contentType=\"video\">" + tmpl(2000000) +
                          "<Representation id=\"1\" mimeType=\"video/mp4\" codecs=\"avc1.64000d\" bandwidth=\"400000\" width=\"320\" height=\"240\"/>"
                          "</AdaptationSet><AdaptationSet id=\"1\" contentType=\"audio\">" + tmpl(6000000) +
                          "<Representation id=\"3\" mimeType=\"audio/mp4\" codecs=\"mp4a.40.2\" bandwidth=\"69000\" audioSamplingRate=\"48000\"/>"
                          "</AdaptationSet></Period></MPD>";
    write_file(d + "/lj.mpd", m);
    AdaptiveSource s(options());
    std::string err;
    REQUIRE(s.open(url("lj.mpd"), &err));
    StreamFormat f;
    REQUIRE(s.select(0, -1, starfish_like(), &f, &err));  // video only: the audio files are 2 s long
    Packet p;
    int64_t first = kNoTime;
    while (first == kNoTime && s.read(&p, &err) == MediaSource::Read::Packet) {
        if (p.type == TrackType::Video) first = p.pts_us;
    }
    std::printf("  first video packet at %.2f s\n", first / 1e6);
    CHECK(first != kNoTime && first < 1000000);
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
