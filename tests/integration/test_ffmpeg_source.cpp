// SPDX-License-Identifier: GPL-3.0-or-later
//
// FfmpegSource against real files, generated at test time by the system ffmpeg CLI (test signal
// video + sine audio). The suite reports SKIP if the CLI or one of its encoders is missing.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/ffmpeg_source.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

using namespace wnk::media;

namespace {

std::string dir() {
    static std::string d = [] {
        std::string base = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/wnk-ff-" +
                           std::to_string(getpid());
        std::system(("mkdir -p '" + base + "'").c_str());
        return base;
    }();
    return d;
}

// 2 s, 25 fps, keyframe every second.
bool make_clip(const std::string &name, const std::string &vcodec, const std::string &acodec, const std::string &extra = "") {
    const std::string out = dir() + "/" + name;
    if (access(out.c_str(), R_OK) == 0) return true;
    const std::string cmd = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x240:rate=25:duration=2 "
                            "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=2 " + extra +
                            " -c:v " + vcodec + " -g 25 -keyint_min 25 -c:a " + acodec + " '" + out + "'";
    return std::system(cmd.c_str()) == 0;
}

bool have_ffmpeg() {
    static const bool ok = std::system("ffmpeg -hide_banner -version >/dev/null 2>&1") == 0;
    return ok;
}

BackendCaps ndl_like_caps() {
    BackendCaps c;
    c.video_codecs = {CodecId::H264, CodecId::Hevc, CodecId::Vp9, CodecId::Av1};
    c.audio_codecs = {CodecId::PcmS16, CodecId::Mp3, CodecId::Opus};  // NDL v2's list
    return c;
}

bool starts_with_start_code(const std::vector<uint8_t> &d) {
    return d.size() > 4 && ((d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) || (d[0] == 0 && d[1] == 0 && d[2] == 1));
}

// NAL unit types present in an Annex-B access unit.
std::vector<int> nal_types(const std::vector<uint8_t> &d, bool hevc) {
    std::vector<int> types;
    for (size_t i = 0; i + 3 < d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            const uint8_t h = d[i + 3];
            types.push_back(hevc ? (h >> 1) & 0x3F : h & 0x1F);
            i += 2;
        }
    }
    return types;
}

bool contains(const std::vector<int> &v, int x) {
    for (int i : v) {
        if (i == x) return true;
    }
    return false;
}

struct Drained {
    std::vector<Packet> video, audio;
};

Drained drain(FfmpegSource &s) {
    Drained d;
    Packet p;
    std::string err;
    while (s.read(&p, &err) == MediaSource::Read::Packet) {
        (p.type == TrackType::Video ? d.video : d.audio).push_back(p);
    }
    return d;
}

#define SKIP_WITHOUT_FFMPEG()                                  \
    if (!have_ffmpeg()) {                                      \
        std::printf("  SKIP: ffmpeg CLI not available\n");     \
        return;                                                \
    }

}  // namespace

TEST("ffmpeg source: library version is the bundled build") {
    const std::string v = FfmpegSource::version();
    std::printf("  %s\n", v.c_str());
    CHECK(v.find("avformat 63.") != std::string::npos);
}

