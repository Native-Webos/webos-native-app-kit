// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/audio/audio.hpp"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"  // declarations only; the decoder is compiled from third_party as C

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wnk::audio {
namespace {

constexpr size_t kMaxFileBytes = 64u << 20;

uint32_t u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void fail(std::string *error, const std::string &why) {
    if (error != nullptr) *error = why;
}

}  // namespace

// ---- decoding -------------------------------------------------------------------------------------

Sound from_samples(const float *in, size_t frames, int rate, int channels) {
    auto pcm = std::make_shared<Pcm>();
    if (in == nullptr || frames == 0 || rate <= 0 || channels <= 0) return pcm;
    // First two channels (a mono source plays on both).
    auto sample = [&](size_t frame, int ch) {
        const int c = channels == 1 ? 0 : std::min(ch, channels - 1);
        return in[frame * static_cast<size_t>(channels) + static_cast<size_t>(c)];
    };
    if (rate == kRate) {
        pcm->samples.resize(frames * kChannels);
        for (size_t f = 0; f < frames; ++f) {
            pcm->samples[f * 2] = sample(f, 0);
            pcm->samples[f * 2 + 1] = sample(f, 1);
        }
        return pcm;
    }
    // Linear interpolation: fine for effects and music at common rates (44.1 kHz → 48 kHz).
    const double step = static_cast<double>(rate) / kRate;
    const auto out_frames = static_cast<size_t>(std::floor(static_cast<double>(frames - 1) / step)) + 1;
    pcm->samples.resize(out_frames * kChannels);
    for (size_t o = 0; o < out_frames; ++o) {
        const double src = static_cast<double>(o) * step;
        const auto i = static_cast<size_t>(src);
        const auto t = static_cast<float>(src - static_cast<double>(i));
        const size_t j = std::min(i + 1, frames - 1);
        for (int ch = 0; ch < 2; ++ch) {
            pcm->samples[o * 2 + static_cast<size_t>(ch)] = sample(i, ch) + (sample(j, ch) - sample(i, ch)) * t;
        }
    }
    return pcm;
}

Sound decode_wav(const uint8_t *data, size_t size, std::string *error) {
    if (data == nullptr || size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        fail(error, "not a RIFF WAVE file");
        return nullptr;
    }
    int format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = nullptr;
    size_t pcm_bytes = 0;
    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint32_t len = u32(data + pos + 4);
        const uint8_t *body = data + pos + 8;
        const size_t avail = std::min<size_t>(len, size - pos - 8);
        if (std::memcmp(data + pos, "fmt ", 4) == 0 && avail >= 16) {
            format = u16(body);
            channels = u16(body + 2);
            rate = u32(body + 4);
            bits = u16(body + 14);
            if (format == 0xFFFE && avail >= 26) format = u16(body + 24);  // WAVE_FORMAT_EXTENSIBLE sub-format
        } else if (std::memcmp(data + pos, "data", 4) == 0) {
            pcm = body;
            pcm_bytes = avail;
        }
        pos += 8 + static_cast<size_t>(len) + (len & 1);  // chunks are padded to even sizes
    }
    if (pcm == nullptr || channels < 1 || channels > 32 || rate < 1000 || rate > 384000) {
        fail(error, "WAVE file without a usable fmt or data chunk");
        return nullptr;
    }
    const bool is_float = format == 3 && bits == 32;
    if (!(format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !is_float) {
        fail(error, "unsupported WAVE encoding (format " + std::to_string(format) + ", " + std::to_string(bits) + " bits)");
        return nullptr;
    }
    const size_t bytes = static_cast<size_t>(bits / 8);
    const size_t frames = pcm_bytes / (bytes * static_cast<size_t>(channels));
    std::vector<float> f(frames * static_cast<size_t>(channels));
    for (size_t i = 0; i < f.size(); ++i) {
        const uint8_t *p = pcm + i * bytes;
        if (is_float) {
            float v;
            std::memcpy(&v, p, 4);
            f[i] = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
        } else if (bits == 8) {
            f[i] = (static_cast<float>(p[0]) - 128.0f) / 128.0f;
        } else if (bits == 16) {
            f[i] = static_cast<float>(static_cast<int16_t>(u16(p))) / 32768.0f;
        } else if (bits == 24) {
            const int32_t v = static_cast<int32_t>((p[0] << 8) | (p[1] << 16) | (static_cast<uint32_t>(p[2]) << 24)) >> 8;
            f[i] = static_cast<float>(v) / 8388608.0f;
        } else {
            f[i] = static_cast<float>(static_cast<int32_t>(u32(p))) / 2147483648.0f;
        }
    }
    return from_samples(f.data(), frames, static_cast<int>(rate), channels);
}

Sound decode_ogg(const uint8_t *data, size_t size, std::string *error) {
    if (data == nullptr || size < 4 || size > kMaxFileBytes || std::memcmp(data, "OggS", 4) != 0) {
        fail(error, "not an Ogg file");
        return nullptr;
    }
    int channels = 0, rate = 0;
    short *out = nullptr;
    const int frames = stb_vorbis_decode_memory(data, static_cast<int>(size), &channels, &rate, &out);
    if (frames <= 0 || out == nullptr || channels <= 0) {
        std::free(out);
        fail(error, "Ogg Vorbis decoding failed");
        return nullptr;
    }
    std::vector<float> f(static_cast<size_t>(frames) * static_cast<size_t>(channels));
    for (size_t i = 0; i < f.size(); ++i) f[i] = static_cast<float>(out[i]) / 32768.0f;
    std::free(out);
    return from_samples(f.data(), static_cast<size_t>(frames), rate, channels);
}

Sound load_sound(const std::string &path, std::string *error) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        fail(error, "cannot open " + path);
        return nullptr;
    }
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, file)) > 0) {
        bytes.insert(bytes.end(), buf, buf + n);
        if (bytes.size() > kMaxFileBytes) {
            std::fclose(file);
            fail(error, path + " is larger than 64 MiB");
            return nullptr;
        }
    }
    std::fclose(file);
    std::string why;
    Sound s;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "OggS", 4) == 0) {
        s = decode_ogg(bytes.data(), bytes.size(), &why);
    } else {
        s = decode_wav(bytes.data(), bytes.size(), &why);
    }
    if (!s) fail(error, path + ": " + why);
    return s;
}

