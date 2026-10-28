#include "stormtty/settings_panel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace stormtty {
namespace {
Color theme_color(std::string_view value) {
    const auto digit=[](char c)->unsigned {return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:c>='A' && c<='F'?c-'A'+10:0;};
    if (value.size()!=7 || value.front()!='#') return {0,0,0,255};
    return {static_cast<std::uint8_t>(digit(value[1])*16+digit(value[2])),static_cast<std::uint8_t>(digit(value[3])*16+digit(value[4])),static_cast<std::uint8_t>(digit(value[5])*16+digit(value[6])),255};
}
Color blend(Color a,Color b,double amount) {
    const auto channel=[&](int x,int y){return static_cast<std::uint8_t>(std::lround(x+(y-x)*amount));};
    return {channel(a.r,b.r),channel(a.g,b.g),channel(a.b,b.b),255};
}
bool has_custom_theme(const Config& config) {
    const auto* named=find_builtin_theme(config.theme.name);
    return !named || *named!=config.theme;
}
std::string theme_title(std::string name) {if (!name.empty() && name[0]>='a' && name[0]<='z') name[0]=static_cast<char>(name[0]-'a'+'A');return name;}
constexpr Color transparent{0,0,0,0};
bool finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y); }
bool contains(Rect outer, Rect inner) {
    return inner.width>0 && inner.height>0 && inner.x>=outer.x-0.01 && inner.y>=outer.y-0.01 &&
        inner.x+inner.width<=outer.x+outer.width+0.01 && inner.y+inner.height<=outer.y+outer.height+0.01;
}
Rect intersection(Rect a, Rect b) {
    const auto x=std::max(a.x,b.x), y=std::max(a.y,b.y);
    return {x,y,std::max(0.0,std::min(a.x+a.width,b.x+b.width)-x),
        std::max(0.0,std::min(a.y+a.height,b.y+b.height)-y)};
}
Rect inset(Rect r, double x, double y) { return {r.x+x,r.y+y,std::max(0.0,r.width-2*x),std::max(0.0,r.height-2*y)}; }
std::string number(double n) { std::ostringstream out; out<<std::setprecision(4)<<n; return out.str(); }
std::string bounded_message(std::string text) {
    for (auto& c:text) if (static_cast<unsigned char>(c)<32 || c==127) c=' ';
    if (text.size()>320) { text.resize(320); while (!text.empty() && (static_cast<unsigned char>(text.back())&0xc0)==0x80) text.pop_back(); if (!text.empty() && static_cast<unsigned char>(text.back())>=0xc0) text.pop_back(); text+="…"; }
    return text;
}
const SettingsControl* find_control(const SettingsPanelLayout& l, std::string_view id) {
    const auto found=std::find_if(l.controls.begin(),l.controls.end(),[&](const auto& c){return c.id==id;});
    return found==l.controls.end()?nullptr:&*found;
}
std::string binding_label(const Config& config, std::string_view id, Platform platform) {
    std::string label;
    for (const auto& binding:config.bindings) if (binding.command==id) {
        if (!label.empty()) label+=" / ";
        label+=format_key_chord(binding.chord,platform);
        if (label.size()>120) {label+="…"; break;}
    }
    return label.empty()?"Not assigned":label;
}
}

