// SPDX-License-Identifier: GPL-3.0-or-later
//
// Application lifecycle state machine (docs/research/APPLICATION-LIFECYCLE.md, "Kit lifecycle model").
//
//   Created ─Start─► Initialising ─Started─► Running ─Background─► Suspended ─Foreground─► Resuming
//                         │                     ▲ │                    │                     │
//                         │                     │ └──────Quit────┐     │ Quit           FramePresented
//                         │                     └────────────────┼─────┼─────────────────────┘
//                         └──────────Quit────────────────────────┴─► Stopping ─Stopped─► Terminated
//
// SDL-free and main-thread only. The app shell feeds it from SDL events: SDL-webOS turns the
// compositor's minimise/fullscreen states into SDL_APP_*BACKGROUND/*FOREGROUND and SAM's "close"
// into SDL_QUIT (SDL-webOS src/video/wayland/SDL_waylandwebos.c, src/core/webos/SDL_webos_init.c).
// A repeated event (WILL and DID background both map to Background) is ignored, not an error.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace wnk::app {

enum class State : uint8_t { Created, Initialising, Running, Suspended, Resuming, Stopping, Terminated };
enum class Event : uint8_t { Start, Started, Background, Foreground, FramePresented, Quit, Stopped };

const char *state_name(State s);
const char *event_name(Event e);

class Lifecycle {
 public:
    struct Transition {
        State from, to;
        Event event;
        uint32_t time_ms;
    };
    using Listener = std::function<void(const Transition &)>;

    State state() const { return state_; }
    // Applies the event. Returns true if the state changed. Events that do not apply in the current
    // state are ignored (false); `ignored()` counts them for diagnostics.
    bool dispatch(Event e, uint32_t time_ms);
    void set_listener(Listener l) { listener_ = std::move(l); }
    // The most recent transitions, oldest first (bounded).
    const std::vector<Transition> &history() const { return history_; }
    uint32_t ignored() const { return ignored_; }

    // Frames should be drawn in these states; in Suspended the loop only waits for events.
    bool renders() const { return state_ == State::Running || state_ == State::Resuming; }
    bool stopping() const { return state_ == State::Stopping || state_ == State::Terminated; }

    static bool next(State from, Event e, State *to);  // the transition table

 private:
    State state_ = State::Created;
    std::vector<Transition> history_;
    uint32_t ignored_ = 0;
    Listener listener_;
};

}  // namespace wnk::app
