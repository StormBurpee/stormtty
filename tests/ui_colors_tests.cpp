#include "core_test.hpp"
#include "stormtty/ui_colors.hpp"
#include "stormtty/chrome_paint.hpp"
#include "stormtty/settings_panel.hpp"
#include "stormtty/context_menu.hpp"
#include "stormtty/settings_reload.hpp"
#include <limits>
#include <cstdio>
using namespace stormtty;
namespace {
double contrast(Color a,Color b) {const auto x=ui_luminance(a),y=ui_luminance(b);return (std::max(x,y)+.05)/(std::min(x,y)+.05);}
void check_palette(const Theme& theme) {
    const auto original=theme;
    const auto c=resolve_ui_colors(theme);
    CHECK(theme==original);
    CHECK(c.canvas==ui_color(theme.background,{}));
    CHECK(c.surface==ui_color(theme.chrome,{}));
    CHECK(c.ink==ui_color(theme.foreground,{}));
    CHECK(c.accent==ui_color(theme.accent,{}));
    CHECK(c.navigation==ui_mix(c.surface,c.canvas,.55));
    CHECK(c.selected==ui_mix(c.surface,c.accent,.16));
    CHECK(c.hover==ui_mix(c.surface,c.ink,.06));
    CHECK(c.raised==c.hover);
    CHECK(c.divider==ui_mix(c.surface,c.ink,.18));
    const auto tab=tab_selection_colors(c);
    auto tab_border=c.selected_border;tab_border.a=180;
    CHECK(tab.surface==c.selected);CHECK(tab.border==tab_border);
    const Color os_background{251,241,231,255},os_foreground{11,21,31,255};
    const auto contrast_tab=tab_selection_colors(c,true,os_background,os_foreground);
    CHECK(contrast_tab.surface==os_background && contrast_tab.border==os_foreground);
    for(const bool selected:{false,true})for(const double amount:{0.,.7037037037037037,1.}) {
        const auto pane=pane_focus_colors(c,selected,amount);
        auto wash=ui_mix(c.surface,c.selected,amount);wash.a=89;
        CHECK(pane.wash==wash);CHECK(pane.marker==c.accent);
        CHECK(pane.label==(selected?c.ink:c.secondary));CHECK(pane.show_marker==selected);
        const auto hc=pane_focus_colors(c,selected,amount,true,os_foreground,os_background);
        CHECK(hc.wash.a==0 && hc.label==os_foreground && hc.marker==os_background && hc.show_marker==selected);
    }
    for(auto color:{c.canvas,c.surface,c.navigation,c.raised,c.ink,c.secondary,c.subdued,c.divider,c.accent,c.control,c.hover,c.pressed,c.selected,c.selected_hover,c.selected_border,c.on_accent,c.positive,c.danger}) CHECK(color.a==255);
    CHECK(contrast(c.on_accent,c.accent)>=4.5);
    const auto floor=std::min(4.5,std::max(
        std::min(contrast({0,0,0,255},c.surface),contrast({0,0,0,255},c.selected)),
        std::min(contrast({255,255,255,255},c.surface),contrast({255,255,255,255},c.selected))));
    CHECK(contrast(c.danger,c.surface)>=floor);
    CHECK(contrast(c.danger,c.selected)>=floor);
}
std::vector<ContextMenuItem> menu_items() {
    return {{"normal","Normal","Ctrl+N",{},true,false,{}}, {"disabled","Disabled",{},"Unavailable reason",false,true,{}},
        ContextMenuItem::separator(), {"danger","Destructive",{}, {},true,true,{}},
        {"submenu","Submenu",{},{},true,false,{{"child","Child",{},{},true,false,{}}}}};
}
Color text_color(const WindowScene& scene,std::string_view text) {
    const auto found=std::find_if(scene.texts.begin(),scene.texts.end(),[&](const auto& t){return t.text==text;});
    CHECK(found!=scene.texts.end());return found->color;
}
void check_menu_palette(ContextMenu& menu,const Theme& theme) {
    menu.set_theme(theme);menu.resize({1024,768});CHECK(menu.open(menu_items(),{20,20},true));
    const auto c=resolve_ui_colors(theme);
    WindowScene scene;menu.append_scene(scene);
    CHECK(std::any_of(scene.primitives.begin(),scene.primitives.end(),[&](const auto& p){return p.background_replace && p.fill==c.surface && p.stroke==c.divider;}));
    CHECK(std::any_of(scene.primitives.begin(),scene.primitives.end(),[&](const auto& p){return p.fill==c.selected && p.stroke==c.selected_border;}));
    CHECK(text_color(scene,"Normal")==c.ink);CHECK(text_color(scene,"Ctrl+N")==c.secondary);
    CHECK(text_color(scene,"Disabled")==c.subdued);CHECK(text_color(scene,"Unavailable reason")==c.subdued);
    CHECK(text_color(scene,"Destructive")==c.danger);CHECK(text_color(scene,"›")==c.secondary);
    const auto& disabled=menu.layout().panels.front().rows[1].bounds;
    CHECK(menu.pointer_move({disabled.x+1,disabled.y+1}));
    scene={};menu.append_scene(scene);
    CHECK(text_color(scene,"Disabled")==c.secondary);CHECK(text_color(scene,"Unavailable reason")==c.secondary);
    CHECK(menu.key({{"END"}}));CHECK(menu.key({{"UP"}}));CHECK(menu.focused_id()=="danger");
    scene={};menu.append_scene(scene);CHECK(text_color(scene,"Destructive")==c.danger);
    const auto generation=menu.generation();menu.set_theme(theme);CHECK(menu.generation()==generation);
    auto renamed=theme;renamed.name="same-palette";renamed.ansi[1]="#00FF00";
    menu.set_theme(renamed);CHECK(menu.generation()==generation); // ANSI is terminal-only.
    // Exact OS colors win over every custom role, including selected danger,
    // disabled hover/reason, hints, submenu arrows and scrolling indicators.
    const Color bg{250,249,248,255},fg{12,13,14,255},hi{1,2,3,255},hitext{4,5,6,255},disabled_text{90,91,92,255};
    menu.set_high_contrast(true,bg,fg,hi,hitext,disabled_text);
    scene={};menu.append_scene(scene);
    CHECK(text_color(scene,"Destructive")==hitext);CHECK(text_color(scene,"Normal")==fg);
    CHECK(text_color(scene,"Disabled")==disabled_text);CHECK(text_color(scene,"Unavailable reason")==disabled_text);
    const auto allowed=[&](Color color){return color.a==0 || color==bg || color==fg || color==hi || color==hitext || color==disabled_text;};
    const auto check_allowed=[&] {
        for(const auto& p:scene.primitives){CHECK(allowed(p.fill));CHECK(allowed(p.stroke));}
        for(const auto& t:scene.texts)CHECK(allowed(t.color));
    };
    check_allowed();
    CHECK(menu.pointer_move({disabled.x+1,disabled.y+1}));scene={};menu.append_scene(scene);
    CHECK(text_color(scene,"Disabled")==disabled_text);CHECK(text_color(scene,"Unavailable reason")==disabled_text);check_allowed();
    CHECK(menu.key({{"HOME"}}));scene={};menu.append_scene(scene);
    CHECK(text_color(scene,"Normal")==hitext);CHECK(text_color(scene,"Ctrl+N")==hitext);check_allowed();
    CHECK(menu.key({{"END"}}));scene={};menu.append_scene(scene);CHECK(text_color(scene,"›")==hitext);check_allowed();
    menu.resize({200,130});CHECK(menu.key({{"HOME"}}));scene={};menu.append_scene(scene);CHECK(text_color(scene,"⌄")==fg);check_allowed();
    menu.set_high_contrast(false);scene={};menu.append_scene(scene);CHECK(text_color(scene,"⌄")==c.accent);
    menu.resize({1024,768});
    scene={};menu.append_scene(scene);CHECK(text_color(scene,"Destructive")==c.danger);
}
void activate(SettingsPanel& panel,std::string_view id) {
    for(unsigned i=0;i<256 && panel.focused_control()!=id;++i) CHECK(panel.key({{"TAB"}}));
    CHECK(panel.focused_control()==id);CHECK(panel.key({{"ENTER"}}));
}
}
int main() {return run_core_tests("semantic UI colors",[]{
    for(const auto& theme:builtin_themes()) {
        check_palette(theme);
        ContextMenu menu;check_menu_palette(menu,theme);
        const auto c=resolve_ui_colors(theme);
        CHECK(contrast(c.ink,c.surface)>=4.5);
        CHECK(contrast(c.secondary,c.surface)>=4.5);
        CHECK(contrast(c.ink,c.selected)>=4.5);
        CHECK(contrast(c.danger,c.surface)>=4.5);CHECK(contrast(c.danger,c.selected)>=4.5);
        CHECK(contrast(c.subdued,c.surface)>=3);CHECK(contrast(c.secondary,c.selected)>=3);
        for(const Color ink:{c.secondary,Color{12,34,56,255}}) {
            WindowScene icon;append_settings_icon(icon,{22,672},8,ink,1.25f,1.2);
            CHECK(icon.primitives.size()==56 && icon.texts.empty());
            for(const auto& p:icon.primitives)CHECK(p.stroke==ink && p.fill.a==0 && p.kind==ChromePrimitiveKind::line);
        }
        Config config;config.theme=theme;SettingsPanel panel;panel.open(config);panel.resize({1040,720});
        WindowScene scene;panel.append_scene(scene,1);
        CHECK(std::any_of(scene.primitives.begin(),scene.primitives.end(),[&](const auto& p){return p.fill==c.surface && p.background_replace;}));
        CHECK(std::any_of(scene.primitives.begin(),scene.primitives.end(),[&](const auto& p){return p.fill==c.navigation;}));
        CHECK(std::any_of(scene.texts.begin(),scene.texts.end(),[&](const auto& t){return t.color==c.ink;}));
        const Color background{255,255,255,255},foreground{0,0,0,255},highlight{0,80,20,255},highlight_text{255,255,0,255},disabled{80,30,10,255};
        panel.set_high_contrast(true,background,foreground,highlight,highlight_text,disabled);
        WindowScene contrast_scene;panel.append_scene(contrast_scene,1);
        const auto allowed=[&](Color paint){return paint.a==0 || paint==background || paint==foreground || paint==highlight || paint==highlight_text || paint==disabled;};
        for(const auto& p:contrast_scene.primitives) {CHECK(allowed(p.fill));CHECK(allowed(p.stroke));}
        for(const auto& t:contrast_scene.texts) CHECK(allowed(t.color));
        CHECK(panel.draft().theme==theme);

    }
    const auto stable=resolve_ui_colors(Theme{});
    // Exact-color high-contrast remapping must not confuse thumb/primary ink
    // with either highlight fills or foreground/detail roles.
    for(auto role:{stable.accent,stable.selected,stable.selected_hover,stable.ink,stable.divider,stable.secondary,stable.subdued,stable.positive}) CHECK(stable.on_accent!=role);
    Theme custom;custom.name="not-a-built-in";custom.background="#301423";custom.chrome="#412033";custom.foreground="#eeddcc";custom.accent="#fF8000";
    check_palette(custom);
    ContextMenu custom_menu;check_menu_palette(custom_menu,custom);
    auto light_custom=custom;light_custom.name="custom";light_custom.chrome="#E4F5EA";light_custom.foreground="#243B30";light_custom.accent="#19735C";
    check_palette(light_custom);check_menu_palette(custom_menu,light_custom);
    // Refresh an open submenu without changing geometry, focus, scroll or press.
    custom_menu.resize({320,260});CHECK(custom_menu.open(menu_items(),{310,250},true));
    CHECK(custom_menu.key({{"END"}}));CHECK(custom_menu.key({{"RIGHT"}}));
    CHECK(custom_menu.focused_id()=="child");
    WindowScene before;custom_menu.append_scene(before);
    const auto child=custom_menu.layout().panels.back().rows.front().bounds;
    CHECK(custom_menu.pointer_down({child.x+1,child.y+1}));
    const auto before_generation=custom_menu.generation();custom_menu.set_theme(custom);
    CHECK(custom_menu.generation()==before_generation+1);CHECK(custom_menu.focused_id()=="child");
    WindowScene after;custom_menu.append_scene(after);
    CHECK(before.primitives.size()==after.primitives.size() && before.texts.size()==after.texts.size());
    for(std::size_t i=0;i<before.primitives.size();++i) {
        auto expected=before.primitives[i];expected.fill=after.primitives[i].fill;expected.stroke=after.primitives[i].stroke;
        CHECK(expected==after.primitives[i]);
    }
    for(std::size_t i=0;i<before.texts.size();++i) {
        auto expected=before.texts[i];expected.color=after.texts[i].color;CHECK(expected==after.texts[i]);
    }
    CHECK(custom_menu.pointer_up({child.x+1,child.y+1}));custom_menu.set_theme(light_custom);CHECK(custom_menu.take_action()->id=="child");
    // Theme changes while OS contrast is active are retained for toggling back.
    custom_menu.set_high_contrast(true);custom_menu.set_theme(light_custom);custom_menu.set_high_contrast(false);
    CHECK(custom_menu.open(menu_items(),{20,20}));WindowScene restored;custom_menu.append_scene(restored);
    CHECK(text_color(restored,"Normal")==resolve_ui_colors(light_custom).ink);
    Theme invalid;invalid.chrome="bad";invalid.foreground="bad";invalid.accent="bad";invalid.background="bad";
    CHECK(resolve_ui_colors(invalid)==resolve_ui_colors(Theme{}));check_menu_palette(custom_menu,invalid);

    // Drive the actual Settings actions: preview, Cancel, failed Save/retry,
    // reopen with external disk changes, and valid/refused/failed Reload. Hosts
    // feed these resulting themes through the same menu palette-only setter.
    auto running=default_config(Platform::windows);const auto original=running;
    SettingsPanel editor;ContextMenu live;
    const auto apply=[&](const Config& config){running=config;live.set_theme(config.theme);check_menu_palette(live,config.theme);};
    editor.open(running);activate(editor,"appearance.theme.daylight");
    auto action=editor.take_action();CHECK(action && action->kind==SettingsActionKind::preview);apply(action->config);
    CHECK(running.theme==*find_builtin_theme("daylight"));
    editor.cancel();action=editor.take_action();CHECK(action && action->kind==SettingsActionKind::cancel);apply(action->config);CHECK(running==original);
    editor.open(running);CHECK(editor.import_palette(light_custom));action=editor.take_action();CHECK(action && action->kind==SettingsActionKind::preview);apply(action->config);
    activate(editor,"save");action=editor.take_action();CHECK(action && action->kind==SettingsActionKind::save);
    editor.persistence_result(false,"fixture failure");CHECK(editor.is_open());CHECK(running.theme==light_custom);
    activate(editor,"save");action=editor.take_action();CHECK(action && action->kind==SettingsActionKind::save);editor.persistence_result(true);apply(action->config);
    const auto saved=running;editor.open(running);CHECK(!editor.take_action());editor.cancel();apply(editor.take_action()->config);CHECK(running==saved);
    auto disk=saved;disk.theme=*find_builtin_theme("ember");editor.open(running,disk);
    action=editor.take_action();CHECK(action && action->kind==SettingsActionKind::preview);apply(action->config);
    SettingsReload reload;CHECK(!reload.begin(true,false));editor.cancel();apply(editor.take_action()->config);CHECK(running==saved);
    CHECK(reload.begin(false,false));SettingsSnapshotResult loaded;loaded.config=disk;loaded.snapshot=ConfigFileSnapshot{};loaded.snapshot->existed=true;
    auto result=reload.complete(reload.generation(),std::move(loaded));CHECK(result.config);apply(*result.config);CHECK(running==disk);
    CHECK(reload.begin(false,false));result=reload.complete(reload.generation(),{});CHECK(!result.config && !result.error.empty());CHECK(running==disk);

    auto renamed=custom;renamed.name="daylight";
    CHECK(resolve_ui_colors(renamed).navigation==resolve_ui_colors(custom).navigation);
    CHECK(resolve_ui_colors(custom).raised!=resolve_ui_colors(Theme{}).raised);
    const Color fallback{12,34,56,255};
    for(auto value:{"","#12345","#1234567","1234567","#GG1234","#00zz00"}) CHECK(ui_color(value,fallback)==fallback);
    CHECK(ui_color("#aB09Ff",fallback)==Color{171,9,255,255});
    CHECK(ui_mix({0,0,0,1},{255,255,255,2},-1)==Color{0,0,0,255});
    CHECK(ui_mix({0,0,0,1},{255,255,255,2},2)==Color{255,255,255,255});
    CHECK(ui_mix({0,0,0,1},{255,255,255,2},std::numeric_limits<double>::quiet_NaN())==Color{0,0,0,255});
    for(unsigned r=0;r<256;r+=17) for(unsigned g=0;g<256;g+=17) for(unsigned b=0;b<256;b+=17) {
        const Color accent{static_cast<std::uint8_t>(r),static_cast<std::uint8_t>(g),static_cast<std::uint8_t>(b),255};
        char hex[8]{};std::snprintf(hex,sizeof hex,"#%02X%02X%02X",r,g,b);custom.accent=hex;
        CHECK(contrast(resolve_ui_colors(custom).on_accent,accent)>=4.5);
        // Accent and surface combinations include the hard mid-luminance case.
        custom.chrome=hex;custom.accent=hex;check_palette(custom);
        custom.accent="#FFFFFF";check_palette(custom);
    }
});}
