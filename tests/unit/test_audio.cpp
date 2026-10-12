// SPDX-License-Identifier: GPL-3.0-or-later
//
// audio: WAV and Ogg decoding to 48 kHz stereo float, resampling, and the mixer (gain, pan, loops,
// streams, the voice limit, clamping, stopping from another thread).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/audio/audio.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace wnk::audio;

namespace {

void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void put32(std::vector<uint8_t> &v, uint32_t x) {
    put16(v, static_cast<uint16_t>(x));
    put16(v, static_cast<uint16_t>(x >> 16));
}

// A WAVE file with `frames` frames of the given encoding; sample(frame, channel) gives the value.
template <typename F>
std::vector<uint8_t> wav(uint16_t format, int channels, uint32_t rate, int bits, size_t frames, F sample) {
    std::vector<uint8_t> data;
    for (size_t f = 0; f < frames; ++f) {
        for (int c = 0; c < channels; ++c) {
            const double v = sample(f, c);
            if (format == 3) {
                float x = static_cast<float>(v);
                uint32_t u;
                std::memcpy(&u, &x, 4);
                put32(data, u);
            } else if (bits == 8) {
                data.push_back(static_cast<uint8_t>(std::lround(v * 127.0 + 128.0)));
            } else if (bits == 16) {
                put16(data, static_cast<uint16_t>(static_cast<int16_t>(std::lround(v * 32767.0))));
            } else if (bits == 24) {
                const auto x = static_cast<int32_t>(std::lround(v * 8388607.0));
                data.push_back(static_cast<uint8_t>(x));
                data.push_back(static_cast<uint8_t>(x >> 8));
                data.push_back(static_cast<uint8_t>(x >> 16));
            }
        }
    }
    std::vector<uint8_t> out = {'R', 'I', 'F', 'F'};
    put32(out, static_cast<uint32_t>(36 + data.size()));
    for (char c : std::string("WAVEfmt ")) out.push_back(static_cast<uint8_t>(c));
    put32(out, 16);
    put16(out, format);
    put16(out, static_cast<uint16_t>(channels));
    put32(out, rate);
    put32(out, rate * static_cast<uint32_t>(channels * bits / 8));
    put16(out, static_cast<uint16_t>(channels * bits / 8));
    put16(out, static_cast<uint16_t>(bits));
    for (char c : std::string("data")) out.push_back(static_cast<uint8_t>(c));
    put32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

bool near(float a, float b, float tol = 0.01f) { return std::fabs(a - b) <= tol; }

}  // namespace

TEST("wav: 8/16/24-bit and float, mono and stereo, decode to stereo float") {
    for (int bits : {8, 16, 24}) {
        auto file = wav(1, 1, kRate, bits, 100, [](size_t f, int) { return f == 10 ? 0.5 : -0.25; });
        std::string err;
        Sound s = decode_wav(file.data(), file.size(), &err);
        REQUIRE(s != nullptr);
        CHECK_EQ(s->frames(), size_t{100});
        CHECK(near(s->samples[20], 0.5f) && near(s->samples[21], 0.5f));  // mono on both sides
        CHECK(near(s->samples[0], -0.25f));
    }
    auto stereo = wav(3, 2, kRate, 32, 10, [](size_t, int c) { return c == 0 ? 0.75 : -0.75; });
    Sound s = decode_wav(stereo.data(), stereo.size());
    REQUIRE(s != nullptr);
    CHECK(near(s->samples[0], 0.75f) && near(s->samples[1], -0.75f));
    // Six channels: the first two are used.
    auto surround = wav(1, 6, kRate, 16, 10, [](size_t, int c) { return c * 0.1; });
    s = decode_wav(surround.data(), surround.size());
    REQUIRE(s != nullptr);
    CHECK(near(s->samples[0], 0.0f) && near(s->samples[1], 0.1f));
}

TEST("wav: other rates are resampled to 48 kHz; bad files are refused with a reason") {
    auto file = wav(1, 1, 24000, 16, 2400, [](size_t f, int) { return static_cast<double>(f) / 2400.0; });  // 0.1 s ramp
    Sound s = decode_wav(file.data(), file.size());
    REQUIRE(s != nullptr);
    CHECK(s->frames() >= 4790 && s->frames() <= 4800);
    CHECK(near(s->samples[2 * 2], 1.0f / 2400.0f, 0.001f));  // frame 2 at 48 kHz is frame 1 at 24 kHz
    CHECK(near(s->samples[2 * 3], 1.5f / 2400.0f, 0.001f));  // halfway between frames 1 and 2
    std::string err;
    const uint8_t junk[] = "RIFF\x04\x00\x00\x00WAVX";
    CHECK(decode_wav(junk, sizeof junk, &err) == nullptr);
    CHECK(!err.empty());
    auto adpcm = wav(2, 1, kRate, 16, 10, [](size_t, int) { return 0.0; });
    CHECK(decode_wav(adpcm.data(), adpcm.size(), &err) == nullptr);
    CHECK(err.find("unsupported") != std::string::npos);
    CHECK(load_sound("/nonexistent.wav", &err) == nullptr);
}

TEST("ogg: a Vorbis file decodes to a 48 kHz tone of the right length") {
    char dir_tmpl[] = "/tmp/wnk-audio-XXXXXX";
    const std::string dir = mkdtemp(dir_tmpl);
    const std::string path = dir + "/a.ogg";
    const std::string cmd = "ffmpeg -loglevel error -y -f lavfi -i sine=frequency=440:duration=0.5:sample_rate=44100 -ac 2 "
                            "-c:a libvorbis '" + path + "' 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) {
        std::printf("  SKIP (no ffmpeg with libvorbis)\n");
        std::system(("rm -rf '" + dir + "'").c_str());
        return;
    }
    std::string err;
    Sound s = load_sound(path, &err);
    REQUIRE(s != nullptr);
    CHECK(std::fabs(s->seconds() - 0.5) < 0.03);
    float peak = 0;
    for (float v : s->samples) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.05f);
    std::system(("rm -rf '" + dir + "'").c_str());
}

