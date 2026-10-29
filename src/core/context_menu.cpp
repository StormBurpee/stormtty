#include "stormtty/context_menu.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace stormtty {
namespace {
constexpr Color transparent{0,0,0,0};
constexpr double padding=6, normal_height=32, reason_height=48, separator_height=9;
bool finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y); }
bool contains(Rect r,Point p) {
    return finite(p) && r.width>0 && r.height>0 && p.x>=r.x && p.y>=r.y && p.x<r.x+r.width && p.y<r.y+r.height;
}
bool contains(Rect a,Rect b) {
    return b.width>0 && b.height>0 && b.x>=a.x-0.01 && b.y>=a.y-0.01 &&
        b.x+b.width<=a.x+a.width+0.01 && b.y+b.height<=a.y+a.height+0.01;
}
Rect intersection(Rect a,Rect b) {
    const auto x=std::max(a.x,b.x),y=std::max(a.y,b.y);
    return {x,y,std::max(0.0,std::min(a.x+a.width,b.x+b.width)-x),std::max(0.0,std::min(a.y+a.height,b.y+b.height)-y)};
}
// UI strings can contain terminal-provided titles. Keep valid bounded UTF-8,
// replace controls/malformed bytes, and do not interpret mnemonic markup.
std::string clean(std::string_view value,std::size_t limit) {
    std::string out;
    for (std::size_t i=0;i<value.size() && out.size()<limit;) {
        const auto c=static_cast<unsigned char>(value[i]);
        if (c<0x20 || c==0x7f) {out+=' ';++i;continue;}
        std::size_t length=c<0x80?1:c>=0xc2 && c<=0xdf?2:c>=0xe0 && c<=0xef?3:c>=0xf0 && c<=0xf4?4:0;
        bool valid=length && i+length<=value.size();
        for (std::size_t j=1;valid && j<length;++j) valid=(static_cast<unsigned char>(value[i+j])&0xc0)==0x80;
        if (valid && length>=3) {
            const auto second=static_cast<unsigned char>(value[i+1]);
            valid=!(c==0xe0 && second<0xa0) && !(c==0xed && second>=0xa0) &&
                !(c==0xf0 && second<0x90) && !(c==0xf4 && second>=0x90);
        }
        if (!valid) {out+='?';++i;continue;}
        if (out.size()+length>limit) break;
        out.append(value.substr(i,length));i+=length;
    }
    return out;
}
std::string folded(std::string value) {
    for (auto& c:value) if (c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
    return value;
}
std::size_t codepoints(std::string_view value) {
    return static_cast<std::size_t>(std::count_if(value.begin(),value.end(),[](unsigned char c){return (c&0xc0)!=0x80;}));
}
bool valid_id(std::string_view id) {
    return !id.empty() && id.size()<=128 && std::all_of(id.begin(),id.end(),[](unsigned char c){return c>=0x21 && c<=0x7e;});
}
}

