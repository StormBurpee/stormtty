#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace stormtty {

template <class Tag> struct Id {
    std::uint64_t value{};
    explicit constexpr operator bool() const noexcept { return value != 0; }
    auto operator<=>(const Id&) const = default;
};
using TabId = Id<struct TabTag>;
using PaneId = Id<struct PaneTag>;
using NodeId = Id<struct NodeTag>;
// An opaque association only. The host, not Workspace, owns session lifetime.
using SessionId = Id<struct SessionTag>;

struct Status {
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
    static Status failure(std::string message) { return {std::move(message)}; }
};
template <class T> struct Result {
    std::optional<T> value;
    std::string error;
    explicit operator bool() const noexcept { return value.has_value(); }
};

struct Point { double x{}, y{}; };
struct Size { double width{}, height{}; };
struct Rect {
    double x{}, y{}, width{}, height{};
    bool contains(Point point) const noexcept;
    auto operator<=>(const Rect&) const = default;
};
enum class Axis { horizontal, vertical }; // horizontal = side by side
// A direction names the placement of the new/moved pane relative to its target.
enum class Direction { left, right, up, down };
struct Pane {
    PaneId id;
    SessionId session;
    std::string profile{"default"};
    std::string title;
    auto operator<=>(const Pane&) const = default;
};
struct Node {
    NodeId id;
    // Exactly one of pane or both children is populated. Branch ratio is the
    // fraction allocated to first after subtracting the divider gap.
    PaneId pane;
    NodeId first, second;
    Axis axis{Axis::horizontal};
    double ratio{0.5};
    bool is_leaf() const noexcept { return static_cast<bool>(pane); }
    auto operator<=>(const Node&) const = default;
};
struct Tab {
    TabId id;
    NodeId root;
    PaneId active_pane;
    std::optional<PaneId> zoomed_pane;
    std::string title; // user override; empty means host-derived active title
    auto operator<=>(const Tab&) const = default;
};
struct WorkspaceState {
    std::uint64_t next_id{1};
    std::vector<Tab> tabs;
    std::map<NodeId, Node> nodes;
    std::map<PaneId, Pane> panes;
    TabId active_tab;
    auto operator<=>(const WorkspaceState&) const = default;
};
struct LayoutMetrics {
    // All geometry is DIP. Hosts may use 1 / dpi_scale for a physical-pixel seam.
    double divider_gap{1.0};
    double divider_hit_width{7.0};
    double minimum_pane_width{80.0};
    double minimum_pane_height{48.0};
};
struct PanePlacement { PaneId pane; SessionId session; Rect bounds; };
struct DividerPlacement {
    NodeId node;
    Axis axis;
    Rect bounds;
    Rect hit_bounds;
    Rect split_bounds;
    double applied_ratio{};
};
struct Layout {
    std::vector<PanePlacement> panes;
    std::vector<DividerPlacement> dividers;
    // On undersized windows every pane survives; geometry is proportionally
    // compressed instead of forcing an oversized native minimum window size.
    bool constrained{};
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};
enum class HitKind { none, pane, divider };
struct HitRegion { HitKind kind{HitKind::none}; PaneId pane; NodeId divider; };
HitRegion hit_test(const Layout&, Point) noexcept;

// UI-thread value model. No OS imports, process handles, callbacks or side effects.
// Failed mutations leave the complete state (including ID counters) unchanged.
class Workspace {
public:
    static constexpr std::size_t maximum_panes = 4096;
    static constexpr std::size_t maximum_tabs = 256;
    static constexpr std::size_t maximum_depth = 128;
    const WorkspaceState& state() const noexcept { return state_; }
    const Tab* tab(TabId) const noexcept;
    const Pane* pane(PaneId) const noexcept;
    const Node* node(NodeId) const noexcept;
    std::optional<TabId> tab_of(PaneId) const;
    std::vector<PaneId> pane_order(TabId) const;
    TabId active_tab() const noexcept { return state_.active_tab; }
    PaneId active_pane() const noexcept;
    Result<TabId> create_tab(SessionId = {}, std::string profile = "default");
    Result<PaneId> split(PaneId target, Direction placement, SessionId = {},
                         std::string profile = "default", double fraction = 0.5);
    // Removing the final pane removes its tab. Removing the final tab leaves an
    // empty valid workspace. Only the host decides whether to close the window.
    Status close_pane(PaneId);
    Status close_tab(TabId);
    Status activate_tab(TabId);
    Status focus(PaneId);
    Status focus_direction(Direction, Rect viewport, LayoutMetrics = {});
    Status set_zoom(PaneId, bool zoomed);
    Status toggle_zoom();
    Status set_split_ratio(NodeId, double first_fraction);
    // Finite requested fractions (even outside 0..1 during a drag) clamp to the
    // actual subtree minima. Undersized/zoom-hidden splits return an error.
    Status resize_split(NodeId, double first_fraction, Rect viewport, LayoutMetrics = {});
    Status equalize_split(NodeId);
    // A move reparents the SAME leaf/session; center drops use swap_panes.
    Status move_pane(PaneId source, PaneId target, Direction placement, double fraction = 0.5);
    Result<TabId> move_pane_to_new_tab(PaneId, std::string title = {});
    Status swap_panes(PaneId, PaneId);
    Status rename_tab(TabId, std::string);
    Status reorder_tab(TabId, std::size_t new_index);
    Status set_pane_title(PaneId, std::string);
    Status bind_session(PaneId, SessionId);
    Layout layout(TabId, Rect viewport, LayoutMetrics = {}) const;
    // Verifies all ownership, reachability, cycles, IDs, focus, ratios and bounds.
    Status validate() const;
    static Result<Workspace> from_state(WorkspaceState);
private:
    WorkspaceState state_;
    Status commit(Workspace&&);
    NodeId leaf_of(PaneId) const;
    void detach(PaneId, bool erase_pane);
    void normalize_focus();
    void insert_leaf(NodeId leaf, PaneId target, Direction, double fraction);
};

// Version 1, metadata-only: does not save/replay shell commands, PTY state,
// terminal output, environment, credentials or live session IDs. Restored pane
// sessions are unbound (SessionId{}); host must explicitly launch fresh sessions.
std::string serialize_workspace(const Workspace&);
Result<Workspace> deserialize_workspace(std::string_view);

} // namespace stormtty
