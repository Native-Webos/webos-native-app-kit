// SPDX-License-Identifier: GPL-3.0-or-later
//
// Central focus manager for remote-control navigation.
//
// The manager knows nodes (id, rectangle, scope, optional explicit neighbours) but not widgets, so
// any UI layer can drive it and it is testable without rendering. It provides:
//   * directional navigation by geometry (deterministic tie-breaking),
//   * explicit neighbour overrides and blocked directions,
//   * nested scopes that can contain focus and remember their last focused node,
//   * a modal stack that traps focus and restores the previous focus when popped,
//   * focus recovery when the focused node is removed or disabled.
// Back handling is deliberately NOT here: the app decides what Back means (close modal, pop screen).
#pragma once

#include "wnk/geometry.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace wnk::focus {

using NodeId = uint32_t;
using ScopeId = uint32_t;

inline constexpr NodeId kNoNode = 0;
inline constexpr NodeId kBlocked = 0xFFFFFFFFu;  // explicit neighbour meaning "do not move this way"
inline constexpr ScopeId kRootScope = 0;

enum class Direction : uint8_t { Up, Down, Left, Right };

struct Neighbours {
    NodeId up = kNoNode, down = kNoNode, left = kNoNode, right = kNoNode;  // kNoNode = use geometry
};

struct NodeSpec {
    NodeId id = kNoNode;
    ScopeId scope = kRootScope;
    Rect rect;
    bool focusable = true;
    Neighbours neighbours;
};

struct ScopeSpec {
    ScopeId id = kRootScope;
    ScopeId parent = kRootScope;
    bool contain = false;          // arrows never leave this scope (e.g. a dialog's button row)
    bool restore_on_enter = true;  // entering the scope by arrows focuses its remembered node
};

class FocusManager {
 public:
    using Listener = std::function<void(NodeId from, NodeId to)>;

    FocusManager();

    // Scopes. The root scope (id 0) always exists. Removing a scope removes its nodes and child scopes.
    bool add_scope(const ScopeSpec &spec);
    bool remove_scope(ScopeId id);
    bool has_scope(ScopeId id) const { return scopes_.count(id) != 0; }

    // Nodes. upsert() adds a node or replaces its spec. Returns false for id 0/kBlocked or unknown scope.
    bool upsert(const NodeSpec &spec);
    bool remove(NodeId id);
    bool set_rect(NodeId id, const Rect &rect);
    bool set_focusable(NodeId id, bool focusable);
    const NodeSpec *node(NodeId id) const;
    size_t node_count() const { return nodes_.size(); }

    NodeId focused() const { return focused_; }
    // Focus a node directly. Fails if it is not focusable or lies outside the active modal.
    bool focus(NodeId id);
    // Focus the scope's remembered node, else its first node in reading order. Fails if empty.
    bool enter_scope(ScopeId id);
    // Move by direction. Returns true if focus changed.
    bool move(Direction dir);

    // Modal stack: focus is trapped in the scope (and its descendants) until pop_modal().
    bool push_modal(ScopeId id);
    bool pop_modal();
    ScopeId active_modal() const { return modals_.empty() ? kRootScope : modals_.back().scope; }
    size_t modal_depth() const { return modals_.size(); }

    NodeId remembered(ScopeId id) const;
    void set_listener(Listener listener) { listener_ = std::move(listener); }

 private:
    struct ScopeState {
        ScopeSpec spec;
        NodeId remembered = kNoNode;
    };
    struct ModalState {
        ScopeId scope;
        NodeId previous_focus;
    };

    bool is_descendant(ScopeId scope, ScopeId ancestor) const;
    bool allowed(const NodeSpec &n) const;  // focusable and inside the active modal
    ScopeId containing_scope(ScopeId scope) const;  // innermost `contain` scope at or above `scope`
    NodeId best_in_direction(const NodeSpec &from, Direction dir) const;
    NodeId first_in_scope(ScopeId scope) const;
    NodeId fallback_after_loss(const NodeSpec &lost) const;
    void set_focus(NodeId id);
    void remember(NodeId id);

    std::map<ScopeId, ScopeState> scopes_;
    std::map<NodeId, NodeSpec> nodes_;
    std::vector<ModalState> modals_;
    NodeId focused_ = kNoNode;
    Listener listener_;
};

}  // namespace wnk::focus
