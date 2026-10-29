#pragma once
#include "stormtty/ribbon.hpp"

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
enum class TabWatchIndicator { none, watching, unread };
struct TabLabelLayout { Rect text, indicator; };
inline TabLabelLayout layout_tab_label(Rect bounds, TabWatchIndicator state) {
    TabLabelLayout result{{bounds.x+10,bounds.y+6,std::max(0.0,bounds.width-40),20},{}};
    // Keep the existing close/dropdown lane and all hit targets unchanged.
    // Even when title text is clipped, state has its own fixed visual slot.
    if(state!=TabWatchIndicator::none && bounds.width>=46 && bounds.height>=20) {
        result.indicator={bounds.x+10,bounds.y+(bounds.height-6)/2,6,6};
        result.text.x+=12;result.text.width=std::max(0.0,result.text.width-12);
    }
    return result;
}
struct ChromeLayout {
    Rect band, tabs_band, menu, new_tab, palette, minimize, maximize, close, drag;
    std::vector<ChromeTab> tabs;
    bool compact{};
};

// One geometry contract for palette painting, keyboard paging and pointer hits.
// The detail shelf is deliberately separate from result rows: descriptions do
// not make every action a two-line card, while the selected operation is clear.
struct CommandPaletteLayout {
    Rect panel, search, detail, hints;
    std::size_t first{}, rows{};
    std::vector<Rect> row_bounds;
};
inline CommandPaletteLayout layout_command_palette(double width, double height,
        std::span<const double> row_heights, std::size_t selected, bool rename=false) {
    width=std::max(0.0,width); height=std::max(0.0,height);
    const double margin=std::min(16.0,width/2), w=std::min(640.0,width-2*margin);
    const double y_margin=std::min(8.0,height/2);
    const double preferred_y=std::min(72.0,std::max(y_margin,height*.12));
    const bool has_detail=!rename && !row_heights.empty();
    const double header=58, footer=has_detail?64:28;
    const double available=std::max(0.0,height-2*y_margin);
    const double budget=std::max(0.0,available-header-footer);
    selected=row_heights.empty()?0:std::min(selected,row_heights.size()-1);
    auto row_height=[&](std::size_t i){return std::clamp(row_heights[i],36.0,48.0);};
    std::size_t first=selected;
    double body=row_heights.empty()?0:std::min(budget,row_height(selected));
    while (!rename && first && selected-first<7 && body+row_height(first-1)<=budget) body+=row_height(--first);
    std::vector<double> heights;
    body=0;
    if (!rename) for(auto i=first;i<row_heights.size() && heights.size()<8;++i) {
        const auto rh=std::min(budget,row_height(i));
        if (rh<=0 || body+rh>budget) break;
        heights.push_back(rh); body+=rh;
    }
    const double h=std::min(available,header+footer+(rename?0:std::max(std::min(48.0,budget),body)));
    const double y=std::min(preferred_y,std::max(y_margin,height-h-y_margin));
    CommandPaletteLayout out;
    out.panel={(width-w)/2,y,w,h}; out.first=first; out.rows=heights.size();
    // A tiny host can only show a clipped search field; no negative rectangles.
    const double inset=std::min(20.0,w/2);
    out.search={out.panel.x+inset,y+std::min(14.0,h),std::max(0.0,w-2*inset-40),std::min(26.0,std::max(0.0,h-14))};
    const double hint_h=std::min(28.0,std::max(0.0,h-header));
    out.hints={out.panel.x+inset,y+h-hint_h,std::max(0.0,w-2*inset),hint_h};
    const double detail_h=has_detail?std::min(36.0,std::max(0.0,h-header-hint_h)):0;
    out.detail={out.panel.x+inset,out.hints.y-detail_h,std::max(0.0,w-2*inset),detail_h};
    double top=y+header;
    for (const auto rh:heights) { out.row_bounds.push_back({out.panel.x+std::min(8.0,w/2),top,std::max(0.0,w-16),rh}); top+=rh; }
    return out;
}

// Keep the title at the window center, not the asymmetric draggable remainder.
// Hide it rather than shift it into another control in a narrow window.
inline Rect layout_window_title(const ChromeLayout& chrome, double right_limit) {
    const double center=chrome.band.x+chrome.band.width/2;
    const double half=std::min(center-chrome.drag.x,right_limit-center);
    if (half<40) return {};
    return {center-half,chrome.band.y,2*half,chrome.band.height};
}

