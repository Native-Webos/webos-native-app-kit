// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gamepads: an SDL-free event model (buttons and axes in SDL GameController's standard layout, so an
// Xbox, PlayStation or generic pad reports the same names) and the translation of a pad into UI
// actions for apps that are not games.
#pragma once

#include "wnk/input/input.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wnk::input {

// Positions, not labels: A is the bottom face button (Cross on PlayStation), B the right one.
enum class PadButton : uint8_t {
    A, B, X, Y, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight, Misc, Count
};
enum class PadAxis : uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

const char *pad_button_name(PadButton b);
const char *pad_axis_name(PadAxis a);

struct GamepadEvent {
    enum class Type : uint8_t { Added, Removed, Button, Axis };
    Type type = Type::Button;
    int id = -1;                       // the pad, stable while it stays connected
    PadButton button = PadButton::A;   // Button
    bool down = false;                 // Button
    PadAxis axis = PadAxis::LeftX;     // Axis
    float value = 0;                   // Axis: sticks -1..1 (right and down positive), triggers 0..1
    std::string name;                  // Added: the pad's product name
    uint32_t time_ms = 0;
};

// D-pad and left stick → Up/Down/Left/Right, A → Confirm, B → Back, Start → PlayPause,
// shoulders → Rewind/FastForward. A held direction repeats like a remote key held down: first after
// `delay_ms`, then every `interval_ms`. Each pad and each source (D-pad, stick) is tracked apart.
class GamepadNavigator {
 public:
    uint32_t delay_ms = 400;
    uint32_t interval_ms = 90;
    float press_threshold = 0.6f;    // stick deflection that counts as a press
    float release_threshold = 0.35f;  // and below which it is released (hysteresis)

    std::vector<ActionEvent> on_event(const GamepadEvent &e);
    // Repeats for directions still held at `now_ms`.
    std::vector<ActionEvent> tick(uint32_t now_ms);
    // Releases everything held (a pad unplugged, the app sent to the background).
    std::vector<ActionEvent> release_all(uint32_t now_ms);

 private:
    struct Held {
        int pad;
        int source;  // 0: buttons (keyed by action), 1: left stick
        Action action;
        uint32_t since, next;
    };
    struct Stick {
        int pad;
        float x = 0, y = 0;
    };
    void press(std::vector<ActionEvent> &out, int pad, int source, Action a, uint32_t t, bool repeats);
    void release(std::vector<ActionEvent> &out, int pad, int source, Action a, uint32_t t);
    std::vector<Held> held_;
    std::vector<Stick> sticks_;
};

}  // namespace wnk::input
