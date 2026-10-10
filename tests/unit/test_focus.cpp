// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/focus/focus.hpp"

#include <vector>

using namespace wnk;
using namespace wnk::focus;

namespace {

NodeSpec mk(NodeId id, ScopeId scope, Rect rect) {
    NodeSpec n;
    n.id = id;
    n.scope = scope;
    n.rect = rect;
    return n;
}

// A 3x3 grid of 100x100 tiles with 20 gaps; ids 1..9 in reading order.
void add_grid(FocusManager &fm, ScopeId scope = kRootScope, NodeId base = 1, float ox = 0, float oy = 0) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            NodeSpec n;
            n.id = base + static_cast<NodeId>(r * 3 + c);
            n.scope = scope;
            n.rect = {ox + c * 120.0f, oy + r * 120.0f, 100, 100};
            fm.upsert(n);
        }
    }
}

}  // namespace

TEST("first arrow focuses the first node in reading order") {
    FocusManager fm;
    add_grid(fm);
    CHECK_EQ(fm.focused(), kNoNode);
    CHECK(fm.move(Direction::Down));
    CHECK_EQ(fm.focused(), 1u);
}

TEST("grid navigation moves to direct neighbours and stops at edges") {
    FocusManager fm;
    add_grid(fm);
    REQUIRE(fm.focus(5));
    CHECK(fm.move(Direction::Right));
    CHECK_EQ(fm.focused(), 6u);
    CHECK(!fm.move(Direction::Right));
    CHECK_EQ(fm.focused(), 6u);
    CHECK(fm.move(Direction::Up));
    CHECK_EQ(fm.focused(), 3u);
    CHECK(fm.move(Direction::Left));
    CHECK_EQ(fm.focused(), 2u);
    CHECK(fm.move(Direction::Down));
    CHECK(fm.move(Direction::Down));
    CHECK_EQ(fm.focused(), 8u);
}

TEST("in-beam candidates beat closer out-of-beam ones") {
    FocusManager fm;
    fm.upsert(mk(1, kRootScope, {0, 0, 100, 100}));
    fm.upsert(mk(2, kRootScope, {400, 0, 100, 100}));    // far, same row
    fm.upsert(mk(3, kRootScope, {150, 300, 100, 100}));  // nearer horizontally but another row
    REQUIRE(fm.focus(1));
    CHECK(fm.move(Direction::Right));
    CHECK_EQ(fm.focused(), 2u);
}

TEST("ties break deterministically by id") {
    FocusManager fm;
    fm.upsert(mk(1, kRootScope, {100, 0, 100, 100}));
    fm.upsert(mk(7, kRootScope, {0, 200, 100, 100}));
    fm.upsert(mk(4, kRootScope, {200, 200, 100, 100}));  // same distance as 7 from node 1
    REQUIRE(fm.focus(1));
    CHECK(fm.move(Direction::Down));
    CHECK_EQ(fm.focused(), 4u);
}

TEST("explicit neighbours override geometry and kBlocked stops movement") {
    FocusManager fm;
    add_grid(fm);
    NodeSpec n = *fm.node(1);
    n.neighbours.right = 9;
    n.neighbours.down = kBlocked;
    fm.upsert(n);
    REQUIRE(fm.focus(1));
    CHECK(!fm.move(Direction::Down));
    CHECK_EQ(fm.focused(), 1u);
    CHECK(fm.move(Direction::Right));
    CHECK_EQ(fm.focused(), 9u);
}

TEST("stale explicit neighbour falls back to geometry") {
    FocusManager fm;
    add_grid(fm);
    NodeSpec n = *fm.node(1);
    n.neighbours.right = 42;  // does not exist
    fm.upsert(n);
    REQUIRE(fm.focus(1));
    CHECK(fm.move(Direction::Right));
    CHECK_EQ(fm.focused(), 2u);
}

TEST("disabled and empty nodes are skipped") {
    FocusManager fm;
    add_grid(fm);
    fm.set_focusable(2, false);
    REQUIRE(fm.focus(1));
    CHECK(fm.move(Direction::Right));
    CHECK_EQ(fm.focused(), 3u);
    CHECK(!fm.focus(2));
    fm.set_rect(4, {0, 120, 0, 0});
    CHECK(!fm.focus(4));
}

TEST("contain scope keeps arrows inside") {
    FocusManager fm;
    REQUIRE(fm.add_scope({10, kRootScope, true}));
    fm.upsert(mk(1, 10, {0, 0, 100, 100}));
    fm.upsert(mk(2, 10, {120, 0, 100, 100}));
    fm.upsert(mk(3, kRootScope, {240, 0, 100, 100}));
    REQUIRE(fm.focus(2));
    CHECK(!fm.move(Direction::Right));
    CHECK_EQ(fm.focused(), 2u);
}

