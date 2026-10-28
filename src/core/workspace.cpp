#include "stormtty/workspace.hpp"
#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace stormtty {
namespace {
bool safe_text(std::string_view text, std::size_t maximum = 4096) {
    return text.size() <= maximum && detail::valid_utf8(text) && std::none_of(text.begin(), text.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}
bool valid_ratio(double r) { return std::isfinite(r) && r > 0.0 && r < 1.0; }
bool valid_direction(Direction d) {
    return d == Direction::left || d == Direction::right || d == Direction::up || d == Direction::down;
}
bool valid_viewport(Rect r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) && std::isfinite(r.height) &&
        std::abs(r.x) <= 1e9 && std::abs(r.y) <= 1e9 && r.width >= 0 && r.height >= 0 &&
        r.width <= 1e9 && r.height <= 1e9;
}
bool valid_metrics(LayoutMetrics m) {
    return std::isfinite(m.divider_gap) && m.divider_gap >= 0 && m.divider_gap <= 100 &&
        std::isfinite(m.divider_hit_width) && m.divider_hit_width > 0 && m.divider_hit_width <= 100 &&
        std::isfinite(m.minimum_pane_width) && m.minimum_pane_width > 0 && m.minimum_pane_width <= 1e6 &&
        std::isfinite(m.minimum_pane_height) && m.minimum_pane_height > 0 && m.minimum_pane_height <= 1e6;
}
Rect intersection(Rect a, Rect b) {
    const auto x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0, std::min(a.x + a.width, b.x + b.width) - x),
                  std::max(0.0, std::min(a.y + a.height, b.y + b.height) - y)};
}
Size minimum_size(const WorkspaceState& state, NodeId id, LayoutMetrics m) {
    const auto& n = state.nodes.at(id);
    if (n.is_leaf()) return {m.minimum_pane_width, m.minimum_pane_height};
    auto a = minimum_size(state, n.first, m), b = minimum_size(state, n.second, m);
    if (n.axis == Axis::horizontal) return {a.width + b.width + m.divider_gap, std::max(a.height, b.height)};
    return {std::max(a.width, b.width), a.height + b.height + m.divider_gap};
}
// No rounding: physical-pixel and terminal-grid rounding belong to the host.
// The stored fraction never changes in response to a DPI/window-size change.
double bounded_ratio(double ratio, double available, double first_min, double second_min) {
    if (available <= 0) return first_min / (first_min + second_min);
    if (available < first_min + second_min) return first_min / (first_min + second_min);
    return std::clamp(ratio, first_min / available, 1.0 - second_min / available);
}
}

