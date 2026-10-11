// SPDX-License-Identifier: GPL-3.0-or-later
//
// SDL adapter: SDL_Event → wnk::input::KeyEvent. Kept apart from the SDL-free core so the mapping
// and focus logic can be tested without SDL.
#pragma once

#include "wnk/input/input.hpp"

#include <SDL.h>

#include <optional>

namespace wnk::input {

// Returns a KeyEvent for SDL_KEYDOWN / SDL_KEYUP, nothing for other events.
std::optional<KeyEvent> key_event_from_sdl(const SDL_Event &event);

// Returns a PointerEvent for mouse motion/buttons/wheel, the window losing the pointer, and the
// SDL-webOS cursor show/hide pseudo-keys; nothing for other events. Positions are converted from
// window units to drawable pixels. Touch-emulated mouse events are ignored.
std::optional<PointerEvent> pointer_event_from_sdl(const SDL_Event &event, SDL_Window *window);

}  // namespace wnk::input
