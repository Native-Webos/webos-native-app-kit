// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/input/input.hpp"

#include <set>

using namespace wnk::input;

namespace {

KeyEvent down(int scancode, uint32_t t, bool repeat = false) { return {scancode, 0, true, repeat, t}; }
KeyEvent up(int scancode, uint32_t t) { return {scancode, 0, false, false, t}; }

}  // namespace

TEST("webOS keymap maps the SDL-webOS remote block") {
    KeyMap m = webos_sdl2_keymap();
    CHECK(m.lookup(482, 0) == Action::Back);
    CHECK(m.lookup(505, 0) == Action::Exit);
    CHECK(m.lookup(486, 0) == Action::Red);
    CHECK(m.lookup(489, 0) == Action::Blue);
    CHECK(m.lookup(480, 0) == Action::ChannelUp);
    CHECK(m.lookup(82, 0) == Action::Up);
    CHECK(m.lookup(40, 0) == Action::Confirm);
    CHECK(m.lookup(484, 0) == Action::Unknown);  // pointer show/hide is not an action
}

TEST("every webOS binding states its provenance, and transport keys are not claimed as sourced") {
    KeyMap m = webos_sdl2_keymap();
    for (const auto &b : m.bindings()) {
        CHECK(b.note != nullptr && b.note[0] != '\0');
        if (b.action == Action::PlayPause || b.action == Action::FastForward || b.action == Action::Rewind) {
            CHECK(b.provenance == Provenance::Assumed);
        }
    }
}

TEST("keys captured on the OLED65C1PTB are marked Measured, nothing else is") {
    KeyMap m = webos_sdl2_keymap();
    const std::set<int> measured = {79, 80, 81, 82, 40, 482, 480, 481, 486, 487, 488, 489};
    for (const auto &b : m.bindings()) {
        const bool is_measured = b.provenance == Provenance::Measured;
        CHECK(is_measured == (b.field == KeyBinding::Field::Scancode && measured.count(b.code) == 1));
    }
}

TEST("keymaps have no duplicate scancode bindings") {
    for (const KeyMap &m : {webos_sdl2_keymap(), host_keymap()}) {
        std::set<int> seen;
        for (const auto &b : m.bindings()) {
            if (b.field != KeyBinding::Field::Scancode) continue;
            CHECK(seen.insert(b.code).second);
        }
    }
}

TEST("host keymap covers navigation and Back") {
    KeyMap m = host_keymap();
    CHECK(m.lookup(41, 0) == Action::Back);  // Escape
    CHECK(m.lookup(42, 0) == Action::Back);  // Backspace
    CHECK(m.lookup(80, 0) == Action::Left);
    CHECK(m.lookup(88, 0) == Action::Confirm);
}

TEST("scancode bindings win over keycode bindings") {
    KeyMap m({{KeyBinding::Field::Keycode, 13, Action::Back, Provenance::Host, "kc"},
              {KeyBinding::Field::Scancode, 40, Action::Confirm, Provenance::Host, "sc"}});
    CHECK(m.lookup(40, 13) == Action::Confirm);
    CHECK(m.lookup(99, 13) == Action::Back);
    CHECK(m.lookup(0, 0) == Action::Unknown);
}

TEST("press, repeat and release with held time") {
    ActionMapper mapper(host_keymap());
    auto p = mapper.map(down(82, 1000));
    REQUIRE(p.has_value());
    CHECK(p->action == Action::Up);
    CHECK(p->phase == Phase::Press);
    auto r = mapper.map(down(82, 1400, true));
    REQUIRE(r.has_value());
    CHECK(r->phase == Phase::Repeat);
    CHECK_EQ(r->held_ms, 400u);
    auto u = mapper.map(up(82, 1600));
    REQUIRE(u.has_value());
    CHECK(u->phase == Phase::Release);
    CHECK_EQ(u->held_ms, 600u);
    CHECK_EQ(mapper.held_count(), 0u);
}

TEST("unpaired key-up and orphan repeats are dropped") {
    ActionMapper mapper(host_keymap());
    CHECK(!mapper.map(up(82, 10)).has_value());
    CHECK(!mapper.map(down(82, 20, true)).has_value());  // repeat without a seen press
    auto r = mapper.map(down(82, 30, true));
    REQUIRE(r.has_value());
    CHECK(r->phase == Phase::Repeat);
    CHECK(mapper.map(up(82, 40)).has_value());
}

TEST("unknown keys produce no events unless asked for") {
    ActionMapper quiet(host_keymap());
    CHECK(!quiet.map(down(4, 1)).has_value());  // 'A'
    CHECK(!quiet.map(up(4, 2)).has_value());
    CHECK_EQ(quiet.held_count(), 0u);
    ActionMapper loud(host_keymap(), true);
    auto e = loud.map(down(4, 1));
    REQUIRE(e.has_value());
    CHECK(e->action == Action::Unknown);
}

TEST("release_all synthesises releases for held keys") {
    ActionMapper mapper(host_keymap());
    mapper.map(down(82, 100));
    mapper.map(down(40, 150));
    mapper.map(down(4, 160));  // unknown, held but not reported
    auto rel = mapper.release_all(400);
    REQUIRE(rel.size() == 2u);
    for (const auto &e : rel) CHECK(e.phase == Phase::Release);
    CHECK_EQ(mapper.held_count(), 0u);
    CHECK(!mapper.map(up(82, 500)).has_value());  // the real key-up arriving later is now unpaired
}

TEST("action names are defined for every action") {
    for (int a = 0; a < static_cast<int>(Action::Count); ++a) {
        CHECK(std::string(action_name(static_cast<Action>(a))) != "?");
    }
}
