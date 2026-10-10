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

}  // namespace wnk::input