TEST("mixer: voices add with gain and pan, end on their own, loops repeat") {
    auto one = std::make_shared<Pcm>();
    one->samples.assign(8, 0.25f);  // 4 frames of 0.25 on both sides
    Mixer m;
    const VoiceId a = m.play(one);
    const VoiceId b = m.play(one, {0.5f, 1.0f, false});  // half gain, hard right
    CHECK(a != kNoVoice && b != kNoVoice && a != b);
    std::vector<float> out(12);
    m.mix(out.data(), 6);
    CHECK(near(out[0], 0.25f));           // left: a only
    CHECK(near(out[1], 0.25f + 0.125f));  // right: a + b
    CHECK(near(out[8], 0.0f));            // frame 4: both ended
    CHECK_EQ(m.active(), size_t{0});
    const VoiceId loop = m.play(one, {1.0f, 0.0f, true});
    m.mix(out.data(), 6);
    CHECK(near(out[10], 0.25f));  // frame 5 is frame 1 of the second pass
    CHECK(m.playing(loop));
    m.stop(loop);
    CHECK(!m.playing(loop));
    CHECK(m.play(nullptr) == kNoVoice);
}

TEST("mixer: streams, the voice limit, master gain and clamping") {
    Mixer m;
    int calls = 0;
    m.play_stream([&calls](float *out, size_t frames) {
        ++calls;
        for (size_t i = 0; i < frames * 2; ++i) out[i] = 0.9f;
        return calls < 2 ? frames : frames / 2;  // the second call ends the stream
    });
    auto loud = tone(1000, 0.1f, 0.9f);
    m.play(loud);
    std::vector<float> out(64 * 2);
    m.mix(out.data(), 64);
    float peak = 0;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    CHECK(near(peak, 1.0f));  // 0.9 + up to 0.9, clamped
    m.mix(out.data(), 64);
    CHECK(near(out[100], 0.0f, 0.95f));
    CHECK_EQ(m.active(), size_t{1});  // the stream ended, the tone goes on
    m.set_master_gain(0);
    m.mix(out.data(), 64);
    CHECK(near(out[10], 0.0f));
    m.stop_all();
    m.set_max_voices(2);
    CHECK(m.play(loud) != kNoVoice);
    CHECK(m.play(loud) != kNoVoice);
    CHECK(m.play(loud) == kNoVoice);
}

TEST("tone: frequency, length and silent edges") {
    Sound t = tone(480, 0.25f, 0.5f);
    CHECK_EQ(t->frames(), size_t{12000});
    CHECK(near(t->samples[0], 0.0f));
    CHECK(near(t->samples.back(), 0.0f, 0.01f));
    int crossings = 0;
    for (size_t f = 1; f < t->frames(); ++f) {
        if ((t->samples[(f - 1) * 2] < 0) != (t->samples[f * 2] < 0)) ++crossings;
    }
    CHECK(crossings >= 238 && crossings <= 242);  // 480 Hz for 0.25 s: 120 periods, two crossings each
}

TEST("mixer: stopping from another thread while mixing is safe") {
    Mixer m;
    auto t = tone(440, 1.0f);
    std::thread player([&m, &t] {
        for (int i = 0; i < 200; ++i) {
            const VoiceId v = m.play(t);
            m.stop(v);
        }
    });
    std::vector<float> out(256 * 2);
    for (int i = 0; i < 200; ++i) m.mix(out.data(), 256);
    player.join();
    CHECK(m.active() <= 1u);
}
