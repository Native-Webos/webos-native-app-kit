// SPDX-License-Identifier: GPL-3.0-or-later
//
// FfmpegSource against real files, generated at test time by the system ffmpeg CLI (test signal
// video + sine audio). The suite reports SKIP if the CLI or one of its encoders is missing. Run
// through tests/integration/http_server.py it also plays the clips over HTTP (local server only).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/ffmpeg_source.hpp"

#include <algorithm>
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
    c.audio_codecs = {CodecId::PcmS16};  // what the NDL backend takes (MP3/Opus passthrough unverified)
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

TEST("ffmpeg source: MPEG-TS H.264 + MP3 → Annex-B passes through, MP3 decoded to PCM (or passed through), time from 0") {
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
    CHECK(f.audio && f.audio->codec == CodecId::PcmS16 && f.audio->channels == 2);  // mono → stereo PCM
    const Drained d = drain(s);
    REQUIRE(!d.video.empty() && !d.audio.empty());
    CHECK(starts_with_start_code(d.video[0].data));
    CHECK(d.video[0].pts_us >= 0 && d.video[0].pts_us < 100000);  // MPEG-TS start_time subtracted
    CHECK(d.audio[0].data.size() > 100);
    BackendCaps mp3 = ndl_like_caps();
    mp3.audio_codecs.push_back(CodecId::Mp3);
    REQUIRE(s.select(v->index, a->index, mp3, &f, &err));
    CHECK(f.audio && f.audio->codec == CodecId::Mp3);  // passthrough when a backend takes MP3
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
    BackendCaps opus = ndl_like_caps();
    opus.audio_codecs.push_back(CodecId::Opus);
    REQUIRE(s.select(v->index, a->index, opus, &f, &err));
    REQUIRE(f.audio.has_value());
    CHECK(f.audio->codec == CodecId::Opus);
    CHECK(f.audio->codec_header.size() >= 19);
    CHECK(std::string(f.audio->codec_header.begin(), f.audio->codec_header.begin() + 8) == "OpusHead");
}

