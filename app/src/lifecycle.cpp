// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/app/lifecycle.hpp"

namespace wnk::app {

const char *state_name(State s) {
    switch (s) {
        case State::Created: return "Created";
        case State::Initialising: return "Initialising";
        case State::Running: return "Running";
        case State::Suspended: return "Suspended";
        case State::Resuming: return "Resuming";
        case State::Stopping: return "Stopping";
        case State::Terminated: return "Terminated";
    }
    return "?";
}

const char *event_name(Event e) {
    switch (e) {
        case Event::Start: return "Start";
        case Event::Started: return "Started";
        case Event::Background: return "Background";
        case Event::Foreground: return "Foreground";
        case Event::FramePresented: return "FramePresented";
        case Event::Quit: return "Quit";
        case Event::Stopped: return "Stopped";
    }
    return "?";
}

bool Lifecycle::next(State from, Event e, State *to) {
    State t = from;
    switch (from) {
        case State::Created:
            if (e == Event::Start) t = State::Initialising;
            break;
        case State::Initialising:
            if (e == Event::Started) t = State::Running;
            if (e == Event::Quit) t = State::Stopping;
            break;
        case State::Running:
            if (e == Event::Background) t = State::Suspended;
            if (e == Event::Quit) t = State::Stopping;
            break;
        case State::Suspended:
            if (e == Event::Foreground) t = State::Resuming;
            if (e == Event::Quit) t = State::Stopping;
            break;
        case State::Resuming:
            if (e == Event::FramePresented) t = State::Running;
            if (e == Event::Background) t = State::Suspended;
            if (e == Event::Quit) t = State::Stopping;
            break;
        case State::Stopping:
            if (e == Event::Stopped) t = State::Terminated;
            break;
        case State::Terminated:
            break;
    }
    if (to != nullptr) *to = t;
    return t != from;
}

bool Lifecycle::dispatch(Event e, uint32_t time_ms) {
    State to;
    if (!next(state_, e, &to)) {
        ++ignored_;
        return false;
    }
    const Transition tr{state_, to, e, time_ms};
    state_ = to;
    if (history_.size() == 32) history_.erase(history_.begin());
    history_.push_back(tr);
    if (listener_) listener_(tr);
    return true;
}

}  // namespace wnk::app
