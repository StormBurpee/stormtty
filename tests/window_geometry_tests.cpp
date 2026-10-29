#include "stormtty/native_window.hpp"
#include "stormtty/settings_panel.hpp"
#include <cstdlib>
#include <array>
#include <iostream>

namespace { void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
} }
int main() {
    using namespace stormtty;
    for (double width:{0.,24.,180.,320.,640.,1044.,1600.}) for(double height:{0.,32.,120.,240.,720.}) {
        std::vector<double> rows(44,36); rows[3]=48; rows[43]=48;
        for(std::size_t selected:{0u,3u,20u,43u,999u}) {
            const auto p=layout_command_palette(width,height,rows,selected);
            require(p.panel.x>=0 && p.panel.y>=0 && p.panel.x+p.panel.width<=width+.001 && p.panel.y+p.panel.height<=height+.001,"palette stays inside even tiny hosts");
            require(p.rows==p.row_bounds.size() && p.rows<=8,"palette paging uses actual painted rows");
            if(p.rows) require(p.first<=std::min(selected,rows.size()-1) && p.first+p.rows>std::min(selected,rows.size()-1),"selected palette result is always visible");
            double bottom=p.panel.y;
            for(const auto& row:p.row_bounds) {
                require(row.width>=0 && row.height>0 && row.x>=p.panel.x && row.x+row.width<=p.panel.x+p.panel.width+.001 && row.y>=bottom && row.y+row.height<=p.detail.y+.001,"palette rows do not overlap the selected-action shelf");
                bottom=row.y+row.height;
            }
            require(p.search.width>=0 && p.detail.width>=0 && p.hints.width>=0,"palette text geometry never negative");
        }
        const auto rename=layout_command_palette(width,height,rows,0,true);
        require(rename.rows==0 && rename.detail.height==0,"rename remains a compact field, not a command list");
    }
    const auto empty_palette=layout_command_palette(1044,720,{},0);
    require(empty_palette.rows==0 && empty_palette.panel.height==170,"empty search reserves useful feedback space");
    for (double width : {0., 24., 120., 300., 640., 1100.}) for (std::size_t count : {0u,1u,4u,256u}) {
        const auto footer=layout_background_footer({0,700,width,26},count);
        double right=0;
        for (const auto& chip:footer.chips) {
            require(chip.x>=right && chip.x+chip.width<=width && chip.y>=700 && chip.y+chip.height<=726,"background chips bounded and disjoint"); right=chip.x+chip.width;
        }
        if (count>footer.chips.size() && width>0) require(footer.overflow.width>0 && footer.overflow.x>=right && footer.overflow.x+footer.overflow.width<=width,"background overflow reachable without overlap");
        if (!count) require(footer.chips.empty() && !footer.overflow.width,"empty footer has no filler");
    }
    require(layout_background_footer({0,700,1100,0},4).chips.empty(),"focus mode hides background geometry");
    for(double width:{0.,1.,12.,24.,80.,120.,300.,480.,640.,1100.,2200.})
        for(double height:{0.,3.,12.,26.}) for(std::size_t count:{0u,1u,4u,256u}) {
        const auto footer=layout_session_footer({13,700,width,height},count,true,true,true);
        std::vector<Rect> cells{footer.directory,footer.git,footer.status,footer.background.overflow};
        cells.insert(cells.end(),footer.background.chips.begin(),footer.background.chips.end());
        for(const auto& r:cells) if(r.width>0 && r.height>0) {
            require(r.x>=13 && r.x+r.width<=13+width+0.001 && r.y>=700 && r.y+r.height<=700+height+0.001,
                "combined footer cells remain bounded at all widths and heights");
            for(const auto& other:cells) if(&r!=&other && other.width>0 && other.height>0)
                require(r.x+r.width<=other.x+0.001 || other.x+other.width<=r.x+0.001,
                    "active context and background hit targets never overlap");
        }
        if(count && width>16 && height>6) require(!footer.background.chips.empty() || footer.background.overflow.width>0,
            "background overflow stays reachable alongside active context");
        if(width>=80 && height>6) require(footer.directory.width>0,"active directory survives narrow background layout");
    }
    const auto combined=layout_session_footer({0,700,1100,26},1,true,true,true);
    require(combined.directory.width>100 && combined.git.width>0 && combined.background.chips.size()==1,
        "wide footer preserves directory and Git beside actual background sessions");
    const auto narrow_footer=layout_session_footer({0,700,300,26},4,true,true,true);
    require(narrow_footer.git.width==0 && narrow_footer.status.width==0 && narrow_footer.background.overflow.width>0,
        "narrow footer drops secondary context before directory and background access");
    const auto compact_notice=layout_notice({184,104,860,590},140,16);
    require(compact_notice.panel==Rect{196,646,164,36},"short notice is compact, workspace-local, and above footer");
    require(compact_notice.text==Rect{208,656,140,16},"notice retains 12px horizontal and 10px vertical padding");
    const auto long_notice=layout_notice({184,104,860,590},536,120);
    require(long_notice.panel.width==560 && long_notice.panel.height==140,"wrapped errors expand within a bounded readable width");
    for(double width:{0.,12.,24.,44.,120.,300.,640.,1100.}) for(double height:{0.,12.,24.,36.,80.,590.}) {
        const Rect viewport{184,104,width,height};
        for(const auto measured:std::array<Point,3>{{{140,16},{2000,2000},{0,0}}}) {
            const auto notice=layout_notice(viewport,measured.x,measured.y);
            require(notice.panel.x>=viewport.x && notice.panel.y>=viewport.y &&
                notice.panel.x+notice.panel.width<=viewport.x+width && notice.panel.y+notice.panel.height<=viewport.y+height,
                "notice stays inside narrow/short viewport without covering footer or rail");
            require(notice.text.x>=notice.panel.x && notice.text.y>=notice.panel.y && notice.text.width>=0 && notice.text.height>=0 &&
                notice.text.x+notice.text.width<=notice.panel.x+notice.panel.width && notice.text.y+notice.text.height<=notice.panel.y+notice.panel.height,
                "notice text remains clipped to owned panel");
        }
    }
    AppearanceConfig appearance;appearance.backdrop=BackdropPreference::automatic;
    auto material=window_material_policy(appearance,true,false,false,true);
    require(material.system_backdrop==3 && material.opacity<1,"Auto requests behind-window Acrylic, not wallpaper-only Mica");
    appearance.backdrop=BackdropPreference::mica;
    require(window_material_policy(appearance,true,false,false,true).system_backdrop==2,"explicit Mica choice preserved");
    appearance.backdrop=BackdropPreference::none;
    require(window_material_policy(appearance,true,false,false,true).opacity==1,"explicit Solid is opaque");
    appearance.backdrop=BackdropPreference::acrylic;
    for(const auto blocked:{window_material_policy(appearance,false,false,false,true),window_material_policy(appearance,true,true,false,true),
            window_material_policy(appearance,true,false,true,true),window_material_policy(appearance,true,false,false,false)})
        require(blocked.system_backdrop==1 && blocked.opacity==1 && blocked.fallback!=MaterialFallback::none,"unsupported/accessibility paths stay opaque");
    appearance.background_opacity=1;
    require(window_material_policy(appearance,true,false,false,true).opacity==1,"saved 100 percent opacity is not silently changed");
    const Rect probe_pane{184,104,500,500},probe_content{204,148,460,440};
    const auto patch=material_probe_patch(probe_pane,probe_content);
    require(patch && patch->x+patch->width<probe_content.x && patch->y>=probe_content.y,"alpha sample only uses empty padding, never terminal cells/header");
    const std::array<Rect,1> scrim{{{0,32,1100,688}}};
    require(!material_probe_patch(probe_pane,probe_content,scrim),"opaque/modal scrim cannot masquerade as canvas alpha");
    const std::array<Rect,1> bottom_toast{{{12,664,1076,44}}};
    require(material_probe_patch(probe_pane,probe_content,bottom_toast).has_value(),"nonintersecting toast leaves padding sample valid");
    require(!material_probe_patch({0,0,20,20},{4,4,12,12}),"insufficient padding produces no sample");
    MaterialRefreshTracker tracker;
    require(tracker.observe({false,false,0}),"first material observation");
    require(!tracker.observe({false,false,0}),"unchanged capability creates no idle work");
    require(tracker.observe({true,false,0}),"first composition surface triggers refresh");
    require(tracker.observe({false,true,1}),"device fallback triggers opaque policy refresh");
    require(!tracker.observe({false,true,1}),"unchanged fallback creates no loop");
    require(tracker.observe({true,false,2}),"device recovery restores requested material");
    require(tracker.observe({true,false,3}),"same-path device reconstruction refreshes native material");
    require(tracker.observe({true,false,3,true}),"host Gaussian activation refreshes material status");
    require(!tracker.observe({true,false,3,true}),"unchanged host Gaussian creates no idle loop");
    require(tracker.observe({true,false,3,false}),"host Gaussian loss refreshes unavailable status");
    auto navigation=layout_project_navigation(1100,700,32,{{1},{2}},0,true,false,true);
    auto chrome=layout_chrome(1100,700,{{1},{2}},{1},46,32,navigation.rail.width,navigation.tabs.height);
    require(chrome.band.height==32 && chrome.tabs_band.y==32,"thin caption is global, tasks occupy the next row");
    require(navigation.expanded && navigation.rail.width==184 && navigation.rail.y==32,"workspace navigation defaults below caption");
    require(chrome.tabs_band.x==navigation.rail.width && chrome.tabs[0].bounds.x>=navigation.rail.width,"workspace owns task tabs, not global caption");
    require(chrome.tabs[0].bounds.height==32 && chrome.tabs[0].bounds.y==36,"32-DIP task tabs centered in 40-DIP workspace row");
    require(chrome.tabs[0].bounds.width==188 && chrome.tabs.size()==2,"preferred task width");
    require(navigation.context.y==72 && navigation.viewport.y==104,"workspace context separates tasks from terminal");
    require(navigation.viewport.y+navigation.viewport.height==navigation.footer.y && navigation.footer.height==26,"factual footer never overlaps terminal");
    require(chrome.drag.width>=48,"global caption reserves a genuine drag target");
    require(chrome_hit_test(chrome,{chrome.maximize.x+10,15},{1100,700},6,false)==NativeHit::maximize,"Snap maximize region");
    require(chrome_hit_test(chrome,{60,20},{1100,700},6,false)==NativeHit::caption,"empty caption remains draggable");
    require(chrome_hit_test(chrome,{220,50},{1100,700},6,false)==NativeHit::client,"task tab never starts window drag");
    require(chrome_hit_test(chrome,{10,100},{1100,700},6,false)==NativeHit::client,"workspace navigation never starts window drag");
    require(chrome_hit_test(chrome,{1,1},{1100,700},6,false)==NativeHit::top_left,"native resize corner");
    require(chrome_hit_test(chrome,{1,699},{1100,700},6,true)==NativeHit::client,"maximized border never resizes");
    std::vector<ProjectWorkspaceId> projects;
    for(unsigned i=1;i<=256;++i)projects.push_back({i});
    for(int width=330;width<=2400;width+=37) for(int height=180;height<=1000;height+=53)
      for(const bool expanded:{false,true}) for(const bool focus:{false,true}) {
        const auto nav=layout_project_navigation(width,height,32,projects,250,expanded,focus,true);
        const auto inside=[&](Rect r) { return r.x>=0 && r.y>=0 && r.width>=0 && r.height>=0 && r.x+r.width<=width && r.y+r.height<=height; };
        for(const auto r:{nav.rail,nav.tabs,nav.context,nav.focus,nav.viewport,nav.footer,nav.toggle,nav.new_project,nav.settings})
            require(inside(r),"all shell chrome remains bounded at narrow/short window sizes");
        require(nav.viewport.x==nav.rail.width && nav.tabs.x==nav.rail.width,"tabs/context/body share navigation boundary");
        require(nav.viewport.x+nav.viewport.width==width && nav.viewport.y+nav.viewport.height==nav.footer.y,"body excludes global footer");
        require(nav.context.y==nav.tabs.y+nav.tabs.height && nav.viewport.y==nav.context.y+nav.context.height,"vertical workspace hierarchy has no gaps/overlap");
        require(nav.projects.size()<=nav.capacity,"navigation rows bounded");
        for(const auto& row:nav.projects) {
            require(inside(row.bounds) && row.bounds.y>=nav.toggle.y+nav.toggle.height,"rows exclude sidebar header");
            const auto bottom=nav.new_project.height>0?nav.new_project.y:nav.settings.height>0?nav.settings.y:nav.footer.y;
            require(row.bounds.y+row.bounds.height<=bottom,"rows exclude sidebar actions");
            require(row.project.value>=1 && row.project.value<=256,"only actual projects are displayed");
        }
        require(focus?nav.rail.width==0 && nav.context.height==0 && nav.footer.height==0:nav.rail.width==(expanded && width>=640?184:44),"focus and narrow windows retain useful terminal width");
        for(int count=1;count<=32;++count) {
            std::vector<TabId> tabs;for(int i=1;i<=count;++i)tabs.push_back({static_cast<std::uint64_t>(i)});
            const auto geometry=layout_chrome(width,height,tabs,tabs.back(),46,32,nav.rail.width,nav.tabs.height);
            require(geometry.tabs_band==nav.tabs,"render/hit-test task band matches navigation model");
            require(geometry.close.x+geometry.close.width==width && geometry.drag.width>=48,"caption/Snap and drag independent of tab count");
            require(geometry.new_tab.x+geometry.new_tab.width<=width,"new task remains bounded");
            for(const auto& tab:geometry.tabs) {
                require(inside(tab.bounds) && tab.bounds.x>=nav.rail.width && tab.bounds.y>=32,"tasks are children of current workspace column");
                require(tab.bounds.x+tab.bounds.width<=geometry.new_tab.x,"task and new-task hit targets don't overlap");
                const Point center{tab.bounds.x+tab.bounds.width/2,tab.bounds.y+tab.bounds.height/2};
                for(const double dpi:{1.0,1.25,1.5,2.0,3.0}) {
                    const Point physical{std::round(center.x*dpi),std::round(center.y*dpi)};
                    require(chrome_hit_test(geometry,{physical.x/dpi,physical.y/dpi},{double(width),double(height)},6,false)==NativeHit::client,"DPI-rounded task hit must not start native drag");
                }
            }
        }
      }
    navigation=layout_project_navigation(330,180,32,{{1}},0,true,false,true);
    chrome=layout_chrome(330,180,{{1},{2},{3}},{2},46,32,navigation.rail.width,navigation.tabs.height);
    require(chrome.compact && chrome.tabs.size()==1 && chrome.tabs[0].tab==TabId{2},"compact mode preserves active task picker");
    require(chrome.new_tab.width==28,"narrow layout retains new task without stealing caption drag");
    const auto no_context=layout_project_navigation(1100,720,32,{{1}},0,false,false,false);
    require(no_context.context.height==0 && no_context.viewport.y==72,"omitted context leaves no blank row");
    const auto empty_nav=layout_project_navigation(1100,720,32,{},999,false,false,true);
    require(empty_nav.projects.empty() && empty_nav.first==0,"no invented workspace entries");
    const auto fullscreen=layout_project_navigation(1100,720,0,{{1}},0,true,true,false);
    chrome=layout_chrome(1100,720,{{1}},{1},46,0,fullscreen.rail.width,fullscreen.tabs.height);
    require(chrome.band.height==0 && chrome.tabs.empty() && fullscreen.viewport==Rect{0,0,1100,720},"fullscreen has no invisible chrome interception");
    ModalFocus modal;
    require(modal.activate()==ModalDecision::cancel,"modal starts with safe Cancel");
    modal.next(); require(modal.activate()==ModalDecision::confirm,"Tab explicitly focuses destructive choice");
    modal.next(); require(modal.activate()==ModalDecision::cancel,"modal focus cycles without escaping");
    modal.select(ModalDecision::confirm); modal.reset();
    require(modal.activate()==ModalDecision::cancel,"new decision resets to Cancel");
    for (int width=330;width<=2200;width+=37) for (int height=180;height<=1000;height+=53) {
        const auto box=layout_modal(width,height);
        require(box.panel.x>=0 && box.panel.y>=48,"modal fits below integrated crown");
        require(box.panel.x+box.panel.width<=width && box.panel.y+box.panel.height<=height,"modal fits resized client");
        require(box.cancel.x+box.cancel.width<=box.confirm.x,"modal actions never overlap");
        require(box.body.y+box.body.height<=box.cancel.y && box.body.height>=18,"compact modal retains warning above actions");
    }
    ChromeTransition transition;
    require(!transition.active(100) && transition.value(100)==0, "resting transition has no scheduler work");
    transition.target(1,100,80);
    require(transition.value(100)==0 && transition.active(100), "transition starts from current presentation");
    require(transition.value(140)>0 && transition.value(140)<1, "finite hover interpolation");
    const auto midway=transition.value(140); transition.target(0,140,80);
    require(transition.value(140)==midway, "interrupted hover continues without jump");
    require(!transition.active(220) && transition.value(220)==0, "transition stops exactly at deadline");
    transition.target(1,300,0);
    require(!transition.active(300) && transition.value(300)==1, "reduced motion commits immediately without timer");
    transition.target(0,400,120); transition.finish();
    require(!transition.active(401) && transition.value(401)==0, "minimize/accessibility finishes timer work");
    ChromeTransition outgoing(1), incoming;
    outgoing.target(0,1000,120); incoming.target(1,1000,120);
    const auto handoff=incoming.value(1040);
    require(handoff>0 && handoff<1 && std::abs(outgoing.value(1040)+handoff-1)<1e-9,
            "selection handoff retains complementary outgoing and incoming emphasis");
    incoming.target(1,1040,120);
    require(incoming.value(1040)==handoff && !incoming.active(1120),
            "unrelated chrome changes never restart the selection deadline");
    incoming.target(0,1040,120); outgoing.target(1,1040,120);
    require(incoming.value(1040)==handoff && std::abs(outgoing.value(1040)+handoff-1)<1e-9,
            "rapid selection reversal continues from the displayed emphasis");
    require(incoming.value(1160)==0 && outgoing.value(1160)==1 && !outgoing.active(1160),
            "reversed handoff reaches rest with no idle timer");
    incoming.target(1,1200,100); incoming.finish();
    require(incoming.value(1201)==1 && !incoming.active(1201),
            "OS reduced motion during reveal settles the owned surface immediately");
    CursorBlinkClock blink;
    blink.configure(true,500,1000);
    require(blink.visible() && !blink.tick(1499),"caret starts with a complete visible interval");
    require(blink.tick(1500) && !blink.visible(),"caret deadline toggles visible phase");
    blink.restart(1999);
    require(blink.visible() && !blink.tick(2000),"queued old timer cannot hide just-typed caret");
    blink.configure(true,500,2200); // Output/settings checks do not restart unchanged policy.
    require(!blink.tick(2498) && blink.tick(2499) && !blink.visible(),"typing restarts a full visible interval");
    blink.configure(false,500,2500);
    require(blink.visible() && !blink.enabled() && !blink.tick(9000),"reduced motion or hidden focus yields steady visible phase and no work");
    blink.configure(true,300,10000);
    require(!blink.tick(10299) && blink.tick(10300),"system interval changes restart the deadline");
    blink.configure(false,0,10301);
    require(blink.visible() && !blink.tick(999999),"system no-blink setting restores steady cursor");
    Utf16Input input;
    require(input.feed(0xd83d).empty(), "hold high surrogate");
    require(input.feed(0xde80) == "\xf0\x9f\x9a\x80", "supplementary scalar");
    require(input.feed(0xdc00) == "\xef\xbf\xbd", "orphan low surrogate");
    input.feed(0xd800);
    require(input.feed(u'A') == "\xef\xbf\xbd" "A", "malformed pair recovery");
    input.feed(0xd800); input.reset();
    require(input.feed(u'B') == "B", "focus cancels pending scalar");
    require(input.feed(0xd800).empty(), "second surrogate start");
    require(input.feed(0xd83d)=="\xef\xbf\xbd", "consecutive high surrogate recovery");
    require(input.feed(0xde80)=="\xf0\x9f\x9a\x80", "new high surrogate still pairs");
    // Developer runtime selection uses stable references, never machine paths,
    // and an old saved default from an Appearance save remains authoritative.
    auto saved_cmd=default_config(Platform::windows);
    const auto original_cmd=saved_cmd;
    apply_developer_shell_default(saved_cmd,true);
    require(saved_cmd==original_cmd && !has_managed_zsh_selection(saved_cmd),"saved Cmd precedence");
    auto custom=saved_cmd; custom.profiles.front().executable="C:/Git/bin/bash.exe";
    const auto original_custom=custom; apply_developer_shell_default(custom,true);
    require(custom==original_custom,"saved custom shell precedence");
    auto fresh=default_config(Platform::windows); apply_developer_shell_default(fresh,false);
    require(has_managed_zsh_selection(fresh),"fresh configured launch selects managed reference");
    const auto once=fresh; apply_developer_shell_default(fresh,false);
    require(fresh==once,"managed default helper is idempotent");
    const auto encoded=serialize_config(fresh,Platform::windows);
    const auto decoded=parse_config(encoded,Platform::windows);
    require(decoded && serialize_config(*decoded.config,Platform::windows)==encoded && has_managed_zsh_selection(*decoded.config),"managed reference persists and resolves on next launch");
    const auto managed=managed_zsh_profile_reference();
    require(managed.arguments.empty() && managed.environment.empty() && managed.working_directory.empty(),"saved reference contains no installation path, inherited PATH or user environment");
    SettingsPanel settings; settings.open(saved_cmd,std::span<const Profile>(&managed,1));
    const auto activate=[&](std::string_view id) {
        for (unsigned i=0;i<256 && settings.focused_control()!=id;++i) settings.key({{"TAB"}});
        require(settings.focused_control()==id,"managed Settings control keyboard-reachable");
        settings.key({{"ENTER"}});
    };
    require(settings.draft()==saved_cmd && !settings.dirty(),"discovery never changes existing selection");
    activate("nav.shell"); activate("shell:stormtty-managed-zsh");
    require(has_managed_zsh_selection(settings.draft()) && settings.original()==saved_cmd,"managed shell selection drafts without mutating old config");
    const auto preview=settings.take_action();
    require(preview && preview->kind==SettingsActionKind::preview,"selection emits reversible draft, not persistence");
    settings.key({{"ENTER",CommandModifier::control}});
    const auto save=settings.take_action();
    require(save && save->kind==SettingsActionKind::save && settings.saving(),"managed choice requires explicit Save");
    const auto saved_reference=parse_config(serialize_config(save->config,Platform::windows),Platform::windows);
    require(saved_reference && has_managed_zsh_selection(*saved_reference.config),"Settings saved reference roundtrip");
    settings.persistence_result(true);
    // Save keeps draft insertion order while reload canonicalizes bindings.
    // This is not an external-file change and must not block reopening Settings.
    require(*saved_reference.config!=save->config,"fixture exercises canonical binding-order difference");
    require(settings_baseline_matches(*saved_reference.config,save->config,Platform::windows),"Save-to-reopen accepts identical persisted settings");
    auto changed_settings=*saved_reference.config; changed_settings.font.size_dip+=1;
    require(!settings_baseline_matches(changed_settings,save->config,Platform::windows),"external font change still rejects stale running baseline");
    changed_settings=*saved_reference.config; changed_settings.default_profile=original_cmd.default_profile;
    require(!settings_baseline_matches(changed_settings,save->config,Platform::windows),"external shell change still rejects stale running baseline");
    changed_settings=*saved_reference.config; changed_settings.bindings.pop_back();
    require(!settings_baseline_matches(changed_settings,save->config,Platform::windows),"external binding change still rejects stale running baseline");
    settings.open(*saved_reference.config,std::span<const Profile>(&managed,1));
    require(has_managed_zsh_selection(settings.draft()) && !settings.dirty(),"next Settings open retains managed choice");
    settings.cancel();settings.take_action();
    // Repeated saves canonicalize binding order, themes and sub-70% opacity.
    // Exact file-identity/byte conflict checks remain in Settings services.
    auto running=*saved_reference.config;
    for (unsigned iteration=0;iteration<12;++iteration) {
        settings.open(running);
        activate("appearance.theme."+builtin_themes()[iteration%builtin_themes().size()].name);
        for (unsigned i=0;i<256 && settings.focused_control()!="appearance.opacity";++i) settings.key({{"TAB"}});
        settings.key({{"HOME"}});
        for (unsigned step=0;step<iteration;++step) settings.key({{"RIGHT"}});
        settings.key({{"ENTER",CommandModifier::control}});
        const auto action=settings.take_action();require(action && action->kind==SettingsActionKind::save,"repeated save action");
        const auto on_disk=parse_config(serialize_config(action->config,Platform::windows),Platform::windows);
        require(on_disk && settings_baseline_matches(*on_disk.config,action->config,Platform::windows),"repeated Save-to-reopen keeps matching baseline");
        settings.persistence_result(true);running=action->config;
        settings.open(running);require(settings.is_open() && !settings.dirty(),"repeated reopen available without restart");
        settings.cancel();settings.take_action();
        auto externally_changed=*on_disk.config;externally_changed.theme.accent="#123456";
        require(!settings_baseline_matches(externally_changed,running,Platform::windows),"external theme edit is still rejected");
    }
    const auto title_chrome=layout_chrome(1044,720,{},{});
    const auto title=layout_window_title(title_chrome,title_chrome.drag.x+title_chrome.drag.width);
    require(title.x+title.width/2==522,"title is centered on entire window");
    require(title.x>=title_chrome.drag.x && title.x+title.width<=title_chrome.palette.x,"title avoids controls");
    require(layout_window_title(title_chrome,500).width==0,"title hides instead of crossing zoom control");
    const auto clock_footer=layout_session_footer({0,700,1044,26},3,true,true,true,true);
    require(clock_footer.clock.width==44 && clock_footer.clock.x+clock_footer.clock.width==1036,"clock has bounded right edge");
    for(const auto& chip:clock_footer.background.chips) require(chip.x+chip.width<clock_footer.clock.x,"clock does not cover parked sessions");
    require(layout_session_footer({0,700,639,26},3,true,true,true,true).clock.width==0,"narrow footer yields clock");
    settings.open(default_config(Platform::windows));
    activate("appearance.clock");
    require(!settings.draft().appearance.show_clock,"clock toggle updates draft");
    settings.cancel(); settings.take_action();
    auto clock_config=default_config(Platform::windows);
    require(clock_config.appearance.show_clock,"clock defaults enabled");
    clock_config.appearance.show_clock=false;
    const auto clock_roundtrip=parse_config(serialize_config(clock_config,Platform::windows),Platform::windows);
    require(clock_roundtrip && !clock_roundtrip.config->appearance.show_clock,"clock preference persists");
    // Declarative ribbon roundtrip, allowlist, priority and exact paint/hit geometry.
    auto ribbon_config=default_config(Platform::windows);
    ribbon_config.ribbon.hidden[ribbon_index(RibbonBinding::git)]=true;
    std::swap(ribbon_config.ribbon.order[0],ribbon_config.ribbon.order[5]);
    auto ribbon_roundtrip=parse_config(serialize_config(ribbon_config,Platform::windows),Platform::windows);
    require(ribbon_roundtrip && ribbon_roundtrip.config->ribbon==ribbon_config.ribbon,"ribbon visibility/order persist");
    require(!parse_config("version = 1\n[ribbon]\nhidden = [\"$(whoami)\"]\n",Platform::windows),"ribbon rejects executable/unknown binding");
    require(!parse_config("version = 1\n[ribbon]\nhidden = [\"clock.local\",\"clock.local\"]\n",Platform::windows),"ribbon rejects duplicates");
    for(int width=0;width<1500;++width) {
        auto f=layout_session_footer({0,700,double(width),26},3,true,true,true,true,ribbon_config.ribbon);
        require(f.git.width==0,"hidden git has no hitbox");
        std::vector<Rect> rects{f.directory,f.git,f.status,f.clock,f.agent,f.background.overflow};
        rects.insert(rects.end(),f.background.chips.begin(),f.background.chips.end());
        for(std::size_t i=0;i<rects.size();++i) if(rects[i].width>0) {
            require(rects[i].x>=0 && rects[i].x+rects[i].width<=width,"ribbon bounded at all widths");
            for(std::size_t j=i+1;j<rects.size();++j) if(rects[j].width>0)
                require(rects[i].x+rects[i].width<=rects[j].x || rects[j].x+rects[j].width<=rects[i].x,"ribbon has no overlap");
        }
    }
    ribbon_config.ribbon.hidden.fill(true);
    const auto hidden=layout_session_footer({0,700,1000,26},3,true,true,true,true,ribbon_config.ribbon);
    require(hidden.directory.width==0 && hidden.background.chips.empty() && hidden.background.overflow.width==0 && hidden.clock.width==0,"hidden items have no controls");
    const auto error_footer=layout_session_footer({0,700,200,26},3,true,true,true,true,ribbon_config.ribbon,{false,false,true});
    require(error_footer.status.width>0,"session error cannot be hidden");
    const auto attention=layout_session_footer({0,700,200,26},3,true,true,true,true,ribbon_config.ribbon,{true,true});
    require(attention.agent.width>0,"essential agent attention cannot be hidden");
    std::cout << "window geometry, input and managed shell profile tests passed\n";
}
