// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/input/sdl_input.hpp"

#include <algorithm>

// The SDL-free core (input.cpp) hard-codes SDL2 scancode numbers; prove them against the headers.
static_assert(SDL_SCANCODE_RETURN == 40 && SDL_SCANCODE_ESCAPE == 41 && SDL_SCANCODE_BACKSPACE == 42);
static_assert(SDL_SCANCODE_RIGHT == 79 && SDL_SCANCODE_LEFT == 80 && SDL_SCANCODE_DOWN == 81 &&
              SDL_SCANCODE_UP == 82 && SDL_SCANCODE_KP_ENTER == 88);
static_assert(SDL_SCANCODE_F1 == 58 && SDL_SCANCODE_F12 == 69 && SDL_SCANCODE_PAUSE == 72 && SDL_SCANCODE_HOME == 74);
static_assert(SDL_SCANCODE_DELETE == 76 && SDL_SCANCODE_END == 77 && SDL_SCANCODE_CLEAR == 156);  // text editing keys
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

// ---- gamepads -------------------------------------------------------------------------------------

namespace {

std::optional<PadButton> pad_button(Uint8 b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return PadButton::A;
        case SDL_CONTROLLER_BUTTON_B: return PadButton::B;
        case SDL_CONTROLLER_BUTTON_X: return PadButton::X;
        case SDL_CONTROLLER_BUTTON_Y: return PadButton::Y;
        case SDL_CONTROLLER_BUTTON_BACK: return PadButton::Back;
        case SDL_CONTROLLER_BUTTON_GUIDE: return PadButton::Guide;
        case SDL_CONTROLLER_BUTTON_START: return PadButton::Start;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return PadButton::LeftStick;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return PadButton::RightStick;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return PadButton::LeftShoulder;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PadButton::RightShoulder;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return PadButton::DpadUp;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return PadButton::DpadDown;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return PadButton::DpadLeft;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return PadButton::DpadRight;
        case SDL_CONTROLLER_BUTTON_MISC1: return PadButton::Misc;
        default: return std::nullopt;  // paddles and touchpad buttons
    }
}

std::optional<PadAxis> pad_axis(Uint8 a) {
    switch (a) {
        case SDL_CONTROLLER_AXIS_LEFTX: return PadAxis::LeftX;
        case SDL_CONTROLLER_AXIS_LEFTY: return PadAxis::LeftY;
        case SDL_CONTROLLER_AXIS_RIGHTX: return PadAxis::RightX;
        case SDL_CONTROLLER_AXIS_RIGHTY: return PadAxis::RightY;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return PadAxis::LeftTrigger;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return PadAxis::RightTrigger;
        default: return std::nullopt;
    }
}

}  // namespace

SdlGamepads::~SdlGamepads() { close_all(); }

void SdlGamepads::close_all() {
    for (auto &[id, pad] : pads_) SDL_GameControllerClose(pad);
    pads_.clear();
}

std::optional<GamepadEvent> SdlGamepads::on_sdl(const SDL_Event &ev) {
    GamepadEvent e;
    switch (ev.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            SDL_GameController *pad = SDL_GameControllerOpen(ev.cdevice.which);  // `which` is a device index here
            if (pad == nullptr) return std::nullopt;
            const SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
            if (pads_.count(id) != 0) {
                SDL_GameControllerClose(pad);  // already open (added twice): keep one reference
                return std::nullopt;
            }
            pads_[id] = pad;
            e.type = GamepadEvent::Type::Added;
            e.id = id;
            const char *name = SDL_GameControllerName(pad);
            e.name = name != nullptr ? name : "gamepad";
            e.time_ms = ev.cdevice.timestamp;
            return e;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {
            auto it = pads_.find(ev.cdevice.which);  // an instance id here
            if (it == pads_.end()) return std::nullopt;
            SDL_GameControllerClose(it->second);
            pads_.erase(it);
            e.type = GamepadEvent::Type::Removed;
            e.id = ev.cdevice.which;
            e.time_ms = ev.cdevice.timestamp;
            return e;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            const auto b = pad_button(ev.cbutton.button);
            if (!b || pads_.count(ev.cbutton.which) == 0) return std::nullopt;
            e.type = GamepadEvent::Type::Button;
            e.id = ev.cbutton.which;
            e.button = *b;
            e.down = ev.type == SDL_CONTROLLERBUTTONDOWN;
            e.time_ms = ev.cbutton.timestamp;
            return e;
        }
        case SDL_CONTROLLERAXISMOTION: {
            const auto a = pad_axis(ev.caxis.axis);
            if (!a || pads_.count(ev.caxis.which) == 0) return std::nullopt;
            e.type = GamepadEvent::Type::Axis;
            e.id = ev.caxis.which;
            e.axis = *a;
            e.value = std::clamp(static_cast<float>(ev.caxis.value) / 32767.0f, -1.0f, 1.0f);
            e.time_ms = ev.caxis.timestamp;
            return e;
        }
        default: return std::nullopt;
    }
}

}  // namespace wnk::input
