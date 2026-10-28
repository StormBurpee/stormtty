#pragma once

#include "stormtty/commands.hpp"
#include "stormtty/renderer.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace stormtty {

enum class ContextMenuItemKind { action, separator };
struct ContextMenuItem {
    std::string id; // Stable semantic ID; the host resolves captured target IDs.
    std::string label;
    std::string shortcut;
    std::string disabled_reason;
    bool enabled{true};
    bool destructive{};
    std::vector<ContextMenuItem> children;
    ContextMenuItemKind kind{ContextMenuItemKind::action};
    static ContextMenuItem separator();
};
enum class ContextMenuActionKind { selected, cancel };
struct ContextMenuAction {
    ContextMenuActionKind kind{ContextMenuActionKind::cancel};
    std::string id; // Owned, empty on cancellation; never a borrowed model index.
};
struct ContextMenuRow {
    std::string id;
    Rect bounds; // DIP; only fully visible rows can be activated.
    bool visible{}, enabled{}, separator{}, selected{};
};
struct ContextMenuPanelLayout {
    Rect bounds, content;
    std::vector<ContextMenuRow> rows;
    bool scrolls_up{}, scrolls_down{};
};
struct ContextMenuLayout { std::vector<ContextMenuPanelLayout> panels; };

// Renderer-backed Graphite menus: no native handles, nested event loops, timers,
// process launch or platform I/O. All geometry/input is DIP. All transitions are
// immediate (including reduced motion). Hosts intercept ALL input while open,
// dismiss on focus loss, and restore prior focus after consuming take_action().
// Every panel uses the shared context-menu layer and increasing nested depth;
// appending never edits underlying chrome. Host validates any captured
// pane/tab/project identity again before executing the owned selected ID.
class ContextMenu {
public:
    static constexpr std::size_t maximum_items = 512;
    static constexpr std::size_t maximum_depth = 8;
    // Bounded owned snapshot. Invalid/empty/over-limit models return false and
    // close any previous menu. Reopening never retains stale presses/actions.
    // keyboard=true selects the first enabled item (e.g. Alt+Space).
    bool open(std::span<const ContextMenuItem>, Point anchor, bool keyboard = false);
    void resize(Size viewport);
    // Hosts may supply the current OS accessibility colors; never read a
    // platform setting here. All supplied colors are rendered opaque.
    void set_high_contrast(bool enabled, Color background = {0,0,0,255},
        Color foreground = {255,255,255,255}, Color highlight = {0,120,215,255},
        Color highlight_text = {255,255,255,255}, Color disabled_text = {128,128,128,255});
    bool is_open() const noexcept { return open_; }
    std::uint64_t generation() const noexcept { return generation_; }
    const ContextMenuLayout& layout() const noexcept { return layout_; }
    std::string_view focused_id() const noexcept;
    void append_scene(WindowScene&, float dpi_scale = 1.0f) const;
    // Consume every event while open, even unmatched/key-up/IME events. Route
    // committed printable input separately through text() to avoid doubling.
    bool key(const KeyEvent&);
    bool text(std::string_view utf8, std::uint64_t monotonic_ms);
    bool pointer_down(Point);
    bool pointer_move(Point);
    bool pointer_up(Point);
    void pointer_cancel() noexcept; // Reset an interrupted click, never activate.
    bool wheel(Point, double lines);
    void cancel();
    std::optional<ContextMenuAction> take_action();
private:
    struct Node {
        std::string id, label, shortcut, disabled_reason;
        bool enabled{}, destructive{}, separator{};
        std::vector<std::size_t> children;
    };
    struct Level {
        std::vector<std::size_t> nodes;
        std::optional<std::size_t> selected; // Position within this level.
        double scroll{};
    };
    struct Hit { std::size_t depth{}, row{}; bool operator==(const Hit&) const = default; };
    bool copy_items(std::span<const ContextMenuItem>, std::vector<std::size_t>&, std::size_t depth);
    void rebuild_layout();
    void ensure_selected_visible(std::size_t depth);
    void select(std::size_t depth, std::optional<std::size_t> row, bool reveal = true);
    void navigate(int direction, bool edge = false);
    void activate(std::size_t depth, std::size_t row, bool keyboard);
    void open_submenu(std::size_t depth, std::size_t row, bool keyboard);
    std::optional<Hit> hit(Point) const noexcept;
    std::optional<std::size_t> panel_at(Point) const noexcept;
    void close();
    std::vector<Node> nodes_;
    std::vector<Level> levels_;
    ContextMenuLayout layout_;
    Size viewport_{1024,768};
    Point anchor_{};
    std::optional<Hit> pressed_;
    std::string typeahead_;
    std::uint64_t typeahead_time_{}, generation_{};
    std::optional<ContextMenuAction> pending_;
    bool open_{};
    bool high_contrast_{};
    Color contrast_background_{0,0,0,255}, contrast_foreground_{255,255,255,255};
    Color contrast_highlight_{0,120,215,255}, contrast_highlight_text_{255,255,255,255};
    Color contrast_disabled_{128,128,128,255};
};

} // namespace stormtty