Sound tone(float frequency_hz, float seconds, float gain) {
    auto pcm = std::make_shared<Pcm>();
    const auto frames = static_cast<size_t>(std::max(0.0f, seconds) * kRate);
    pcm->samples.resize(frames * kChannels);
    const size_t fade = std::min<size_t>(frames / 2, kRate / 200);  // 5 ms
    for (size_t f = 0; f < frames; ++f) {
        float env = 1;
        if (fade > 0 && f < fade) env = static_cast<float>(f) / static_cast<float>(fade);
        if (fade > 0 && frames - f <= fade) env = std::min(env, static_cast<float>(frames - f - 1) / static_cast<float>(fade));
        const float v = gain * env * std::sin(2.0f * 3.14159265f * frequency_hz * static_cast<float>(f) / kRate);
        pcm->samples[f * 2] = pcm->samples[f * 2 + 1] = v;
    }
    return pcm;
}

// ---- mixer ----------------------------------------------------------------------------------------

VoiceId Mixer::play(Sound sound, PlayOptions options) {
    if (!sound || sound->frames() == 0) return kNoVoice;
    std::lock_guard<std::mutex> lock(mutex_);
    if (voices_.size() >= max_voices_) return kNoVoice;
    const VoiceId id = next_++;
    voices_.push_back({id, std::move(sound), {}, 0, options});
    return id;
}

VoiceId Mixer::play_stream(StreamFn fn, PlayOptions options) {
    if (!fn) return kNoVoice;
    std::lock_guard<std::mutex> lock(mutex_);
    if (voices_.size() >= max_voices_) return kNoVoice;
    const VoiceId id = next_++;
    voices_.push_back({id, nullptr, std::move(fn), 0, options});
    return id;
}

void Mixer::stop(VoiceId voice) {
    std::lock_guard<std::mutex> lock(mutex_);
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [voice](const Voice &v) { return v.id == voice; }),
                  voices_.end());
}

void Mixer::stop_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    voices_.clear();
}

void Mixer::set_gain(VoiceId voice, float gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &v : voices_) {
        if (v.id == voice) v.options.gain = gain;
    }
}

void Mixer::set_master_gain(float gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    master_ = std::max(0.0f, gain);
}

bool Mixer::playing(VoiceId voice) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(voices_.begin(), voices_.end(), [voice](const Voice &v) { return v.id == voice; });
}

size_t Mixer::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return voices_.size();
}

void Mixer::set_max_voices(size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_voices_ = std::max<size_t>(1, n);
}

float Mixer::peak() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peak_;
}

void Mixer::mix(float *out, size_t frames) {
    std::fill(out, out + frames * kChannels, 0.0f);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &v : voices_) {
        const float pan = std::clamp(v.options.pan, -1.0f, 1.0f);
        const float gl = v.options.gain * master_ * std::min(1.0f, 1.0f - pan);
        const float gr = v.options.gain * master_ * std::min(1.0f, 1.0f + pan);
        if (v.stream) {
            scratch_.assign(frames * kChannels, 0.0f);
            const size_t got = std::min(frames, v.stream(scratch_.data(), frames));
            for (size_t f = 0; f < got; ++f) {
                out[f * 2] += scratch_[f * 2] * gl;
                out[f * 2 + 1] += scratch_[f * 2 + 1] * gr;
            }
            if (got < frames) v.stream = nullptr;  // ended: removed below
            continue;
        }
        const std::vector<float> &s = v.sound->samples;
        const size_t total = v.sound->frames();
        size_t f = 0;
        while (f < frames) {
            if (v.pos >= total) {
                if (!v.options.loop) break;
                v.pos = 0;
            }
            const size_t n = std::min(frames - f, total - v.pos);
            const float *src = s.data() + v.pos * kChannels;
            float *dst = out + f * kChannels;
            for (size_t i = 0; i < n; ++i) {
                dst[i * 2] += src[i * 2] * gl;
                dst[i * 2 + 1] += src[i * 2 + 1] * gr;
            }
            v.pos += n;
            f += n;
        }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const Voice &v) {
                                     return v.sound ? (!v.options.loop && v.pos >= v.sound->frames()) : !v.stream;
                                 }),
                  voices_.end());
    for (size_t i = 0; i < frames * kChannels; ++i) {
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
        peak_ = std::max(peak_, std::fabs(out[i]));
    }
}

}  // namespace wnk::audio
