#pragma once

#include "stormtty/chrome_geometry.hpp"
#include "stormtty/renderer.hpp"

namespace stormtty {

struct HeaderTabPresentation {
    ChromeTab geometry;
    std::string title;
    TabWatchIndicator watch{TabWatchIndicator::none};
    bool selected{};
    double selection_amount{};
    double hover_amount{};
    double close_hover_amount{};
};

struct HeaderButtonHovers {
    double menu{};
    double new_tab{};
    double palette{};
    double minimize{};
    double maximize{};
    double close{};
    double zoom_return{};
    double focus{};
};

// Synchronous immutable view of already resolved presentation. Tab geometry and
// labels are owned by the caller's snapshot; layout contains only DIP geometry.
// Neither the view nor any span/string reference is retained after append_scene.
struct WindowHeaderView {
    const ChromeLayout& layout;
    std::span<const HeaderTabPresentation> tabs;
    Rect zoom_return;
    Rect focus;
    HeaderButtonHovers hover;
    double width{};
    bool maximized{};
    bool focus_mode{};
};

struct WindowHeaderStyle {
    Color surface;
    Color raised;
    Color seam;
    Color ink;
    Color secondary;
    Color accent;
    Color selected_tab_surface;
    Color selected_tab_border;
    double scale{1};
    bool high_contrast{};
};

// Domain scene composition, not native rasterization. The host owns fullscreen
// admission, identity, layout/hits, material policy, animation state and timers.
class WindowHeaderRenderer final {
public:
    void append_scene(
        WindowScene& scene,
        const WindowHeaderView& view,
        const WindowHeaderStyle& style) const;
};

} // namespace stormtty
