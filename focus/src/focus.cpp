// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/focus/focus.hpp"

#include <cmath>
#include <limits>
#include <tuple>

namespace wnk::focus {
namespace {

// Android's FocusFinder candidate rule: the candidate must lie further in the direction than the
// source, measured on both edges, so overlapping or "behind" nodes are never chosen.
bool in_direction(const Rect &src, const Rect &dst, Direction dir) {
    switch (dir) {
        case Direction::Right: return (src.x < dst.x || src.right() <= dst.x) && src.right() < dst.right();
        case Direction::Left: return (src.right() > dst.right() || src.x >= dst.right()) && src.x > dst.x;
        case Direction::Down: return (src.y < dst.y || src.bottom() <= dst.y) && src.bottom() < dst.bottom();
        case Direction::Up: return (src.bottom() > dst.bottom() || src.y >= dst.bottom()) && src.y > dst.y;
    }
    return false;
}

// Overlap on the axis perpendicular to movement ("beam").
bool in_beam(const Rect &src, const Rect &dst, Direction dir) {
    if (dir == Direction::Left || dir == Direction::Right) return dst.y < src.bottom() && dst.bottom() > src.y;
    return dst.x < src.right() && dst.right() > src.x;
}

float major_distance(const Rect &src, const Rect &dst, Direction dir) {
    switch (dir) {
        case Direction::Right: return std::max(0.0f, dst.x - src.right());
        case Direction::Left: return std::max(0.0f, src.x - dst.right());
        case Direction::Down: return std::max(0.0f, dst.y - src.bottom());
        case Direction::Up: return std::max(0.0f, src.y - dst.bottom());
    }
    return 0;
}

float minor_distance(const Rect &src, const Rect &dst, Direction dir) {
    if (dir == Direction::Left || dir == Direction::Right) return std::fabs(dst.cy() - src.cy());
    return std::fabs(dst.cx() - src.cx());
}

NodeId explicit_neighbour(const NodeSpec &n, Direction dir) {
    switch (dir) {
        case Direction::Up: return n.neighbours.up;
        case Direction::Down: return n.neighbours.down;
        case Direction::Left: return n.neighbours.left;
        case Direction::Right: return n.neighbours.right;
    }
    return kNoNode;
}

}  // namespace

FocusManager::FocusManager() { scopes_[kRootScope] = ScopeState{ScopeSpec{kRootScope, kRootScope, false, true}}; }

bool FocusManager::add_scope(const ScopeSpec &spec) {
    if (spec.id == kRootScope || scopes_.count(spec.id) || !scopes_.count(spec.parent)) return false;
    scopes_[spec.id] = ScopeState{spec};
    return true;
}

bool FocusManager::remove_scope(ScopeId id) {
    if (id == kRootScope || !scopes_.count(id)) return false;
    // Close modals that live inside the scope first, so focus restoration still sees valid nodes.
    while (!modals_.empty() && is_descendant(modals_.back().scope, id)) pop_modal();
    std::vector<NodeId> doomed_nodes;
    for (const auto &[nid, n] : nodes_) {
        if (is_descendant(n.scope, id)) doomed_nodes.push_back(nid);
    }
    for (NodeId nid : doomed_nodes) remove(nid);
    std::vector<ScopeId> doomed_scopes;
    for (const auto &[sid, s] : scopes_) {
        if (sid != kRootScope && is_descendant(sid, id)) doomed_scopes.push_back(sid);
    }
    for (ScopeId sid : doomed_scopes) scopes_.erase(sid);
    return true;
}

bool FocusManager::upsert(const NodeSpec &spec) {
    if (spec.id == kNoNode || spec.id == kBlocked || !scopes_.count(spec.scope)) return false;
    nodes_[spec.id] = spec;
    if (spec.id == focused_ && !allowed(spec)) {
        const NodeId next = fallback_after_loss(spec);
        set_focus(next);
    }
    return true;
}

bool FocusManager::remove(NodeId id) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    const NodeSpec lost = it->second;
    NodeId next = kNoNode;
    if (focused_ == id) next = fallback_after_loss(lost);
    nodes_.erase(it);
    for (auto &[sid, s] : scopes_) {
        if (s.remembered == id) s.remembered = kNoNode;
    }
    for (auto &m : modals_) {
        if (m.previous_focus == id) m.previous_focus = kNoNode;
    }
    if (focused_ == id) set_focus(next);
    return true;
}

bool FocusManager::set_rect(NodeId id, const Rect &rect) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    it->second.rect = rect;
    return true;
}

bool FocusManager::set_focusable(NodeId id, bool focusable) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    NodeSpec spec = it->second;
    spec.focusable = focusable;
    return upsert(spec);
}