TEST("rows remember their column when re-entered") {
    FocusManager fm;
    fm.add_scope({1, kRootScope});
    fm.add_scope({2, kRootScope});
    for (NodeId i = 0; i < 5; ++i) {
        fm.upsert(mk(10 + i, 1, {i * 120.0f, 0, 100, 100}));
        fm.upsert(mk(20 + i, 2, {i * 120.0f, 200, 100, 100}));
    }
    REQUIRE(fm.focus(10));
    CHECK(fm.move(Direction::Down));
    CHECK_EQ(fm.focused(), 20u);
    for (int i = 0; i < 3; ++i) fm.move(Direction::Right);
    CHECK_EQ(fm.focused(), 23u);
    CHECK(fm.move(Direction::Up));
    CHECK_EQ(fm.focused(), 10u);  // row 1 remembers column 0, not the geometric neighbour 13
    CHECK(fm.move(Direction::Down));
    CHECK_EQ(fm.focused(), 23u);  // row 2 remembers column 3
}

TEST("modal traps focus and restores it when popped") {
    FocusManager fm;
    add_grid(fm);
    fm.add_scope({50, kRootScope});
    fm.upsert(mk(100, 50, {500, 500, 200, 80}));
    fm.upsert(mk(101, 50, {720, 500, 200, 80}));
    REQUIRE(fm.focus(5));
    REQUIRE(fm.push_modal(50));
    CHECK_EQ(fm.focused(), 100u);
    CHECK(!fm.focus(1));  // outside the modal
    CHECK(fm.move(Direction::Right));
    CHECK_EQ(fm.focused(), 101u);
    CHECK(!fm.move(Direction::Up));  // grid is above, but trapped
    REQUIRE(fm.pop_modal());
    CHECK_EQ(fm.focused(), 5u);
    CHECK_EQ(fm.modal_depth(), 0u);
}

TEST("nested modals unwind in order") {
    FocusManager fm;
    add_grid(fm);
    fm.add_scope({50, kRootScope});
    fm.add_scope({60, kRootScope});
    fm.upsert(mk(100, 50, {500, 500, 100, 100}));
    fm.upsert(mk(200, 60, {800, 500, 100, 100}));
    fm.focus(3);
    fm.push_modal(50);
    fm.push_modal(60);
    CHECK_EQ(fm.focused(), 200u);
    fm.pop_modal();
    CHECK_EQ(fm.focused(), 100u);
    fm.pop_modal();
    CHECK_EQ(fm.focused(), 3u);
}

TEST("removing the focused node recovers to a sensible neighbour") {
    FocusManager fm;
    add_grid(fm);
    REQUIRE(fm.focus(5));
    REQUIRE(fm.remove(5));
    CHECK(fm.focused() != kNoNode);
    const NodeSpec *n = fm.node(fm.focused());
    REQUIRE(n != nullptr);
    // One of the four direct neighbours of the removed centre tile (ties by id → 2).
    CHECK_EQ(fm.focused(), 2u);
}

TEST("removing a scope removes its nodes and closes its modal") {
    FocusManager fm;
    add_grid(fm);
    fm.add_scope({50, kRootScope});
    fm.upsert(mk(100, 50, {500, 500, 100, 100}));
    fm.focus(7);
    fm.push_modal(50);
    REQUIRE(fm.remove_scope(50));
    CHECK_EQ(fm.modal_depth(), 0u);
    CHECK(fm.node(100) == nullptr);
    CHECK_EQ(fm.focused(), 7u);
}

TEST("listener sees every focus change once") {
    FocusManager fm;
    add_grid(fm);
    std::vector<std::pair<NodeId, NodeId>> changes;
    fm.set_listener([&](NodeId a, NodeId b) { changes.push_back({a, b}); });
    fm.focus(1);
    fm.focus(1);  // no change, no callback
    fm.move(Direction::Right);
    REQUIRE(changes.size() == 2u);
    CHECK_EQ(changes[0].second, 1u);
    CHECK_EQ(changes[1].first, 1u);
    CHECK_EQ(changes[1].second, 2u);
}

TEST("enter_scope uses remembered node, else reading order") {
    FocusManager fm;
    fm.add_scope({1, kRootScope});
    add_grid(fm, 1, 1);
    CHECK(fm.enter_scope(1));
    CHECK_EQ(fm.focused(), 1u);
    fm.focus(6);
    fm.focus(1);
    CHECK_EQ(fm.remembered(1), 1u);
    CHECK(!fm.enter_scope(99));
}
