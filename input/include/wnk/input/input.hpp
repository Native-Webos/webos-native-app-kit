// SPDX-License-Identifier: GPL-3.0-or-later
//
// Remote-control input: raw key events → semantic actions.
//
//   platform event ─► KeyEvent (this header; SDL-free) ─► KeyMap ─► ActionEvent ─► focus / app
//
// Widgets and application code only ever see ActionEvents. Raw codes stay in the keymap tables,
// where every entry records where its number came from (docs/research/REMOTE-INPUT.md).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace wnk::input {

enum class Action : uint8_t {
    Unknown = 0,
    Up, Down, Left, Right,
    Confirm, Back, Exit, Home,
    PlayPause, Play, Pause, Stop, Rewind, FastForward,
    ChannelUp, ChannelDown,
    Red, Green, Yellow, Blue,
    VolumeUp, VolumeDown, Mute,
    Count
};

const char *action_name(Action action);

// Where a keymap entry's number comes from. Only Measured entries are facts about a real TV.
enum class Provenance : uint8_t {
    Source,    // read in the SDL build's source/headers we ship (e.g. SDL-webOS SDL_scancode.h)
    Measured,  // captured on a TV with the SDL build we ship (record in docs/research/COMPATIBILITY.md)
    Assumed,   // plausible but unverified; kept so the key works if it arrives
    Host,      // development keyboard on the host only
};

const char *provenance_name(Provenance p);

struct KeyEvent {
    int scancode = 0;      // SDL_Scancode value (SDL2 numbering)
    int keycode = 0;       // SDL_Keycode value
    bool down = false;     // key down (true) or key up (false)
    bool repeat = false;   // auto-repeat down event
    uint32_t time_ms = 0;  // event timestamp
};

enum class Phase : uint8_t { Press, Repeat, Release };

struct ActionEvent {
    Action action = Action::Unknown;
    Phase phase = Phase::Press;
    uint32_t held_ms = 0;  // on Release: how long the key was held; on Repeat: time since Press
    KeyEvent source;       // the raw event, for diagnostics only
};

struct KeyBinding {
    enum class Field : uint8_t { Scancode, Keycode };
    Field field;
    int code;
    Action action;
    Provenance provenance;
    const char *note;
};

class KeyMap {
 public:
    KeyMap() = default;
    explicit KeyMap(std::vector<KeyBinding> bindings) : bindings_(std::move(bindings)) {}
    void add(const KeyBinding &binding) { bindings_.push_back(binding); }
    // Scancode bindings win over keycode bindings; first match in table order otherwise.
    Action lookup(int scancode, int keycode) const;
    const KeyBinding *find(int scancode, int keycode) const;
    const std::vector<KeyBinding> &bindings() const { return bindings_; }

 private:
    std::vector<KeyBinding> bindings_;
};

// Development keyboard: arrows, Enter/keypad Enter = Confirm, Escape/Backspace = Back,
// F1-F4 = colour keys, Space = PlayPause, Home = Home, F12 = Exit.
KeyMap host_keymap();

// webosbrew SDL-webOS 2.30.x (the SDL the kit bundles). D-pad/OK use standard scancodes; the webOS
// block (Back 482, Exit 505, colours 486-489, CH 480/481) comes from its SDL_scancode.h; transport
// keys are Assumed until captured on a TV.
KeyMap webos_sdl2_keymap();

// The keymap for the platform this binary was built for.
KeyMap default_keymap();

// Turns KeyEvents into ActionEvents using a KeyMap.
//  * A key-up with no matching key-down (e.g. a press that started before the app had focus) is
//    dropped, so handlers never see an unpaired Release.
//  * Repeats are reported as Phase::Repeat with the time since the press.
//  * Keys that map to Action::Unknown are reported only if `report_unknown` is set, so unbound keys
//    have no side effects by default (LG self-checklist row 40, PlxNative remote-keys.md §6).
class ActionMapper {
 public:
    explicit ActionMapper(KeyMap map, bool report_unknown = false)
        : map_(std::move(map)), report_unknown_(report_unknown) {}

    std::optional<ActionEvent> map(const KeyEvent &event);
    // Synthesise Release for every held key (call when focus is lost or the app is backgrounded).
    std::vector<ActionEvent> release_all(uint32_t time_ms);
    size_t held_count() const { return held_.size(); }
    const KeyMap &keymap() const { return map_; }

 private:
    struct Held {
        int scancode;
        Action action;
        uint32_t since_ms;
        KeyEvent last;
    };
    KeyMap map_;
    bool report_unknown_;
    std::vector<Held> held_;
};

// ---- pointer (Magic Remote) ------------------------------------------------------------------------
//
// The Magic Remote moves an on-screen pointer. SDL-webOS delivers it as ordinary SDL mouse events in
// window coordinates (wayland pointer → SDL_MOUSEMOTION/BUTTON/WHEEL) and announces the pointer
// appearing or hiding as two pseudo-keys in its webOS scancode block (484/485, include/SDL_scancode.h).
// Whether the remote's OK in pointer mode arrives as a left click, as Enter, or as both is not knowable
// from source; apps must accept either (record in docs/compatibility/records.json once measured).

struct PointerEvent {
    enum class Type : uint8_t {
        Move,     // position changed
        Press,    // button down at position
        Release,  // button up at position
        Wheel,    // wheel turned; position is not reported (use the last Move)
        Shown,    // pointer appeared (SDL-webOS CursorShow key)
        Hidden,   // pointer hidden: timeout or the user switched to the D-pad (CursorHide key)
        Left,     // pointer left the window
    };
    Type type = Type::Move;
    float x = 0, y = 0;  // drawable pixels (not window units, not logical units)
    int button = 1;      // 1 left, 2 middle, 3 right
    float wheel = 0;     // notches; positive = away from the user (scroll towards the start)
    uint32_t time_ms = 0;
};

const char *pointer_type_name(PointerEvent::Type t);

// SDL-webOS 2.x scancodes for the pointer show/hide pseudo-keys (include/SDL_scancode.h).
inline constexpr int kScancodeCursorShow = 484;
inline constexpr int kScancodeCursorHide = 485;

// Shown/Hidden for a key-down of the cursor pseudo-keys, nothing for every other key.
std::optional<PointerEvent> pointer_visibility_from_key(const KeyEvent &key);

}  // namespace wnk::input