const NodeSpec *FocusManager::node(NodeId id) const {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

bool FocusManager::is_descendant(ScopeId scope, ScopeId ancestor) const {
    for (int guard = 0; guard < 256; ++guard) {
        if (scope == ancestor) return true;
        if (scope == kRootScope) return false;
        auto it = scopes_.find(scope);
        if (it == scopes_.end()) return false;
        scope = it->second.spec.parent;
    }
    return false;
}

bool FocusManager::allowed(const NodeSpec &n) const {
    return n.focusable && !n.rect.empty() && is_descendant(n.scope, active_modal());
}

ScopeId FocusManager::containing_scope(ScopeId scope) const {
    for (int guard = 0; guard < 256 && scope != kRootScope; ++guard) {
        auto it = scopes_.find(scope);
        if (it == scopes_.end()) break;
        if (it->second.spec.contain) return scope;
        scope = it->second.spec.parent;
    }
    return kRootScope;
}

bool FocusManager::focus(NodeId id) {
    const NodeSpec *n = node(id);
    if (n == nullptr || !allowed(*n)) return false;
    set_focus(id);
    return true;
}

NodeId FocusManager::first_in_scope(ScopeId scope) const {
    NodeId best = kNoNode;
    std::tuple<float, float, NodeId> best_key{};
    for (const auto &[id, n] : nodes_) {
        if (!allowed(n) || !is_descendant(n.scope, scope)) continue;
        auto key = std::make_tuple(n.rect.y, n.rect.x, id);
        if (best == kNoNode || key < best_key) {
            best = id;
            best_key = key;
        }
    }
    return best;
}

bool FocusManager::enter_scope(ScopeId id) {
    auto it = scopes_.find(id);
    if (it == scopes_.end()) return false;
    NodeId target = it->second.remembered;
    const NodeSpec *n = node(target);
    if (n == nullptr || !allowed(*n) || !is_descendant(n->scope, id)) target = first_in_scope(id);
    if (target == kNoNode) return false;
    set_focus(target);
    return true;
}

NodeId FocusManager::best_in_direction(const NodeSpec &from, Direction dir) const {
    const ScopeId fence = containing_scope(from.scope);
    NodeId best = kNoNode;
    std::tuple<int, float, NodeId> best_key{};
    for (const auto &[id, n] : nodes_) {
        if (id == from.id || !allowed(n) || !is_descendant(n.scope, fence)) continue;
        if (!in_direction(from.rect, n.rect, dir)) continue;
        const float major = major_distance(from.rect, n.rect, dir);
        const float minor = minor_distance(from.rect, n.rect, dir);
        // Beam candidates always win; then Android's weighting favours the major axis.
        auto key = std::make_tuple(in_beam(from.rect, n.rect, dir) ? 0 : 1, 13.0f * major * major + minor * minor, id);
        if (best == kNoNode || key < best_key) {
            best = id;
            best_key = key;
        }
    }
    return best;
}

bool FocusManager::move(Direction dir) {
    const NodeSpec *from = node(focused_);
    if (from == nullptr) {
        // Nothing focused yet: any arrow focuses the first node of the active modal (or root).
        const NodeId first = first_in_scope(active_modal());
        if (first == kNoNode) return false;
        set_focus(first);
        return true;
    }

    NodeId target = explicit_neighbour(*from, dir);
    if (target == kBlocked) return false;
    if (target != kNoNode) {
        const NodeSpec *t = node(target);
        if (t == nullptr || !allowed(*t) || !is_descendant(t->scope, containing_scope(from->scope))) {
            target = kNoNode;  // stale or disallowed override: fall back to geometry
        }
    }
    if (target == kNoNode) target = best_in_direction(*from, dir);
    if (target == kNoNode) return false;

    // Entering a different scope by arrow: go to the node that scope remembers (row/column memory).
    const NodeSpec *t = node(target);
    if (t->scope != from->scope && !is_descendant(from->scope, t->scope)) {
        auto it = scopes_.find(t->scope);
        if (it != scopes_.end() && it->second.spec.restore_on_enter) {
            const NodeSpec *r = node(it->second.remembered);
            if (r != nullptr && allowed(*r) && r->id != from->id) target = r->id;
        }
    }
    set_focus(target);
    return true;
}

bool FocusManager::push_modal(ScopeId id) {
    if (!scopes_.count(id) || id == kRootScope) return false;
    modals_.push_back({id, focused_});
    if (!enter_scope(id)) set_focus(kNoNode);
    return true;
}

bool FocusManager::pop_modal() {
    if (modals_.empty()) return false;
    const ModalState m = modals_.back();
    modals_.pop_back();
    const NodeSpec *prev = node(m.previous_focus);
    if (prev != nullptr && allowed(*prev)) {
        set_focus(prev->id);
    } else {
        set_focus(first_in_scope(active_modal()));
    }
    return true;
}

NodeId FocusManager::remembered(ScopeId id) const {
    auto it = scopes_.find(id);
    return it == scopes_.end() ? kNoNode : it->second.remembered;
}

NodeId FocusManager::fallback_after_loss(const NodeSpec &lost) const {
    auto it = scopes_.find(lost.scope);
    if (it != scopes_.end() && it->second.remembered != lost.id) {
        const NodeSpec *r = node(it->second.remembered);
        if (r != nullptr && allowed(*r)) return r->id;
    }
    NodeId best = kNoNode;
    std::tuple<int, float, NodeId> best_key{};
    for (const auto &[id, n] : nodes_) {
        if (id == lost.id || !allowed(n)) continue;
        const float dx = n.rect.cx() - lost.rect.cx(), dy = n.rect.cy() - lost.rect.cy();
        // Prefer the same scope, then the nearest node.
        auto key = std::make_tuple(n.scope == lost.scope ? 0 : 1, dx * dx + dy * dy, id);
        if (best == kNoNode || key < best_key) {
            best = id;
            best_key = key;
        }
    }
    return best;
}

void FocusManager::remember(NodeId id) {
    const NodeSpec *n = node(id);
    if (n == nullptr) return;
    ScopeId s = n->scope;
    for (int guard = 0; guard < 256; ++guard) {
        auto it = scopes_.find(s);
        if (it == scopes_.end()) break;
        it->second.remembered = id;
        if (s == kRootScope) break;
        s = it->second.spec.parent;
    }
}

void FocusManager::set_focus(NodeId id) {
    if (id == focused_) return;
    const NodeId from = focused_;
    focused_ = id;
    remember(id);
    if (listener_) listener_(from, id);
}

}  // namespace wnk::focus
