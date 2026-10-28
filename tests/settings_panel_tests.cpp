#include "core_test.hpp"
#include "stormtty/settings_panel.hpp"
#include <algorithm>
#include <limits>
#include <set>

using namespace stormtty;
namespace {
const SettingsControl& item(const SettingsPanel& panel,std::string_view id) {
    const auto& controls=panel.layout().controls;
    const auto found=std::find_if(controls.begin(),controls.end(),[&](const auto& c){return c.id==id;});
    CHECK(found!=controls.end()); return *found;
}
void focus(SettingsPanel& panel,std::string_view id) {
    for (unsigned i=0;i<200 && panel.focused_control()!=id;++i) CHECK(panel.key({{"TAB",CommandModifier::none}}));
    CHECK(panel.focused_control()==id);
}
void activate(SettingsPanel& panel,std::string_view id) {
    focus(panel,id); CHECK(panel.key({{"ENTER",CommandModifier::none}}));
}
void click(SettingsPanel& panel,std::string_view id) {
    const auto bounds=item(panel,id).bounds;CHECK(item(panel,id).visible);
    const Point point{bounds.x+bounds.width/2,bounds.y+bounds.height/2};
    CHECK(panel.pointer_down(point));CHECK(panel.pointer_up(point));
}
void validate_scene(const WindowScene& scene,double width,double height,double scale) {
    for (const auto& primitive:scene.primitives) {
        const auto& r=primitive.rect;
        CHECK(std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) && std::isfinite(r.height));
        if (primitive.kind==ChromePrimitiveKind::line) {
            CHECK(r.x>=0 && r.y>=0 && r.x<=width*scale && r.y<=height*scale);
            CHECK(r.x+r.width>=0 && r.y+r.height>=0);
        } else CHECK(r.x>=0 && r.y>=0 && r.width>0 && r.height>0);
        CHECK(r.x+r.width<=width*scale+0.02 && r.y+r.height<=height*scale+0.02);
    }
    for (const auto& text:scene.texts) {
        const auto& r=text.rect;
        CHECK(std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) && std::isfinite(r.height));
        CHECK(r.x>=0 && r.y>=0 && r.width>0 && r.height>0);
        CHECK(r.x+r.width<=width*scale+0.02 && r.y+r.height<=height*scale+0.02);
        CHECK(text.size_dip>=1 && text.size_dip<=256);
    }
}
}
int main() {return run_core_tests("settings panel",[] {
    const auto defaults=default_config(Platform::windows);
    SettingsPanel panel;
    CHECK(!panel.is_open());CHECK(!panel.key({{"TAB"}}));CHECK(!panel.pointer_down({0,0}));
    panel.set_material_status("Opaque: Windows Transparency effects are off.");
    const auto material_generation=panel.generation();
    panel.set_material_status("Opaque: Windows Transparency effects are off.");CHECK(panel.generation()==material_generation);
    panel.open(defaults);CHECK(panel.is_open());CHECK(!panel.dirty());CHECK(panel.draft()==defaults);
    CHECK(!panel.take_action());
    WindowScene material_scene;panel.append_scene(material_scene);
    CHECK(std::any_of(material_scene.texts.begin(),material_scene.texts.end(),[](const auto& t){return t.text=="Opaque: Windows Transparency effects are off.";}));
    CHECK(panel.draft()==defaults && !panel.dirty());
    CHECK(std::any_of(material_scene.texts.begin(),material_scene.texts.end(),[](const auto& t){return t.text=="Save changes" && t.alignment==ChromeTextAlignment::center;}));
    CHECK(std::any_of(material_scene.texts.begin(),material_scene.texts.end(),[](const auto& t){return t.text=="Appearance" && t.alignment==ChromeTextAlignment::leading;}));
    CHECK(std::none_of(material_scene.texts.begin(),material_scene.texts.end(),[](const auto& t){return t.text=="S" || t.text=="Local preferences" || t.text=="Applies to StormTTY." || t.text=="Your sessions keep running.";}));
    CHECK(std::any_of(material_scene.primitives.begin(),material_scene.primitives.end(),[](const auto& p){return p.kind==ChromePrimitiveKind::line;}));
    const auto font_value=std::find_if(material_scene.texts.begin(),material_scene.texts.end(),[](const auto& t){return t.text=="15";});
    CHECK(font_value!=material_scene.texts.end() && font_value->alignment==ChromeTextAlignment::center);
    const auto decrease=item(panel,"font.decrease").bounds,increase=item(panel,"font.increase").bounds;
    CHECK(near(font_value->rect.x+font_value->rect.width/2,(decrease.x+decrease.width+increase.x)/2));
    CHECK(near(font_value->rect.y+font_value->rect.height/2,decrease.y+decrease.height/2));
    // Semantic controls are unique and every field fits the ordinary modal.
    std::set<std::string> ids;
    for (const auto& c:panel.layout().controls) {CHECK(ids.insert(c.id).second);CHECK(c.visible);}
    activate(panel,"font.increase");CHECK(panel.draft().font.size_dip==16);CHECK(panel.original()==defaults);
    auto preview=panel.take_action();CHECK(preview && preview->kind==SettingsActionKind::preview);CHECK(preview->config.font.size_dip==16);
    activate(panel,"font.ligatures");CHECK(panel.draft().font.ligatures);
    activate(panel,"font.family");CHECK(panel.draft().font.family!=defaults.font.family);
    activate(panel,"appearance.motion");CHECK(panel.draft().appearance.reduced_motion==ReducedMotionPreference::enabled);
    for (const auto& [id,value]:std::vector<std::pair<std::string,BackdropPreference>>{{"auto",BackdropPreference::automatic},{"none",BackdropPreference::none},{"mica",BackdropPreference::mica},{"acrylic",BackdropPreference::acrylic}}) {
        activate(panel,"appearance.backdrop."+id);CHECK(panel.draft().appearance.backdrop==value);
    }
    focus(panel,"appearance.opacity");CHECK(panel.key({{"HOME"}}));CHECK(near(panel.draft().appearance.background_opacity,0));
    CHECK(panel.key({{"LEFT"}}));CHECK(near(panel.draft().appearance.background_opacity,0));
    CHECK(panel.key({{"RIGHT"}}));CHECK(near(panel.draft().appearance.background_opacity,0.01));
    CHECK(panel.key({{"END"}}));CHECK(near(panel.draft().appearance.background_opacity,1));
    CHECK(panel.key({{"RIGHT"}}));CHECK(near(panel.draft().appearance.background_opacity,1));
    auto slider=item(panel,"appearance.opacity").bounds;
    CHECK(panel.pointer_down({slider.x+8,slider.y+10}));CHECK(near(panel.draft().appearance.background_opacity,0));
    CHECK(panel.pointer_move({slider.x+slider.width*5,slider.y+10}));CHECK(near(panel.draft().appearance.background_opacity,1));
    CHECK(panel.pointer_up({slider.x+slider.width/2,slider.y+10}));CHECK(near(panel.draft().appearance.background_opacity,0.5));
    CHECK(panel.take_action());CHECK(!panel.take_action()); // Rapid slider previews coalesce.
    CHECK(panel.pointer_down({slider.x+8,slider.y+10}));panel.pointer_cancel();
    const auto canceled_drag=panel.draft();CHECK(panel.pointer_move({slider.x+slider.width,slider.y+10}));CHECK(panel.pointer_up({slider.x+slider.width,slider.y+10}));CHECK(panel.draft()==canceled_drag);
    focus(panel,"font.decrease");for (int i=0;i<100;++i) CHECK(panel.key({{"ENTER"}}));CHECK(panel.draft().font.size_dip==6);
    focus(panel,"font.increase");for (int i=0;i<100;++i) CHECK(panel.key({{"ENTER"}}));CHECK(panel.draft().font.size_dip==72);
    CHECK(validate_config(panel.draft(),Platform::windows));
    const auto previous=panel.draft();panel.open(defaults);CHECK(panel.draft()==previous); // No reset on repeated open.
    CHECK(panel.key({{"A"}}));CHECK(panel.key({{"A"},false,false,false}));CHECK(panel.draft()==previous);
    CHECK(panel.pointer_down({0,0}));CHECK(panel.pointer_up({0,0}));CHECK(panel.is_open());
    CHECK(panel.key({{"ESCAPE"}}));CHECK(!panel.is_open());CHECK(panel.draft()==defaults);
    const auto cancel=panel.take_action();CHECK(cancel && cancel->kind==SettingsActionKind::cancel && cancel->config==defaults);

    // Named themes change the actual draft and sample palette, remain reversible,
    // and preserve existing customized palettes until an explicit selection.
    for (const auto platform:{Platform::windows,Platform::macos}) {
        SettingsPanel themes(platform);auto config=default_config(platform);themes.open(config);
        for (const auto& theme:builtin_themes()) {
            click(themes,"appearance.theme."+theme.name);CHECK(themes.draft().theme==theme);
            if (theme!=config.theme) {const auto action=themes.take_action();CHECK(action && action->kind==SettingsActionKind::preview && action->config.theme==theme);}
            WindowScene themed;themes.append_scene(themed);
            const auto sample=std::find_if(themed.texts.begin(),themed.texts.end(),[](const auto& t){return t.text==">_  ABC abc 0123 {} [] -> !=";});
            CHECK(sample!=themed.texts.end());
            if (theme.name=="daylight") CHECK(sample->color==Color{35,43,51,255});
        }
        focus(themes,"appearance.theme.daylight");CHECK(themes.key({{"LEFT"}}));CHECK(themes.draft().theme.name=="instrument");
        themes.cancel();CHECK(themes.take_action()->config.theme==config.theme);
        config.theme.accent="#123456";themes.open(config);click(themes,"appearance.theme.daylight");
        CHECK(themes.draft().theme.name=="daylight");click(themes,"appearance.theme.custom");CHECK(themes.draft().theme==config.theme);
        click(themes,"save");const auto saved=themes.take_action();CHECK(saved && saved->kind==SettingsActionKind::save);
        themes.persistence_result(true);themes.open(saved->config);CHECK(!themes.dirty() && themes.draft().theme==config.theme);
        themes.cancel();
    }

    // Shell selection preserves configured executable/arguments/environment.
    Profile git;git.id="git-bash";git.label="Git Bash";git.executable="C:\\Program Files\\Git\\bin\\bash.exe";git.arguments={"--login","-i"};git.environment["CHERE_INVOKING"]="1";
    auto invalid=git;invalid.id="bad shell";
    auto duplicate=defaults.profiles.front();duplicate.executable="wrong.exe";
    const std::vector discovered{git,invalid,duplicate};
    panel.open(defaults,discovered);click(panel,"nav.shell");CHECK(panel.section()==SettingsSection::shell);
    CHECK(std::none_of(panel.layout().controls.begin(),panel.layout().controls.end(),[](const auto& c){return c.id=="shell:bad shell";}));
    CHECK(panel.draft().profiles.size()==defaults.profiles.size());
    click(panel,"shell:git-bash");CHECK(panel.draft().default_profile=="git-bash");CHECK(panel.draft().profiles.back()==git);
    click(panel,"shell:git-bash");CHECK(panel.draft().profiles.size()==defaults.profiles.size()+1);
    click(panel,"shell:"+defaults.default_profile);CHECK(panel.draft().profiles.front()==defaults.profiles.front());
    click(panel,"shell:git-bash");click(panel,"save");CHECK(panel.saving());CHECK(panel.is_open());
    auto save=panel.take_action();CHECK(save && save->kind==SettingsActionKind::save && save->config.default_profile=="git-bash");
    CHECK(panel.key({{"ESCAPE"}}));panel.cancel();CHECK(panel.is_open());CHECK(!panel.take_action());
    panel.persistence_result(false,"Disk full\nPlease retry");CHECK(!panel.saving());CHECK(panel.is_open());CHECK(panel.error()=="Disk full Please retry");CHECK(panel.dirty());
    click(panel,"save");CHECK(panel.take_action());panel.persistence_result(true);CHECK(!panel.is_open());CHECK(!panel.dirty());CHECK(panel.original().default_profile=="git-bash");
    panel.persistence_result(false,"stale response");CHECK(!panel.is_open());

    // Shortcut capture never invokes or steals a command. Existing conflicts,
    // OS reservations, AltGr/composition and malformed chords are all rejected.
    panel.open_for_binding(defaults,"pane.split_down");CHECK(panel.section()==SettingsSection::shortcuts);CHECK(panel.capturing_shortcut());
    const auto prior_bindings=panel.draft().bindings;
    CHECK(panel.key({{"F4",CommandModifier::alt}}));CHECK(panel.capturing_shortcut());CHECK(!panel.error().empty());CHECK(panel.draft().bindings==prior_bindings);
    const auto conflict=std::find_if(defaults.bindings.begin(),defaults.bindings.end(),[](const auto& b){return b.command=="tab.new";});CHECK(conflict!=defaults.bindings.end());
    CHECK(panel.key({conflict->chord}));CHECK(panel.error().starts_with("Already assigned to"));CHECK(panel.draft().bindings==prior_bindings);
    CHECK(panel.key({{"Q",CommandModifier::control|CommandModifier::shift},true}));CHECK(panel.capturing_shortcut());
    CHECK(panel.key({{"Q",CommandModifier::control|CommandModifier::shift},false,true}));CHECK(panel.capturing_shortcut());
    CHECK(panel.key({{std::string(1000,'X'),CommandModifier::control|CommandModifier::shift}}));CHECK(panel.capturing_shortcut());
    const KeyChord assigned{"F12",CommandModifier::control|CommandModifier::shift};
    CHECK(panel.key({assigned}));CHECK(!panel.capturing_shortcut());CHECK(panel.error().empty());CHECK(validate_config(panel.draft(),Platform::windows));
    CHECK(std::any_of(panel.draft().bindings.begin(),panel.draft().bindings.end(),[&](const auto& b){return b.command=="pane.split_down" && b.chord==assigned;}));
    CHECK(panel.take_action()->kind==SettingsActionKind::preview);
    panel.open_for_binding(defaults,"pane.split_down");CHECK(panel.capturing_shortcut());CHECK(panel.key({{"ESCAPE"}}));CHECK(!panel.capturing_shortcut());CHECK(panel.is_open());
    panel.open_for_binding(defaults,"pane.split_down");CHECK(panel.key({{"BACKSPACE"}}));CHECK(!panel.capturing_shortcut());
    CHECK(std::none_of(panel.draft().bindings.begin(),panel.draft().bindings.end(),[](const auto& b){return b.command=="pane.split_down";}));
    CHECK(panel.key({{"ENTER",CommandModifier::control}}));CHECK(panel.saving());CHECK(panel.take_action()->kind==SettingsActionKind::save);panel.persistence_result(true);

    // A terminal shortcut is allowed only after an explicit confirmation. It
    // is not mislabeled as OS-reserved, and holding a key cannot confirm it.
    for (const auto platform:{Platform::windows,Platform::macos}) {
        SettingsPanel shortcuts(platform);const auto original=default_config(platform);
        shortcuts.open_for_binding(original,"pane.split_right");
        const KeyChord alt_bracket{"]",CommandModifier::alt};
        CHECK(shortcuts.key({alt_bracket}));CHECK(shortcuts.capturing_shortcut());CHECK(shortcuts.draft()==original);CHECK(shortcuts.error().empty());
        CHECK(item(shortcuts,"binding.confirm").visible);
        shortcuts.resize({480,480});WindowScene warning;shortcuts.append_scene(warning);
        CHECK(std::any_of(warning.texts.begin(),warning.texts.end(),[](const auto& t){return t.text.ends_with(" replaces terminal input");}));
        shortcuts.resize({1024,768});
        for (int repeat=0;repeat<5;++repeat) CHECK(shortcuts.key({alt_bracket}));
        CHECK(shortcuts.draft()==original);
        CHECK(shortcuts.key({{"TAB"}}));CHECK(shortcuts.focused_control()=="binding.confirm");
        CHECK(shortcuts.key({{"ENTER"},true}));CHECK(shortcuts.draft()==original); // IME
        CHECK(shortcuts.key({{"ENTER"},false,true}));CHECK(shortcuts.draft()==original); // AltGr
        CHECK(shortcuts.key({{"ENTER"}}));CHECK(!shortcuts.capturing_shortcut());
        CHECK(shortcuts.draft().allow_terminal_shortcut_override);CHECK(validate_config(shortcuts.draft(),platform));
        const auto roundtrip=parse_config(serialize_config(shortcuts.draft(),platform),platform);CHECK(roundtrip);
        CHECK(resolve_command({alt_bracket},roundtrip.config->bindings,{1,1},platform)=="pane.split_right");
        CHECK(!resolve_command({alt_bracket,true},roundtrip.config->bindings,{1,1},platform));
        CHECK(!resolve_command({alt_bracket,false,true},roundtrip.config->bindings,{1,1},platform));
        shortcuts.cancel();CHECK(shortcuts.take_action()->config==original);
        shortcuts.open_for_binding(original,"pane.split_right");CHECK(shortcuts.key({alt_bracket}));
        CHECK(shortcuts.key({{"ESCAPE"}}));CHECK(shortcuts.draft()==original);CHECK(!shortcuts.capturing_shortcut());
        shortcuts.open_for_binding(original,"pane.split_right");CHECK(shortcuts.key({alt_bracket}));click(shortcuts,"binding.confirm");
        CHECK(shortcuts.draft().allow_terminal_shortcut_override);
        click(shortcuts,"save");CHECK(shortcuts.take_action()->kind==SettingsActionKind::save);shortcuts.persistence_result(true);
        shortcuts.open(shortcuts.original());CHECK(shortcuts.is_open());CHECK(!shortcuts.dirty());
    }

    // Focus trapping, reverse traversal, scrolling to off-screen controls, and
    // clipping across tiny/resized/high-DPI viewports are portable guarantees.
    for (const auto size:std::vector<Size>{{1280,800},{1024,768},{720,620},{480,480},{320,260},{40,40},{0,0}}) {
        for (const auto scale:{1.0f,1.25f,1.5f,2.0f,3.0f}) {
            SettingsPanel responsive;responsive.resize(size);responsive.open(defaults,discovered);
            WindowScene scene;responsive.append_scene(scene,scale);validate_scene(scene,size.width,size.height,scale);
            const auto first=responsive.focused_control();CHECK(responsive.key({{"TAB",CommandModifier::shift}}));CHECK(responsive.key({{"TAB"}}));CHECK(responsive.focused_control()==first);
            activate(responsive,"nav.shortcuts");focus(responsive,"binding:window.fullscreen");
            if (responsive.layout().content.height>=52 && responsive.layout().content.width>0) CHECK(item(responsive,"binding:window.fullscreen").visible);
            WindowScene shortcuts;responsive.append_scene(shortcuts,scale);validate_scene(shortcuts,size.width,size.height,scale);
            responsive.cancel();
        }
    }
    SettingsPanel bounds;bounds.open(defaults);bounds.resize({std::numeric_limits<double>::infinity(),768});CHECK(bounds.layout().panel.width>0);
    const auto before=bounds.draft();CHECK(bounds.pointer_down({std::numeric_limits<double>::quiet_NaN(),1}));CHECK(bounds.pointer_move({1,std::numeric_limits<double>::infinity()}));CHECK(bounds.pointer_up({1,1}));CHECK(bounds.draft()==before);
    WindowScene hidden;const auto panel_bounds=bounds.layout().panel;
    hidden.texts.push_back({{float(panel_bounds.x+20),float(panel_bounds.y+20),100,20},"SHOULD NOT LEAK"});
    hidden.texts.push_back({{0,0,10,10},"outside"});bounds.append_scene(hidden);
    CHECK(hidden.texts[0].text=="SHOULD NOT LEAK" && hidden.texts[0].color.a==255 && hidden.texts[0].z.layer==ChromeLayer::base);
    CHECK(hidden.texts[1].text=="outside" && hidden.texts[1].color.a==255);
    for (std::size_t i=2;i<hidden.texts.size();++i) CHECK(hidden.texts[i].z.layer==ChromeLayer::settings);
    for (const auto& primitive:hidden.primitives) CHECK(primitive.z.layer==ChromeLayer::settings);
    const auto paint=chrome_paint_order(hidden);
    const auto underlying=std::find_if(paint.begin(),paint.end(),[](const auto& item){return item.kind==ChromeDrawKind::text && item.index==0;});
    const auto scrim=std::find_if(paint.begin(),paint.end(),[](const auto& item){return item.kind==ChromeDrawKind::primitive && item.index==0;});
    CHECK(underlying<scrim);
    CHECK(std::any_of(hidden.primitives.begin(),hidden.primitives.end(),[](const auto& p){return p.background_replace;}));
    // Actual OS high-contrast colors, including a light background, replace
    // every decorative paint. HighlightText remains readable on selected rows.
    for (const auto background:std::vector<Color>{{0,0,0,255},{255,255,255,255}}) {
        const Color foreground=background.r==0?Color{255,255,255,255}:Color{0,0,0,255};
        const Color highlight{0,72,64,255}, highlight_text{255,245,200,255}, disabled{135,115,130,255};
        const auto config_before=bounds.draft();
        bounds.set_high_contrast(true,background,foreground,highlight,highlight_text,disabled);
        const auto generation=bounds.generation();
        bounds.set_high_contrast(true,background,foreground,highlight,highlight_text,disabled);CHECK(bounds.generation()==generation);
        CHECK(bounds.draft()==config_before);
        WindowScene high;bounds.append_scene(high);
        const std::vector<Color> allowed{background,foreground,highlight,highlight_text,disabled};
        for (const auto& primitive:high.primitives) {
            for (const auto color:{primitive.fill,primitive.stroke}) if (color.a) {CHECK(color.a==255);CHECK(std::find(allowed.begin(),allowed.end(),color)!=allowed.end());}
        }
        for (const auto& label:high.texts) {CHECK(label.color.a==255);CHECK(std::find(allowed.begin(),allowed.end(),label.color)!=allowed.end());}
        const auto selected=std::find_if(high.texts.begin(),high.texts.end(),[](const auto& t){return t.text=="Appearance" && t.size_dip==13;});
        CHECK(selected!=high.texts.end() && selected->color==highlight_text);
        const auto save_text=std::find_if(high.texts.begin(),high.texts.end(),[](const auto& t){return t.text=="Save changes";});
        CHECK(save_text!=high.texts.end() && save_text->color==highlight_text);
    }
    // High contrast remains semantic even if a custom theme aliases every color.
    auto monochrome=defaults;monochrome.theme={"custom","#000000","#000000","#000000","#000000"};
    SettingsPanel contrast;contrast.open(monochrome);contrast.set_high_contrast(true,{0,0,0,255},{255,255,255,255},{0,72,64,255},{255,245,200,255});
    WindowScene contrast_scene;contrast.append_scene(contrast_scene);
    const auto contrast_sample=std::find_if(contrast_scene.texts.begin(),contrast_scene.texts.end(),[](const auto& t){return t.text==">_  ABC abc 0123 {} [] -> !=";});
    CHECK(contrast_sample!=contrast_scene.texts.end() && contrast_sample->color==Color{255,255,255,255});
    CHECK(contrast.draft()==monochrome);
    click(bounds,"save");CHECK(bounds.saving());WindowScene saving;bounds.append_scene(saving);
    const auto disabled_label=std::find_if(saving.texts.begin(),saving.texts.end(),[](const auto& t){return t.text=="Saving…";});
    CHECK(disabled_label!=saving.texts.end() && disabled_label->color==Color{135,115,130,255});
    bounds.persistence_result(false,"Retry");bounds.set_high_contrast(false);
    WindowScene graphite;bounds.append_scene(graphite);CHECK(graphite.primitives.front().fill.a<255);
    bounds.cancel();WindowScene closed;bounds.append_scene(closed);CHECK(closed.primitives.empty() && closed.texts.empty());
});}
