// SPDX-License-Identifier: GPL-3.0-or-later
//
// App sound: sound effects and music mixed into the TV's audio output (SDL audio, PulseAudio or ALSA
// on webOS). This is for an app's own sounds (interface clicks, game effects and music, generated
// audio); video and its soundtrack go through media/.
//
//   auto click = audio::load_sound(app_dir + "/sounds/click.ogg", &err);   // WAV or Ogg Vorbis
//   ctx.audio()->play(click);                                               // any thread
//   auto music = ctx.audio()->play(theme, {.gain = 0.5f, .loop = true});
//   ctx.audio()->stop(music);
//
// Everything is converted to 48 kHz stereo float when loaded, so playing costs one add per sample.
// The mixer is SDL-free; audio/sdl/ opens the device and calls Mixer::mix from SDL's audio thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wnk::audio {

constexpr int kRate = 48000;   // output and Sound sample rate
constexpr int kChannels = 2;   // interleaved left, right

// Decoded audio, 48 kHz interleaved stereo float in [-1, 1].
struct Pcm {
    std::vector<float> samples;
    size_t frames() const { return samples.size() / kChannels; }
    double seconds() const { return static_cast<double>(frames()) / kRate; }
};
using Sound = std::shared_ptr<const Pcm>;

// RIFF WAVE: PCM 8/16/24/32-bit or float 32, any rate; mono plays on both sides, and of more than two
// channels the first two (front left and right) are used.
Sound decode_wav(const uint8_t *data, size_t size, std::string *error = nullptr);
// Ogg Vorbis (stb_vorbis), whole file decoded at once: keep long music in a stream instead.
Sound decode_ogg(const uint8_t *data, size_t size, std::string *error = nullptr);
// By content: "RIFF....WAVE" or "OggS". Files over 64 MiB are refused.
Sound load_sound(const std::string &path, std::string *error = nullptr);
// A sine tone with 5 ms fades at both ends (no click): interface beeps, tests.
Sound tone(float frequency_hz, float seconds, float gain = 0.3f);
// Interleaved samples at `rate` with `channels` channels → 48 kHz stereo (linear interpolation).
Sound from_samples(const float *interleaved, size_t frames, int rate, int channels);

using VoiceId = uint64_t;
constexpr VoiceId kNoVoice = 0;

struct PlayOptions {
    float gain = 1;     // linear
    float pan = 0;      // -1 left … 0 centre … 1 right
    bool loop = false;
};

class Mixer {
 public:
    // Fills `frames` interleaved stereo frames on the audio thread; returning fewer ends the stream.
    // It runs with the mixer locked: no blocking, no allocation if it can be avoided.
    using StreamFn = std::function<size_t(float *out, size_t frames)>;

    Mixer() = default;
    Mixer(const Mixer &) = delete;
    Mixer &operator=(const Mixer &) = delete;

    // All of these may be called from any thread.
    VoiceId play(Sound sound, PlayOptions options = {});
    VoiceId play_stream(StreamFn fn, PlayOptions options = {});
    void stop(VoiceId voice);
    void stop_all();
    void set_gain(VoiceId voice, float gain);
    void set_master_gain(float gain);
    bool playing(VoiceId voice) const;
    size_t active() const;
    // Voices beyond this are refused (play returns kNoVoice): a game that fires a sound per frame
    // must not grow the mixing work without bound.
    void set_max_voices(size_t n);

    // The audio thread: adds every voice into `out` (cleared first) and clamps to [-1, 1].
    void mix(float *out, size_t frames);
    // Peak absolute sample of everything mixed so far (diagnostics and tests).
    float peak() const;

 private:
    struct Voice {
        VoiceId id;
        Sound sound;
        StreamFn stream;
        size_t pos = 0;
        PlayOptions options;
    };
    mutable std::mutex mutex_;
    std::vector<Voice> voices_;
    std::vector<float> scratch_;
    VoiceId next_ = 1;
    float master_ = 1;
    float peak_ = 0;
    size_t max_voices_ = 32;
};

}  // namespace wnk::audio
