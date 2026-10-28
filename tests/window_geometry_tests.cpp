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
    settings.cancel();
    std::cout << "window geometry, input and managed shell profile tests passed\n";
}