SettingsPanel::SettingsPanel(Platform platform) : platform_(platform) {}
void SettingsPanel::open(const Config& config, std::span<const Profile> discovered) {
    open(config,config,discovered);
}
void SettingsPanel::open(const Config& running, const Config& config, std::span<const Profile> discovered) {
    // Repeated open requests must not erase an unsaved or in-flight draft.
    if (open_) return;
    original_=running; initial_draft_=config; draft_=config; profiles_.clear(); families_.clear(); pending_.reset();
    error_.clear(); capture_command_.clear(); terminal_override_.reset(); hover_.clear(); pressed_.clear();
    open_=true; saving_=false; dragging_opacity_=false; scroll_=0;
    section_=SettingsSection::appearance; focus_="nav.appearance";
    const auto valid=validate_config(config,platform_);
    if (!valid) error_=bounded_message(valid.error);
    // Binding insertion order is not a disk change. Compare serialized meaning
    // only for the informational status; persistence keeps its exact file token.
    loaded_changes_=valid && serialize_config(config,platform_)!=serialize_config(running,platform_);
    if (loaded_changes_) pending_=SettingsAction{SettingsActionKind::preview,draft_};
    for (const auto& profile:config.profiles) {
        if (profiles_.size()==64) break;
        profiles_.push_back(profile);
    }
    // Validate candidates in isolation. Discovery cannot replace a configured
    // command, inject an invalid draft or silently alter a selected shell.
    for (const auto& candidate:discovered.first(std::min<std::size_t>(discovered.size(),128))) {
        if (profiles_.size()==128) break;
        if (std::any_of(profiles_.begin(),profiles_.end(),[&](const auto& p){return p.id==candidate.id;})) continue;
        auto probe=default_config(platform_); probe.profiles={candidate}; probe.default_profile=candidate.id;
        if (validate_config(probe,platform_)) profiles_.push_back(candidate);
    }
    families_.push_back(config.font.family);
    for (const auto* family: {"Cascadia Mono","Cascadia Code","Consolas","JetBrains Mono"})
        if (std::find(families_.begin(),families_.end(),family)==families_.end()) families_.emplace_back(family);
    ++generation_; rebuild_layout();
}
void SettingsPanel::open_for_binding(const Config& config, std::string_view command_id, std::span<const Profile> discovered) {
    if (saving_) return;
    if (!open_) open(config,discovered);
    if (!find_command(command_id)) { error_="That command is not available."; ++generation_; return; }
    section_=SettingsSection::shortcuts; scroll_=0; terminal_override_.reset(); capture_command_=std::string(command_id);
    focus_="binding:"+capture_command_; error_.clear(); ++generation_; rebuild_layout(); ensure_focus_visible();
}
void SettingsPanel::resize(Size viewport) {
    if (!std::isfinite(viewport.width) || !std::isfinite(viewport.height)) return;
    viewport_={std::clamp(viewport.width,0.0,32768.0),std::clamp(viewport.height,0.0,32768.0)};
    ++generation_; rebuild_layout(); ensure_focus_visible();
}
void SettingsPanel::set_high_contrast(bool enabled, Color background, Color foreground,
                                      Color highlight, Color highlight_text, Color disabled_text) {
    background.a=foreground.a=highlight.a=highlight_text.a=disabled_text.a=255;
    if (high_contrast_==enabled && high_background_==background && high_foreground_==foreground &&
        high_highlight_==highlight && high_highlight_text_==highlight_text && high_disabled_text_==disabled_text) return;
    high_contrast_=enabled;high_background_=background;high_foreground_=foreground;
    high_highlight_=highlight;high_highlight_text_=highlight_text;high_disabled_text_=disabled_text;
    ++generation_;
}
void SettingsPanel::set_material_status(std::string status) {
    status=bounded_message(std::move(status));
    if(material_status_==status) return;
    material_status_=std::move(status);++generation_;
}
void SettingsPanel::rebuild_layout() {
    layout_={};
    const double margin=viewport_.width<600?8:24;
    const double w=std::min(916.0,std::max(0.0,viewport_.width-margin*2));
    const double h=std::min(708.0,std::max(0.0,viewport_.height-(viewport_.height<540?16:80)));
    const double x=(viewport_.width-w)/2, y=(viewport_.height-h)/2;
    layout_.panel={x,y,w,h}; layout_.compact=w<720;
    const double pad=w<500?16:28;
    const double footer_h=std::min(78.0,h);
    layout_.footer={x,y+h-footer_h,w,footer_h};
    auto add=[&](std::string id,Rect bounds,bool content=false,bool enabled=true) {
        layout_.controls.push_back({std::move(id),bounds,contains(content?layout_.content:layout_.panel,bounds),enabled&&!saving_});
    };
    add("close",{x+w-48,y+19,28,28});
    double content_x=x+226, content_w=std::max(0.0,w-254), content_y=y+145;
    if (layout_.compact) {
        const double nav_y=y+76, nav_w=std::max(0.0,(w-2*pad-12)/3);
        layout_.navigation={x+pad,nav_y,w-2*pad,36};
        add("nav.appearance",{x+pad,nav_y,nav_w,36});
        add("nav.shell",{x+pad+nav_w+6,nav_y,nav_w,36});
        add("nav.shortcuts",{x+pad+(nav_w+6)*2,nav_y,nav_w,36});
        content_x=x+pad; content_w=std::max(0.0,w-2*pad); content_y=y+183;
    } else {
        layout_.navigation={x+16,y+84,180,std::max(0.0,h-178)};
        add("nav.appearance",{x+16,y+104,180,40});
        add("nav.shell",{x+16,y+150,180,40});
        add("nav.shortcuts",{x+16,y+196,180,40});
    }
    layout_.content={content_x,content_y,content_w,std::max(0.0,layout_.footer.y-content_y-16)};
    document_height_=section_==SettingsSection::appearance?448.0:
        section_==SettingsSection::shell?profiles_.size()*72.0:72.0+command_registry().size()*60.0;
    scroll_=std::clamp(scroll_,0.0,std::max(0.0,document_height_-layout_.content.height));
    const double top=content_y-scroll_, right=content_x+content_w;
    if (section_==SettingsSection::appearance) {
        layout_.preview={content_x,top+40,content_w,38};
        const auto count=builtin_themes().size()+(has_custom_theme(initial_draft_)?1:0);
        const double theme_width=std::max(0.0,(content_w-(count-1)*6)/count);
        std::size_t theme_index=0;
        for (const auto& theme:builtin_themes()) add("appearance.theme."+theme.name,{content_x+theme_index++*(theme_width+6),top,theme_width,30},true);
        if (has_custom_theme(initial_draft_)) add("appearance.theme.custom",{content_x+theme_index*(theme_width+6),top,theme_width,30},true);
        const double cw=std::min(content_w*0.49,230.0);
        add("font.family",{right-cw,top+94,cw,34},true);
        add("font.decrease",{right-118,top+150,32,32},true,draft_.font.size_dip>6);
        add("font.increase",{right-32,top+150,32,32},true,draft_.font.size_dip<72);
        add("font.ligatures",{right-42,top+206,42,24},true);
        add("appearance.opacity",{right-cw,top+262,cw,34},true);
        const double gap=5, sw=std::max(0.0,(content_w-3*gap)/4);
        for (std::size_t i=0;i<4;++i) {
            constexpr std::array ids{"auto","none","mica","acrylic"};
            add(std::string("appearance.backdrop.")+ids[i],{content_x+i*(sw+gap),top+360,sw,32},true);
        }
        add("appearance.motion",{right-cw,top+406,cw,34},true);
    } else if (section_==SettingsSection::shell) {
        for (std::size_t i=0;i<profiles_.size();++i)
            add("shell:"+profiles_[i].id,{content_x,top+i*72,content_w,62},true);
    } else {
        layout_.preview={content_x,top,content_w,54};
        std::size_t i=0;
        for (const auto& command:command_registry())
            add("binding:"+std::string(command.id),{content_x,top+70+i++*60,content_w,52},true);
    }
    const double button_w=std::min(120.0,std::max(0.0,(w-2*pad-10)/2));
    add("cancel",{x+w-pad-button_w*2-10,layout_.footer.y+23,button_w,34});
    add(terminal_override_?"binding.confirm":"save",{x+w-pad-button_w,layout_.footer.y+23,button_w,34});
}
void SettingsPanel::ensure_focus_visible() {
    const auto* item=find_control(layout_,focus_);
    if (!item || focus_.starts_with("nav.") || focus_=="save" || focus_=="binding.confirm" || focus_=="cancel" || focus_=="close" || layout_.content.height<=0) return;
    const double bottom=layout_.content.y+layout_.content.height;
    if (item->bounds.y<layout_.content.y) scroll_-=layout_.content.y-item->bounds.y;
    else if (item->bounds.y+item->bounds.height>bottom) scroll_+=item->bounds.y+item->bounds.height-bottom;
    rebuild_layout();
}
void SettingsPanel::focus_control(std::string id) {
    if (focus_==id) return;
    focus_=std::move(id); ++generation_; ensure_focus_visible();
}
const SettingsControl* SettingsPanel::hit(Point point) const noexcept {
    if (!finite(point)) return nullptr;
    for (const auto& item:layout_.controls) if (item.visible && item.enabled && item.bounds.contains(point)) return &item;
    return nullptr;
}
void SettingsPanel::changed() {
    error_.clear(); ++generation_; rebuild_layout();
    const auto status=validate_config(draft_,platform_);
    if (!status) {error_=bounded_message(status.error); return;}
    pending_=SettingsAction{SettingsActionKind::preview,draft_};
}
void SettingsPanel::activate(std::string_view id,int direction) {
    if (!open_ || saving_) return;
    if (id=="cancel" || id=="close") {cancel();return;}
    if (id=="save") {request_save();return;}
    if (id=="binding.confirm" && terminal_override_) {assign_binding(*terminal_override_,true);return;}
    if (id.starts_with("nav.")) {
        section_=id=="nav.shell"?SettingsSection::shell:id=="nav.shortcuts"?SettingsSection::shortcuts:SettingsSection::appearance;
        capture_command_.clear(); terminal_override_.reset(); scroll_=0; ++generation_; rebuild_layout(); return;
    }
    if (id.starts_with("binding:")) {
        capture_command_=std::string(id.substr(8)); terminal_override_.reset(); error_.clear(); ++generation_; return;
    }
    const auto before=draft_;
    if (id.starts_with("appearance.theme.")) {
        const auto name=id.substr(std::string_view("appearance.theme.").size());
        if (name=="custom" && has_custom_theme(initial_draft_)) draft_.theme=initial_draft_.theme;
        else if (const auto* theme=find_builtin_theme(name)) draft_.theme=*theme;
    } else if (id=="font.family") {
        const auto found=std::find(families_.begin(),families_.end(),draft_.font.family);
        const auto index=found==families_.end()?0:static_cast<int>(found-families_.begin());
        const auto count=static_cast<int>(families_.size());
        draft_.font.family=families_[static_cast<std::size_t>((index+direction+count)%count)];
    } else if (id=="font.decrease") draft_.font.size_dip=std::clamp(draft_.font.size_dip-1.0,6.0,72.0);
    else if (id=="font.increase") draft_.font.size_dip=std::clamp(draft_.font.size_dip+1.0,6.0,72.0);
    else if (id=="font.ligatures") draft_.font.ligatures=!draft_.font.ligatures;
    else if (id=="appearance.opacity") draft_.appearance.background_opacity=std::clamp(std::round((draft_.appearance.background_opacity+direction*0.01)*100)/100,0.0,1.0);
    else if (id=="appearance.motion") {
        const int current=static_cast<int>(draft_.appearance.reduced_motion);
        draft_.appearance.reduced_motion=static_cast<ReducedMotionPreference>((current+direction+3)%3);
    } else if (id.starts_with("appearance.backdrop.")) {
        const auto value=id.substr(std::string_view("appearance.backdrop.").size());
        draft_.appearance.backdrop=value=="auto"?BackdropPreference::automatic:value=="none"?BackdropPreference::none:value=="mica"?BackdropPreference::mica:BackdropPreference::acrylic;
    } else if (id.starts_with("shell:")) {
        const auto profile_id=id.substr(6);
        const auto found=std::find_if(profiles_.begin(),profiles_.end(),[&](const auto& p){return p.id==profile_id;});
        if (found==profiles_.end()) return;
        if (std::none_of(draft_.profiles.begin(),draft_.profiles.end(),[&](const auto& p){return p.id==profile_id;})) {
            if (draft_.profiles.size()>=64) {error_="The configuration already has 64 profiles. Remove one before adding another."; ++generation_; return;}
            draft_.profiles.push_back(*found);
        }
        draft_.default_profile=std::string(profile_id);
    }
    const auto valid=validate_config(draft_,platform_);
    if (!valid) {draft_=before;error_=bounded_message(valid.error);++generation_;return;}
    if (draft_!=before) changed();
}
void SettingsPanel::capture_binding(const KeyEvent& event) {
    if (!event.key_down || event.composing || event.alt_graph) return;
    const auto& chord=event.chord;
    if (chord.key=="SHIFT" || chord.key=="CONTROL" || chord.key=="CTRL" || chord.key=="ALT" || chord.key=="COMMAND" || chord.key=="WIN" || chord.key.empty()) return;
    if (chord.key=="ESCAPE") {capture_command_.clear();terminal_override_.reset();error_.clear();++generation_;rebuild_layout();return;}
    // Confirmation is a separate, focusable action. Repeated key-down events
    // cannot opt in merely because the user held a terminal key down.
    if (terminal_override_ && chord.modifiers==CommandModifier::none) {
        if (chord.key=="TAB") {focus_control("binding.confirm");return;}
        if (chord.key=="ENTER" && focus_=="binding.confirm") {assign_binding(*terminal_override_,true);return;}
    }
    terminal_override_.reset();
    focus_="binding:"+capture_command_;
    rebuild_layout();
    assign_binding(chord,false);
}
void SettingsPanel::assign_binding(const KeyChord& chord,bool confirm_terminal_override) {
    auto candidate=draft_;
    std::erase_if(candidate.bindings,[&](const auto& b){return b.command==capture_command_;});
    if (!(chord.key=="BACKSPACE" && chord.modifiers==CommandModifier::none) &&
        !(chord.key=="DELETE" && chord.modifiers==CommandModifier::none)) {
        if (chord.key.size()>32) {error_="That key cannot be assigned.";++generation_;return;}
        for (const auto& existing:candidate.bindings) if (existing.chord==chord) {
            const auto* command=find_command(existing.command);
            error_="Already assigned to "+std::string(command?command->label:existing.command)+". Choose another shortcut.";
            ++generation_; return;
        }
        candidate.bindings.push_back({capture_command_,chord});
    }
    // Validate OS reservations and conflicts before offering a terminal override.
    auto valid=validate_bindings(candidate.bindings,platform_,true);
    if (valid && is_terminal_reserved(chord,platform_) &&
        !(chord.modifiers==CommandModifier::none && (chord.key=="BACKSPACE" || chord.key=="DELETE"))) {
        if (!confirm_terminal_override) {
            terminal_override_=chord;error_.clear();++generation_;rebuild_layout();return;
        }
        candidate.allow_terminal_shortcut_override=true;
    }
    if (valid) valid=validate_bindings(candidate.bindings,platform_,candidate.allow_terminal_shortcut_override);
    if (valid) valid=validate_config(candidate,platform_);
    if (!valid) {error_=bounded_message(valid.error);++generation_;rebuild_layout();return;}
    draft_=std::move(candidate); capture_command_.clear(); terminal_override_.reset(); focus_="nav.shortcuts"; changed();
}
bool SettingsPanel::key(const KeyEvent& event) {
    if (!open_) return false;
    if (saving_ || !event.key_down || event.composing || event.alt_graph) return true;
    if (capturing_shortcut()) {capture_binding(event);return true;}
    const auto& key=event.chord.key;
    const auto modifiers=event.chord.modifiers;
    if (key=="ESCAPE") {cancel();return true;}
    if (key=="TAB" && (modifiers==CommandModifier::none || modifiers==CommandModifier::shift)) {
        std::vector<std::string> order;
        for (const auto& item:layout_.controls) if (item.enabled) order.push_back(item.id);
        if (!order.empty()) {
            const auto it=std::find(order.begin(),order.end(),focus_);
            const auto index=it==order.end()?0:static_cast<int>(it-order.begin());
            const auto count=static_cast<int>(order.size()), step=modifiers==CommandModifier::shift?-1:1;
            focus_control(order[static_cast<std::size_t>((index+step+count)%count)]);
        }
    } else if ((key=="ENTER" || key=="SPACE") && modifiers==CommandModifier::none) activate(focus_);
    else if (key=="ENTER" && (modifiers==CommandModifier::control || modifiers==CommandModifier::command)) request_save();
    else if ((key=="LEFT" || key=="RIGHT" || key=="UP" || key=="DOWN") && modifiers==CommandModifier::none) {
        const int direction=(key=="LEFT" || key=="UP")?-1:1;
        if (focus_=="font.family" || focus_=="appearance.motion" || focus_=="appearance.opacity") activate(focus_,direction);
        else if (focus_.starts_with("nav.")) {
            constexpr std::array ids{"nav.appearance","nav.shell","nav.shortcuts"};
            const auto current=std::find(ids.begin(),ids.end(),focus_);
            const int index=current==ids.end()?0:static_cast<int>(current-ids.begin());
            const auto id=ids[static_cast<std::size_t>((index+direction+3)%3)];
            focus_control(id); activate(id);
        } else if (focus_.starts_with("shell:") || focus_.starts_with("binding:")) {
            const std::string prefix=focus_.starts_with("shell:")?"shell:":"binding:";
            std::vector<std::string> entries;
            for (const auto& item:layout_.controls) if (item.enabled && item.id.starts_with(prefix)) entries.push_back(item.id);
            const auto current=std::find(entries.begin(),entries.end(),focus_);
            if (!entries.empty() && current!=entries.end()) {
                const int index=static_cast<int>(current-entries.begin()), count=static_cast<int>(entries.size());
                const auto id=entries[static_cast<std::size_t>((index+direction+count)%count)];
                focus_control(id);if (prefix=="shell:") activate(id);
            }
        } else if (focus_.starts_with("appearance.theme.")) {
            std::vector<std::string> ids;
            for (const auto& item:layout_.controls) if (item.id.starts_with("appearance.theme.")) ids.push_back(item.id);
            const auto current=std::find(ids.begin(),ids.end(),focus_);
            const auto index=static_cast<int>(current-ids.begin()),count=static_cast<int>(ids.size());
            const auto id=ids[static_cast<std::size_t>((index+direction+count)%count)];
            focus_control(id);activate(id);
        } else if (focus_.starts_with("appearance.backdrop.")) {
            constexpr std::array ids{"appearance.backdrop.auto","appearance.backdrop.none","appearance.backdrop.mica","appearance.backdrop.acrylic"};
            const auto current=std::find(ids.begin(),ids.end(),focus_);
            const auto index=current==ids.end()?0:static_cast<int>(current-ids.begin());
            const auto id=ids[static_cast<std::size_t>((index+direction+4)%4)];
            focus_control(id);activate(id);
        }
    } else if ((key=="HOME" || key=="END") && focus_=="appearance.opacity" && modifiers==CommandModifier::none) {
        draft_.appearance.background_opacity=key=="HOME"?0.0:1.0; changed();
    } else if ((key=="PAGEUP" || key=="PAGEDOWN") && modifiers==CommandModifier::none) {
        scroll_+=(key=="PAGEUP"?-1:1)*std::max(48.0,layout_.content.height-48); ++generation_;rebuild_layout();
    }
    return true;
}
void SettingsPanel::adjust_opacity(Point p) {
    const auto* slider=find_control(layout_,"appearance.opacity");
    if (!slider || !finite(p)) return;
    const double width=std::max(1.0,slider->bounds.width-16);
    const double t=std::clamp((p.x-slider->bounds.x-8)/width,0.0,1.0);
    const double value=std::round(t*100)/100;
    if (value!=draft_.appearance.background_opacity) {draft_.appearance.background_opacity=value;changed();}
}
bool SettingsPanel::pointer_down(Point point) {
    if (!open_) return false;
    if (saving_) return true;
    const auto* item=hit(point);
    pressed_=item?item->id:"";
    if (item) {
        const auto id=item->id; focus_control(id);
        if (id=="appearance.opacity") {dragging_opacity_=true;adjust_opacity(point);}
    }
    ++generation_;return true;
}
bool SettingsPanel::pointer_move(Point point) {
    if (!open_) return false;
    if (dragging_opacity_) adjust_opacity(point);
    const auto* item=hit(point);const auto hover=item?item->id:"";
    if (hover_!=hover) {hover_=hover;++generation_;}
    return true;
}
bool SettingsPanel::pointer_up(Point point) {
    if (!open_) return false;
    if (dragging_opacity_) {adjust_opacity(point);dragging_opacity_=false;pressed_.clear();++generation_;return true;}
    const auto* item=hit(point);
    const auto id=item?item->id:"";
    const bool invoke=!id.empty() && id==pressed_;
    pressed_.clear();++generation_;
    if (invoke) activate(id);
    return true;
}
void SettingsPanel::pointer_cancel() noexcept {
    if (dragging_opacity_ || !pressed_.empty() || !hover_.empty()) ++generation_;
    dragging_opacity_=false;pressed_.clear();hover_.clear();
}
bool SettingsPanel::wheel(Point point,double lines) {
    if (!open_) return false;
    if (!saving_ && finite(point) && std::isfinite(lines) && layout_.content.contains(point)) {
        scroll_-=std::clamp(lines,-50.0,50.0)*36.0;++generation_;rebuild_layout();
    }
    return true;
}
void SettingsPanel::request_save() {
    if (!open_ || saving_) return;
    const auto valid=validate_config(draft_,platform_);
    if (!valid) {error_=bounded_message(valid.error);++generation_;return;}
    capture_command_.clear();terminal_override_.reset();error_.clear();saving_=true;dragging_opacity_=false;pressed_.clear();
    pending_=SettingsAction{SettingsActionKind::save,draft_};++generation_;rebuild_layout();
}
void SettingsPanel::cancel() {
    if (!open_ || saving_) return;
    pending_=SettingsAction{SettingsActionKind::cancel,original_};
    draft_=original_;initial_draft_=original_;loaded_changes_=false;open_=false;capture_command_.clear();terminal_override_.reset();dragging_opacity_=false;pressed_.clear();++generation_;
}
std::optional<SettingsAction> SettingsPanel::take_action() {return std::exchange(pending_,std::nullopt);}
void SettingsPanel::persistence_result(bool success,std::string error) {
    if (!open_ || !saving_) return;
    saving_=false;
    if (success) {original_=draft_;initial_draft_=draft_;loaded_changes_=false;open_=false;pending_.reset();}
    else {error_=bounded_message(error.empty()?"Settings could not be saved. Your changes are still here.":std::move(error));focus_="save";}
    ++generation_;rebuild_layout();
}

