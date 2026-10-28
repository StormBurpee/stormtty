#pragma once

#include "stormtty/workspace.hpp"
#include "stormtty/project_workspaces.hpp"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>
#include <span>
#include <optional>

namespace stormtty {

// Portable, DIP geometry shared by painting and native hit testing. The actual
// window, metrics and input services remain platform-owned.
struct ChromeTab { TabId tab; Rect bounds, close; };
struct ChromeLayout {
    Rect band, tabs_band, menu, new_tab, palette, minimize, maximize, close, drag;
    std::vector<ChromeTab> tabs;
    bool compact{};
};

// The global caption owns only app controls and native drag/Snap. Task tabs
// belong to the workspace column below it, never to the navigation column.
inline ChromeLayout layout_chrome(double width, double height,
                                 const std::vector<TabId>& tabs, TabId active,
                                 double caption_width = 46.0, double caption_height = 32.0,
                                 double content_left = 0.0, double tab_height = 40.0) {
    ChromeLayout result;
    width=std::max(0.0,width); height=std::max(0.0,height);
    caption_height=std::clamp(caption_height,0.0,height);
    caption_width=std::clamp(caption_width,0.0,width/3);
    content_left=std::clamp(content_left,0.0,width);
    result.band={0,0,width,caption_height};
    result.close={width-caption_width,0,caption_width,caption_height};
    result.maximize={width-2*caption_width,0,caption_width,caption_height};
    result.minimize={width-3*caption_width,0,caption_width,caption_height};
    const double available=result.minimize.x,control_height=std::min(28.0,caption_height);
    const double menu_x=std::min(12.0,available),menu_width=std::min(28.0,std::max(0.0,available-menu_x));
    result.menu={menu_x,(caption_height-control_height)/2,menu_width,control_height};
    const double palette_width=available>=88?28:0;
    result.palette={std::max(0.0,available-36),result.menu.y,palette_width,palette_width>0?control_height:0};
    const double drag_x=menu_x+menu_width+8,drag_right=palette_width>0?result.palette.x:available;
    result.drag={std::min(drag_x,available),0,std::max(0.0,drag_right-drag_x),caption_height};
    tab_height=caption_height>0?std::clamp(tab_height,0.0,height-caption_height):0;
    result.tabs_band={content_left,caption_height,width-content_left,tab_height};
    if (tab_height<=0 || result.tabs_band.width<=0) return result;
    constexpr double gap=4,new_button=32;
    const double start=content_left+std::min(6.0,result.tabs_band.width);
    const double end=width-std::min(6.0,result.tabs_band.width);
    const double space=std::max(0.0,end-start-new_button);
    result.compact=!tabs.empty() && space<96*tabs.size()+gap*(tabs.size()-1);
    std::vector<TabId> visible=tabs;
    if(result.compact)visible=active?std::vector<TabId>{active}:std::vector<TabId>{};
    const double tab_width=visible.empty()?0:std::min(188.0,std::max(0.0,(space-gap*(visible.size()-1))/visible.size()));
    const double tab_h=std::min(32.0,tab_height),tab_y=caption_height+(tab_height-tab_h)/2;
    double x=start;
    for(const auto id:visible) {
        const Rect bounds{x,tab_y,tab_width,tab_h};
        const double close_w=std::min(24.0,tab_width);
        result.tabs.push_back({id,bounds,{x+std::max(0.0,tab_width-28),tab_y+4,close_w,std::max(0.0,tab_h-8)}});
        x+=tab_width+gap;
    }
    if(visible.empty())x=start;
    const double new_width=std::min(28.0,std::max(0.0,end-x));
    result.new_tab={x,tab_y,new_width,new_width>0?tab_h:0};
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
    Rect rail, toggle, new_project, settings, tabs, context, focus, viewport, footer;
    std::vector<ProjectNavigationItem> projects;
    std::size_t first{}, capacity{};
    bool expanded{};
};
inline ProjectNavigationLayout layout_project_navigation(double width, double height, double caption,
        const std::vector<ProjectWorkspaceId>& projects, std::size_t first, bool expanded,
        bool focus_mode, bool show_context) {
    ProjectNavigationLayout result;
    width=std::max(0.0,width); height=std::max(0.0,height); caption=std::clamp(caption,0.0,height);
    result.expanded=expanded && width>=640 && !focus_mode;
    const double rail=focus_mode?0:std::min(width,result.expanded?184.0:44.0);
    const double footer=focus_mode?0:std::min(26.0,height-caption);
    result.footer={0,height-footer,width,footer};
    result.rail={0,caption,rail,std::max(0.0,result.footer.y-caption)};
    const double tab_h=caption>0?std::min(40.0,result.rail.height):0;
    result.tabs={rail,caption,std::max(0.0,width-rail),tab_h};
    const double context=focus_mode||!show_context?0:std::min(32.0,std::max(0.0,result.rail.height-tab_h));
    result.context={rail,caption+tab_h,std::max(0.0,width-rail),context};
    result.focus={std::max(rail,width-32),result.context.y+(context-std::min(24.0,context))/2,
        std::min(24.0,std::max(0.0,width-rail)),std::min(24.0,context)};
    result.viewport={rail,caption+tab_h+context,std::max(0.0,width-rail),std::max(0.0,result.footer.y-caption-tab_h-context)};
    if (rail==0) return result;
    result.toggle={std::min(6.0,rail),caption+4,std::min(32.0,std::max(0.0,rail-12)),std::min(28.0,std::max(0.0,result.rail.height-4))};
    const double settings_h=result.rail.height>=76?36:0;
    result.settings={4,result.footer.y-4-settings_h,std::max(0.0,rail-8),settings_h};
    const double new_h=result.rail.height>=156?36:0;
    result.new_project={4,result.settings.y-4-new_h,std::max(0.0,rail-8),new_h};
    const double bottom=new_h?result.new_project.y:settings_h?result.settings.y:result.footer.y;
    result.capacity=static_cast<std::size_t>(std::max(0.0,std::floor((bottom-(caption+40))/36)));
    const auto maximum_first=projects.size()>result.capacity?projects.size()-result.capacity:0;
    result.first=std::min(first,maximum_first);
    for (std::size_t row=0;row<result.capacity && result.first+row<projects.size();++row)
        result.projects.push_back({projects[result.first+row],{4,caption+40+row*36,std::max(0.0,rail-8),34}});
    return result;
}

// Compact, bounded footer placements. Overflow always remains reachable; the
// host supplies project-qualified identities and owns actions, not geometry.
struct BackgroundFooterLayout {
    Rect label, overflow;
    std::vector<Rect> chips;
};
inline BackgroundFooterLayout layout_background_footer(Rect footer, std::size_t count) {
    BackgroundFooterLayout result;
    if (footer.width<=0 || footer.height<=0 || !count) return result;
    const double inset=std::min(6.0,footer.width/2);
    const double available=std::max(0.0,footer.width-inset*2);
    const double heading=available>=300?100:0;
    result.label={footer.x+inset,footer.y+3,heading,std::max(0.0,footer.height-6)};
    double x=footer.x+inset+heading;
    const double room=std::max(0.0,available-heading);
    const auto capacity=static_cast<std::size_t>(std::floor(room/184));
    const auto visible=std::min(count,capacity);
    const bool overflow=visible<count;
    const auto shown=overflow && visible && room-visible*184<56?visible-1:visible;
    for (std::size_t i=0;i<shown;++i) {
        result.chips.push_back({x,footer.y+3,180,std::max(0.0,footer.height-6)}); x+=184;
    }
    if (shown<count) result.overflow={x,footer.y+3,std::max(0.0,footer.x+footer.width-inset-x),std::max(0.0,footer.height-6)};
    return result;
}

// Diagnostic sample belongs to empty left padding, not terminal cells, headers,
// sidebar material, or overlays. No sample is safer than an occluded sample.
inline std::optional<Rect> material_probe_patch(Rect pane, Rect content, std::span<const Rect> occluders={}) {
    const Rect patch{pane.x+4,content.y+2,std::min(8.0,content.x-pane.x-8),std::min(8.0,content.height-4)};
    if(!std::isfinite(patch.x) || !std::isfinite(patch.y) || !std::isfinite(patch.width) || !std::isfinite(patch.height) ||
        patch.width<1 || patch.height<1 || patch.x+patch.width>content.x || patch.x+patch.width>pane.x+pane.width ||
        patch.y<pane.y || patch.y+patch.height>pane.y+pane.height) return std::nullopt;
    for(const auto& r:occluders) if(r.width>0 && r.height>0 && patch.x<r.x+r.width && patch.x+patch.width>r.x && patch.y<r.y+r.height && patch.y+patch.height>r.y) return std::nullopt;
    return patch;
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
