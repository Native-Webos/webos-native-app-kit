// SPDX-License-Identifier: GPL-3.0-or-later
//
// SDL adapter: SDL_Event → wnk::input::KeyEvent. Kept apart from the SDL-free core so the mapping
// and focus logic can be tested without SDL.
#pragma once

#include "wnk/input/gamepad.hpp"
#include "wnk/input/input.hpp"

#include <SDL.h>

#include <map>
#include <optional>

namespace wnk::input {

// Returns a KeyEvent for SDL_KEYDOWN / SDL_KEYUP, nothing for other events.
std::optional<KeyEvent> key_event_from_sdl(const SDL_Event &event);

// Returns a PointerEvent for mouse motion/buttons/wheel, the window losing the pointer, and the
// SDL-webOS cursor show/hide pseudo-keys; nothing for other events. Positions are converted from
// window units to drawable pixels. Touch-emulated mouse events are ignored.
std::optional<PointerEvent> pointer_event_from_sdl(const SDL_Event &event, SDL_Window *window);

// Game controllers through SDL's GameController API (standard layout, SDL's built-in mapping list
// plus any loaded with SDL_GameControllerAddMappingsFromFile). Opens each pad as it is plugged in.
class SdlGamepads {
 public:
    SdlGamepads() = default;
    ~SdlGamepads();
    SdlGamepads(const SdlGamepads &) = delete;
    SdlGamepads &operator=(const SdlGamepads &) = delete;
    // A GamepadEvent for SDL_CONTROLLER* events (opening or closing the pad on device added/removed).
    std::optional<GamepadEvent> on_sdl(const SDL_Event &event);
    size_t connected() const { return pads_.size(); }
    void close_all();  // before SDL_Quit

 private:
    std::map<SDL_JoystickID, SDL_GameController *> pads_;
};

}  // namespace wnk::input