TEST("ffmpeg source: MP4 H.264 + AAC → Annex-B video with SPS/PPS at keyframes, AAC decoded to PCM") {
    SKIP_WITHOUT_FFMPEG();
    REQUIRE(make_clip("h264_aac.mp4", "libx264", "aac"));
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open("file://" + dir() + "/h264_aac.mp4", &err));
    const MediaInfo &info = s.info();
    CHECK(info.container.find("mp4") != std::string::npos);
    CHECK(std::abs(info.duration_us - 2000000) < 100000);
    CHECK(info.seekable);
    const TrackInfo *v = info.best(TrackType::Video);
    const TrackInfo *a = info.best(TrackType::Audio);
    REQUIRE(v != nullptr && a != nullptr);
    CHECK(v->codec == CodecId::H264);
    CHECK_EQ(v->width, 320);
    CHECK(a->codec == CodecId::Aac);
    StreamFormat f;
    REQUIRE(s.select(v->index, a->index, ndl_like_caps(), &f, &err));
    REQUIRE(f.audio.has_value());
    CHECK(f.audio->codec == CodecId::PcmS16);
    CHECK_EQ(f.audio->channels, 2);
    CHECK_EQ(f.audio->sample_rate, 48000);
    const Drained d = drain(s);
    CHECK_EQ(d.video.size(), size_t{50});
    REQUIRE(!d.video.empty());
    CHECK(d.video[0].key);
    CHECK(starts_with_start_code(d.video[0].data));
    const auto types = nal_types(d.video[0].data, false);
    CHECK(contains(types, 7) && contains(types, 8) && contains(types, 5));  // SPS, PPS, IDR
    CHECK(d.video[25].key);
    CHECK(contains(nal_types(d.video[25].data, false), 7));  // parameter sets repeated at the next keyframe
    CHECK(d.video[0].pts_us == 0);
    // ~2 s of 48 kHz stereo S16 (AAC adds a little padding).
    size_t pcm = 0;
    int64_t last = -1;
    bool monotonic = true;
    for (const auto &p : d.audio) {
        pcm += p.data.size();
        if (p.pts_us != kNoTime) {
            monotonic = monotonic && p.pts_us > last;
            last = p.pts_us;
        }
    }
    CHECK(monotonic);
    CHECK(pcm % 4 == 0);
    CHECK(pcm >= size_t{48000 * 4 * 19 / 10} && pcm <= size_t{48000 * 4 * 22 / 10});
}

TEST("ffmpeg source: MKV HEVC + AC-3 5.1 → VPS/SPS/PPS before keyframes, audio downmixed to stereo") {
    SKIP_WITHOUT_FFMPEG();
    if (!make_clip("hevc_ac3.mkv", "libx265 -x265-params log-level=error", "ac3", "-ac 6")) {
        std::printf("  SKIP: no libx265/ac3 encoder\n");
        return;
    }
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/hevc_ac3.mkv", &err));
    const TrackInfo *v = s.info().best(TrackType::Video);
    const TrackInfo *a = s.info().best(TrackType::Audio);
    REQUIRE(v != nullptr && a != nullptr);
    CHECK(v->codec == CodecId::Hevc);
    CHECK(a->codec == CodecId::Ac3);
    CHECK_EQ(a->channels, 6);
    StreamFormat f;
    REQUIRE(s.select(v->index, a->index, ndl_like_caps(), &f, &err));
    CHECK(f.audio && f.audio->codec == CodecId::PcmS16 && f.audio->channels == 2);
    const Drained d = drain(s);
    REQUIRE(!d.video.empty());
    const auto t = nal_types(d.video[0].data, true);
    CHECK(contains(t, 32) && contains(t, 33) && contains(t, 34));  // VPS, SPS, PPS
    REQUIRE(!d.audio.empty());
    // A 440 Hz sine on every channel must come out as a non-silent stereo signal.
    const auto *pcm = reinterpret_cast<const int16_t *>(d.audio[d.audio.size() / 2].data.data());
    int peak = 0;
    for (size_t i = 0; i < d.audio[d.audio.size() / 2].data.size() / 2; ++i) peak = std::max(peak, std::abs(int(pcm[i])));
    CHECK(peak > 1000);
}

TEST("ffmpeg source: MPEG-TS H.264 + MP3 → Annex-B passes through, MP3 passes through, time starts at 0") {
    SKIP_WITHOUT_FFMPEG();
    REQUIRE(make_clip("h264_mp3.ts", "libx264", "libmp3lame"));
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/h264_mp3.ts", &err));
    CHECK_EQ(s.info().container, std::string("mpegts"));
    const TrackInfo *v = s.info().best(TrackType::Video);
    const TrackInfo *a = s.info().best(TrackType::Audio);
    REQUIRE(v && a);
    StreamFormat f;
    REQUIRE(s.select(v->index, a->index, ndl_like_caps(), &f, &err));
    CHECK(f.audio && f.audio->codec == CodecId::Mp3);
    const Drained d = drain(s);
    REQUIRE(!d.video.empty() && !d.audio.empty());
    CHECK(starts_with_start_code(d.video[0].data));
    CHECK(d.video[0].pts_us >= 0 && d.video[0].pts_us < 100000);  // MPEG-TS start_time subtracted
    CHECK(d.audio[0].data.size() > 100);
}