ContextMenuItem ContextMenuItem::separator() {
    ContextMenuItem result;result.kind=ContextMenuItemKind::separator;result.enabled=false;return result;
}
bool ContextMenu::copy_items(std::span<const ContextMenuItem> input,std::vector<std::size_t>& output,std::size_t depth) {
    if (depth>=maximum_depth || input.size()>maximum_items) return false;
    for (const auto& item:input) {
        if (nodes_.size()==maximum_items) return false;
        const bool separator=item.kind==ContextMenuItemKind::separator;
        if (separator && !item.children.empty()) return false;
        if (!separator && (!valid_id(item.id) || item.label.empty() ||
            std::any_of(nodes_.begin(),nodes_.end(),[&](const auto& n){return n.id==item.id;}))) return false;
        const auto index=nodes_.size();
        nodes_.push_back({separator?"":item.id,clean(item.label,256),clean(item.shortcut,96),clean(item.disabled_reason,256),
            item.enabled&&!separator,item.destructive,separator,{}});
        // A local vector avoids retaining a reference into nodes_ during growth.
        std::vector<std::size_t> children;
        if (!item.children.empty() && !copy_items(item.children,children,depth+1)) return false;
        nodes_[index].children=std::move(children);
        output.push_back(index);
    }
    // Adjacent/leading/trailing separators do not create blank menu edges.
    std::size_t retained=0;
    bool previous_separator=true;
    for (const auto index:output) {
        const bool separator=nodes_[index].separator;
        if (!separator || !previous_separator) output[retained++]=index;
        previous_separator=separator;
    }
    output.resize(retained);
    if (!output.empty() && nodes_[output.back()].separator) output.pop_back();
    return !output.empty();
}
bool ContextMenu::open(std::span<const ContextMenuItem> items,Point anchor,bool keyboard) {
    close();pending_.reset();nodes_.clear();++generation_;
    if (!finite(anchor)) return false;
    std::vector<std::size_t> roots;
    if (!copy_items(items,roots,0)) {nodes_.clear();return false;}
    anchor_={std::clamp(anchor.x,0.0,viewport_.width),std::clamp(anchor.y,0.0,viewport_.height)};
    levels_.push_back({std::move(roots),std::nullopt,0});open_=true;
    rebuild_layout();if (keyboard) navigate(1,true);
    return true;
}
void ContextMenu::close() {
    open_=false;levels_.clear();layout_.panels.clear();pressed_.reset();typeahead_.clear();typeahead_time_=0;
}
void ContextMenu::cancel() {
    if (!open_) return;
    pending_=ContextMenuAction{ContextMenuActionKind::cancel,{}};close();++generation_;
}
std::optional<ContextMenuAction> ContextMenu::take_action() {return std::exchange(pending_,std::nullopt);}
void ContextMenu::set_theme(const Theme& theme) {
    const auto colors=resolve_ui_colors(theme);
    if (colors_==colors) return;
    colors_=colors;++generation_;
}
void ContextMenu::set_high_contrast(bool enabled,Color background,Color foreground,Color highlight,Color highlight_text,Color disabled_text) {
    background.a=foreground.a=highlight.a=highlight_text.a=disabled_text.a=255;
    if (high_contrast_==enabled && contrast_background_==background && contrast_foreground_==foreground &&
        contrast_highlight_==highlight && contrast_highlight_text_==highlight_text && contrast_disabled_==disabled_text) return;
    high_contrast_=enabled;contrast_background_=background;contrast_foreground_=foreground;
    contrast_highlight_=highlight;contrast_highlight_text_=highlight_text;contrast_disabled_=disabled_text;++generation_;
}
void ContextMenu::resize(Size viewport) {
    if (!std::isfinite(viewport.width) || !std::isfinite(viewport.height)) return;
    viewport_={std::clamp(viewport.width,0.0,32768.0),std::clamp(viewport.height,0.0,32768.0)};
    pressed_.reset();++generation_;rebuild_layout();
    for (std::size_t d=0;d<levels_.size();++d) ensure_selected_visible(d);
}
std::string_view ContextMenu::focused_id() const noexcept {
    if (!open_ || levels_.empty()) return {};
    const auto& level=levels_.back();
    return level.selected && *level.selected<level.nodes.size()?nodes_[level.nodes[*level.selected]].id:std::string_view{};
}
void ContextMenu::rebuild_layout() {
    layout_.panels.clear();if (!open_) return;
    const double margin=std::min({8.0,viewport_.width/4,viewport_.height/4});
    const double available_width=std::max(0.0,viewport_.width-2*margin),available_height=std::max(0.0,viewport_.height-2*margin);
    for (std::size_t depth=0;depth<levels_.size();++depth) {
        auto& level=levels_[depth];
        double width=248,document_height=0;
        for (const auto index:level.nodes) {
            const auto& node=nodes_[index];
            document_height+=node.separator?separator_height:!node.enabled && !node.disabled_reason.empty()?reason_height:normal_height;
            const double hint=node.children.empty()?std::min(120.0,codepoints(node.shortcut)*6.4):18;
            width=std::max(width,codepoints(node.label)*7.1+hint+44+(hint>0?18:0));
            if (!node.enabled && !node.disabled_reason.empty()) width=std::max(width,std::min(360.0,codepoints(node.disabled_reason)*6.2+40));
        }
        width=std::min({width,360.0,available_width});
        const double height=std::min(document_height+padding*2,available_height);
        double x=anchor_.x,y=anchor_.y;
        if (depth>0) {
            const auto& parent=layout_.panels[depth-1];
            x=parent.bounds.x+parent.bounds.width+4;
            if (x+width>viewport_.width-margin) x=parent.bounds.x-width-4;
            const auto selected=levels_[depth-1].selected;
            y=selected && *selected<parent.rows.size()?parent.rows[*selected].bounds.y-padding:parent.bounds.y;
        }
        x=std::clamp(x,margin,std::max(margin,viewport_.width-margin-width));
        y=std::clamp(y,margin,std::max(margin,viewport_.height-margin-height));
        const bool overflowing=document_height+padding*2>height;
        // Dedicated scroll strips cannot conceal an actionable row. Extremely
        // tiny viewports stay bounded and remain dismissible via Escape.
        const double strip=overflowing?std::min(18.0,std::max(0.0,(height-padding*2)/4)):0;
        ContextMenuPanelLayout layout;
        layout.bounds={x,y,width,height};
        layout.content={x+std::min(padding,width/2),y+std::min(padding+strip,height/2),std::max(0.0,width-padding*2),std::max(0.0,height-padding*2-strip*2)};
        level.scroll=std::clamp(level.scroll,0.0,std::max(0.0,document_height-layout.content.height));
        layout.scrolls_up=level.scroll>0.01;
        layout.scrolls_down=level.scroll+layout.content.height<document_height-0.01;
        double row_y=layout.content.y-level.scroll;
        for (std::size_t i=0;i<level.nodes.size();++i) {
            const auto& node=nodes_[level.nodes[i]];
            const double row_height=node.separator?separator_height:!node.enabled && !node.disabled_reason.empty()?reason_height:normal_height;
            const Rect bounds{layout.content.x,row_y,layout.content.width,row_height};
            layout.rows.push_back({node.id,bounds,contains(layout.content,bounds),node.enabled,node.separator,level.selected==i});
            row_y+=row_height;
        }
        layout_.panels.push_back(std::move(layout));
    }
}
void ContextMenu::ensure_selected_visible(std::size_t depth) {
    if (depth>=levels_.size() || depth>=layout_.panels.size()) return;
    auto& level=levels_[depth];if (!level.selected || *level.selected>=layout_.panels[depth].rows.size()) return;
    const auto& layout=layout_.panels[depth];const auto row=layout.rows[*level.selected].bounds;
    if (row.y<layout.content.y) level.scroll-=layout.content.y-row.y;
    else if (row.y+row.height>layout.content.y+layout.content.height) level.scroll+=row.y+row.height-layout.content.y-layout.content.height;
    rebuild_layout();
}
void ContextMenu::select(std::size_t depth,std::optional<std::size_t> row,bool reveal) {
    if (depth>=levels_.size() || (row && *row>=levels_[depth].nodes.size())) return;
    if (levels_[depth].selected==row) return;
    levels_.resize(depth+1);levels_[depth].selected=row;pressed_.reset();++generation_;rebuild_layout();
    if (reveal) ensure_selected_visible(depth);
}
void ContextMenu::navigate(int direction,bool edge) {
    if (levels_.empty()) return;
    const auto depth=levels_.size()-1;const auto count=levels_[depth].nodes.size();if (!count) return;
    const auto old=levels_[depth].selected;
    std::size_t current=edge || !old?(direction>0?count-1:0):*old;
    for (std::size_t step=0;step<count;++step) {
        current=direction>0?(current+1)%count:(current+count-1)%count;
        const auto& node=nodes_[levels_[depth].nodes[current]];
        if (!node.separator && node.enabled) {select(depth,current);ensure_selected_visible(depth);return;}
    }
}
void ContextMenu::open_submenu(std::size_t depth,std::size_t row,bool keyboard) {
    if (depth>=levels_.size() || row>=levels_[depth].nodes.size() || depth+1>=maximum_depth) return;
    const auto& node=nodes_[levels_[depth].nodes[row]];
    if (!node.enabled || node.children.empty()) return;
    const bool existing=depth+1<levels_.size() && levels_[depth+1].nodes==node.children;
    levels_[depth].selected=row;
    if (!existing) {
        levels_.resize(depth+1);levels_.push_back({node.children,std::nullopt,0});pressed_.reset();
        typeahead_.clear();typeahead_time_=0;++generation_;rebuild_layout();
    }
    if (keyboard && !levels_[depth+1].selected) navigate(1,true);
}
void ContextMenu::activate(std::size_t depth,std::size_t row,bool keyboard) {
    if (depth>=levels_.size() || row>=levels_[depth].nodes.size()) return;
    const auto& node=nodes_[levels_[depth].nodes[row]];
    if (!node.enabled || node.separator) return;
    if (!node.children.empty()) {open_submenu(depth,row,keyboard);return;}
    pending_=ContextMenuAction{ContextMenuActionKind::selected,node.id};close();++generation_;
}
bool ContextMenu::key(const KeyEvent& event) {
    if (!open_) return false;
    if (!event.key_down || event.composing || event.alt_graph) return true;
    const auto& key=event.chord.key;
    if (key=="ESCAPE") {cancel();return true;}
    if (key=="TAB") {cancel();return true;}
    if (event.chord.modifiers!=CommandModifier::none) return true;
    pressed_.reset();
    if (key=="UP" || key=="DOWN" || key=="HOME" || key=="END" || key=="LEFT" || key=="RIGHT") {
        typeahead_.clear();typeahead_time_=0;
    }
    if (key=="UP" || key=="DOWN") navigate(key=="DOWN"?1:-1);
    else if (key=="HOME" || key=="END") navigate(key=="HOME"?1:-1,true);
    else if (key=="LEFT" && levels_.size()>1) {levels_.pop_back();++generation_;rebuild_layout();}
    else if ((key=="RIGHT" || key=="ENTER" || key=="SPACE") && !levels_.empty()) {
        const auto depth=levels_.size()-1;const auto selected=levels_[depth].selected;
        if (selected) {
            if (key=="RIGHT") open_submenu(depth,*selected,true);
            else activate(depth,*selected,true);
        }
    }
    return true;
}
bool ContextMenu::text(std::string_view utf8,std::uint64_t now) {
    if (!open_) return false;
    if (utf8.empty() || utf8.size()>32 || levels_.empty() ||
        std::any_of(utf8.begin(),utf8.end(),[](unsigned char c){return c<0x20 || c==0x7f;})) return true;
    const auto input=folded(clean(utf8,32));if (input!=folded(std::string(utf8))) return true;
    const bool timed_out=now<typeahead_time_ || now-typeahead_time_>700;
    if (timed_out) typeahead_.clear();
    const bool cycling=typeahead_==input;
    if (!cycling) {
        if (typeahead_.size()+input.size()>64) typeahead_.clear();
        typeahead_+=input;
    }
    typeahead_time_=now;
    const auto depth=levels_.size()-1;const auto& level=levels_[depth];const auto count=level.nodes.size();
    const auto start=level.selected?(*level.selected+(cycling || timed_out?1:0))%count:0;
    for (std::size_t step=0;step<count;++step) {
        const auto row=(start+step)%count;const auto& node=nodes_[level.nodes[row]];
        if (node.enabled && !node.separator && folded(node.label).starts_with(typeahead_)) {select(depth,row);break;}
    }
    return true;
}
std::optional<std::size_t> ContextMenu::panel_at(Point point) const noexcept {
    if (!finite(point)) return std::nullopt;
    for (std::size_t d=layout_.panels.size();d>0;--d) if (contains(layout_.panels[d-1].bounds,point)) return d-1;
    return std::nullopt;
}
std::optional<ContextMenu::Hit> ContextMenu::hit(Point point) const noexcept {
    const auto depth=panel_at(point);if (!depth) return std::nullopt;
    const auto& layout=layout_.panels[*depth];
    for (std::size_t row=0;row<layout.rows.size();++row) {
        const auto& item=layout.rows[row];
        if (item.visible && !item.separator && contains(item.bounds,point)) return Hit{*depth,row};
    }
    return std::nullopt;
}
bool ContextMenu::pointer_move(Point point) {
    if (!open_) return false;
    const auto item=hit(point);
    if (item) {
        select(item->depth,item->row,false);
        if (!pressed_) open_submenu(item->depth,item->row,false);
    }
    return true;
}
bool ContextMenu::pointer_down(Point point) {
    if (!open_) return false;
    pressed_.reset();
    if (!finite(point)) return true;
    if (!panel_at(point)) {cancel();return true;}
    const auto item=hit(point);
    if (item) {
        select(item->depth,item->row,false);
        if (nodes_[levels_[item->depth].nodes[item->row]].enabled) pressed_=item;
    } else {
        const auto depth=*panel_at(point);const auto& layout=layout_.panels[depth];
        if (point.y<layout.content.y && layout.scrolls_up) wheel(point,1);
        else if (point.y>=layout.content.y+layout.content.height && layout.scrolls_down) wheel(point,-1);
    }
    return true;
}
bool ContextMenu::pointer_up(Point point) {
    if (!open_) return false;
    const auto item=hit(point),pressed=std::exchange(pressed_,std::nullopt);
    if (item && pressed && item==pressed) activate(item->depth,item->row,false);
    return true;
}
void ContextMenu::pointer_cancel() noexcept {pressed_.reset();}
bool ContextMenu::wheel(Point point,double lines) {
    if (!open_) return false;
    const auto depth=panel_at(point);
    if (depth && std::isfinite(lines)) {
        levels_.resize(*depth+1);levels_[*depth].scroll-=std::clamp(lines,-50.0,50.0)*normal_height;
        pressed_.reset();++generation_;rebuild_layout();
    }
    return true;
}