bool Rect::contains(Point p) const noexcept {
    return p.x >= x && p.y >= y && p.x < x + width && p.y < y + height;
}
HitRegion hit_test(const Layout& layout, Point point) noexcept {
    // Deepest divider wins at intersections; traversal is root-first.
    for (auto it = layout.dividers.rbegin(); it != layout.dividers.rend(); ++it)
        if (it->hit_bounds.contains(point)) return {HitKind::divider, {}, it->node};
    for (const auto& p : layout.panes)
        if (p.bounds.contains(point)) return {HitKind::pane, p.pane, {}};
    return {};
}
const Tab* Workspace::tab(TabId id) const noexcept {
    auto it = std::find_if(state_.tabs.begin(), state_.tabs.end(), [&](const Tab& t) { return t.id == id; });
    return it == state_.tabs.end() ? nullptr : &*it;
}
const Pane* Workspace::pane(PaneId id) const noexcept {
    auto it = state_.panes.find(id); return it == state_.panes.end() ? nullptr : &it->second;
}
const Node* Workspace::node(NodeId id) const noexcept {
    auto it = state_.nodes.find(id); return it == state_.nodes.end() ? nullptr : &it->second;
}
PaneId Workspace::active_pane() const noexcept {
    const auto* t = tab(state_.active_tab); return t ? t->active_pane : PaneId{};
}
std::vector<PaneId> Workspace::pane_order(TabId id) const {
    std::vector<PaneId> result;
    const auto* t = tab(id); if (!t) return result;
    std::function<void(NodeId)> visit = [&](NodeId n) {
        const auto& item = state_.nodes.at(n);
        if (item.is_leaf()) result.push_back(item.pane);
        else { visit(item.first); visit(item.second); }
    };
    visit(t->root); return result;
}
std::optional<TabId> Workspace::tab_of(PaneId id) const {
    if (!pane(id)) return std::nullopt;
    for (const auto& t : state_.tabs) {
        auto order = pane_order(t.id);
        if (std::find(order.begin(), order.end(), id) != order.end()) return t.id;
    }
    return std::nullopt;
}
NodeId Workspace::leaf_of(PaneId id) const {
    for (const auto& [key, n] : state_.nodes) if (n.pane == id) return key;
    return {};
}
Status Workspace::validate() const {
    if (state_.tabs.size() > maximum_tabs || state_.panes.size() > maximum_panes ||
        state_.nodes.size() > maximum_panes * 2) return Status::failure("Workspace limit exceeded");
    if (state_.tabs.empty()) {
        if (!state_.nodes.empty() || !state_.panes.empty() || state_.active_tab)
            return Status::failure("Empty workspace has orphaned state");
    } else if (!tab(state_.active_tab)) return Status::failure("Active tab is stale");
    std::set<std::uint64_t> all_ids;
    std::set<NodeId> visited;
    std::set<PaneId> leaves;
    std::set<SessionId> sessions;
    auto unique = [&](std::uint64_t id) { return id && all_ids.insert(id).second; };
    for (const auto& [id, p] : state_.panes) {
        if (id != p.id || !unique(id.value)) return Status::failure("Invalid or duplicate pane ID");
        if (!safe_text(p.title) || p.profile.empty() || !safe_text(p.profile, 128))
            return Status::failure("Invalid pane metadata");
        if (p.session && !sessions.insert(p.session).second) return Status::failure("Session belongs to more than one pane");
    }
    for (const auto& [id, n] : state_.nodes) {
        if (id != n.id || !unique(id.value)) return Status::failure("Invalid or duplicate node ID");
        if (n.is_leaf()) {
            if (n.first || n.second || !pane(n.pane) || n.ratio != 0.5 || n.axis != Axis::horizontal) return Status::failure("Invalid leaf");
        } else if (!n.first || !n.second || n.first == n.second || !node(n.first) || !node(n.second) ||
                   !valid_ratio(n.ratio) || (n.axis != Axis::horizontal && n.axis != Axis::vertical))
            return Status::failure("Invalid split");
    }
    for (const auto& t : state_.tabs) {
        if (!unique(t.id.value) || !node(t.root) || !safe_text(t.title)) return Status::failure("Invalid tab");
        std::set<PaneId> tab_panes;
        std::vector<std::pair<NodeId, std::size_t>> stack{{t.root, 1}};
        while (!stack.empty()) {
            auto [id, depth] = stack.back(); stack.pop_back();
            if (depth > maximum_depth) return Status::failure("Split tree is too deep");
            if (!visited.insert(id).second) return Status::failure("Cycle or multiply owned node");
            const auto& n = state_.nodes.at(id);
            if (n.is_leaf()) {
                if (!leaves.insert(n.pane).second) return Status::failure("Pane appears more than once");
                tab_panes.insert(n.pane);
            } else { stack.emplace_back(n.second, depth + 1); stack.emplace_back(n.first, depth + 1); }
        }
        if (!tab_panes.contains(t.active_pane)) return Status::failure("Tab focus is stale");
        if (t.zoomed_pane && (*t.zoomed_pane != t.active_pane || !tab_panes.contains(*t.zoomed_pane)))
            return Status::failure("Tab zoom is stale or unfocused");
    }
    if (visited.size() != state_.nodes.size() || leaves.size() != state_.panes.size())
        return Status::failure("Workspace contains unreachable nodes or panes");
    if (!state_.next_id || state_.next_id > std::numeric_limits<std::uint64_t>::max() - 4 ||
        (!all_ids.empty() && state_.next_id <= *all_ids.rbegin())) return Status::failure("Invalid next ID counter");
    return {};
}
Result<Workspace> Workspace::from_state(WorkspaceState state) {
    Workspace w; w.state_ = std::move(state); auto status = w.validate();
    if (!status) return {{}, std::move(status.error)};
    return {std::move(w), {}};
}
Status Workspace::commit(Workspace&& w) {
    auto status = w.validate(); if (!status) return status;
    state_ = std::move(w.state_); return {};
}
void Workspace::normalize_focus() {
    for (auto& t : state_.tabs) {
        const auto order = pane_order(t.id);
        if (std::find(order.begin(), order.end(), t.active_pane) == order.end()) t.active_pane = order.front();
        if (t.zoomed_pane && *t.zoomed_pane != t.active_pane) t.zoomed_pane.reset();
    }
    if (!tab(state_.active_tab)) state_.active_tab = state_.tabs.empty() ? TabId{} : state_.tabs.front().id;
}
Result<TabId> Workspace::create_tab(SessionId session, std::string profile) {
    if (state_.tabs.size() >= maximum_tabs || state_.panes.size() >= maximum_panes)
        return {{}, "Workspace limit exceeded"};
    Workspace w = *this;
    TabId tid{w.state_.next_id++}; PaneId pid{w.state_.next_id++}; NodeId nid{w.state_.next_id++};
    w.state_.panes.emplace(pid, Pane{pid, session, std::move(profile), {}});
    w.state_.nodes.emplace(nid, Node{nid, pid, {}, {}, Axis::horizontal, 0.5});
    w.state_.tabs.push_back({tid, nid, pid, {}, {}}); w.state_.active_tab = tid;
    auto status = commit(std::move(w)); if (!status) return {{}, std::move(status.error)};
    return {tid, {}};
}
void Workspace::insert_leaf(NodeId leaf, PaneId target, Direction direction, double fraction) {
    const auto target_leaf = leaf_of(target);
    const auto target_tab = *tab_of(target);
    const NodeId branch{state_.next_id++};
    for (auto& [id, n] : state_.nodes) {
        if (id == leaf) continue;
        if (n.first == target_leaf) n.first = branch;
        if (n.second == target_leaf) n.second = branch;
    }
    for (auto& t : state_.tabs) if (t.id == target_tab && t.root == target_leaf) t.root = branch;
    const bool before = direction == Direction::left || direction == Direction::up;
    const auto axis = direction == Direction::left || direction == Direction::right ? Axis::horizontal : Axis::vertical;
    state_.nodes.emplace(branch, Node{branch, {}, before ? leaf : target_leaf, before ? target_leaf : leaf,
                                     axis, before ? fraction : 1.0 - fraction});
}
Result<PaneId> Workspace::split(PaneId target, Direction direction, SessionId session,
                                 std::string profile, double fraction) {
    if (!pane(target)) return {{}, "Pane no longer exists"};
    if (!valid_direction(direction) || !valid_ratio(fraction)) return {{}, "Invalid split direction or fraction"};
    if (state_.panes.size() >= maximum_panes) return {{}, "Pane limit exceeded"};
    Workspace w = *this; auto tid = *w.tab_of(target);
    PaneId pid{w.state_.next_id++}; NodeId nid{w.state_.next_id++};
    w.state_.panes.emplace(pid, Pane{pid, session, std::move(profile), {}});
    w.state_.nodes.emplace(nid, Node{nid, pid, {}, {}, Axis::horizontal, 0.5});
    w.insert_leaf(nid, target, direction, fraction);
    for (auto& t : w.state_.tabs) if (t.id == tid) { t.active_pane = pid; t.zoomed_pane.reset(); }
    w.state_.active_tab = tid;
    auto status = commit(std::move(w)); if (!status) return {{}, std::move(status.error)};
    return {pid, {}};
}
void Workspace::detach(PaneId pid, bool erase_pane) {
    const NodeId leaf = leaf_of(pid);
    const auto tid = *tab_of(pid);
    NodeId parent, sibling;
    for (const auto& [id, n] : state_.nodes) {
        if (n.first == leaf || n.second == leaf) { parent = id; sibling = n.first == leaf ? n.second : n.first; break; }
    }
    if (parent) {
        for (auto& [id, n] : state_.nodes) {
            (void)id;
            if (n.first == parent) n.first = sibling;
            if (n.second == parent) n.second = sibling;
        }
        for (auto& t : state_.tabs) if (t.id == tid && t.root == parent) t.root = sibling;
        state_.nodes.erase(parent);
    } else {
        auto it = std::find_if(state_.tabs.begin(), state_.tabs.end(), [&](const Tab& t) { return t.id == tid; });
        const auto index = static_cast<std::size_t>(it - state_.tabs.begin());
        state_.tabs.erase(it);
        if (state_.active_tab == tid) state_.active_tab = state_.tabs.empty() ? TabId{} : state_.tabs[std::min(index, state_.tabs.size() - 1)].id;
    }
    state_.nodes.erase(leaf); if (erase_pane) state_.panes.erase(pid);
}
Status Workspace::close_pane(PaneId id) {
    if (!pane(id)) return Status::failure("Pane no longer exists");
    Workspace w = *this; w.detach(id, true); w.normalize_focus(); return commit(std::move(w));
}
Status Workspace::close_tab(TabId id) {
    if (!tab(id)) return Status::failure("Tab no longer exists");
    Workspace w = *this;
    for (auto pid : pane_order(id)) w.detach(pid, true);
    w.normalize_focus(); return commit(std::move(w));
}
Status Workspace::activate_tab(TabId id) {
    if (!tab(id)) return Status::failure("Tab no longer exists");
    state_.active_tab = id; return {};
}
Status Workspace::focus(PaneId id) {
    auto tid = tab_of(id); if (!tid) return Status::failure("Pane no longer exists");
    for (auto& t : state_.tabs) if (t.id == *tid) {
        t.active_pane = id; if (t.zoomed_pane) t.zoomed_pane = id;
    }
    state_.active_tab = *tid; return {};
}
Status Workspace::set_zoom(PaneId id, bool zoomed) {
    auto tid = tab_of(id); if (!tid) return Status::failure("Pane no longer exists");
    for (auto& t : state_.tabs) if (t.id == *tid) {
        t.active_pane = id; t.zoomed_pane = zoomed ? std::optional<PaneId>(id) : std::nullopt;
    }
    state_.active_tab = *tid; return {};
}
Status Workspace::toggle_zoom() {
    auto* t = tab(state_.active_tab); if (!t) return Status::failure("No active tab");
    return set_zoom(t->active_pane, !t->zoomed_pane);
}
Status Workspace::set_split_ratio(NodeId id, double ratio) {
    auto* n = node(id); if (!n || n->is_leaf()) return Status::failure("Split no longer exists");
    if (!valid_ratio(ratio)) return Status::failure("Split ratio must be finite and strictly between zero and one");
    state_.nodes.at(id).ratio = ratio; return {};
}
Status Workspace::equalize_split(NodeId id) { return set_split_ratio(id, 0.5); }
Status Workspace::resize_split(NodeId id, double ratio, Rect viewport, LayoutMetrics metrics) {
    const auto* n = node(id); if (!n || n->is_leaf()) return Status::failure("Split no longer exists");
    if (!std::isfinite(ratio)) return Status::failure("Invalid split ratio");
    if (!valid_viewport(viewport) || !valid_metrics(metrics)) return Status::failure("Invalid layout geometry");
    for (const auto& t : state_.tabs) {
        const auto l = layout(t.id, viewport, metrics);
        for (const auto& d : l.dividers) if (d.node == id) {
            const auto a = minimum_size(state_, n->first, metrics), b = minimum_size(state_, n->second, metrics);
            const bool horizontal = n->axis == Axis::horizontal;
            const double extent = horizontal ? d.split_bounds.width : d.split_bounds.height;
            const double available = std::max(0.0, extent - metrics.divider_gap);
            if (available < (horizontal ? a.width + b.width : a.height + b.height))
                return Status::failure("Window is too small to resize this split within pane minimums");
            return set_split_ratio(id, bounded_ratio(ratio, available, horizontal ? a.width : a.height,
                                                     horizontal ? b.width : b.height));
        }
    }
    return Status::failure("Split is hidden by pane zoom");
}
Status Workspace::move_pane(PaneId source, PaneId target, Direction direction, double fraction) {
    if (!pane(source) || !pane(target)) return Status::failure("Pane no longer exists");
    if (source == target) return Status::failure("Cannot move a pane onto itself");
    if (!valid_direction(direction) || !valid_ratio(fraction)) return Status::failure("Invalid pane placement");
    Workspace w = *this; const auto leaf = *w.node(w.leaf_of(source));
    w.detach(source, false); w.state_.nodes.emplace(leaf.id, leaf);
    const auto tid = *w.tab_of(target); w.insert_leaf(leaf.id, target, direction, fraction);
    w.normalize_focus();
    for (auto& t : w.state_.tabs) if (t.id == tid) { t.active_pane = source; t.zoomed_pane.reset(); }
    w.state_.active_tab = tid; return commit(std::move(w));
}
Result<TabId> Workspace::move_pane_to_new_tab(PaneId source, std::string title) {
    if (!pane(source)) return {{}, "Pane no longer exists"};
    Workspace w = *this; const auto leaf = *w.node(w.leaf_of(source));
    w.detach(source, false); w.state_.nodes.emplace(leaf.id, leaf);
    const TabId tid{w.state_.next_id++};
    w.state_.tabs.push_back({tid, leaf.id, source, {}, std::move(title)});
    w.normalize_focus(); w.state_.active_tab = tid;
    auto status = commit(std::move(w)); if (!status) return {{}, std::move(status.error)};
    return {tid, {}};
}
Status Workspace::swap_panes(PaneId a, PaneId b) {
    if (!pane(a) || !pane(b)) return Status::failure("Pane no longer exists");
    if (a == b) return {};
    Workspace w = *this;
    std::swap(w.state_.nodes.at(w.leaf_of(a)).pane, w.state_.nodes.at(w.leaf_of(b)).pane);
    w.normalize_focus(); w.focus(a); return commit(std::move(w));
}
Status Workspace::rename_tab(TabId id, std::string title) {
    if (!tab(id)) return Status::failure("Tab no longer exists");
    if (!safe_text(title)) return Status::failure("Tab title is too long or contains control characters");
    for (auto& t : state_.tabs) if (t.id == id) t.title = std::move(title);
    return {};
}
Status Workspace::reorder_tab(TabId id, std::size_t index) {
    if (!tab(id)) return Status::failure("Tab no longer exists");
    if (index >= state_.tabs.size()) return Status::failure("Tab index is out of range");
    auto it = std::find_if(state_.tabs.begin(), state_.tabs.end(), [&](const Tab& t) { return t.id == id; });
    const auto old = static_cast<std::size_t>(it - state_.tabs.begin());
    if (old < index) std::rotate(it, it + 1, state_.tabs.begin() + static_cast<std::ptrdiff_t>(index + 1));
    else if (old > index) std::rotate(state_.tabs.begin() + static_cast<std::ptrdiff_t>(index), it, it + 1);
    return {};
}
Status Workspace::set_pane_title(PaneId id, std::string title) {
    if (!pane(id)) return Status::failure("Pane no longer exists");
    if (!safe_text(title)) return Status::failure("Pane title is too long or contains control characters");
    state_.panes.at(id).title = std::move(title); return {};
}
Status Workspace::bind_session(PaneId id, SessionId session) {
    if (!pane(id)) return Status::failure("Pane no longer exists");
    for (const auto& [other, p] : state_.panes)
        if (session && other != id && p.session == session) return Status::failure("Session already belongs to a pane");
    state_.panes.at(id).session = session; return {};
}
Layout Workspace::layout(TabId id, Rect viewport, LayoutMetrics metrics) const {
    Layout output;
    auto* t = tab(id); if (!t) { output.error = "Tab no longer exists"; return output; }
    if (!valid_viewport(viewport) || !valid_metrics(metrics)) { output.error = "Invalid layout geometry"; return output; }
    if (t->zoomed_pane) {
        output.panes.push_back({*t->zoomed_pane, pane(*t->zoomed_pane)->session, viewport});
        output.constrained = viewport.width < metrics.minimum_pane_width || viewport.height < metrics.minimum_pane_height;
        return output;
    }
    std::map<NodeId, Size> minimums;
    std::function<Size(NodeId)> measure = [&](NodeId id_) {
        const auto& n = state_.nodes.at(id_);
        Size s{metrics.minimum_pane_width, metrics.minimum_pane_height};
        if (!n.is_leaf()) {
            const auto a = measure(n.first), b = measure(n.second);
            s = n.axis == Axis::horizontal ? Size{a.width + b.width + metrics.divider_gap, std::max(a.height, b.height)}
                                          : Size{std::max(a.width, b.width), a.height + b.height + metrics.divider_gap};
        }
        minimums.emplace(id_, s); return s;
    };
    const auto min = measure(t->root); output.constrained = viewport.width < min.width || viewport.height < min.height;
    std::function<void(NodeId, Rect)> place = [&](NodeId id_, Rect bounds) {
        const auto& n = state_.nodes.at(id_);
        if (n.is_leaf()) { output.panes.push_back({n.pane, pane(n.pane)->session, bounds}); return; }
        const auto a = minimums.at(n.first), b = minimums.at(n.second);
        const bool horizontal = n.axis == Axis::horizontal;
        const double extent = horizontal ? bounds.width : bounds.height;
        const double gap = std::min(metrics.divider_gap, extent);
        const double available = std::max(0.0, extent - gap);
        const double ratio = bounded_ratio(n.ratio, available, horizontal ? a.width : a.height, horizontal ? b.width : b.height);
        const double first_size = available * ratio;
        Rect first = bounds, second = bounds, divider = bounds, hit = bounds;
        if (horizontal) {
            first.width = first_size; second.x += first_size + gap; second.width = std::max(0.0, available - first_size);
            divider.x += first_size; divider.width = gap;
            hit.x = divider.x + gap / 2 - metrics.divider_hit_width / 2; hit.width = metrics.divider_hit_width;
        } else {
            first.height = first_size; second.y += first_size + gap; second.height = std::max(0.0, available - first_size);
            divider.y += first_size; divider.height = gap;
            hit.y = divider.y + gap / 2 - metrics.divider_hit_width / 2; hit.height = metrics.divider_hit_width;
        }
        output.dividers.push_back({id_, n.axis, divider, intersection(hit, bounds), bounds, ratio});
        place(n.first, first); place(n.second, second);
    };
    place(t->root, viewport); return output;
}
Status Workspace::focus_direction(Direction direction, Rect viewport, LayoutMetrics metrics) {
    if (!valid_direction(direction)) return Status::failure("Invalid focus direction");
    if (!active_pane()) return Status::failure("No active pane");
    // Navigation uses the underlying arrangement even while zoomed, then zooms
    // the destination. This keeps keyboard focus useful without unzoom flicker.
    Workspace unzoomed = *this;
    for (auto& t : unzoomed.state_.tabs) if (t.id == state_.active_tab) t.zoomed_pane.reset();
    const auto l = unzoomed.layout(state_.active_tab, viewport, metrics);
    if (!l) return Status::failure(l.error);
    const auto current = std::find_if(l.panes.begin(), l.panes.end(), [&](const auto& p) { return p.pane == active_pane(); });
    if (current == l.panes.end()) return Status::failure("Active pane has no geometry");
    const auto r = current->bounds;
    const bool horizontal = direction == Direction::left || direction == Direction::right;
    const bool positive = direction == Direction::right || direction == Direction::down;
    const double cx = r.x + r.width / 2, cy = r.y + r.height / 2;
    std::optional<std::tuple<int, double, double, std::uint64_t>> best;
    PaneId destination;
    for (const auto& p : l.panes) {
        if (p.pane == current->pane) continue;
        const auto b = p.bounds; const double dx = b.x + b.width / 2 - cx, dy = b.y + b.height / 2 - cy;
        const double main = horizontal ? dx : dy;
        if ((positive && main <= 1e-9) || (!positive && main >= -1e-9)) continue;
        const double overlap = horizontal ? std::min(r.y + r.height, b.y + b.height) - std::max(r.y, b.y)
                                          : std::min(r.x + r.width, b.x + b.width) - std::max(r.x, b.x);
        const double edge_gap = horizontal ? (positive ? std::max(0.0, b.x - (r.x + r.width)) : std::max(0.0, r.x - (b.x + b.width)))
                                           : (positive ? std::max(0.0, b.y - (r.y + r.height)) : std::max(0.0, r.y - (b.y + b.height)));
        auto score = std::tuple{overlap > 1e-9 ? 0 : 1, edge_gap, std::abs(horizontal ? dy : dx), p.pane.value};
        if (!best || score < *best) { best = score; destination = p.pane; }
    }
    if (!destination) return Status::failure("No pane in that direction");
    return focus(destination);
}
} // namespace stormtty
