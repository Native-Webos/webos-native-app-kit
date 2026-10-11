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
static_assert(SDL_SCANCODE_WEBOS_CURSOR_SHOW == wnk::input::kScancodeCursorShow &&
              SDL_SCANCODE_WEBOS_CURSOR_HIDE == wnk::input::kScancodeCursorHide);
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

std::optional<PointerEvent> pointer_event_from_sdl(const SDL_Event &event, SDL_Window *window) {
    float sx = 1, sy = 1;
    if (window != nullptr) {
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        if (ww > 0 && wh > 0 && dw > 0 && dh > 0) {
            sx = static_cast<float>(dw) / static_cast<float>(ww);
            sy = static_cast<float>(dh) / static_cast<float>(wh);
        }
    }
    PointerEvent p;
    switch (event.type) {
        case SDL_MOUSEMOTION:
            if (event.motion.which == SDL_TOUCH_MOUSEID) return std::nullopt;
            p.type = PointerEvent::Type::Move;
            p.x = static_cast<float>(event.motion.x) * sx;
            p.y = static_cast<float>(event.motion.y) * sy;
            p.time_ms = event.motion.timestamp;
            return p;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (event.button.which == SDL_TOUCH_MOUSEID) return std::nullopt;
            p.type = event.type == SDL_MOUSEBUTTONDOWN ? PointerEvent::Type::Press : PointerEvent::Type::Release;
            p.x = static_cast<float>(event.button.x) * sx;
            p.y = static_cast<float>(event.button.y) * sy;
            p.button = event.button.button;
            p.time_ms = event.button.timestamp;
            return p;
        case SDL_MOUSEWHEEL:
            if (event.wheel.which == SDL_TOUCH_MOUSEID) return std::nullopt;
            p.type = PointerEvent::Type::Wheel;
            p.wheel = static_cast<float>(event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y);
            p.time_ms = event.wheel.timestamp;
            return p;
        case SDL_WINDOWEVENT:
            if (event.window.event != SDL_WINDOWEVENT_LEAVE) return std::nullopt;
            p.type = PointerEvent::Type::Left;
            p.time_ms = event.window.timestamp;
            return p;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (auto k = key_event_from_sdl(event)) return pointer_visibility_from_key(*k);
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

}  // namespace wnk::input