void ContextMenu::append_scene(WindowScene& scene,float scale) const {
    if (!open_ || !std::isfinite(scale) || scale<=0 || scale>8) return;
    const Rect window{0,0,viewport_.width,viewport_.height};
    const auto panel_fill=high_contrast_?contrast_background_:colors_.surface;
    const auto stroke=high_contrast_?contrast_foreground_:colors_.divider;
    const auto normal_ink=high_contrast_?contrast_foreground_:colors_.ink;
    const auto secondary_ink=high_contrast_?contrast_foreground_:colors_.secondary;
    const auto disabled_ink=high_contrast_?contrast_disabled_:colors_.subdued;
    const auto pixels=[&](Rect r){return PixelRect{float(r.x*scale),float(r.y*scale),float(r.width*scale),float(r.height*scale)};};
    ChromeZOrder z{ChromeLayer::context_menu};
    auto box=[&](Rect bounds,Color fill,Color stroke=transparent,double radius=0,double weight=0,bool replace=false) {
        const auto r=intersection(bounds,window);if (r.width<=0 || r.height<=0) return;
        scene.primitives.push_back({pixels(r),fill,stroke,float(std::min({radius,r.width/2,r.height/2})*scale),float(weight*scale),ChromePrimitiveKind::rectangle,replace,z});
    };
    auto label=[&](Rect bounds,const std::string& value,Color color,float size=13,bool bold=false) {
        const auto r=intersection(bounds,window);if (r.width<=0 || r.height<=0 || value.empty()) return;
        scene.texts.push_back({pixels(r),value,color,size,bold,"Segoe UI Variable Text",ChromeTextAlignment::leading,z});
    };
    for (std::size_t depth=0;depth<layout_.panels.size();++depth) {
        const auto& layout=layout_.panels[depth];const auto p=layout.bounds;
        if (p.width<=0 || p.height<=0) continue;
        // A submenu is a complete higher surface: its background covers only
        // overlapping pixels of the parent, without removing the parent's label.
        z.depth=static_cast<std::uint16_t>(depth);
        if (!high_contrast_) box({p.x+2,p.y+4,p.width,p.height},{0,0,0,48},transparent,10);
        box(p,panel_fill,stroke,10,1,true);
        for (std::size_t row=0;row<layout.rows.size();++row) {
            const auto& item=layout.rows[row];const auto& node=nodes_[levels_[depth].nodes[row]];
            if (!item.visible) continue;
            const auto r=item.bounds;
            if (node.separator) {box({r.x+8,r.y+4,std::max(0.0,r.width-16),1},stroke);continue;}
            if (item.selected) box(r,high_contrast_?contrast_highlight_:colors_.selected,
                node.enabled?(high_contrast_?contrast_foreground_:colors_.selected_border):transparent,6,node.enabled?0.75:0);
            // Inactive rows retain a quieter ink, but their hover background
            // needs the stronger secondary role (especially in light themes).
            const auto inactive_ink=!high_contrast_ && item.selected?colors_.secondary:disabled_ink;
            const auto color=!node.enabled?inactive_ink:high_contrast_ && item.selected?contrast_highlight_text_:
                !high_contrast_ && node.destructive?colors_.danger:normal_ink;
            const auto hint_ink=!node.enabled?inactive_ink:high_contrast_ && item.selected?contrast_highlight_text_:secondary_ink;
            const double left=r.x+10,arrow=node.children.empty()?0:14;
            const double hint=node.shortcut.empty() || r.width<220 || arrow?0:std::min(116.0,codepoints(node.shortcut)*6.4+4);
            const double label_width=std::max(0.0,r.width-20-arrow-hint-(hint>0?12:0));
            const double label_height=!node.enabled && !node.disabled_reason.empty()?22:r.height;
            label({left,r.y,label_width,label_height},node.label,color,13,item.selected && node.enabled);
            if (hint>0) label({r.x+r.width-10-hint,r.y,hint,r.height},node.shortcut,hint_ink,11.5f);
            if (arrow>0 && r.width>32) label({r.x+r.width-22,r.y,14,normal_height},"›",hint_ink,17);
            if (!node.enabled && !node.disabled_reason.empty()) label({left,r.y+23,std::max(0.0,r.width-20),20},node.disabled_reason,inactive_ink,11);
        }
        if (layout.scrolls_up || layout.scrolls_down) {
            const auto strip_width=std::min(18.0,p.width);
            if (layout.scrolls_up) label({p.x+(p.width-strip_width)/2,p.y+padding,strip_width,std::max(0.0,layout.content.y-p.y-padding)},"⌃",high_contrast_?contrast_foreground_:colors_.accent,12);
            if (layout.scrolls_down) label({p.x+(p.width-strip_width)/2,layout.content.y+layout.content.height,strip_width,std::max(0.0,p.y+p.height-padding-layout.content.y-layout.content.height)},"⌄",high_contrast_?contrast_foreground_:colors_.accent,12);
        }
    }
}

} // namespace stormtty