// Owned notices stay in the active workspace, above (never over) its footer.
// Text dimensions are measured by the native chrome font; geometry stays portable.
struct NoticeLayout { Rect panel, text; };
inline NoticeLayout layout_notice(Rect viewport, double text_width, double text_height) {
    const double width=std::max(0.0,viewport.width), height=std::max(0.0,viewport.height);
    const double margin_x=std::min(12.0,width/2), margin_y=std::min(12.0,height/2);
    const double available_width=std::min(560.0,std::max(0.0,width-2*margin_x));
    const double available_height=std::max(0.0,height-2*margin_y);
    const double measured_width=std::isfinite(text_width)?std::max(0.0,text_width):available_width;
    const double measured_height=std::isfinite(text_height)?std::max(0.0,text_height):available_height;
    const double panel_width=std::min(available_width,std::ceil(measured_width)+24);
    const double panel_height=std::min(available_height,std::max(36.0,std::ceil(measured_height)+20));
    const Rect panel{viewport.x+margin_x,viewport.y+height-margin_y-panel_height,panel_width,panel_height};
    const double padding_x=std::min(12.0,panel_width/2),padding_y=std::min(10.0,panel_height/2);
    return {panel,{panel.x+padding_x,panel.y+padding_y,std::max(0.0,panel_width-2*padding_x),std::max(0.0,panel_height-2*padding_y)}};
}

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
inline BackgroundFooterLayout layout_background_footer(Rect footer, std::size_t count, bool show_heading = true) {
    BackgroundFooterLayout result;
    if (footer.width<=0 || footer.height<=0 || !count) return result;
    const double inset=show_heading?std::min(6.0,footer.width/2):std::min(4.0,footer.width/4);
    const double available=std::max(0.0,footer.width-inset*2);
    const double heading=show_heading && available>=300?100:0;
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

// Active shell context and background sessions share one strip. Geometry is
// presentation-only: all restore targets remain host-owned SessionIds.
struct SessionFooterLayout {
    Rect directory, git, status, divider, clock, agent;
    BackgroundFooterLayout background;
};
inline SessionFooterLayout layout_session_footer(Rect footer, std::size_t parked,
        bool active, bool git, bool status, bool clock=false) {
    SessionFooterLayout result;
    if (footer.width<=0 || footer.height<=0) return result;
    // Secondary decoration yields entirely before active context or background controls.
    if (clock && footer.width>=640 && footer.height>=20) {
        result.clock={footer.x+footer.width-52,footer.y+3,44,footer.height-6};
        footer.width-=60;
    }
    if (!active) { result.background=layout_background_footer(footer,parked,false); return result; }
    const double inset=std::min(8.0,footer.width/2);
    const double usable=std::max(0.0,footer.width-inset*2);
    const double background_width=parked?std::min(usable*0.46,std::max(64.0,std::min(560.0,parked*184.0+12))):0;
    const double gap=parked?std::min(12.0,std::max(0.0,usable-background_width)):0;
    const double context=std::max(0.0,usable-background_width-gap);
    const double row_y=footer.y+std::min(3.0,footer.height/2), row_h=std::max(0.0,footer.height-6);
    // Drop secondary details before shrinking the directory or hiding overflow.
    const double status_width=status && context>=320?72:0;
    const double git_width=git && context>=240?std::min(180.0,context*0.34):0;
    const double status_gap=status_width?12:0, git_gap=git_width?16:0;
    const double directory_width=std::max(0.0,context-status_width-status_gap-git_width-git_gap);
    result.directory={footer.x+inset,row_y,directory_width,row_h};
    if (git_width) result.git={result.directory.x+directory_width+git_gap,row_y,git_width,row_h};
    if (status_width) result.status={footer.x+inset+context-status_width,row_y,status_width,row_h};
    if (parked) {
        const double x=footer.x+inset+context+gap;
        result.divider={x-gap/2,footer.y+6,1,std::max(0.0,footer.height-12)};
        result.background=layout_background_footer({x,footer.y,background_width,footer.height},parked,false);
    }
    return result;
}

// Configured ribbon uses one allocation for painting, hit testing and tooltips.
// Priority controls admission; user order controls placement. Hidden/overflowed
// items have empty geometry. Background actions remain in the command registry.
inline SessionFooterLayout layout_session_footer(Rect footer, std::size_t parked,
        bool active, bool git, bool status, bool clock, const RibbonConfig& config,
        RibbonHostData host={}) {
    SessionFooterLayout result;
    if(footer.width<=0 || footer.height<12 || !valid_ribbon(config)) return result;
    std::array<bool,6> shown{active,active&&git,active&&status,parked!=0,host.agent_available,clock};
    for(std::size_t i=0;i<shown.size();++i) shown[i]=shown[i] && !config.hidden[i];
    if(host.session_error) shown[ribbon_index(RibbonBinding::status)]=true;
    if(host.agent_attention) shown[ribbon_index(RibbonBinding::agent)]=true;
    if(host.background_attention && parked) shown[ribbon_index(RibbonBinding::background)]=true;
    std::array<double,6> widths{};
    const double inset=std::min(8.0,footer.width/4);
    const double available=std::max(0.0,footer.width-2*inset);
    double used=0;std::size_t count=0;
    auto priority=ribbon_bindings;
    if(host.compact_background) {
        priority[ribbon_index(RibbonBinding::background)].minimum=96;
        priority[ribbon_index(RibbonBinding::background)].preferred=208;
        priority[ribbon_index(RibbonBinding::status)].minimum=112;
        priority[ribbon_index(RibbonBinding::status)].preferred=176;
    }
    if(host.session_error) priority[ribbon_index(RibbonBinding::status)].priority=host.compact_background?104:99;
    if(host.compact_background) {
        priority[ribbon_index(RibbonBinding::background)].priority=103;
        if(host.agent_attention) priority[ribbon_index(RibbonBinding::agent)].priority=105;
    }
    // Reserve narrow attention/navigation affordances before ordinary admission.
    // User order is still used below. Explicitly hidden quiet background stays hidden.
    if(host.compact_background) {
        const std::array<bool,6> essential{false,false,host.session_error,
            shown[ribbon_index(RibbonBinding::background)],host.agent_attention,false};
        const auto essentials=std::count(essential.begin(),essential.end(),true);
        double minimums=0;
        for(const auto& item:priority) if(essential[ribbon_index(item.binding)]) minimums+=item.minimum;
        if(essentials && minimums+12*(essentials-1)>available) {
            const double gap=std::min(12.0,available/(2*essentials));
            const double share=(available-gap*(essentials-1))/essentials;
            double x=footer.x+inset;
            for(const auto item:config.order) if(essential[ribbon_index(item)]) {
                const Rect r{x,footer.y+3,share,footer.height-6}; x+=share+gap;
                if(item==RibbonBinding::status) result.status=r;
                if(item==RibbonBinding::background) result.background.overflow=r;
                if(item==RibbonBinding::agent) result.agent=r;
            }
            return result;
        }
    }
    std::stable_sort(priority.begin(),priority.end(),[](const auto& a,const auto& b){return a.priority>b.priority;});
    for(const auto& item:priority) {
        const auto i=ribbon_index(item.binding);if(!shown[i]) continue;
        const double gap=count?12:0;
        if(used+gap+item.minimum<=available) {widths[i]=item.minimum;used+=gap+item.minimum;++count;}
        else if(item.binding==RibbonBinding::agent && host.agent_attention && !count) {widths[i]=available;used=available;++count;}
    }
    // Grow admitted items without stealing room from more important values.
    for(const auto& item:priority) {auto& width=widths[ribbon_index(item.binding)];if(width<=0) continue;
        const double extra=std::min(available-used,item.preferred-width);width+=extra;used+=extra;}
    double x=footer.x+inset;
    for(auto item:config.order) {
        const double width=widths[ribbon_index(item)];if(width<=0) continue;
        const Rect rect{x,footer.y+3,width,footer.height-6};x+=width+12;
        switch(item) {
        case RibbonBinding::directory:result.directory=rect;break;
        case RibbonBinding::git:result.git=rect;break;
        case RibbonBinding::status:result.status=rect;break;
        case RibbonBinding::clock:result.clock=rect;break;
        case RibbonBinding::agent:result.agent=rect;break;
        case RibbonBinding::background:
            if(host.compact_background) result.background.overflow=rect;
            else result.background=layout_background_footer({rect.x,footer.y,rect.width,footer.height},parked,false);
            break;
        }
    }
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

// There is no visual transition *from* an unobserved selection. Seed new views
// at their real state; animate only changes between states the user has seen.
// This also avoids fading restored pane labels up from an invisible baseline.
template<class Map, class Key>
void target_chrome_selection(Map& transitions, Key key, double selected,
                             std::uint64_t now, std::uint64_t duration) {
    auto [entry,inserted]=transitions.try_emplace(key,selected);
    if (!inserted) entry->second.target(selected,now,duration);
}

} // namespace stormtty
