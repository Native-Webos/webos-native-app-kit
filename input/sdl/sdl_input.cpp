// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/input/sdl_input.hpp"

// The SDL-free core (input.cpp) hard-codes SDL2 scancode numbers; prove them against the headers.
static_assert(SDL_SCANCODE_RETURN == 40 && SDL_SCANCODE_ESCAPE == 41 && SDL_SCANCODE_BACKSPACE == 42);
static_assert(SDL_SCANCODE_RIGHT == 79 && SDL_SCANCODE_LEFT == 80 && SDL_SCANCODE_DOWN == 81 &&
              SDL_SCANCODE_UP == 82 && SDL_SCANCODE_KP_ENTER == 88);
static_assert(SDL_SCANCODE_F1 == 58 && SDL_SCANCODE_F12 == 69 && SDL_SCANCODE_PAUSE == 72 && SDL_SCANCODE_HOME == 74);
static_assert(SDL_SCANCODE_STOP == 120 && SDL_SCANCODE_MUTE == 127 && SDL_SCANCODE_VOLUMEUP == 128 &&
              SDL_SCANCODE_VOLUMEDOWN == 129);
static_assert(SDL_SCANCODE_AUDIOSTOP == 260 && SDL_SCANCODE_AUDIOPLAY == 261 && SDL_SCANCODE_AC_HOME == 269 &&
              SDL_SCANCODE_AC_BACK == 270);
static_assert(SDL_SCANCODE_AUDIOREWIND == 285 && SDL_SCANCODE_AUDIOFASTFORWARD == 286);
#if defined(WNK_SDL_WEBOS)
static_assert(SDL_SCANCODE_WEBOS_BACK == 482 && SDL_SCANCODE_WEBOS_EXIT == 505 && SDL_SCANCODE_WEBOS_HOME == 384);
static_assert(SDL_SCANCODE_WEBOS_RED == 486 && SDL_SCANCODE_WEBOS_BLUE == 489 && SDL_SCANCODE_WEBOS_CH_UP == 480);
#endif

namespace wnk::input {

std::optional<KeyEvent> key_event_from_sdl(const SDL_Event &event) {
    if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP) return std::nullopt;
    KeyEvent k;
    k.scancode = static_cast<int>(event.key.keysym.scancode);
    k.keycode = static_cast<int>(event.key.keysym.sym);
    k.down = event.type == SDL_KEYDOWN;
    k.repeat = event.key.repeat != 0;
    k.time_ms = event.key.timestamp;
    return k;
}

}  // namespace wnk::input
