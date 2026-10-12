// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/input/gamepad.hpp"

#include <algorithm>
#include <cmath>

namespace wnk::input {
namespace {

constexpr int kButtons = 0, kStick = 1;

Action button_action(PadButton b) {
    switch (b) {
        case PadButton::DpadUp: return Action::Up;
        case PadButton::DpadDown: return Action::Down;
        case PadButton::DpadLeft: return Action::Left;
        case PadButton::DpadRight: return Action::Right;
        case PadButton::A: return Action::Confirm;
        case PadButton::B: return Action::Back;
        case PadButton::Start: return Action::PlayPause;
        case PadButton::LeftShoulder: return Action::Rewind;
        case PadButton::RightShoulder: return Action::FastForward;
        default: return Action::Unknown;
    }
}

bool is_direction(Action a) { return a == Action::Up || a == Action::Down || a == Action::Left || a == Action::Right; }

ActionEvent event(Action a, Phase p, uint32_t held, uint32_t t) {
    ActionEvent e;
    e.action = a;
    e.phase = p;
    e.held_ms = held;
    e.source.time_ms = t;
    return e;
}

}  // namespace

const char *pad_button_name(PadButton b) {
    static const char *names[] = {"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
                                  "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc"};
    const auto i = static_cast<size_t>(b);
    return i < sizeof names / sizeof names[0] ? names[i] : "?";
}

const char *pad_axis_name(PadAxis a) {
    static const char *names[] = {"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};
    const auto i = static_cast<size_t>(a);
    return i < sizeof names / sizeof names[0] ? names[i] : "?";
}

void GamepadNavigator::press(std::vector<ActionEvent> &out, int pad, int source, Action a, uint32_t t, bool repeats) {
    held_.push_back({pad, source, a, t, repeats ? t + delay_ms : UINT32_MAX});
    out.push_back(event(a, Phase::Press, 0, t));
}

void GamepadNavigator::release(std::vector<ActionEvent> &out, int pad, int source, Action a, uint32_t t) {
    auto it = std::find_if(held_.begin(), held_.end(),
                           [&](const Held &h) { return h.pad == pad && h.source == source && h.action == a; });
    if (it == held_.end()) return;
    out.push_back(event(a, Phase::Release, t - it->since, t));
    held_.erase(it);
}

std::vector<ActionEvent> GamepadNavigator::on_event(const GamepadEvent &e) {
    std::vector<ActionEvent> out;
    switch (e.type) {
        case GamepadEvent::Type::Added: break;
        case GamepadEvent::Type::Removed: {
            for (auto it = held_.begin(); it != held_.end();) {
                if (it->pad == e.id) {
                    out.push_back(event(it->action, Phase::Release, e.time_ms - it->since, e.time_ms));
                    it = held_.erase(it);
                } else {
                    ++it;
                }
            }
            sticks_.erase(std::remove_if(sticks_.begin(), sticks_.end(), [&](const Stick &s) { return s.pad == e.id; }),
                          sticks_.end());
            break;
        }
        case GamepadEvent::Type::Button: {
            const Action a = button_action(e.button);
            if (a == Action::Unknown) break;
            if (e.down) {
                const bool already = std::any_of(held_.begin(), held_.end(), [&](const Held &h) {
                    return h.pad == e.id && h.source == kButtons && h.action == a;
                });
                if (!already) press(out, e.id, kButtons, a, e.time_ms, is_direction(a));
            } else {
                release(out, e.id, kButtons, a, e.time_ms);
            }
            break;
        }
        case GamepadEvent::Type::Axis: {
            if (e.axis != PadAxis::LeftX && e.axis != PadAxis::LeftY) break;
            auto it = std::find_if(sticks_.begin(), sticks_.end(), [&](const Stick &s) { return s.pad == e.id; });
            if (it == sticks_.end()) it = sticks_.insert(sticks_.end(), Stick{e.id});
            (e.axis == PadAxis::LeftX ? it->x : it->y) = std::clamp(e.value, -1.0f, 1.0f);
            // The stick points one way at a time: its dominant axis, with hysteresis around the centre.
            auto current = std::find_if(held_.begin(), held_.end(), [&](const Held &h) { return h.pad == e.id && h.source == kStick; });
            const Action was = current != held_.end() ? current->action : Action::Unknown;
            const float ax = std::fabs(it->x), ay = std::fabs(it->y);
            const float mag = std::max(ax, ay);
            Action now = Action::Unknown;
            const float threshold = was == Action::Unknown ? press_threshold : release_threshold;
            if (mag >= threshold) {
                now = ax >= ay ? (it->x > 0 ? Action::Right : Action::Left) : (it->y > 0 ? Action::Down : Action::Up);
            }
            if (now == was) break;
            if (was != Action::Unknown) release(out, e.id, kStick, was, e.time_ms);
            if (now != Action::Unknown && mag >= press_threshold) press(out, e.id, kStick, now, e.time_ms, true);
            break;
        }
    }
    return out;
}

std::vector<ActionEvent> GamepadNavigator::tick(uint32_t now) {
    std::vector<ActionEvent> out;
    for (auto &h : held_) {
        if (h.next == UINT32_MAX || static_cast<int32_t>(now - h.next) < 0) continue;
        out.push_back(event(h.action, Phase::Repeat, now - h.since, now));
        h.next = now + interval_ms;
    }
    return out;
}

std::vector<ActionEvent> GamepadNavigator::release_all(uint32_t now) {
    std::vector<ActionEvent> out;
    for (const auto &h : held_) out.push_back(event(h.action, Phase::Release, now - h.since, now));
    held_.clear();
    sticks_.clear();
    return out;
}

}  // namespace wnk::input
