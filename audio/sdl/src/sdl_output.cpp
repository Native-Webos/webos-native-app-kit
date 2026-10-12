// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/audio/sdl_output.hpp"

#include <SDL.h>

namespace wnk::audio {
namespace {

void callback(void *user, Uint8 *stream, int len) {
    static_cast<Mixer *>(user)->mix(reinterpret_cast<float *>(stream), static_cast<size_t>(len) / (sizeof(float) * kChannels));
}

const char *format_name(SDL_AudioFormat f) {
    switch (f) {
        case AUDIO_F32SYS: return "f32";
        case AUDIO_S16SYS: return "s16";
        case AUDIO_S32SYS: return "s32";
        case AUDIO_U8: return "u8";
        default: return "other";
    }
}

}  // namespace

std::unique_ptr<SdlOutput> SdlOutput::open(Mixer &mixer, std::string *error) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        if (error != nullptr) *error = std::string("SDL audio init: ") + SDL_GetError();
        return nullptr;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = kRate;
    want.format = AUDIO_F32SYS;
    want.channels = kChannels;
    want.samples = 1024;  // 21 ms at 48 kHz
    want.callback = callback;
    want.userdata = &mixer;
    // No allowed changes: SDL converts to the device's format, so the mixer always sees 48 kHz stereo float.
    const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev == 0) {
        if (error != nullptr) *error = std::string("SDL_OpenAudioDevice: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return nullptr;
    }
    auto out = std::unique_ptr<SdlOutput>(new SdlOutput());
    out->device_ = dev;
    const char *driver = SDL_GetCurrentAudioDriver();
    out->describe_ = std::string(driver != nullptr ? driver : "?") + ", " + std::to_string(have.freq) + " Hz, " +
                     format_name(have.format) + ", " + std::to_string(have.channels) + " ch, " +
                     std::to_string(have.samples) + " frames";
    SDL_PauseAudioDevice(dev, 0);
    return out;
}

SdlOutput::~SdlOutput() {
    if (device_ != 0) SDL_CloseAudioDevice(device_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SdlOutput::pause(bool paused) {
    if (paused == paused_) return;
    paused_ = paused;
    SDL_PauseAudioDevice(device_, paused ? 1 : 0);
}

}  // namespace wnk::audio
