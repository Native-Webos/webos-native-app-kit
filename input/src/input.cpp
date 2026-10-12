// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/input/input.hpp"

#include <algorithm>

namespace wnk::input {
namespace {

// SDL2 scancode and keycode numbers used below. They are stable parts of the SDL2 ABI; the SDL
// adapter (input/sdl) static_asserts them against the SDL headers it is compiled with.
namespace sc {
constexpr int Return = 40, Escape = 41, Backspace = 42, Space = 44;
constexpr int F1 = 58, F2 = 59, F3 = 60, F4 = 61, F12 = 69;
constexpr int Pause = 72, Home = 74, Delete = 76, End = 77;
constexpr int Right = 79, Left = 80, Down = 81, Up = 82, KpEnter = 88;
constexpr int Stop = 120, Mute = 127, VolumeUp = 128, VolumeDown = 129, Clear = 156;
constexpr int AudioNext = 258, AudioPrev = 259, AudioStop = 260, AudioPlay = 261, AudioMute = 262;
constexpr int AcHome = 269, AcBack = 270;
constexpr int AudioRewind = 285, AudioFastForward = 286;
// webosbrew SDL-webOS 2.x webOS block (include/SDL_scancode.h, release-2.30.12-webos.6).
constexpr int WebOSHome = 384, WebOSChUp = 480, WebOSChDown = 481, WebOSBack = 482;
constexpr int WebOSCursorShow = 484, WebOSCursorHide = 485;
constexpr int WebOSRed = 486, WebOSGreen = 487, WebOSYellow = 488, WebOSBlue = 489;
constexpr int WebOSGuide = 495, WebOSExit = 505;
}  // namespace sc

using F = KeyBinding::Field;

}  // namespace

const char *pointer_type_name(PointerEvent::Type t) {
    switch (t) {
        case PointerEvent::Type::Move: return "move";
        case PointerEvent::Type::Press: return "press";
        case PointerEvent::Type::Release: return "release";
        case PointerEvent::Type::Wheel: return "wheel";
        case PointerEvent::Type::Shown: return "shown";
        case PointerEvent::Type::Hidden: return "hidden";
        case PointerEvent::Type::Left: return "left";
    }
    return "?";
}

std::optional<PointerEvent> pointer_visibility_from_key(const KeyEvent &key) {
    if (!key.down || key.repeat) return std::nullopt;
    if (key.scancode != kScancodeCursorShow && key.scancode != kScancodeCursorHide) return std::nullopt;
    PointerEvent p;
    p.type = key.scancode == kScancodeCursorShow ? PointerEvent::Type::Shown : PointerEvent::Type::Hidden;
    p.time_ms = key.time_ms;
    return p;
}

const char *edit_key_name(EditKey key) {
    switch (key) {
        case EditKey::Backspace: return "Backspace";
        case EditKey::Delete: return "Delete";
        case EditKey::Left: return "Left";
        case EditKey::Right: return "Right";
        case EditKey::Home: return "Home";
        case EditKey::End: return "End";
        case EditKey::Enter: return "Enter";
        case EditKey::Clear: return "Clear";
    }
    return "?";
}

std::optional<EditKey> edit_key_from(const KeyEvent &key) {
    switch (key.scancode) {
        case sc::Backspace: return EditKey::Backspace;
        case sc::Delete: return EditKey::Delete;
        case sc::Left: return EditKey::Left;
        case sc::Right: return EditKey::Right;
        case sc::Home: return EditKey::Home;
        case sc::End: return EditKey::End;
        case sc::Return:
        case sc::KpEnter: return EditKey::Enter;
        case sc::Clear: return EditKey::Clear;
        default: return std::nullopt;
    }
}

bool key_types_text(const KeyEvent &key) {
    // SDL keycodes of printable keys are their Unicode code point; others carry SDLK_SCANCODE_MASK.
    constexpr int kScancodeMask = 1 << 30;
    return key.keycode >= 0x20 && key.keycode != 0x7F && (key.keycode & kScancodeMask) == 0;
}

const char *action_name(Action action) {
    switch (action) {
        case Action::Unknown: return "Unknown";
        case Action::Up: return "Up";
        case Action::Down: return "Down";
        case Action::Left: return "Left";
        case Action::Right: return "Right";
        case Action::Confirm: return "Confirm";
        case Action::Back: return "Back";
        case Action::Exit: return "Exit";
        case Action::Home: return "Home";
        case Action::PlayPause: return "PlayPause";
        case Action::Play: return "Play";
        case Action::Pause: return "Pause";
        case Action::Stop: return "Stop";
        case Action::Rewind: return "Rewind";
        case Action::FastForward: return "FastForward";
        case Action::ChannelUp: return "ChannelUp";
        case Action::ChannelDown: return "ChannelDown";
        case Action::Red: return "Red";
        case Action::Green: return "Green";
        case Action::Yellow: return "Yellow";
        case Action::Blue: return "Blue";
        case Action::VolumeUp: return "VolumeUp";
        case Action::VolumeDown: return "VolumeDown";
        case Action::Mute: return "Mute";
        case Action::Count: break;
    }
    return "?";
}

const char *provenance_name(Provenance p) {
    switch (p) {
        case Provenance::Source: return "source";
        case Provenance::Measured: return "measured";
        case Provenance::Assumed: return "assumed";
        case Provenance::Host: return "host";
    }
    return "?";
}

const KeyBinding *KeyMap::find(int scancode, int keycode) const {
    for (const auto &b : bindings_) {
        if (b.field == F::Scancode && b.code == scancode && scancode != 0) return &b;
    }
    for (const auto &b : bindings_) {
        if (b.field == F::Keycode && b.code == keycode && keycode != 0) return &b;
    }
    return nullptr;
}

Action KeyMap::lookup(int scancode, int keycode) const {
    const KeyBinding *b = find(scancode, keycode);
    return b != nullptr ? b->action : Action::Unknown;
}

KeyMap host_keymap() {
    const auto H = Provenance::Host;
    return KeyMap({
        {F::Scancode, sc::Up, Action::Up, H, "arrow"},
        {F::Scancode, sc::Down, Action::Down, H, "arrow"},
        {F::Scancode, sc::Left, Action::Left, H, "arrow"},
        {F::Scancode, sc::Right, Action::Right, H, "arrow"},
        {F::Scancode, sc::Return, Action::Confirm, H, "Enter"},
        {F::Scancode, sc::KpEnter, Action::Confirm, H, "keypad Enter"},
        {F::Scancode, sc::Escape, Action::Back, H, "Escape"},
        {F::Scancode, sc::Backspace, Action::Back, H, "Backspace"},
        {F::Scancode, sc::AcBack, Action::Back, H, "AC Back"},
        {F::Scancode, sc::Home, Action::Home, H, "Home"},
        {F::Scancode, sc::F12, Action::Exit, H, "F12 stands in for the remote's Exit"},
        {F::Scancode, sc::Space, Action::PlayPause, H, "Space"},
        {F::Scancode, sc::AudioPlay, Action::PlayPause, H, "media key"},
        {F::Scancode, sc::AudioStop, Action::Stop, H, "media key"},
        {F::Scancode, sc::AudioRewind, Action::Rewind, H, "media key"},
        {F::Scancode, sc::AudioFastForward, Action::FastForward, H, "media key"},
        {F::Scancode, sc::F1, Action::Red, H, "F1 stands in for Red"},
        {F::Scancode, sc::F2, Action::Green, H, "F2 stands in for Green"},
        {F::Scancode, sc::F3, Action::Yellow, H, "F3 stands in for Yellow"},
        {F::Scancode, sc::F4, Action::Blue, H, "F4 stands in for Blue"},
        {F::Scancode, sc::Mute, Action::Mute, H, "media key"},
        {F::Scancode, sc::VolumeUp, Action::VolumeUp, H, "media key"},
        {F::Scancode, sc::VolumeDown, Action::VolumeDown, H, "media key"},
    });
}

KeyMap webos_sdl2_keymap() {
    const auto S = Provenance::Source;
    const auto M = Provenance::Measured;
    const auto A = Provenance::Assumed;
    // Measured = captured with the kit's bundled SDL-webOS 2.30.12 on an LG OLED65C1PTB, webOS 6.5.3,
    // Magic Remote, 2026-10-11 (docs/compatibility/records.json). Other models remain unmeasured.
    return KeyMap({
        {F::Scancode, sc::Up, Action::Up, M, "standard scancode 82"},
        {F::Scancode, sc::Down, Action::Down, M, "standard scancode 81"},
        {F::Scancode, sc::Left, Action::Left, M, "standard scancode 80"},
        {F::Scancode, sc::Right, Action::Right, M, "standard scancode 79"},
        {F::Scancode, sc::Return, Action::Confirm, M, "OK = SDL_SCANCODE_RETURN 40"},
        {F::Scancode, sc::KpEnter, Action::Confirm, S, "keypad Enter"},
        // webOS block, SDL-webOS include/SDL_scancode.h.
        {F::Scancode, sc::WebOSBack, Action::Back, M, "SDL_SCANCODE_WEBOS_BACK; needs KEYS_BACK hint"},
        {F::Scancode, sc::WebOSExit, Action::Exit, S, "SDL_SCANCODE_WEBOS_EXIT; needs KEYS_EXIT hint"},
        {F::Scancode, sc::WebOSHome, Action::Home, S, "SDL_SCANCODE_WEBOS_HOME; only with KEYS_HOME (not requested)"},
        {F::Scancode, sc::WebOSChUp, Action::ChannelUp, M, "SDL_SCANCODE_WEBOS_CH_UP"},
        {F::Scancode, sc::WebOSChDown, Action::ChannelDown, M, "SDL_SCANCODE_WEBOS_CH_DOWN"},
        {F::Scancode, sc::WebOSRed, Action::Red, M, "SDL_SCANCODE_WEBOS_RED"},
        {F::Scancode, sc::WebOSGreen, Action::Green, M, "SDL_SCANCODE_WEBOS_GREEN"},
        {F::Scancode, sc::WebOSYellow, Action::Yellow, M, "SDL_SCANCODE_WEBOS_YELLOW"},
        {F::Scancode, sc::WebOSBlue, Action::Blue, M, "SDL_SCANCODE_WEBOS_BLUE"},
        // Transport: SDL-webOS's evdev table maps KEY_PLAY/KEY_PLAYPAUSE→AUDIOPLAY, KEY_FASTFORWARD,
        // KEY_REWIND, KEY_STOP, KEY_PAUSE. Whether LG's compositor delivers them that way is unknown.
        {F::Scancode, sc::AudioPlay, Action::PlayPause, A, "evdev KEY_PLAY/KEY_PLAYPAUSE via linux table"},
        {F::Scancode, sc::Pause, Action::Pause, A, "evdev KEY_PAUSE via linux table"},
        {F::Scancode, sc::AudioStop, Action::Stop, A, "evdev KEY_STOPCD via linux table"},
        {F::Scancode, sc::Stop, Action::Stop, A, "evdev KEY_STOP via linux table"},
        {F::Scancode, sc::AudioRewind, Action::Rewind, A, "evdev KEY_REWIND via linux table"},
        {F::Scancode, sc::AudioFastForward, Action::FastForward, A, "evdev KEY_FASTFORWARD via linux table"},
        // Codes LG's system SDL produced on webOS 4.5 (PlxNative remote-keys.md). Kept as Assumed in
        // case SDL-webOS forwards LG's private codes unchanged.
        {F::Scancode, 450, Action::Play, A, "LG system SDL Play (plx measurement, other SDL)"},
        {F::Scancode, 451, Action::FastForward, A, "LG system SDL FF (plx measurement, other SDL)"},
        {F::Scancode, 452, Action::Rewind, A, "LG system SDL RW (plx measurement, other SDL)"},
        {F::Scancode, sc::Mute, Action::Mute, A, "volume keys are normally kept by the TV"},
        {F::Scancode, sc::VolumeUp, Action::VolumeUp, A, "volume keys are normally kept by the TV"},
        {F::Scancode, sc::VolumeDown, Action::VolumeDown, A, "volume keys are normally kept by the TV"},
        // Development keyboards plugged into the TV.
        {F::Scancode, sc::Escape, Action::Back, Provenance::Host, "USB keyboard Escape"},
    });
}

KeyMap default_keymap() {
#if defined(WNK_PLATFORM_WEBOS)
    return webos_sdl2_keymap();
#else
    return host_keymap();
#endif
}

std::optional<ActionEvent> ActionMapper::map(const KeyEvent &event) {
    auto it = std::find_if(held_.begin(), held_.end(), [&](const Held &h) { return h.scancode == event.scancode; });
    if (!event.down) {
        if (it == held_.end()) return std::nullopt;  // unpaired key-up
        Held h = *it;
        held_.erase(it);
        if (h.action == Action::Unknown && !report_unknown_) return std::nullopt;
        return ActionEvent{h.action, Phase::Release, event.time_ms - h.since_ms, event};
    }
    if (it != held_.end()) {
        // Down while already held: an auto-repeat (flagged or not).
        it->last = event;
        if (it->action == Action::Unknown && !report_unknown_) return std::nullopt;
        return ActionEvent{it->action, Phase::Repeat, event.time_ms - it->since_ms, event};
    }
    const Action action = map_.lookup(event.scancode, event.keycode);
    if (event.repeat) {
        // A repeat whose press we never saw: start tracking but do not report a Press.
        held_.push_back({event.scancode, action, event.time_ms, event});
        return std::nullopt;
    }
    held_.push_back({event.scancode, action, event.time_ms, event});
    if (action == Action::Unknown && !report_unknown_) return std::nullopt;
    return ActionEvent{action, Phase::Press, 0, event};
}

std::vector<ActionEvent> ActionMapper::release_all(uint32_t time_ms) {
    std::vector<ActionEvent> out;
    for (const auto &h : held_) {
        if (h.action == Action::Unknown && !report_unknown_) continue;
        KeyEvent up = h.last;
        up.down = false;
        up.repeat = false;
        up.time_ms = time_ms;
        out.push_back({h.action, Phase::Release, time_ms - h.since_ms, up});
    }
    held_.clear();
    return out;
}

}  // namespace wnk::input
