#pragma once

#include "stormtty/chrome_geometry.hpp"
#include "stormtty/renderer.hpp"

namespace stormtty {

struct RibbonContent {
    std::string directory;
    std::string head;
    std::string status;
    bool detached{};
    bool subdued{};
    bool failed{};
    bool directory_hovered{};

    std::string clock;

    std::string agent;
    bool agent_attention{};
};

struct RibbonActivity {
    std::string label; // Already fitted using native text metrics by the host.
    bool visible{};
    bool attention{};
    bool running{};
};

struct RibbonStyle {
    Color surface;
    Color raised;
    Color seam;
    Color activity_surface;

    Color ink;
    Color secondary;
    Color accent;

    double scale{1};
};

// Stateless domain paint only. The host owns layout, interaction, animation,
// text measurement and actor identity. No native graphics resources live here.
class RibbonRenderer final {
public:
    static Rect activity_label_bounds(Rect bounds, bool attention, bool running);

    void append_scene(
        WindowScene& scene,
        Rect footer,
        const SessionFooterLayout& layout,
        const RibbonContent& content,
        const RibbonActivity& activity,
        const RibbonStyle& style) const;
};

} // namespace stormtty