TEST("ffmpeg source: MKV AV1 → temporal units (delimiter first, sequence header on keyframes); PQ colour") {
    SKIP_WITHOUT_FFMPEG();
    if (!make_clip("av1_pq.mkv", "libsvtav1 -preset 12 -pix_fmt yuv420p10le", "libopus",
                   "-color_trc smpte2084 -color_primaries bt2020 -colorspace bt2020nc")) {
        std::printf("  SKIP: no libsvtav1/libopus encoder\n");
        return;
    }
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/av1_pq.mkv", &err));
    const TrackInfo *v = s.info().best(TrackType::Video);
    REQUIRE(v);
    CHECK(v->codec == CodecId::Av1);
    StreamFormat f;
    REQUIRE(s.select(v->index, -1, ndl_like_caps(), &f, &err));
    REQUIRE(f.video.has_value());
    CHECK_EQ(f.video->colour.transfer, 16);
    CHECK_EQ(f.video->colour.primaries, 9);
    CHECK_EQ(f.video->colour.matrix, 9);
    const Drained d = drain(s);
    REQUIRE(d.video.size() >= 40);
    int keys = 0;
    for (const Packet &p : d.video) {
        REQUIRE(p.data.size() > 4);
        CHECK(p.data[0] == 0x12 && p.data[1] == 0x00);  // OBU_TEMPORAL_DELIMITER, size 0
        if (p.key) {
            ++keys;
            CHECK(((p.data[2] >> 3) & 0x0F) == 1);  // then OBU_SEQUENCE_HEADER
        }
    }
    CHECK(keys >= 2);
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

TEST("ffmpeg source: MPEG-TS (no index) seeks back to the keyframe before the target") {
    SKIP_WITHOUT_FFMPEG();
    REQUIRE(make_clip("h264_mp3.ts", "libx264", "libmp3lame"));
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/h264_mp3.ts", &err));
    StreamFormat f;
    REQUIRE(s.select(s.info().best(TrackType::Video)->index, s.info().best(TrackType::Audio)->index, ndl_like_caps(), &f, &err));
    for (const int64_t target : {int64_t{1500000}, int64_t{1000000}, int64_t{600000}, int64_t{0}}) {
        REQUIRE(s.seek(target, &err));
        Packet p;
        do {
            REQUIRE(s.read(&p, &err) == MediaSource::Read::Packet);
        } while (p.type != TrackType::Video);
        // Keyframes every second; video starts a little after 0 here (the MP3 encoder delay).
        CHECK(p.key);
        CHECK(p.pts_us <= std::max<int64_t>(target, 100000));  // nothing before the first frame at 0.023 s
        CHECK(target - p.pts_us < 1000000);
        // The rest of the clip follows, through to the end.
        const Drained rest = drain(s);
        CHECK_EQ(rest.video.size(), static_cast<size_t>(p.pts_us > 500000 ? 24 : 49));
    }
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

TEST("ffmpeg source: AAC passed through to a backend that wants ADTS gets a header per frame") {
    SKIP_WITHOUT_FFMPEG();
    REQUIRE(make_clip("h264_aac.mp4", "libx264", "aac"));
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/h264_aac.mp4", &err));
    BackendCaps caps;
    caps.video_codecs = {CodecId::H264};
    caps.audio_codecs = {CodecId::Aac};
    caps.aac_adts = true;
    StreamFormat f;
    REQUIRE(s.select(s.info().best(TrackType::Video)->index, s.info().best(TrackType::Audio)->index, caps, &f, &err));
    REQUIRE(f.audio && f.audio->codec == CodecId::Aac);
    const Drained d = drain(s);
    REQUIRE(!d.audio.empty());
    for (const Packet &p : d.audio) {
        REQUIRE(p.data.size() > 7);
        CHECK(p.data[0] == 0xFF && p.data[1] == 0xF1);
        const size_t len = ((p.data[3] & 3u) << 11) | (static_cast<size_t>(p.data[4]) << 3) | (p.data[5] >> 5);
        CHECK_EQ(len, p.data.size());
    }
    CHECK_EQ((d.audio[0].data[2] >> 2) & 0xF, 3);  // 48 kHz
}

// ---- AC-3 re-encoding (a backend that takes AC-3 but no PCM: Starfish) -------------------------

namespace {

BackendCaps starfish_like_caps() {
    BackendCaps c;
    c.video_codecs = {CodecId::H264, CodecId::Hevc};
    c.audio_codecs = {CodecId::Aac, CodecId::Ac3, CodecId::Eac3};
    c.aac_adts = true;
    return c;
}
// What StarfishBackend::caps() says now: stereo PCM at libpf's rates as well.
BackendCaps starfish_pcm_caps() {
    BackendCaps c = starfish_like_caps();
    c.audio_codecs.push_back(CodecId::PcmS16);
    c.pcm_max_channels = 2;
    c.pcm_rates = {8000, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    return c;
}

struct Ac3Frame {
    int fscod = -1;  // 0: 48 kHz, 1: 44.1 kHz, 2: 32 kHz
    int acmod = -1;  // 1: mono, 2: stereo, 7: 3/2
};

bool parse_ac3(const std::vector<uint8_t> &d, Ac3Frame *f) {
    if (d.size() < 7 || d[0] != 0x0B || d[1] != 0x77) return false;
    f->fscod = d[4] >> 6;
    f->acmod = d[6] >> 5;
    return true;
}

// Writes the packets as a raw .ac3 file and has the ffmpeg CLI decode it: duration in seconds and
// peak level in dB (both NaN on failure).
std::pair<double, double> decode_with_cli(const std::vector<Packet> &audio, const std::string &name) {
    const std::string path = dir() + "/" + name;
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        for (const Packet &p : audio) std::fwrite(p.data.data(), 1, p.data.size(), f);
        std::fclose(f);
    }
    const std::string cmd = "ffmpeg -hide_banner -nostats -i '" + path + "' -af volumedetect -f null - 2>&1";
    FILE *pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) return {NAN, NAN};
    double duration = NAN, peak = NAN;
    char line[512];
    while (std::fgets(line, sizeof line, pipe) != nullptr) {
        const std::string l = line;
        if (const auto at = l.find("time="); at != std::string::npos) {
            int h = 0, m = 0;
            double sec = 0;
            if (std::sscanf(l.c_str() + at + 5, "%d:%d:%lf", &h, &m, &sec) == 3) duration = h * 3600 + m * 60 + sec;
        }
        if (const auto at = l.find("max_volume:"); at != std::string::npos) peak = std::atof(l.c_str() + at + 11);
    }
    pclose(pipe);
    return {duration, peak};
}

}  // namespace

TEST("audio path: as it is, else PCM, else re-encoded to AC-3, else none") {
    TrackInfo flac;
    flac.type = TrackType::Audio;
    flac.codec = CodecId::Unknown;
    flac.codec_label = "flac";
    CHECK(audio_path(starfish_like_caps(), flac) == AudioPath::None);
    flac.decodable = true;
    CHECK(audio_path(ndl_like_caps(), flac) == AudioPath::Pcm);
    CHECK(audio_path(starfish_like_caps(), flac) == AudioPath::None);
    flac.ac3_encodable = true;
    CHECK(audio_path(starfish_like_caps(), flac) == AudioPath::Ac3);
    CHECK(audio_path(ndl_like_caps(), flac) == AudioPath::Pcm);  // PCM is preferred
    TrackInfo aac = flac;
    aac.codec = CodecId::Aac;
    CHECK(audio_path(starfish_like_caps(), aac) == AudioPath::Passthrough);
}

TEST("ffmpeg source: 7.1 FLAC at 96 kHz → AC-3 5.1 at 48 kHz, 1536-sample frames, continuous time") {
    SKIP_WITHOUT_FFMPEG();
    if (!make_clip("h264_flac71.mkv", "libx264", "flac", "-ac 8 -ar 96000")) {
        std::printf("  SKIP: no flac encoder\n");
        return;
    }
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/h264_flac71.mkv", &err));
    const TrackInfo *v = s.info().best(TrackType::Video);
    const TrackInfo *a = s.info().best(TrackType::Audio);
    REQUIRE(v && a);
    CHECK(a->codec == CodecId::Unknown);
    CHECK_EQ(a->channels, 8);
    CHECK(a->decodable && a->ac3_encodable);
    StreamFormat f;
    REQUIRE(s.select(v->index, a->index, starfish_like_caps(), &f, &err));
    REQUIRE(f.audio.has_value());
    CHECK(f.audio->codec == CodecId::Ac3);
    CHECK_EQ(f.audio->channels, 6);
    CHECK_EQ(f.audio->sample_rate, 48000);
    const Drained d = drain(s);
    CHECK_EQ(d.video.size(), size_t{50});
    REQUIRE(d.audio.size() >= 60);  // 2 s / 32 ms
    for (size_t i = 0; i < d.audio.size(); ++i) {
        Ac3Frame fr;
        REQUIRE(parse_ac3(d.audio[i].data, &fr));
        CHECK_EQ(fr.fscod, 0);
        CHECK_EQ(fr.acmod, 7);
        CHECK_EQ(d.audio[i].data.size(), size_t{2560});  // 640 kb/s
        CHECK_EQ(d.audio[i].duration_us, 32000);
        if (i > 0) CHECK_EQ(d.audio[i].pts_us - d.audio[i - 1].pts_us, 32000);
    }
    CHECK(d.audio[0].pts_us >= 0 && d.audio[0].pts_us < 50000);
    const auto [duration, peak] = decode_with_cli(d.audio, "flac71.ac3");
    CHECK(std::abs(duration - 2.0) < 0.1);
    CHECK(peak > -30.0);  // the sine survives
}

TEST("ffmpeg source: mono MP3 and 44.1 kHz stereo 24-bit PCM keep their layout and rate in AC-3") {
    SKIP_WITHOUT_FFMPEG();
    REQUIRE(make_clip("h264_mp3.ts", "libx264", "libmp3lame"));
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/h264_mp3.ts", &err));
    StreamFormat f;
    REQUIRE(s.select(s.info().best(TrackType::Video)->index, s.info().best(TrackType::Audio)->index,
                     starfish_like_caps(), &f, &err));
    REQUIRE(f.audio && f.audio->codec == CodecId::Ac3);
    CHECK_EQ(f.audio->channels, 1);
    CHECK_EQ(f.audio->sample_rate, 48000);
    Drained d = drain(s);
    REQUIRE(!d.audio.empty());
    Ac3Frame fr;
    REQUIRE(parse_ac3(d.audio[0].data, &fr));
    CHECK_EQ(fr.acmod, 1);
    CHECK_EQ(d.audio[0].data.size(), size_t{768});  // 192 kb/s

    if (!make_clip("h264_pcm441.mkv", "libx264", "pcm_s24le", "-ac 2 -ar 44100")) {
        std::printf("  SKIP: no pcm_s24le encoder\n");
        return;
    }
    FfmpegSource p;
    REQUIRE(p.open(dir() + "/h264_pcm441.mkv", &err));
    REQUIRE(p.select(p.info().best(TrackType::Video)->index, p.info().best(TrackType::Audio)->index,
                     starfish_like_caps(), &f, &err));
    REQUIRE(f.audio && f.audio->codec == CodecId::Ac3);
    CHECK_EQ(f.audio->channels, 2);
    CHECK_EQ(f.audio->sample_rate, 44100);
    d = drain(p);
    REQUIRE(!d.audio.empty());
    REQUIRE(parse_ac3(d.audio[0].data, &fr));
    CHECK_EQ(fr.fscod, 1);
    CHECK_EQ(fr.acmod, 2);
    const auto [duration, peak] = decode_with_cli(d.audio, "pcm441.ac3");
    CHECK(std::abs(duration - 2.0) < 0.1);
    CHECK(peak > -30.0);
}

TEST("ffmpeg source: AC-3 re-encoding restarts its timeline at a seek") {
    SKIP_WITHOUT_FFMPEG();
    if (!make_clip("h264_flac71.mkv", "libx264", "flac", "-ac 8 -ar 96000")) {
        std::printf("  SKIP: no flac encoder\n");
        return;
    }
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/h264_flac71.mkv", &err));
    StreamFormat f;
    REQUIRE(s.select(s.info().best(TrackType::Video)->index, s.info().best(TrackType::Audio)->index,
                     starfish_like_caps(), &f, &err));
    Packet p;
    for (int i = 0; i < 20; ++i) REQUIRE(s.read(&p, &err) == MediaSource::Read::Packet);
    REQUIRE(s.seek(1000000, &err));
    const Drained d = drain(s);
    REQUIRE(!d.video.empty() && !d.audio.empty());
    CHECK(d.video[0].key);
    CHECK_EQ(d.video[0].pts_us, 1000000);
    // Audio resumes near the video, not where the encoder stood before the seek.
    CHECK(std::llabs(d.audio[0].pts_us - d.video[0].pts_us) < 100000);
    for (size_t i = 1; i < d.audio.size(); ++i) CHECK_EQ(d.audio[i].pts_us - d.audio[i - 1].pts_us, 32000);
}

// ---- subtitles ---------------------------------------------------------------------------------

namespace {

// Two SubRip cues (the second with two lines and an italic tag) muxed into a 2 s clip.
bool make_subtitled(const std::string &name, const std::string &scodec) {
    const std::string out = dir() + "/" + name;
    if (access(out.c_str(), R_OK) == 0) return true;
    const std::string srt = dir() + "/cues.srt";
    if (FILE *f = std::fopen(srt.c_str(), "w")) {
        std::fputs("1\n00:00:00,200 --> 00:00:00,900\nFirst cue\n\n"
                   "2\n00:00:01,000 --> 00:00:01,800\n<i>Second</i> cue\nline two\n\n", f);
        std::fclose(f);
    }
    if (!make_clip("h264_aac.mp4", "libx264", "aac")) return false;
    const std::string cmd = "ffmpeg -hide_banner -loglevel error -y -i '" + dir() + "/h264_aac.mp4' -i '" + srt +
                            "' -map 0 -map 1 -c:v copy -c:a copy -c:s " + scodec + " -metadata:s:s:0 language=eng '" +
                            out + "'";
    return std::system(cmd.c_str()) == 0;
}

std::vector<Packet> subtitle_packets(FfmpegSource &s) {
    std::vector<Packet> subs;
    Packet p;
    std::string err;
    while (s.read(&p, &err) == MediaSource::Read::Packet) {
        if (p.type == TrackType::Subtitle) subs.push_back(p);
    }
    return subs;
}

}  // namespace

TEST("ffmpeg source: text subtitle tracks (SRT and ASS in MKV, timed text in MP4) become plain cues") {
    SKIP_WITHOUT_FFMPEG();
    for (const auto &[name, codec] : {std::pair<std::string, std::string>{"subs_srt.mkv", "srt"},
                                      {"subs_ass.mkv", "ass"},
                                      {"subs_mov_text.mp4", "mov_text"}}) {
        std::printf("  %s\n", name.c_str());
        REQUIRE(make_subtitled(name, codec));
        FfmpegSource s;
        std::string err;
        REQUIRE(s.open(dir() + "/" + name, &err));
        const TrackInfo *t = s.info().best(TrackType::Subtitle);
        REQUIRE(t != nullptr);
        CHECK(t->subtitle == SubtitleKind::Text);
        CHECK_EQ(t->language, std::string("eng"));
        StreamFormat f;
        REQUIRE(s.select(s.info().best(TrackType::Video)->index, s.info().best(TrackType::Audio)->index,
                         ndl_like_caps(), &f, &err));
        const auto subs = subtitle_packets(s);
        std::vector<Packet> cues;  // MP4 also carries empty "gap" samples between cues
        for (const Packet &x : subs) {
            if (!x.subtitle->empty()) cues.push_back(x);
        }
        REQUIRE(cues.size() == 2u);
        const SubtitleCue &a = *cues[0].subtitle, &b = *cues[1].subtitle;
        CHECK_EQ(a.track, t->index);
        CHECK_EQ(a.text, std::string("First cue"));
        CHECK_EQ(b.text, std::string("Second cue\nline two"));
        // Content time starts at the earliest stream (AAC priming may shift everything a little).
        CHECK(std::abs(a.start_us - 200000) <= 30000);
        CHECK(std::abs((a.end_us - a.start_us) - 700000) <= 1000);
        CHECK(std::abs((b.start_us - a.start_us) - 800000) <= 1000);
        CHECK(std::abs((b.end_us - b.start_us) - 800000) <= 1000);
    }
}

TEST("ffmpeg source: subtitles read while a seek looks for its keyframe are delivered") {
    SKIP_WITHOUT_FFMPEG();
    REQUIRE(make_subtitled("subs_srt.mkv", "srt"));
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/subs_srt.mkv", &err));
    StreamFormat f;
    REQUIRE(s.select(s.info().best(TrackType::Video)->index, -1, ndl_like_caps(), &f, &err));
    REQUIRE(s.seek(1500000, &err));  // keyframe at 1.0 s, the second cue starts there
    const auto subs = subtitle_packets(s);
    REQUIRE(subs.size() == 1u);
    CHECK(subs[0].subtitle->text.find("Second") == 0);
}

// ---- over HTTP (needs http_server.py: WNK_TEST_HTTP_BASE and WNK_TEST_FILES_DIR) ------------------

namespace {

std::string env(const char *name) {
    const char *v = std::getenv(name);
    return v != nullptr ? v : "";
}

// Copies a generated clip into the served directory; returns its URL.
std::string serve_clip(const std::string &name) {
    std::system(("cp '" + dir() + "/" + name + "' '" + env("WNK_TEST_FILES_DIR") + "/'").c_str());
    return env("WNK_TEST_HTTP_BASE") + "/files/" + name;
}

FfmpegSourceOptions with_http() {
    FfmpegSourceOptions o;
    o.http = wnk::net::make_curl_transport();
    o.http_stream.buffer_bytes = 128u << 10;  // smaller than the clips, so reads cross refills
    o.http_stream.keep_behind_bytes = 32u << 10;
    return o;
}

#define SKIP_WITHOUT_SERVER()                                            \
    if (env("WNK_TEST_HTTP_BASE").empty() || env("WNK_TEST_FILES_DIR").empty()) { \
        std::printf("  SKIP: run through http_server.py\n");             \
        return;                                                          \
    }

}  // namespace

TEST("ffmpeg source over HTTP: same packets as the local file, for MP4 (index first or last), MKV and TS") {
    SKIP_WITHOUT_FFMPEG();
    SKIP_WITHOUT_SERVER();
    REQUIRE(make_clip("h264_aac.mp4", "libx264", "aac"));
    REQUIRE(make_clip("h264_aac_faststart.mp4", "libx264", "aac", "-movflags +faststart"));
    REQUIRE(make_clip("hevc_ac3.mkv", "libx265 -x265-params log-level=error", "ac3", "-ac 6"));
    REQUIRE(make_clip("h264_mp3.ts", "libx264", "libmp3lame"));
    for (const char *name : {"h264_aac.mp4", "h264_aac_faststart.mp4", "hevc_ac3.mkv", "h264_mp3.ts"}) {
        std::printf("  %s\n", name);
        FfmpegSource local, remote(with_http());
        std::string err;
        REQUIRE(local.open(dir() + "/" + name, &err));
        const std::string url = serve_clip(name);
        REQUIRE(remote.open(url + "?token=abc", &err));  // the query must not hide the extension
        CHECK(remote.info().seekable);
        CHECK_EQ(remote.info().container, local.info().container);
        CHECK(std::abs(remote.info().duration_us - local.info().duration_us) < 1000);
        StreamFormat lf, rf;
        const int v = local.info().best(TrackType::Video)->index, a = local.info().best(TrackType::Audio)->index;
        REQUIRE(local.select(v, a, ndl_like_caps(), &lf, &err));
        REQUIRE(remote.select(v, a, ndl_like_caps(), &rf, &err));
        Drained l = drain(local), r = drain(remote);
        CHECK_EQ(r.video.size(), l.video.size());
        CHECK_EQ(r.audio.size(), l.audio.size());
        bool same = r.video.size() == l.video.size();
        for (size_t i = 0; same && i < l.video.size(); ++i) {
            same = l.video[i].data == r.video[i].data && l.video[i].pts_us == r.video[i].pts_us;
        }
        CHECK(same);
        // Seek back into the middle: lands on the same keyframe as the local file.
        REQUIRE(local.seek(1200000, &err));
        REQUIRE(remote.seek(1200000, &err));
        Packet lp, rp;
        do {
            REQUIRE(local.read(&lp, &err) == MediaSource::Read::Packet);
        } while (lp.type != TrackType::Video);
        do {
            REQUIRE(remote.read(&rp, &err) == MediaSource::Read::Packet);
        } while (rp.type != TrackType::Video);
        CHECK(rp.key);
        CHECK_EQ(rp.pts_us, lp.pts_us);
    }
}

TEST("ffmpeg source over HTTP: missing resource reports the status; no transport; interrupt") {
    SKIP_WITHOUT_FFMPEG();
    SKIP_WITHOUT_SERVER();
    std::string err;
    FfmpegSource s(with_http());
    CHECK(!s.open(env("WNK_TEST_HTTP_BASE") + "/files/nope.mkv", &err));
    CHECK_EQ(err, std::string("HTTP 404"));
    FfmpegSource bare;
    CHECK(!bare.open(env("WNK_TEST_HTTP_BASE") + "/files/nope.mkv", &err));
    CHECK(err.find("no HTTP transport") != std::string::npos);

    REQUIRE(make_clip("h264_aac.mp4", "libx264", "aac"));
    const std::string url = serve_clip("h264_aac.mp4");
    s.interrupt();
    CHECK(!s.open(url, &err));
    CHECK_EQ(err, std::string("interrupted"));
    s.reset_interrupt();
    REQUIRE(s.open(url, &err));
    StreamFormat f;
    REQUIRE(s.select(s.info().best(TrackType::Video)->index, -1, ndl_like_caps(), &f, &err));
    Packet p;
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

TEST("ffmpeg source: stereo FLAC at 96 kHz -> stereo PCM at 48 kHz for Starfish, continuous, tone intact") {
    SKIP_WITHOUT_FFMPEG();
    if (!make_clip("h264_flac20_96k.mkv", "libx264", "flac", "-ac 2 -ar 96000")) {
        std::printf("  SKIP: no flac encoder\n");
        return;
    }
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/h264_flac20_96k.mkv", &err));
    const TrackInfo *v = s.info().best(TrackType::Video);
    const TrackInfo *a = s.info().best(TrackType::Audio);
    REQUIRE(v && a);
    StreamFormat f;
    REQUIRE(s.select(v->index, a->index, starfish_pcm_caps(), &f, &err));
    REQUIRE(f.audio.has_value());
    CHECK(f.audio->codec == CodecId::PcmS16);
    CHECK_EQ(f.audio->sample_rate, 48000);
    CHECK_EQ(f.audio->channels, 2);
    const Drained d = drain(s);
    REQUIRE(!d.audio.empty());
    int64_t samples = 0;
    double peak = 0;
    for (size_t i = 0; i < d.audio.size(); ++i) {
        const auto &p = d.audio[i];
        const size_t frames = p.data.size() / 4;
        samples += static_cast<int64_t>(frames);
        CHECK_EQ(p.duration_us, static_cast<int64_t>(frames) * 1000000 / 48000);
        if (i > 0) {
            // Each packet starts where the last one ended (within the integer rounding of a duration).
            CHECK(std::llabs(p.pts_us - (d.audio[i - 1].pts_us + d.audio[i - 1].duration_us)) <= 1000);
        }
        const auto *pcm = reinterpret_cast<const int16_t *>(p.data.data());
        for (size_t k = 0; k < frames * 2; ++k) peak = std::max(peak, std::abs(pcm[k] / 32768.0));
    }
    CHECK(std::llabs(samples - 96000) < 2400);  // 2 s at 48 kHz, within 50 ms
    CHECK(d.audio[0].pts_us >= 0 && d.audio[0].pts_us < 50000);
    CHECK(peak > 0.05);  // the sine survives the resampler

    // After a seek the resampler starts again from the new position.
    REQUIRE(s.seek(1000000, &err));
    Packet pk;
    int64_t first = kNoTime;
    while (first == kNoTime && s.read(&pk, &err) == MediaSource::Read::Packet) {
        if (pk.type == TrackType::Audio) first = pk.pts_us;
    }
    CHECK(first != kNoTime && std::llabs(first - 1000000) < 100000);
}

TEST("ffmpeg source: 7.1 still goes to AC-3 5.1 for Starfish's stereo PCM; 48 kHz stereo keeps its rate") {
    SKIP_WITHOUT_FFMPEG();
    if (!make_clip("h264_flac71.mkv", "libx264", "flac", "-ac 8 -ar 96000") ||
        !make_clip("h264_flac20_48k.mkv", "libx264", "flac", "-ac 2 -ar 48000")) {
        std::printf("  SKIP: no flac encoder\n");
        return;
    }
    FfmpegSource s;
    std::string err;
    REQUIRE(s.open(dir() + "/h264_flac71.mkv", &err));
    StreamFormat f;
    REQUIRE(s.select(s.info().best(TrackType::Video)->index, s.info().best(TrackType::Audio)->index, starfish_pcm_caps(), &f, &err));
    CHECK(f.audio->codec == CodecId::Ac3);
    CHECK_EQ(f.audio->channels, 6);
    FfmpegSource t;
    REQUIRE(t.open(dir() + "/h264_flac20_48k.mkv", &err));
    StreamFormat g;
    REQUIRE(t.select(t.info().best(TrackType::Video)->index, t.info().best(TrackType::Audio)->index, starfish_pcm_caps(), &g, &err));
    CHECK(g.audio->codec == CodecId::PcmS16);
    CHECK_EQ(g.audio->sample_rate, 48000);
    const Drained d = drain(t);
    int64_t samples = 0;
    for (const auto &p : d.audio) samples += static_cast<int64_t>(p.data.size() / 4);
    CHECK(std::llabs(samples - 96000) < 2400);
}
