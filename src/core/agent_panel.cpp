#include "stormtty/agent_panel.hpp"
#include "stormtty/agent_display_text.hpp"
#include <algorithm>
#include <cmath>
#include <utility>
#include <limits>
namespace stormtty {
namespace {
Color blend(Color a,Color b,double amount) {
    auto mix=[&](int x,int y){return static_cast<std::uint8_t>(std::lround(x+(y-x)*amount));};
    return {mix(a.r,b.r),mix(a.g,b.g),mix(a.b,b.b),255};
}
Color color(std::string_view s) {
    auto d=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:0;};
    if(s.size()!=7) return {0,0,0,255};
    return {static_cast<std::uint8_t>(d(s[1])*16+d(s[2])),static_cast<std::uint8_t>(d(s[3])*16+d(s[4])),static_cast<std::uint8_t>(d(s[5])*16+d(s[6])),255};
}
}
AgentPanelView agent_panel_view(const AgentSnapshot& snapshot,std::string name,std::string directory,bool available) {
    AgentPanelView v;v.workspace_id=snapshot.association.workspace_persistent_id;v.workspace_name=std::move(name);v.directory=std::move(directory);
    v.auth=snapshot.auth;v.account_label=snapshot.account_label;v.login_id=snapshot.login_id;v.auth_url=snapshot.auth_url;
    v.provider=snapshot.association.provider;v.provider_generation=snapshot.generation;v.error=snapshot.error;
    v.execution_scope="Read-only sandbox; network disabled; untrusted commands require approval; user reviews";
    if(!available) v.state=AgentPanelState::unavailable;
    else if(snapshot.connection==AgentConnection::disconnected) v.state=AgentPanelState::disconnected;
    else if(snapshot.connection==AgentConnection::connecting) v.state=AgentPanelState::connecting;
    else if(snapshot.connection==AgentConnection::failed) v.state=AgentPanelState::failed;
    else switch(snapshot.conversation) {
        case AgentConversation::idle:v.state=AgentPanelState::ready;break;
        case AgentConversation::starting:case AgentConversation::running:case AgentConversation::interrupting:v.state=AgentPanelState::running;break;
        case AgentConversation::completed:v.state=AgentPanelState::completed;break;
        case AgentConversation::interrupted:v.state=AgentPanelState::interrupted;break;
        case AgentConversation::failed:v.state=AgentPanelState::failed;break;
    }
    if(snapshot.transcript_truncated) v.transcript.push_back({"History","Earlier transcript exceeded the retained history limit."});
    for(const auto& m:snapshot.transcript) v.transcript.push_back({m.kind,m.text});
    if(!snapshot.diff.empty()) v.transcript.push_back({"Provider diff · evidence, not an apply transaction",snapshot.diff});
    for(const auto& a:snapshot.approvals) v.approvals.push_back({a.key,a.reason.empty()?a.kind:a.reason,a.scope,a.can_accept});
    return v;
}
void AgentPanel::open(AgentPanelView v) { update(std::move(v)); open_=true; focus_="composer"; layout(); }
void AgentPanel::update(AgentPanelView v) {
    const bool approval_changed=v.approvals.size()!=view_.approvals.size() || (!v.approvals.empty()&&!view_.approvals.empty()&&(v.approvals.front().key!=view_.approvals.front().key || v.approvals.front().scope!=view_.approvals.front().scope || v.approvals.front().title!=view_.approvals.front().title || v.approvals.front().allow_supported!=view_.approvals.front().allow_supported));
    if(approval_changed) {pressed_.clear();pending_.reset();scroll_=0;}
    if(v.auth!=view_.auth||v.login_id!=view_.login_id){pressed_.clear();pending_.reset();}
    if(v.directory!=view_.directory||v.execution_scope!=view_.execution_scope||v.connection_scope!=view_.connection_scope) {confirming_=false;connecting_=false;pressed_.clear();}
    if(v.workspace_id!=view_.workspace_id||v.provider_generation!=view_.provider_generation||v.settings_generation!=view_.settings_generation) { confirming_=false; connecting_=false; pending_.reset(); scroll_=0; }
    view_=std::move(v); if(!can_start()&&!connecting_) confirming_=false; layout();
}
void AgentPanel::close() { settings_reload_confirm_=false;editing_settings_=false;settings_error_.clear(); open_=false; confirming_=false; connecting_=false; pressed_.clear(); pending_.reset(); }
void AgentPanel::settings_result(bool success,std::string error) {if(success){editing_settings_=false;focus_="composer";}settings_error_=std::move(error);layout();}
void AgentPanel::settings_reloaded(const AgentProviderSettings& settings,std::string error) {
    settings_reload_confirm_=false;settings_error_=std::move(error);
    if(settings_error_.empty()){settings_draft_=settings;focus_="settings.executable";settings_select_all_=true;}layout();
}
void AgentPanel::resize(Size v) { viewport_=v; layout(); }
const std::string& AgentPanel::draft() const { static const std::string empty; auto i=drafts_.find(view_.workspace_id); return i==drafts_.end()?empty:i->second; }
bool AgentPanel::can_start() const { return view_.auth!=AgentAuth::signed_out && view_.auth!=AgentAuth::signing_in && view_.auth!=AgentAuth::failed && !view_.workspace_id.empty() && !view_.execution_scope.empty() && view_.approvals.empty() && (view_.state==AgentPanelState::ready||view_.state==AgentPanelState::completed||view_.state==AgentPanelState::interrupted) && !draft().empty(); }
std::string AgentPanel::status_label() const {
    if(!view_.approvals.empty()) return "Needs approval";
    if(view_.auth==AgentAuth::signed_out)return "Sign in needed";
    if(view_.auth==AgentAuth::signing_in)return "Signing in";
    switch(view_.state) {
    case AgentPanelState::unavailable:return "Unavailable"; case AgentPanelState::disconnected:return "Disconnected";
    case AgentPanelState::connecting:return "Connecting"; case AgentPanelState::ready:return "Ready";
    case AgentPanelState::running:return "Running"; case AgentPanelState::completed:return "Completed";
    case AgentPanelState::interrupted:return "Interrupted"; case AgentPanelState::failed:return "Error";
    } return "Unavailable";
}
void AgentPanel::layout() {
    const bool onboarding=view_.transcript.empty() && view_.approvals.empty() && view_.state!=AgentPanelState::running && !confirming_;
    const auto w=std::max(0.0,std::min(640.0,viewport_.width-32));
    const auto h=std::max(0.0,std::min(onboarding||editing_settings_?500.0:!view_.approvals.empty()||confirming_?540.0:640.0,viewport_.height-(viewport_.height<440?32:80)));
    panel_={std::max(0.0,viewport_.width-w-24),std::max(16.0,(viewport_.height-h)/2),w,h};
    composer_={panel_.x+20,panel_.y+std::max(120.0,h-132),std::max(0.0,w-40),64};
    transcript_={panel_.x+20,panel_.y+132,std::max(0.0,w-40),std::max(0.0,composer_.y-panel_.y-152)};
    controls_.clear();
    auto add=[&](std::string id,std::string title,Rect r,bool enabled=true){controls_.push_back({std::move(id),std::move(title),r,enabled});};
    add("close","Close",{panel_.x+w-82,panel_.y+16,62,28});
    if(editing_settings_) {
        add("settings.executable","Executable",{transcript_.x,panel_.y+130,transcript_.width,36},!view_.setup_busy);
        add("settings.save",view_.setup_busy?"Saving…":"Save",{panel_.x+w-142,panel_.y+h-42,122,30},!view_.setup_busy&&view_.setup_available);
        if(view_.setup_reload_available) add("settings.reload",settings_reload_confirm_?"Discard & reload":"Reload saved",{panel_.x+132,panel_.y+h-42,160,30},!view_.setup_busy);
        add("settings.cancel","Cancel",{panel_.x+20,panel_.y+h-42,100,30},!view_.setup_busy);
        return;
    }
    add("settings.open","Advanced",{panel_.x+w-182,panel_.y+16,92,28},(view_.setup_available||view_.setup_reload_available)&&!view_.setup_busy);
    const bool needs_onboarding=view_.transcript.empty() && view_.approvals.empty() && !confirming_ &&
        (view_.state==AgentPanelState::unavailable || view_.state==AgentPanelState::connecting || view_.state==AgentPanelState::disconnected || view_.auth==AgentAuth::signed_out || view_.auth==AgentAuth::signing_in || view_.auth==AgentAuth::failed);
    if(needs_onboarding) {
        const auto y=panel_.y+panel_.height-78;
        if(view_.auth==AgentAuth::signing_in) {
            add("open_sign_in","Continue in browser",{panel_.x+20,y,180,34},!view_.auth_url.empty()&&!pending_);
            add("cancel_sign_in","Cancel sign-in",{panel_.x+w-154,y,134,34},!pending_);
        } else if(view_.auth==AgentAuth::signed_out || view_.auth==AgentAuth::failed) {
            add("sign_in","Sign in to ChatGPT",{panel_.x+20,y,180,34},!pending_);
            add("refresh_account","Check again",{panel_.x+w-142,y,122,34},!pending_);
        } else if(view_.state==AgentPanelState::unavailable) {
            add("install_codex","Install Codex",{panel_.x+20,y,144,34},!pending_);
            add("connect","Check again",{panel_.x+w-142,y,122,34},!pending_);
        } else if(view_.state==AgentPanelState::disconnected) add("connect","Connect Codex",{panel_.x+20,y,144,34},!pending_);
        if(std::none_of(controls_.begin(),controls_.end(),[&](auto& c){return c.id==focus_&&c.enabled;})) focus_="close";
        return;
    }
    add("composer","Task",composer_);
    const auto y=composer_.y+72;
    if(view_.state==AgentPanelState::disconnected||view_.state==AgentPanelState::failed) add("connect",connecting_?"Connect":"Review connect",{panel_.x+w-142,y,122,30},!view_.connection_scope.empty()&&!pending_&&!view_.setup_busy);
    else add("start",confirming_?"Start task":"Review task",{panel_.x+w-142,y,122,30},can_start()&&!pending_&&!view_.setup_busy);
    if(!confirming_ && view_.disconnect_available && (view_.state==AgentPanelState::ready||view_.state==AgentPanelState::completed||view_.state==AgentPanelState::interrupted||view_.state==AgentPanelState::failed||view_.state==AgentPanelState::disconnected)) add("disconnect","Disconnect",{panel_.x+20,y,100,30},!pending_);
    if(view_.state==AgentPanelState::running) add("interrupt","Interrupt",{panel_.x+20,y,100,30},!pending_);
    if(confirming_) add("cancel","Cancel",{panel_.x+20,y,100,30});
    if(!view_.approvals.empty()) {
        const auto ay=transcript_.y+std::max(0.0,transcript_.height-36);
        add("decline","Decline",{panel_.x+20,ay,100,30},!pending_);
        add("allow","Allow once",{panel_.x+132,ay,110,30},view_.approvals.front().allow_supported&&!pending_);
    }
    if(std::none_of(controls_.begin(),controls_.end(),[&](auto& c){return c.id==focus_;})) focus_="composer";
}
void AgentPanel::text(std::string_view t) {
    if(!open_||pending_)return;
    if(editing_settings_) {
        if(view_.setup_busy)return;
        std::string* field=focus_=="settings.executable"?&settings_draft_.executable:focus_=="settings.account"?&settings_draft_.account_reference:nullptr;
        if(!field)return;
        const auto limit=focus_=="settings.executable"?32768u:256u;
        if(t.size()>limit-(settings_select_all_?0:field->size()))return;
        if(settings_select_all_){field->clear();settings_select_all_=false;}
        field->append(t);settings_reload_confirm_=false;settings_error_.clear();layout();return;
    }
    if(focus_!="composer"||confirming_) return;
    auto& d=drafts_[view_.workspace_id]; if(t.size()>16384-d.size()) return; for(char c:t) if((static_cast<unsigned char>(c)>=32||c=='\n')&&c!=127&&d.size()<16384) d+=c;
    layout();
}
void AgentPanel::activate(std::string_view id) {
    if(id=="settings.open"&&(view_.setup_available||view_.setup_reload_available)&&!view_.setup_busy) {editing_settings_=true;settings_draft_=view_.provider_settings;settings_reload_confirm_=false;settings_error_=view_.setup_available?std::string{}:view_.setup_status;focus_="settings.executable";settings_select_all_=true;confirming_=connecting_=false;layout();return;}
    if(editing_settings_) {
        if(view_.setup_busy)return;
        if(id=="settings.cancel"&&settings_reload_confirm_){settings_reload_confirm_=false;settings_error_.clear();layout();return;}
        if(id=="settings.cancel"||id=="close"){settings_reload_confirm_=false;editing_settings_=false;settings_error_.clear();focus_="composer";layout();return;}
        if(id=="settings.reload"&&view_.setup_reload_available) {
            if(settings_draft_!=view_.provider_settings&&!settings_reload_confirm_){settings_reload_confirm_=true;settings_error_="Reload discards unsaved edits. Choose Discard & reload or Cancel.";layout();return;}
            AgentPanelAction action;action.kind=AgentPanelActionKind::reload_settings;action.workspace_id=view_.workspace_id;pending_=std::move(action);settings_reload_confirm_=false;return;
        }
        if(id=="settings.save") {
            settings_reload_confirm_=false;
            settings_draft_.account_reference.clear();settings_error_=validate_agent_provider_settings(settings_draft_);if(!settings_error_.empty()||!view_.setup_available)return;
            AgentPanelAction action;action.kind=AgentPanelActionKind::save_settings;action.workspace_id=view_.workspace_id;action.provider_settings=settings_draft_;pending_=std::move(action);
        }
        return;
    }
    if(id=="close") {close();return;} if(id=="cancel") {confirming_=false;connecting_=false;layout();return;} if(pending_||view_.setup_busy) return;
    AgentPanelAction a; a.workspace_id=view_.workspace_id; a.provider_generation=view_.provider_generation;a.settings_generation=view_.settings_generation;
    if(id=="sign_in" && (view_.auth==AgentAuth::signed_out||view_.auth==AgentAuth::failed)) a.kind=AgentPanelActionKind::sign_in;
    else if(id=="cancel_sign_in"&&view_.auth==AgentAuth::signing_in){a.kind=AgentPanelActionKind::cancel_sign_in;a.login_id=view_.login_id;}
    else if(id=="open_sign_in"&&view_.auth==AgentAuth::signing_in&&!view_.auth_url.empty()){a.kind=AgentPanelActionKind::open_sign_in;a.login_id=view_.login_id;}
    else if(id=="refresh_account")a.kind=AgentPanelActionKind::refresh_account;
    else if(id=="install_codex")a.kind=AgentPanelActionKind::install_codex;
    else if(id=="connect"&&view_.state==AgentPanelState::unavailable)a.kind=AgentPanelActionKind::connect;
    else if(id=="connect"&&!view_.connection_scope.empty()&&(view_.state==AgentPanelState::disconnected||view_.state==AgentPanelState::failed)) {if(!connecting_){confirming_=true;connecting_=true;focus_="cancel";scroll_=0;layout();return;}a.kind=AgentPanelActionKind::connect;confirming_=false;connecting_=false;}
    else if(id=="start"&&can_start()) { if(!confirming_) {confirming_=true;focus_="cancel";scroll_=0;layout();return;} a.kind=AgentPanelActionKind::start;a.prompt=draft(); confirming_=false; }
    else if(id=="disconnect"&&(view_.state==AgentPanelState::ready||view_.state==AgentPanelState::completed||view_.state==AgentPanelState::interrupted||view_.state==AgentPanelState::failed||view_.state==AgentPanelState::disconnected)) a.kind=AgentPanelActionKind::disconnect;
    else if(id=="interrupt"&&view_.state==AgentPanelState::running) a.kind=AgentPanelActionKind::interrupt;
    else if((id=="allow"||id=="decline")&&!view_.approvals.empty()) {if(id=="allow"&&!view_.approvals.front().allow_supported)return; a.kind=id=="allow"?AgentPanelActionKind::allow:AgentPanelActionKind::decline;a.approval_key=view_.approvals.front().key;}
    else return;
    pending_=std::move(a); layout();
}
bool AgentPanel::key(const KeyEvent& e) {
    if(!open_) return false;
    if(!e.key_down||e.composing||e.alt_graph) return true;
    const auto& k=e.chord.key;
    if(editing_settings_&&view_.setup_busy)return true;
    if(editing_settings_&&k=="A"&&has_modifier(e.chord.modifiers,CommandModifier::control)) {settings_select_all_=true;return true;}
    if(editing_settings_&&k=="BACKSPACE") {
        auto* field=focus_=="settings.executable"?&settings_draft_.executable:focus_=="settings.account"?&settings_draft_.account_reference:nullptr;
        if(field){if(settings_select_all_)field->clear();else if(!field->empty()){auto n=field->size()-1;while(n>0&&(static_cast<unsigned char>((*field)[n])&0xc0)==0x80)--n;field->resize(n);}settings_select_all_=false;settings_reload_confirm_=false;settings_error_.clear();layout();}return true;
    }
    if(k=="ESCAPE") {if(editing_settings_){activate("settings.cancel");return true;}if(confirming_) {confirming_=false;connecting_=false;layout();} else close();return true;}
    if(k=="TAB") {
        auto i=std::find_if(controls_.begin(),controls_.end(),[&](auto& c){return c.id==focus_;});
        auto n=i==controls_.end()?0:std::distance(controls_.begin(),i);
        const auto count=static_cast<int>(controls_.size());
        for(int step=0;step<count;++step) {
            n=(n+(has_modifier(e.chord.modifiers,CommandModifier::shift)?count-1:1))%count;
            if(controls_[n].enabled){focus_=controls_[n].id;break;}
        }
        settings_select_all_=editing_settings_;
    }
    else if(k=="ENTER") {if(editing_settings_&&(focus_=="settings.executable"||focus_=="settings.account")){focus_="settings.save";settings_select_all_=true;}else if(focus_!="composer") activate(focus_); else if(has_modifier(e.chord.modifiers,CommandModifier::shift)) text("\n");else {focus_="start";activate("start");}}
    else if(k=="BACKSPACE"&&focus_=="composer"&&!confirming_&&!pending_) {auto& d=drafts_[view_.workspace_id];if(!d.empty()){auto n=d.size()-1;while(n>0&&(static_cast<unsigned char>(d[n])&0xc0)==0x80)--n;d.resize(n);}layout();}
    else if(k=="PAGEUP") scroll((view_.approvals.empty()&&!confirming_)?5:-5); else if(k=="PAGEDOWN") scroll((view_.approvals.empty()&&!confirming_)?-5:5);
    return true;
}
void AgentPanel::pointer_down(Point p) {pressed_.clear();for(auto& c:controls_)if(c.bounds.contains(p)){focus_=c.id;settings_select_all_=editing_settings_;if(c.enabled)pressed_=c.id;break;}}
bool AgentPanel::pointer_move(Point p) {std::string hover;for(const auto& c:controls_)if(c.enabled&&c.bounds.contains(p)){hover=c.id;break;}if(hover==hover_)return false;hover_=std::move(hover);return true;}
void AgentPanel::pointer_up(Point p) {auto id=std::exchange(pressed_,{});for(auto& c:controls_)if(c.id==id&&c.enabled&&c.bounds.contains(p)){activate(id);break;}}
void AgentPanel::scroll(double lines) {
    if(!std::isfinite(lines))return;
    std::size_t bytes=0;for(const auto& m:view_.transcript)bytes+=m.text.size()+m.role.size()+2;
    if(confirming_)bytes=view_.connection_scope.size()+view_.execution_scope.size()+view_.workspace_name.size()+256;
    if(!view_.approvals.empty()) bytes=view_.approvals.front().scope.size()+view_.approvals.front().title.size()+64;
    scroll_=static_cast<std::size_t>(std::clamp(static_cast<double>(scroll_)+lines,0.0,static_cast<double>(bytes)));
}
std::optional<AgentPanelAction> AgentPanel::take_action() {auto a=std::exchange(pending_,{});layout();return a;}
void AgentPanel::append_scene(WindowScene& s,const Theme& t,float scale,const TextWrapper& native_wrap) const {
    if(!open_)return;
    const auto bg=color(t.background),fg=color(t.foreground),chrome=color(t.chrome),accent=color(t.accent); const ChromeZOrder z{ChromeLayer::modal};
    const auto border=blend(chrome,fg,.18),control=blend(chrome,fg,.035),subdued=blend(fg,chrome,.48);
    const Color primary_ink=(accent.r*299+accent.g*587+accent.b*114)>140000?Color{17,21,32,255}:Color{255,255,255,255};
    auto px=[&](Rect r){return PixelRect{float(r.x*scale),float(r.y*scale),float(r.width*scale),float(r.height*scale)};};
    auto box=[&](Rect r,Color fill,Color stroke=Color{}){ChromePrimitive p;p.rect=px(r);p.fill=fill;p.stroke=stroke;p.stroke_width=scale;p.radius=6*scale;p.z=z;s.primitives.push_back(p);};
    auto label=[&](Rect r,std::string text,float size=13,bool bold=false){ChromeText x;x.rect=px(r);x.text=agent_display_text(text);x.color=fg;x.size_dip=size;x.semibold=bold;x.z=z;x.wrap=true;s.texts.push_back(std::move(x));};
    auto button=[&](const AgentPanelControl& c) {
        const bool primary=c.enabled&&(c.id=="start"||c.id=="connect"||c.id=="settings.save"||c.id=="sign_in"||c.id=="open_sign_in"||c.id=="install_codex");
        auto fill=primary?accent:control;
        if(c.enabled&&c.id==pressed_)fill=blend(fill,primary?chrome:fg,.12);
        else if(c.enabled&&c.id==hover_)fill=blend(fill,fg,.075);
        box(c.bounds,fill,c.id==focus_?accent:border);
        label({c.bounds.x+10,c.bounds.y,c.bounds.width-20,c.bounds.height},c.label,12,primary);
        auto& text=s.texts.back();text.color=!c.enabled?subdued:primary?primary_ink:fg;text.alignment=ChromeTextAlignment::center;text.wrap=false;
    };
    box(panel_,chrome,border);s.primitives.back().radius=11*scale;
    box({panel_.x+20,panel_.y,64,2},accent);
    label({panel_.x+24,panel_.y+16,panel_.width-220,30},editing_settings_?"Codex settings":"Codex",21,true);
    box({panel_.x+24,panel_.y+62,panel_.width-48,1},border);
    if(!editing_settings_) {
        label({panel_.x+24,panel_.y+76,panel_.width-200,22},view_.workspace_name,14,true);
        label({panel_.x+24,panel_.y+100,panel_.width-48,20},view_.directory,11);s.texts.back().color=subdued;s.texts.back().wrap=false;
        const Rect badge{panel_.x+panel_.width-160,panel_.y+76,136,24};
        box(badge,blend(chrome,accent,.10));
        label(badge,status_label(),11,true);s.texts.back().color=accent;s.texts.back().alignment=ChromeTextAlignment::center;s.texts.back().wrap=false;
    }
    auto document=[&](std::string text,Rect area,bool from_end) {
        // Bound each draw, not the underlying evidence: all scope/transcript text
        // remains reachable with PageUp/PageDown, including long provider scopes.
        const auto pixel_width=static_cast<float>(area.width*scale);
        std::vector<std::string> uncached_lines;
        if(wrapped_input_!=text||wrapped_width_!=pixel_width||wrapped_scale_!=scale||wrapped_native_!=static_cast<bool>(native_wrap)) {
            std::vector<std::string> lines;
            if(native_wrap) try {
                ChromeText probe;probe.rect={0,0,pixel_width,1.0e8f};probe.size_dip=12;probe.wrap=true;
                probe.text=agent_display_text(text,std::numeric_limits<std::size_t>::max());
                lines=native_wrap(probe);
                std::string joined;bool intact=true;
                for(const auto& line:lines) {
                    joined+=line;if(agent_display_text(line,65536)!=line)intact=false;
                    const auto bracket=line.rfind('[');
                    if(bracket!=std::string::npos){const auto tail=std::string_view(line).substr(bracket);if(tail.find(']')==std::string_view::npos&&(tail.starts_with("[U+")||tail.starts_with("[byte:")||std::string_view("[U+").starts_with(tail)||std::string_view("[byte:").starts_with(tail)))intact=false;}
                }
                auto expected=probe.text;expected.erase(std::remove(expected.begin(),expected.end(),'\n'),expected.end());
                if(!intact||joined!=expected)lines.clear();
            }catch(...){lines.clear();} // Render-only native metrics must never break input or permission state.
            if(lines.empty())lines=agent_display_word_lines(text,area.width,12);
            // Keep at most one bounded cache. Larger authored snapshots still
            // render/scroll completely, but cannot retain an unbounded cache.
            std::size_t bytes=0;for(const auto& line:lines)bytes+=line.size();
            if(text.size()<=2u*1024u*1024u&&bytes<=8u*1024u*1024u&&lines.size()<=262144) {
                wrapped_input_=text;wrapped_lines_=std::move(lines);wrapped_width_=pixel_width;wrapped_scale_=scale;wrapped_native_=static_cast<bool>(native_wrap);
            }else {uncached_lines=std::move(lines);wrapped_input_.clear();wrapped_lines_.clear();wrapped_width_=0;}
        }
        const auto& lines=uncached_lines.empty()?wrapped_lines_:uncached_lines;
        const auto rows=static_cast<std::size_t>(std::max(0.0,area.height/19));
        const auto maximum=lines.size()>rows?lines.size()-rows:0;
        const auto offset=std::min(scroll_,maximum);const auto begin=from_end?maximum-offset:offset;
        for(std::size_t i=0;i<rows&&begin+i<lines.size();++i) {label({area.x,area.y+i*19,area.width,19},lines[begin+i],12,lines[begin+i]=="You"||lines[begin+i]=="Codex");s.texts.back().wrap=false;if(lines[begin+i]=="You"||lines[begin+i]=="Codex")s.texts.back().color=accent;}
    };
    if(editing_settings_) {
        label({transcript_.x,panel_.y+70,transcript_.width,28},settings_error_.empty()?"Local overrides · Auto-detection is used by default.":settings_error_,12,true);
        for(const auto& c:controls_) {
            if(c.id=="settings.executable"||c.id=="settings.account") {
                label({c.bounds.x,c.bounds.y-20,c.bounds.width,20},c.label,12,true);
                box(c.bounds,bg,c.id==focus_?accent:chrome);
                const auto& value=c.id=="settings.executable"?settings_draft_.executable:settings_draft_.account_reference;
                label({c.bounds.x+8,c.bounds.y+6,c.bounds.width-16,c.bounds.height-12},value.empty()?(c.id=="settings.executable"?"C:\\Tools\\codex.exe":"Optional account label"):value,12);
            } else button(c);
        }
        if(panel_.height>350) label({transcript_.x,panel_.y+190,transcript_.width,panel_.height-244},"Use an absolute path only if automatic discovery misses Codex.\nLeave it empty to use automatic discovery.\n\nSign-in is managed by Codex, not by these settings.",12);
        return;
    }
    if(!view_.approvals.empty()||confirming_) {
        box({transcript_.x-4,transcript_.y-4,transcript_.width+8,transcript_.height+8},blend(chrome,accent,.055),border);
    }
    const bool has_composer=std::any_of(controls_.begin(),controls_.end(),[](const auto& c){return c.id=="composer";});
    if(!has_composer) {
        const bool compact=panel_.height<380;
        const auto x=panel_.x+24, y=panel_.y+(compact?120:156), w=panel_.width-48;
        std::string title, body;
        if(view_.auth==AgentAuth::signing_in){title="Finish signing in";body="Continue in your browser to connect your ChatGPT account. This panel updates when sign-in is complete.";}
        else if(view_.auth==AgentAuth::signed_out||view_.auth==AgentAuth::failed){title="Connect your account";body="Codex is installed. Sign in to ChatGPT to use it in this workspace. Your password stays in the browser.";}
        else if(view_.state==AgentPanelState::connecting){title="Getting Codex ready";body="Looking for the installed CLI and checking your sign-in. No task is started.";}
        else if(view_.state==AgentPanelState::disconnected){title="Codex is available";body="Connect the local Codex provider to check your account. You will review the connection before continuing.";}
        else {title="Install Codex to get started";body="StormTTY could not find the Codex CLI on this computer. Install it, then choose Check again. Already installed elsewhere? Set an executable override in Advanced.";}
        if(compact)body=view_.auth==AgentAuth::signing_in?"Continue in your browser to finish sign-in.":view_.auth==AgentAuth::signed_out?"Sign in with your ChatGPT account.":view_.state==AgentPanelState::connecting?"Checking the installed CLI and your account…":"Install Codex, or set its location in Advanced.";
        box({x,y-10,w,compact?90.0:156.0},blend(chrome,fg,.025),border);
        label({x+20,y+8,w-40,compact?24.0:32.0},title,compact?16:20,true);
        label({x+20,y+(compact?36:52),w-40,compact?40.0:86.0},body,13);s.texts.back().color=subdued;
        if(!view_.error.empty()) document(view_.error,{x,y+162,w,std::max(0.0,panel_.height-400)},false);
    }
    else if(connecting_) document("Connect Codex? · PageDown for full scope\nWorkspace: "+view_.workspace_name+"\n"+view_.connection_scope,transcript_,false);
    else if(confirming_) document("Start a Codex task?\nWorkspace: "+view_.workspace_name+"\nProvider: "+view_.provider+"\nScope: "+view_.execution_scope+"\n\nOnly Start task authorizes this prompt.",transcript_,false);
    else if(!view_.approvals.empty()) {const auto& a=view_.approvals.front();document("Permission request · "+std::to_string(view_.approvals.size())+" pending\nPageUp / PageDown to inspect the full scope\n\n"+a.title+"\n"+a.scope,{transcript_.x,transcript_.y,transcript_.width,std::max(0.0,transcript_.height-44)},false);}
    else if(!view_.error.empty()) document(view_.error,transcript_,false);
    else if(view_.transcript.empty()&&!view_.account_label.empty()) document("Signed in as "+view_.account_label+"\n\nWhat would you like to work on?\n\nAsk about this workspace, investigate an issue, or review a change.",transcript_,false);
    else if(view_.transcript.empty()) document(view_.state==AgentPanelState::unavailable?"Codex was not found\n\nInstall the Codex CLI, then try again. If it is already installed in a custom location, choose Advanced.":view_.state==AgentPanelState::connecting?"Finding Codex and checking your account…\n\nThis does not start a task.":"What would you like to work on?\n\nAsk about this workspace, investigate an issue, or review a change. You will review the scope before a task starts.",transcript_,false);
    else {std::string text;for(const auto& m:view_.transcript) text+=(m.role=="user"?"You":m.role=="assistant"?"Codex":m.role)+"\n"+m.text+"\n\n";document(std::move(text),transcript_,true);}

    if(has_composer) {box(composer_,bg,focus_=="composer"?accent:chrome);label({composer_.x+10,composer_.y+8,composer_.width-20,composer_.height-16},draft().empty()?"Ask Codex about this workspace…":draft());}
    label({panel_.x+20,panel_.y+panel_.height-26,panel_.width-40,18},view_.fixture?"Authored UI fixture · no provider or model calls":has_composer?"Enter to review · Shift+Enter for a new line · Esc to close":"No task starts until you review and confirm it",10);
    s.texts.back().color=subdued;s.texts.back().wrap=false;
    for(const auto& c:controls_)if(c.id!="composer")button(c);
}
}
