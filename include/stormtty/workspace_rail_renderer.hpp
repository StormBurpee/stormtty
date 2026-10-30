#pragma once

#include "stormtty/chrome_geometry.hpp"
#include "stormtty/renderer.hpp"

#include <span>
#include <string>

namespace stormtty {

struct WorkspaceRailRow {
    ProjectWorkspaceId id;
    Rect bounds;
    std::string name;
    std::string icon;
    Color icon_color;
    bool selected{};
    bool unread{};
    double focus_amount{};
    double hover_amount{};
};

// DIP geometry and resolved presentation, borrowed only during append_scene.
// The host keeps catalog lookup, stable identity, hit testing and animation.
struct WorkspaceRailView {
    Rect rail;
    Rect toggle;
    Rect new_workspace;
    Rect settings;
    double caption_height{};
    bool expanded{};
    std::span<const WorkspaceRailRow> rows;
    double toggle_hover{};
    double new_workspace_hover{};
    double settings_hover{};
};

struct WorkspaceRailStyle {
    Color surface;
    Color raised;
    Color selected_surface;
    Color seam;
    Color ink;
    Color secondary;
    Color accent;
    double scale{1};
    bool high_contrast{};
};

// Workspace navigation visuals only, not layout, interaction or rasterization.
// No native resources, callbacks, retained views, clocks or model dependencies.
class WorkspaceRailRenderer final {
public:
    void append_scene(WindowScene& scene, const WorkspaceRailView& view,
                      const WorkspaceRailStyle& style) const;
};

} // namespace stormtty
