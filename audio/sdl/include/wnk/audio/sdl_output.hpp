// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SDL audio device a Mixer plays into: 48 kHz stereo float requested, SDL converting to whatever
// the driver takes (PulseAudio, else ALSA, on webOS; libpulse.so.0 and libasound.so.2 are loaded at
// runtime and present on every inventoried firmware). The mixer runs on SDL's audio thread.
#pragma once

#include "wnk/audio/audio.hpp"

#include <memory>
#include <string>

namespace wnk::audio {

class SdlOutput {
 public:
    // Initialises SDL's audio subsystem and opens the default device, started. nullptr and *error on failure.
    static std::unique_ptr<SdlOutput> open(Mixer &mixer, std::string *error = nullptr);
    ~SdlOutput();
    SdlOutput(const SdlOutput &) = delete;
    SdlOutput &operator=(const SdlOutput &) = delete;

    void pause(bool paused);  // e.g. while the app is in the background
    bool paused() const { return paused_; }
    // "pulseaudio, 48000 Hz, f32, 2 ch, 1024 frames" as obtained.
    const std::string &describe() const { return describe_; }

 private:
    SdlOutput() = default;
    uint32_t device_ = 0;
    bool paused_ = false;
    std::string describe_;
};

}  // namespace wnk::audio