TEST("ffmpeg source: WebM VP9 + Opus → Opus passes through with its header") {
    SKIP_WITHOUT_FFMPEG();
    if (!make_clip("vp9_opus.webm", "libvpx-vp9 -deadline realtime -cpu-used 8", "libopus")) {
        std::printf("  SKIP: no libvpx-vp9/libopus encoder\n");
        return;
    }
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/vp9_opus.webm", &err));
    const TrackInfo *v = s.info().best(TrackType::Video);
    const TrackInfo *a = s.info().best(TrackType::Audio);
    REQUIRE(v && a);
    CHECK(v->codec == CodecId::Vp9);
    StreamFormat f;
    REQUIRE(s.select(v->index, a->index, ndl_like_caps(), &f, &err));
    REQUIRE(f.audio.has_value());
    CHECK(f.audio->codec == CodecId::Opus);
    CHECK(f.audio->codec_header.size() >= 19);
    CHECK(std::string(f.audio->codec_header.begin(), f.audio->codec_header.begin() + 8) == "OpusHead");
}

TEST("ffmpeg source: seek lands on a keyframe at or before the target") {
    SKIP_WITHOUT_FFMPEG();
    REQUIRE(make_clip("h264_aac.mp4", "libx264", "aac"));
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/h264_aac.mp4", &err));
    StreamFormat f;
    REQUIRE(s.select(s.info().best(TrackType::Video)->index, s.info().best(TrackType::Audio)->index, ndl_like_caps(), &f, &err));
    REQUIRE(s.seek(1500000, &err));
    Packet p;
    do {
        REQUIRE(s.read(&p, &err) == MediaSource::Read::Packet);
    } while (p.type != TrackType::Video);
    CHECK(p.key);
    CHECK_EQ(p.pts_us, int64_t{1000000});
    REQUIRE(s.seek(0, &err));
    do {
        REQUIRE(s.read(&p, &err) == MediaSource::Read::Packet);
    } while (p.type != TrackType::Video);
    CHECK_EQ(p.pts_us, int64_t{0});
}

TEST("ffmpeg source: unsupported video codec and missing files are refused with a reason; interrupt stops reads") {
    SKIP_WITHOUT_FFMPEG();
    REQUIRE(make_clip("h264_aac.mp4", "libx264", "aac"));
    FfmpegSource s;
    std::string err;
    CHECK(!s.open(dir() + "/does-not-exist.mkv", &err));
    CHECK(!err.empty());
    CHECK(!s.open("http://example.com/a.mp4", &err));
    REQUIRE(s.open(dir() + "/h264_aac.mp4", &err));
    BackendCaps only_hevc;
    only_hevc.video_codecs = {CodecId::Hevc};
    StreamFormat f;
    CHECK(!s.select(s.info().best(TrackType::Video)->index, -1, only_hevc, &f, &err));
    CHECK(err.find("h264") != std::string::npos);
    REQUIRE(s.select(s.info().best(TrackType::Video)->index, -1, ndl_like_caps(), &f, &err));
    s.interrupt();
    Packet p;
    CHECK(s.read(&p, &err) == MediaSource::Read::Interrupted);
    s.reset_interrupt();
    CHECK(s.read(&p, &err) == MediaSource::Read::Packet);
}

TEST("stereo mix: 5.1 downmix keeps left/right apart, adds centre to both, drops LFE, never clips") {
    // Channel order FL FR FC LFE BL BR, one sample each, all at full scale except where noted.
    const float fl = 0.5f, fr = -0.5f, fc = 0.4f, lfe = 1.0f, bl = 0.2f, br = -0.2f;
    const float *planes[6] = {&fl, &fr, &fc, &lfe, &bl, &br};
    const int order[6] = {0, 1, 2, 3, 4, 5};
    int16_t out[2];
    mix_to_stereo_s16(planes, 6, 1, order, out);
    CHECK(out[0] > 0 && out[1] < 0);  // left stays positive, right negative
    const float ones[6] = {1, 1, 1, 1, 1, 1};
    const float *full[6] = {&ones[0], &ones[1], &ones[2], &ones[3], &ones[4], &ones[5]};
    mix_to_stereo_s16(full, 6, 1, order, out);
    CHECK(out[0] <= 32767 && out[0] > 32000);  // normalised, not clipped
    const float mono = 0.25f;
    const float *m[1] = {&mono};
    mix_to_stereo_s16(m, 1, 1, nullptr, out);
    CHECK_EQ(out[0], out[1]);
}