void SettingsPanel::append_scene(WindowScene& scene,float scale) const {
    if (!open_ || !std::isfinite(scale) || scale<=0 || scale>8 || layout_.panel.width<=0 || layout_.panel.height<=0) return;
    // Contrast mode uses stable semantic paint tokens before mapping to the OS
    // palette; arbitrary custom theme colors cannot alias those token roles.
    const auto palette=high_contrast_?Theme{}:draft_.theme;
    const Color canvas=theme_color(palette.background),ink=theme_color(palette.foreground);
    const Color panel_color=theme_color(palette.chrome),accent=theme_color(palette.accent);
    const Color nav_color=blend(panel_color,canvas,.55),border=blend(panel_color,ink,.18);
    const Color muted=blend(ink,panel_color,.26),subdued=blend(ink,panel_color,.42),selection=blend(panel_color,accent,.22);
    const Color control=blend(panel_color,ink,.035),control_hover=blend(panel_color,ink,.075),control_pressed=blend(panel_color,ink,.12);
    const Color selected_hover=blend(panel_color,accent,.30),selected_border=blend(panel_color,accent,.55);
    const Color primary_ink=(accent.r*299+accent.g*587+accent.b*114)>140000?Color{17,21,32,255}:Color{255,255,255,255};
    const Color green=blend(accent,ink,.25);
    const auto pixels=[&](Rect r){return PixelRect{float(r.x*scale),float(r.y*scale),float(r.width*scale),float(r.height*scale)};};
    const auto& p=layout_.panel;
    const auto first_primitive=scene.primitives.size();
    // The settings surface paints after complete lower chrome layers. Its scrim
    // dims underlying shapes and text together without mutating their scene.
    const auto first_text=scene.texts.size();
    const Rect window{0,0,viewport_.width,viewport_.height};
    auto box=[&](Rect r,Color fill,Color stroke=transparent,double radius=0,double weight=0,bool replace=false,std::optional<Rect> clip={}) {
        r=intersection(r,clip?*clip:window);if (r.width<=0 || r.height<=0) return;
        scene.primitives.push_back({pixels(r),fill,stroke,float(radius*scale),float(weight*scale),ChromePrimitiveKind::rectangle,replace});
    };
    auto line=[&](double x,double y,double width,Color color) {box({x,y,width,1},color);};
    auto text=[&](Rect r,std::string value,Color color,float size=13,bool bold=false,std::string family="Segoe UI Variable Text",bool content=false,ChromeTextAlignment alignment=ChromeTextAlignment::leading) {
        if (content && !contains(layout_.content,r)) return;
        r=intersection(r,content?layout_.content:p);if (r.width<=0 || r.height<=0) return;
        scene.texts.push_back({pixels(r),std::move(value),color,size,bold,std::move(family),alignment});
    };
    auto label=[&](Rect r,std::string value,Color color,float size=13,bool bold=false,bool content=false) {text(r,std::move(value),color,size,bold,"Segoe UI Variable Text",content);};
    auto button=[&](std::string_view id,std::string title,bool selected=false,bool primary=false) {
        const auto* c=find_control(layout_,id);if (!c || !c->visible) return;
        const bool hover=c->id==hover_, pressed=c->id==pressed_, focused=c->id==focus_;
        Color fill=primary?accent:selected?selection:control;
        if (hover && !primary) fill=selected?selected_hover:control_hover;
        if (pressed) fill=primary?blend(accent,panel_color,.15):control_pressed;
        box(c->bounds,fill,focused?accent:selected?selected_border:border,6,focused?1.5:1);
        if (!c->enabled) {fill.a=140;box(c->bounds,Color{panel_color.r,panel_color.g,panel_color.b,100},transparent,6);}
        const auto fg=!c->enabled?subdued:primary?primary_ink:ink;
        text(inset(c->bounds,c->bounds.width<=40?8:12,0),std::move(title),fg,13,primary||selected,"Segoe UI Variable Text",false,ChromeTextAlignment::center);
    };
    box(window,{4,6,10,148});
    for (int i=4;i>=1;--i) box({p.x-i*3.0,p.y-i+5.0,p.width+i*6.0,p.height+i*4.0},{0,0,0,static_cast<std::uint8_t>(8+(4-i)*4)},transparent,12+i*2);
    box(p,panel_color,border,11,1,true);
    // Restrained accent, not a glowing window frame.
    box({p.x+28,p.y,64,2},accent,transparent,1);
    const double pad=p.width<500?16:28;
    // A geometric icon, not a text badge: independent of font bearings/fallback.
    const Point cog_center{p.x+pad+15,p.y+39};
    if (contains(p,{cog_center.x-13,cog_center.y-13,26,26})) {
        const auto stroke=[&](Point a,Point b) {
            scene.primitives.push_back({pixels({a.x,a.y,b.x-a.x,b.y-a.y}),transparent,muted,0,float(1.5*scale),ChromePrimitiveKind::line});
        };
        constexpr double tau=6.283185307179586;
        const auto point=[&](double angle,double radius) {return Point{cog_center.x+std::cos(angle)*radius,cog_center.y+std::sin(angle)*radius};};
        for (int i=0;i<32;++i) {
            const auto radius=[](int vertex){return vertex%4==1 || vertex%4==2?11.0:8.0;};
            stroke(point(tau*i/32,radius(i)),point(tau*(i+1)/32,radius((i+1)%32)));
        }
        for (int i=0;i<24;++i) stroke(point(tau*i/24,3.5),point(tau*(i+1)/24,3.5));
    }
    label({p.x+pad+43,p.y+17,std::max(0.0,p.width-pad-106),29},"Settings",ink,23,true);
    label({p.x+pad+44,p.y+45,std::max(0.0,p.width-pad-110),19},"Appearance, shells and keyboard shortcuts",muted,12);
    if (const auto* close=find_control(layout_,"close"); close && close->visible) {
        if (hover_=="close" || focus_=="close") box(close->bounds,control_hover,focus_=="close"?accent:transparent,5,focus_=="close"?1:0);
        text(close->bounds,"×",muted,20,false,"Segoe UI Variable Text",false,ChromeTextAlignment::center);
    }
    line(p.x+pad,p.y+76,std::max(0.0,p.width-pad*2),border);
    if (!layout_.compact) {
        box({p.x+1,p.y+77,207,std::max(0.0,p.height-156)},nav_color);
        box({p.x+208,p.y+78,1,std::max(0.0,p.height-157)},border);
        label({p.x+30,p.y+84,150,16},"PREFERENCES",subdued,10,true);

    }
    const std::array<std::pair<std::string_view,std::string_view>,3> nav{{{"nav.appearance","Appearance"},{"nav.shell","Shell"},{"nav.shortcuts","Shortcuts"}}};
    const std::string active=section_==SettingsSection::appearance?"nav.appearance":section_==SettingsSection::shell?"nav.shell":"nav.shortcuts";
    for (const auto& [id,title]:nav) {
        const auto* c=find_control(layout_,id);if (!c || !c->visible) continue;
        const bool selected=id==active;
        if (selected || hover_==id || focus_==id) box(c->bounds,selected?selection:control_hover,focus_==id?accent:transparent,6,focus_==id?1:0);
        if (selected && !layout_.compact) box({c->bounds.x,c->bounds.y+11,2,18},accent,transparent,1);
        const double offset=layout_.compact?12:42;
        if (!layout_.compact) {
            if (id=="nav.shortcuts") {
                const Color glyph=selected?accent:muted;
                box({c->bounds.x+14,c->bounds.y+14,20,13},transparent,glyph,3,1);
                for (int row=0;row<2;++row) for (int column=0;column<3;++column)
                    box({c->bounds.x+18+column*4.0,c->bounds.y+18+row*4.0,2,1},glyph);
            } else label({c->bounds.x+14,c->bounds.y+8,24,24},id=="nav.appearance"?"Aa":">_",selected?accent:muted,12,true);
        }
        label({c->bounds.x+offset,c->bounds.y,std::max(0.0,c->bounds.width-offset-8),c->bounds.height},std::string(title),selected?ink:muted,13,selected);
    }
    const auto& content=layout_.content;
    const std::string title=section_==SettingsSection::appearance?"Appearance":section_==SettingsSection::shell?"Default shell":"Keyboard shortcuts";
    const std::string subtitle=section_==SettingsSection::appearance?"Font, transparency and motion.":section_==SettingsSection::shell?"Choose the shell for new tabs and panes.":"Assign shortcuts to terminal actions.";
    label({content.x,content.y-61,content.width,28},title,ink,21,true);
    label({content.x,content.y-31,content.width,20},subtitle,muted,12);
    const double top=content.y-scroll_, right=content.x+content.width;
    auto content_box=[&](Rect r,Color fill,Color stroke=transparent,double radius=0,double weight=0) {box(r,fill,stroke,radius,weight,false,content);};
    auto row_labels=[&](double y,std::string title_text,std::string help,double width) {
        label({content.x,top+y,std::max(0.0,width),23},std::move(title_text),ink,13,true,true);
        label({content.x,top+y+23,std::max(0.0,width),18},std::move(help),subdued,11,false,true);
    };
    if (section_==SettingsSection::appearance) {
        const auto preview=layout_.preview;
        content_box(preview,canvas,border,7,1);
        content_box({preview.x,preview.y+12,2,preview.height-24},accent,transparent,1);
        for (const auto& theme:builtin_themes()) button("appearance.theme."+theme.name,theme_title(theme.name),draft_.theme==theme);
        if (has_custom_theme(initial_draft_)) button("appearance.theme.custom","Custom",draft_.theme==initial_draft_.theme);
        text({preview.x+16,preview.y+4,std::max(0.0,preview.width-32),30},">_  ABC abc 0123 {} [] -> !=",ink,float(std::clamp(draft_.font.size_dip,10.0,22.0)),false,draft_.font.family,true);
        const double cw=std::min(content.width*0.49,230.0), lw=std::max(0.0,content.width-cw-20);
        row_labels(90,"Terminal font","System fallback if not installed",lw);
        button("font.family",draft_.font.family+"   ›");
        row_labels(146,"Font size","Measured in display-independent pixels",std::max(0.0,content.width-138));
        button("font.decrease","−");button("font.increase","+");
        text({right-86,top+150,54,32},number(draft_.font.size_dip),ink,14,true,"Segoe UI Variable Text",true,ChromeTextAlignment::center);
        row_labels(202,"Font ligatures","Join supported programming symbols",std::max(0.0,content.width-62));
        if (const auto* c=find_control(layout_,"font.ligatures");c && c->visible) {
            box(c->bounds,draft_.font.ligatures?accent:border,focus_==c->id?ink:transparent,12,focus_==c->id?1:0);
            box({c->bounds.x+(draft_.font.ligatures?21:3),c->bounds.y+3,18,18},draft_.font.ligatures?primary_ink:ink,transparent,9);
        }
        row_labels(258,"Background opacity","Text always stays crisp and opaque",lw);
        if (const auto* slider=find_control(layout_,"appearance.opacity");slider && slider->visible) {
            const auto r=slider->bounds;
            if (focus_==slider->id) box(r,transparent,accent,5,1);
            const double progress=draft_.appearance.background_opacity;
            const double width=std::max(0.0,r.width-16), track_y=r.y+27;
            box({r.x+8,track_y,width,3},border,transparent,1.5);
            box({r.x+8,track_y,width*progress,3},accent,transparent,1.5);
            box({r.x+8+width*progress-5,track_y-4,10,11},ink,transparent,5);
            label({r.x+8,r.y-2,std::max(0.0,r.width-16),21},number(std::round(draft_.appearance.background_opacity*100))+"%",ink,12,true,true);
        }
        row_labels(313,"Window material",material_status_.empty()?"Auto uses Acrylic; Mica uses wallpaper, not other windows.":material_status_,content.width);
        constexpr std::array ids{"appearance.backdrop.auto","appearance.backdrop.none","appearance.backdrop.mica","appearance.backdrop.acrylic"};
        constexpr std::array names{"Auto","Solid","Mica","Acrylic"};
        const auto material=draft_.appearance.backdrop;
        const int selected=material==BackdropPreference::automatic?0:material==BackdropPreference::none?1:material==BackdropPreference::mica?2:3;
        for (std::size_t i=0;i<ids.size();++i) button(ids[i],names[i],selected==static_cast<int>(i));
        row_labels(402,"Reduce motion","Use system settings or reduce animations",lw);
        const std::string motion=draft_.appearance.reduced_motion==ReducedMotionPreference::system?"Follow system":draft_.appearance.reduced_motion==ReducedMotionPreference::enabled?"Always reduce":"Allow motion";
        button("appearance.motion",motion+"   ›");
    } else if (section_==SettingsSection::shell) {
        for (const auto& profile:profiles_) {
            const auto* c=find_control(layout_,"shell:"+profile.id);if (!c || !c->visible) continue;
            const auto r=c->bounds;const bool selected=draft_.default_profile==profile.id;
            box(r,selected?selection:hover_==c->id?control_hover:control,focus_==c->id?accent:selected?selected_border:border,7,focus_==c->id?1.5:1);
            box({r.x+14,r.y+15,32,32},selected?selection:control_hover,transparent,7);
            label({r.x+21,r.y+18,23,25},">_",selected?accent:muted,12,true,true);
            label({r.x+59,r.y+7,std::max(0.0,r.width-148),23},profile.label,ink,13,true,true);
            label({r.x+59,r.y+31,std::max(0.0,r.width-82),20},profile.executable,subdued,11,false,true);
            if (selected) label({r.x+r.width-75,r.y+8,62,21},"DEFAULT",accent,9,true,true);
            box({r.x+r.width-28,r.y+36,11,11},transparent,selected?accent:subdued,5.5,1);
            if (selected) box({r.x+r.width-25,r.y+39,5,5},accent,transparent,2.5);
        }
    } else {
        content_box(layout_.preview,capturing_shortcut()?selection:control,capturing_shortcut()?accent:border,7,1);
        label({content.x+14,top+6,std::max(0.0,content.width-28),22},terminal_override_?format_key_chord(*terminal_override_,platform_)+" sends terminal input":capturing_shortcut()?"Press your shortcut now":"Select an action to assign a shortcut",capturing_shortcut()?ink:muted,13,true,true);
        label({content.x+14,top+29,std::max(0.0,content.width-28),18},terminal_override_?"Use shortcut to assign it · Tab then Enter confirms · Esc cancels":"Esc cancels capture · Backspace clears · Conflicts never overwrite",subdued,11,false,true);
        for (const auto& command:command_registry()) {
            const auto* c=find_control(layout_,"binding:"+std::string(command.id));if (!c || !c->visible) continue;
            const auto r=c->bounds;const bool capturing=capture_command_==command.id;
            box(r,capturing?selection:hover_==c->id?control_hover:control,focus_==c->id?accent:border,6,focus_==c->id?1.5:1);
            const double shortcut_width=std::min(170.0,r.width*0.44);
            label({r.x+13,r.y+5,std::max(0.0,r.width-shortcut_width-33),24},std::string(command.label),ink,12,true,true);
            label({r.x+13,r.y+27,std::max(0.0,r.width-26),17},std::string(command.description),subdued,10,false,true);
            label({r.x+r.width-shortcut_width-12,r.y+7,shortcut_width,21},capturing?"Press keys…":binding_label(draft_,command.id,platform_),capturing?accent:muted,11,capturing,true);
        }
    }
    if (document_height_>content.height && content.height>20) {
        const double thumb=std::max(22.0,content.height*content.height/document_height_);
        const double y=content.y+scroll_/(document_height_-content.height)*std::max(0.0,content.height-thumb);
        box({content.x+content.width+8,content.y,3,content.height},border,transparent,1.5);
        box({content.x+content.width+8,y,3,std::min(thumb,content.height)},muted,transparent,1.5);
    }
    // The footer is anchored, not part of the scroll region.
    box({p.x+1,layout_.footer.y,p.width-2,std::max(0.0,layout_.footer.height-1)},nav_color,transparent,10);
    box({p.x+1,layout_.footer.y,p.width-2,std::min(16.0,layout_.footer.height)},nav_color);
    line(p.x+1,layout_.footer.y,std::max(0.0,p.width-2),border);
    if (p.width>550) {
        const double available=std::max(0.0,p.width-2*pad-276);
        if (!error_.empty()) {
            label({p.x+pad,layout_.footer.y+13,available,24},"Couldn't apply settings",{244,164,164,255},12,true);
            label({p.x+pad,layout_.footer.y+37,available,21},error_,muted,11);
        } else {
            const std::string status=terminal_override_?format_key_chord(*terminal_override_,platform_)+" replaces terminal input":saving_?"Saving settings…":capturing_shortcut()?"Press a shortcut…":dirty()?"Unsaved changes":loaded_changes_?"Loaded changes from disk":"No unsaved changes";
            box({p.x+pad,layout_.footer.y+28,5,5},dirty()?accent:green,transparent,2.5);
            label({p.x+pad+14,layout_.footer.y+17,std::max(0.0,available-14),25},status,muted,12);
            label({p.x+pad+14,layout_.footer.y+42,std::max(0.0,available-14),18},terminal_override_?"Use shortcut to confirm · Esc cancels":capturing_shortcut()?"Esc to stop · Backspace to clear":platform_==Platform::macos?"Esc to cancel · Cmd+Enter to save":"Esc to cancel · Ctrl+Enter to save",subdued,10);
        }
    } else if (!error_.empty()) {
        label({p.x+pad,layout_.footer.y+2,std::max(0.0,p.width-2*pad),18},error_,{244,164,164,255},10);
    } else if (terminal_override_) {
        label({p.x+pad,layout_.footer.y+2,std::max(0.0,p.width-2*pad),18},format_key_chord(*terminal_override_,platform_)+" replaces terminal input",muted,10);
    } else if (loaded_changes_ && !dirty()) {
        label({p.x+pad,layout_.footer.y+2,std::max(0.0,p.width-2*pad),18},"Loaded changes from disk",muted,10);
    }
    button("cancel","Cancel");
    if (terminal_override_) button("binding.confirm","Use shortcut",false,true);
    else button("save",saving_?"Saving…":"Save changes",false,true);
    for (std::size_t i=first_primitive;i<scene.primitives.size();++i) scene.primitives[i].z={ChromeLayer::settings};
    for (std::size_t i=first_text;i<scene.texts.size();++i) scene.texts[i].z={ChromeLayer::settings};
    if (high_contrast_) {
        // Keep semantic geometry identical while replacing every visible paint
        // with supplied system colors. Selected text uses HighlightText, not
        // WindowText: Windows supports light as well as dark contrast themes.
        auto is_highlight=[&](Color color) {
            return color==accent || color==selection || color==selected_hover ||
                color==blend(accent,panel_color,.15);
        };
        auto is_foreground=[&](Color color) {
            return color==ink || color==border || color==muted || color==subdued || color==green;
        };
        auto behind=[&](Point center,std::size_t before) {
            while (before>first_primitive) {
                const auto& primitive=scene.primitives[--before];
                if (primitive.fill.a==255 && Rect{primitive.rect.x,primitive.rect.y,primitive.rect.width,primitive.rect.height}.contains(center))
                    return primitive.fill;
            }
            return high_background_;
        };
        for (std::size_t i=first_primitive;i<scene.primitives.size();++i) {
            auto& primitive=scene.primitives[i];
            const Point center{primitive.rect.x+primitive.rect.width/2,primitive.rect.y+primitive.rect.height/2};
            const auto parent=behind(center,i);
            if (primitive.fill.a) {
                if (is_highlight(primitive.fill)) primitive.fill=high_highlight_;
                else if (primitive.fill==primary_ink) primitive.fill=high_highlight_text_; // Enabled toggle thumb.
                else if (is_foreground(primitive.fill)) primitive.fill=parent==high_highlight_?high_highlight_text_:high_foreground_;
                else primitive.fill=high_background_;
            }
            if (primitive.stroke.a) primitive.stroke=(primitive.fill==high_highlight_ || (!primitive.fill.a && parent==high_highlight_))?high_highlight_text_:high_foreground_;
        }
        for (std::size_t i=first_text;i<scene.texts.size();++i) {
            auto& label=scene.texts[i];
            const Point center{label.rect.x+label.rect.width/2,label.rect.y+label.rect.height/2};
            bool disabled=false;
            for (const auto& item:layout_.controls) if (item.visible && !item.enabled && item.bounds.contains({center.x/scale,center.y/scale})) {disabled=true;break;}
            label.color=disabled?high_disabled_text_:behind(center,scene.primitives.size())==high_highlight_?high_highlight_text_:high_foreground_;
        }
    }
}

} // namespace stormtty
