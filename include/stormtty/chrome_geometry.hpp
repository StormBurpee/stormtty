#pragma once

#include "stormtty/workspace.hpp"
#include "stormtty/project_workspaces.hpp"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>

namespace stormtty {

// Portable, DIP geometry shared by painting and native hit testing. The actual
// window, metrics and input services remain platform-owned.
struct ChromeTab { TabId tab; Rect bounds, close; };
struct ChromeLayout {
    Rect band, menu, new_tab, palette, minimize, maximize, close, drag;
    std::vector<ChromeTab> tabs;
    bool compact{};
};

inline ChromeLayout layout_chrome(double width, double height,
                                 const std::vector<TabId>& tabs, TabId active,
                                 double caption_width = 46.0,
                                 double band_height = 48.0) {
    ChromeLayout result;
    width = std::max(0.0, width);
    band_height = std::min(std::max(0.0, height), band_height);
    caption_width = std::min(caption_width, width / 3.0);
    result.band = {0, 0, width, band_height};
    result.close = {width - caption_width, 0, caption_width, band_height};
    result.maximize = {width - 2 * caption_width, 0, caption_width, band_height};
    result.minimize = {width - 3 * caption_width, 0, caption_width, band_height};
    const double available = result.minimize.x;
    const double control_y = std::min(8.0, band_height);
    const double control_height = std::min(32.0, std::max(0.0, band_height - control_y));
    result.menu = {12, control_y, std::min(28.0, std::max(0.0, available - 12)), control_height};
    if (available < 120) {
        result.drag = {std::min(40.0, available), 0, std::max(0.0, available - 40), band_height};
        result.compact = true;
        return result;
    }
    result.palette = {available - 36, control_y, 28, control_height};
    constexpr double start = 52, gap = 4, drag_minimum = 48, new_button = 32;
    const double tab_space = std::max(0.0, result.palette.x - start - drag_minimum - new_button);
    result.compact = !tabs.empty() && tab_space < 96 * static_cast<double>(tabs.size()) + gap * (tabs.size() - 1);
    std::vector<TabId> visible = tabs;
    if (result.compact) visible = active ? std::vector<TabId>{active} : std::vector<TabId>{};
    // Compact mode retains one real tab picker and the caption drag gap. The
    // palette/menu still expose New Tab when its dedicated button cannot fit.
    const bool show_new = !result.compact || result.palette.x - start - drag_minimum >= 96 + new_button;
    const double space = std::max(0.0, result.palette.x - start - drag_minimum - (show_new ? new_button : 0));
    const double tab_width = visible.empty() ? 0 : std::min(188.0, std::max(0.0, (space - gap * (visible.size() - 1)) / visible.size()));
    double x = start;
    for (const auto id : visible) {
        Rect bounds{x, control_y, tab_width, control_height};
        Rect close{bounds.x + std::max(0.0, bounds.width - 28), control_y + 4, std::min(24.0, bounds.width), std::max(0.0, control_height - 8)};
        result.tabs.push_back({id, bounds, close});
        x += tab_width + gap;
    }
    if (!visible.empty()) x -= gap;
    result.new_tab = {x + (show_new ? gap : 0), control_y, show_new ? 28.0 : 0.0, show_new ? control_height : 0.0};
    const double drag_x = std::min(x + (show_new ? new_button : 0), result.palette.x - drag_minimum);
    result.drag = {drag_x, 0, std::max(0.0, result.palette.x - drag_x), band_height};
    return result;
}

enum class NativeHit {
    client, caption, minimize, maximize, close,
    left, right, top, bottom, top_left, top_right, bottom_left, bottom_right
};

inline NativeHit chrome_hit_test(const ChromeLayout& layout, Point p,
                                Size size, double resize_border, bool maximized) noexcept {
    // Caption controls take precedence at the top edge, matching native Windows.
    if (layout.close.contains(p)) return NativeHit::close;
    if (layout.maximize.contains(p)) return NativeHit::maximize;
    if (layout.minimize.contains(p)) return NativeHit::minimize;
    if (!maximized) {
        const bool l = p.x < resize_border, r = p.x >= size.width - resize_border;
        const bool t = p.y < resize_border, b = p.y >= size.height - resize_border;
        if (t && l) return NativeHit::top_left;
        if (t && r) return NativeHit::top_right;
        if (b && l) return NativeHit::bottom_left;
        if (b && r) return NativeHit::bottom_right;
        if (l) return NativeHit::left;
        if (r) return NativeHit::right;
        if (t) return NativeHit::top;
        if (b) return NativeHit::bottom;
    }
    if (layout.menu.contains(p) || layout.new_tab.contains(p) || layout.palette.contains(p)) return NativeHit::client;
    for (const auto& tab : layout.tabs) if (tab.bounds.contains(p)) return NativeHit::client;
    return layout.band.contains(p) ? NativeHit::caption : NativeHit::client;
}

struct ProjectNavigationItem { ProjectWorkspaceId project; Rect bounds; };
struct ProjectNavigationLayout {
    Rect rail, toggle, new_project, context, focus, viewport;
    std::vector<ProjectNavigationItem> projects;
    std::size_t first{}, capacity{};
    bool expanded{};
};
inline ProjectNavigationLayout layout_project_navigation(double width, double height, double crown,
        const std::vector<ProjectWorkspaceId>& projects, std::size_t first, bool expanded,
        bool focus_mode, bool show_context) {
    ProjectNavigationLayout result;
    width=std::max(0.0,width); height=std::max(0.0,height); crown=std::clamp(crown,0.0,height);
    result.expanded=expanded && width>=640;
    const double rail=focus_mode?0:std::min(width,result.expanded?184.0:44.0);
    const double context=focus_mode||!show_context?0:std::min(28.0,height-crown);
    result.rail={0,crown,rail,height-crown};
    result.context={rail,crown,std::max(0.0,width-rail),context};
    result.focus={std::max(rail,width-32),crown+4,std::min(24.0,std::max(0.0,width-rail)),context>0?20.0:0.0};
    result.viewport={rail,crown+context,std::max(0.0,width-rail),std::max(0.0,height-crown-context)};
    if (rail==0) return result;
    result.toggle={6,crown+4,32,28};
    result.new_project={4,std::max(crown+36,height-42),std::max(0.0,rail-8),36};
    result.capacity=static_cast<std::size_t>(std::max(0.0,std::floor((result.new_project.y-(crown+40))/36)));
    const auto maximum_first=projects.size()>result.capacity?projects.size()-result.capacity:0;
    result.first=std::min(first,maximum_first);
    for (std::size_t row=0;row<result.capacity && result.first+row<projects.size();++row)
        result.projects.push_back({projects[result.first+row],{4,crown+40+row*36,std::max(0.0,rail-8),34}});
    return result;
}

// Shared presentation-only modal geometry and keyboard decision state. Hosts
// own the asynchronous callback, target identity and input interception.
struct ModalLayout { Rect panel, title, body, cancel, confirm; };
inline ModalLayout layout_modal(double width,double height,double crown=48) {
    const double w=std::min(480.0,std::max(0.0,width-32));
    const double h=std::min(228.0,std::max(0.0,height-crown-16));
    const double x=(width-w)/2,y=crown+std::max(0.0,(height-crown-h)/2);
    const double padding=h<200?12:24, button=std::min(132.0,std::max(0.0,(w-padding*2-8)/2));
    const double button_y=y+h-padding-32, body_y=y+padding+34;
    return {{x,y,w,h},{x+padding,y+padding,std::max(0.0,w-padding*2),28},
        {x+padding,body_y,std::max(0.0,w-padding*2),std::max(0.0,button_y-body_y-8)},
        {x+w-padding-button*2-8,button_y,button,32},{x+w-padding-button,button_y,button,32}};
}
enum class ModalDecision { cancel, confirm };
class ModalFocus {
public:
    void reset() noexcept { confirm_=false; }
    void next() noexcept { confirm_=!confirm_; }
    void select(ModalDecision decision) noexcept { confirm_=decision==ModalDecision::confirm; }
    ModalDecision activate() const noexcept { return confirm_?ModalDecision::confirm:ModalDecision::cancel; }
    bool confirm_focused() const noexcept { return confirm_; }
private:
    bool confirm_{}; // Destructive confirmation is never the default.
};

// Finite presentation-only transitions. The host schedules a timer only while
// active(); model focus, hit testing and terminal grids commit immediately.
class ChromeTransition {
public:
    explicit ChromeTransition(double initial = 0) : from_(initial), target_(initial) {}
    double value(std::uint64_t now) const noexcept {
        if (!active(now)) return target_;
        const double t = static_cast<double>(now - started_) / duration_;
        const double eased = 1 - (1-t)*(1-t)*(1-t);
        return from_ + (target_ - from_) * eased;
    }
    void target(double next, std::uint64_t now, std::uint64_t duration) noexcept {
        if (next == target_) return;
        from_ = value(now); target_ = next; started_ = now;
        duration_ = from_ == target_ ? 0 : duration;
    }
    bool active(std::uint64_t now) const noexcept { return duration_ && now >= started_ && now - started_ < duration_; }
    void finish() noexcept { from_ = target_; duration_ = 0; }
private:
    double from_{}, target_{};
    std::uint64_t started_{}, duration_{};
};

} // namespace stormtty
