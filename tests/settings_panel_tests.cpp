#include "core_test.hpp"
#include "stormtty/settings_panel.hpp"
#include <algorithm>
#include <limits>
#include <fstream>
#include <chrono>
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
    {
    // Import/export are explicit native host actions, not persistence requests.
    SettingsPanel exchange;exchange.open(defaults);
    activate(exchange,"appearance.import");auto import_action=exchange.take_action();
    CHECK(import_action && import_action->kind==SettingsActionKind::import_theme);
    CHECK(!exchange.dirty());CHECK(!exchange.saving());
    auto imported=*find_builtin_theme("ember");imported.name="custom";
    CHECK(exchange.import_palette(imported));CHECK(exchange.dirty());
    CHECK(exchange.draft().theme==imported);CHECK(exchange.original()==defaults);
    auto preview=exchange.take_action();CHECK(preview && preview->kind==SettingsActionKind::preview);
    activate(exchange,"appearance.theme.graphite");(void)exchange.take_action();
    activate(exchange,"appearance.theme.custom");CHECK(exchange.draft().theme==imported);(void)exchange.take_action();
    activate(exchange,"appearance.export");auto export_action=exchange.take_action();
    CHECK(export_action && export_action->kind==SettingsActionKind::export_theme);
    CHECK(export_action->config.theme==imported);CHECK(!exchange.saving());
    exchange.theme_exchange_notice("Cursor and selection colors weren’t imported.");
    CHECK(exchange.error().empty());WindowScene notice_scene;exchange.append_scene(notice_scene);
    CHECK(std::any_of(notice_scene.texts.begin(),notice_scene.texts.end(),[](const auto& t){return t.text=="Cursor and selection colors weren’t imported.";}));
    auto broken=imported;broken.background="invalid";
    CHECK(!exchange.import_palette(broken));CHECK(exchange.draft().theme==imported);
    exchange.theme_exchange_error("Bad theme");CHECK(exchange.error()=="Bad theme");CHECK(exchange.draft().theme==imported);
    exchange.theme_exchange_notice("Palette exported.");CHECK(exchange.error().empty());CHECK(exchange.draft().theme==imported);
    exchange.cancel();auto rollback=exchange.take_action();CHECK(rollback && rollback->kind==SettingsActionKind::cancel);
    CHECK(rollback->config==defaults);CHECK(!exchange.import_palette(imported));
    exchange.open(defaults);CHECK(exchange.import_palette(imported));(void)exchange.take_action();
    activate(exchange,"save");CHECK(exchange.saving());CHECK(!exchange.import_palette(Theme{}));
    CHECK(exchange.draft().theme==imported);
    SettingsPanel mac_exchange(Platform::macos);mac_exchange.open(default_config(Platform::macos));
    for(const auto& c:mac_exchange.layout().controls)CHECK(c.id!="appearance.import" && c.id!="appearance.export");
    }
    // Backend is a stable draft preference, not a process launch or live preview.
    SettingsPanel backend;
    backend.set_console_backend_status(true,"Diagnostic CLI override active; saved choice applies without the override.");
    backend.open(defaults);
    activate(backend,"nav.shell");
    CHECK(item(backend,"console.system").enabled && item(backend,"console.bundled").enabled);
    activate(backend,"console.bundled");
    CHECK(backend.draft().windows_console_backend==WindowsConsoleBackend::bundled_preview);
    CHECK(backend.original().windows_console_backend==WindowsConsoleBackend::system);
    const auto draft_before_status=backend.draft();
    backend.set_console_backend_status(true,"Diagnostic CLI override active; saved choice applies without the override.");
    CHECK(backend.draft()==draft_before_status);
    const auto backend_preview=backend.take_action();
    CHECK(backend_preview && backend_preview->kind==SettingsActionKind::preview);
    WindowScene backend_scene;backend.append_scene(backend_scene);
    CHECK(std::any_of(backend_scene.texts.begin(),backend_scene.texts.end(),[](const auto& t){return t.text.starts_with("Diagnostic CLI override active");}));
    backend.set_console_backend_status(true,"Preview · native AMD64 · resize unqualified · ConPTY 1.25.260930003");
    WindowScene concise_backend;backend.append_scene(concise_backend);
    CHECK(std::count_if(concise_backend.texts.begin(),concise_backend.texts.end(),[](const auto& t){return t.text=="For new tabs and panes";})==1);
    CHECK(std::count_if(concise_backend.texts.begin(),concise_backend.texts.end(),[](const auto& t){return t.text.find("1.25.260930003")!=std::string::npos;})==1);
    CHECK(std::count_if(concise_backend.texts.begin(),concise_backend.texts.end(),[](const auto& t){return t.text.find("unqualified")!=std::string::npos;})==1);
    CHECK(std::none_of(concise_backend.texts.begin(),concise_backend.texts.end(),[](const auto& t){return t.text.find("Save applies")!=std::string::npos || t.text.find("not fully qualified")!=std::string::npos;}));
    backend.cancel();const auto backend_cancel=backend.take_action();
    CHECK(backend_cancel && backend_cancel->kind==SettingsActionKind::cancel && backend_cancel->config==defaults);
    backend.open(defaults);activate(backend,"nav.shell");activate(backend,"console.bundled");activate(backend,"save");
    const auto backend_save=backend.take_action();CHECK(backend_save && backend_save->kind==SettingsActionKind::save);
    CHECK(backend_save->config.windows_console_backend==WindowsConsoleBackend::bundled_preview);
    backend.persistence_result(false,"Disk is read-only");CHECK(backend.is_open() && backend.dirty());
    CHECK(backend.original()==defaults);activate(backend,"save");backend.take_action();backend.persistence_result(true);
    CHECK(!backend.is_open());
    // Unavailable saved choices are retained; System remains keyboard-reachable recovery.
    backend.set_console_backend_status(false,"Bundled runtime missing; rebuild or select System.");
    backend.open(backend_save->config);activate(backend,"nav.shell");
    CHECK(backend.draft().windows_console_backend==WindowsConsoleBackend::bundled_preview);
    CHECK(!item(backend,"console.bundled").enabled && item(backend,"console.system").enabled);
    activate(backend,"console.system");CHECK(backend.draft().windows_console_backend==WindowsConsoleBackend::system);
    backend.cancel();backend.take_action();
    SettingsPanel mac_backend(Platform::macos);auto mac_config=default_config(Platform::macos);
    mac_config.windows_console_backend=WindowsConsoleBackend::bundled_preview;
    mac_backend.open(mac_config);activate(mac_backend,"nav.shell");
    CHECK(std::none_of(mac_backend.layout().controls.begin(),mac_backend.layout().controls.end(),[](const auto& c){return c.id.starts_with("console.");}));
    CHECK(mac_backend.draft().windows_console_backend==WindowsConsoleBackend::bundled_preview);
    for (const auto size:std::vector<Size>{{1024,768},{480,480},{320,260}}) {
        SettingsPanel compact;compact.set_console_backend_status(true,"Available");compact.resize(size);compact.open(defaults);
        activate(compact,"nav.shell");focus(compact,"console.system");CHECK(compact.key({{"RIGHT"}}));
        CHECK(compact.focused_control()=="console.bundled" && compact.draft().windows_console_backend==WindowsConsoleBackend::bundled_preview);
        CHECK(compact.key({{"LEFT"}}));CHECK(compact.draft().windows_console_backend==WindowsConsoleBackend::system);
        WindowScene scene;compact.append_scene(scene);validate_scene(scene,size.width,size.height,1.0);
    }
    // Additional blur is independent of tint and publishes reversible previews.
    SettingsPanel blur;blur.open(defaults);focus(blur,"appearance.blur");
    CHECK(blur.key({{"END"}}));CHECK(blur.draft().appearance.additional_blur==64);
    CHECK(blur.key({{"RIGHT"}}));CHECK(blur.draft().appearance.additional_blur==64);
    CHECK(blur.key({{"LEFT"}}));CHECK(blur.draft().appearance.additional_blur==63);
    CHECK(blur.key({{"HOME"}}));CHECK(blur.draft().appearance.additional_blur==0);
    CHECK(blur.key({{"LEFT"}}));CHECK(blur.draft().appearance.additional_blur==0);
    auto blur_slider=item(blur,"appearance.blur").bounds;
    CHECK(blur.pointer_down({blur_slider.x+8,blur_slider.y+10}));
    CHECK(blur.pointer_move({blur_slider.x+blur_slider.width*5,blur_slider.y+10}));CHECK(blur.draft().appearance.additional_blur==64);
    CHECK(blur.pointer_up({blur_slider.x+blur_slider.width/2,blur_slider.y+10}));CHECK(blur.draft().appearance.additional_blur==32);
    CHECK(blur.draft().appearance.background_opacity==defaults.appearance.background_opacity);
    auto blur_preview=blur.take_action();CHECK(blur_preview && blur_preview->kind==SettingsActionKind::preview && blur_preview->config.appearance.additional_blur==32);
    CHECK(!blur.take_action());
    CHECK(blur.pointer_down({blur_slider.x+8,blur_slider.y+10}));blur.pointer_cancel();
    CHECK(blur.pointer_move({blur_slider.x+blur_slider.width,blur_slider.y+10}));CHECK(blur.draft().appearance.additional_blur==0);
    blur.cancel();auto blur_cancel=blur.take_action();CHECK(blur_cancel && blur_cancel->kind==SettingsActionKind::cancel && blur_cancel->config==defaults);
    blur.open(defaults);focus(blur,"appearance.blur");CHECK(blur.key({{"RIGHT"}}));activate(blur,"save");
    auto blur_save=blur.take_action();CHECK(blur_save && blur_save->kind==SettingsActionKind::save && blur_save->config.appearance.additional_blur==1);
    blur.persistence_result(false,"Test failure");CHECK(blur.is_open() && blur.dirty());
    activate(blur,"save");blur.take_action();blur.persistence_result(true);CHECK(!blur.is_open());
    auto blur_reload=parse_config(serialize_config(blur_save->config,Platform::windows),Platform::windows);CHECK(blur_reload);
    blur.open(*blur_reload.config);CHECK(!blur.dirty() && blur.draft().appearance.additional_blur==1);blur.cancel();
    for(const auto size:std::vector<Size>{{1024,768},{480,480},{320,260}}) {
        SettingsPanel compact_blur;compact_blur.resize(size);compact_blur.open(defaults);focus(compact_blur,"appearance.blur");
        if(compact_blur.layout().content.height>=34) CHECK(item(compact_blur,"appearance.blur").visible);
        CHECK(compact_blur.key({{"END"}}));
        WindowScene scene;compact_blur.append_scene(scene);validate_scene(scene,size.width,size.height,1.0);
    }
    SettingsPanel mac_blur(Platform::macos);mac_blur.open(default_config(Platform::macos));CHECK(!item(mac_blur,"appearance.blur").enabled);
    SettingsPanel panel;
    CHECK(!panel.is_open());CHECK(!panel.key({{"TAB"}}));CHECK(!panel.pointer_down({0,0}));
    panel.set_material_status("Opaque: Windows Transparency effects are off.");
    const auto material_generation=panel.generation();
    panel.set_material_status("Opaque: Windows Transparency effects are off.");CHECK(panel.generation()==material_generation);
    panel.open(defaults);CHECK(panel.is_open());CHECK(!panel.dirty());CHECK(panel.draft()==defaults);
    CHECK(!panel.take_action());
    focus(panel,"appearance.backdrop.auto");
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
    // Semantic controls are unique; the optional clock is reachable by scrolling.
    std::set<std::string> ids;
    for (const auto& c:panel.layout().controls) CHECK(ids.insert(c.id).second);
    activate(panel,"appearance.clock");CHECK(!panel.draft().appearance.show_clock);
    activate(panel,"appearance.clock");CHECK(panel.draft().appearance.show_clock);
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

    // Native custom editor never publishes a partial hex value. Escape cancels
    // the edit first; the modal's next Escape rolls back the entire preview.
    SettingsPanel editor;editor.open(defaults);
    activate(editor,"appearance.color.value");
    editor.key({{"1"}});editor.key({{"ENTER"}});CHECK(editor.draft()==defaults);
    CHECK(!editor.error().empty());editor.key({{"ESCAPE"}});CHECK(editor.is_open());
    activate(editor,"appearance.color.value");
    for (char digit:std::string("ABCDEF")) editor.key({{std::string(1,digit)}});
    editor.key({{"ENTER"}});CHECK(editor.draft().theme.background=="#ABCDEF");
    CHECK(editor.draft().theme.name=="custom");
    const auto customized=editor.draft();CHECK(editor.take_action()->config==customized);
    activate(editor,"appearance.theme.midnight");activate(editor,"appearance.theme.custom");
    CHECK(editor.draft()==customized);
    activate(editor,"appearance.color.field"); // Foreground
    activate(editor,"appearance.color.value");
    for (char digit:std::string("123456")) editor.key({{std::string(1,digit)}});
    editor.key({{"ENTER"}});CHECK(editor.draft().theme.foreground=="#123456");
    activate(editor,"save");const auto custom_saved=editor.take_action();CHECK(custom_saved->kind==SettingsActionKind::save);
    editor.persistence_result(true);editor.open(custom_saved->config);CHECK(!editor.dirty());
    activate(editor,"appearance.theme.ember");editor.cancel();CHECK(editor.take_action()->config==custom_saved->config);

    // Reopening reads new disk content without replacing the running Cancel
    // baseline or committing external shell/binding changes during preview.
    const auto lifecycle_path=std::filesystem::current_path()/("settings-editor-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".stormtty");
    struct RemoveFixture {std::filesystem::path path;~RemoveFixture(){std::error_code ignored;std::filesystem::remove(path,ignored);}} remove_fixture{lifecycle_path};
    const auto write_file=[&](std::string_view bytes) {std::ofstream file(lifecycle_path,std::ios::binary|std::ios::trunc);file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));file.close();CHECK(file.good());};
    auto disk_config=defaults;disk_config.theme=*find_builtin_theme("daylight");disk_config.font.size_dip=20;
    disk_config.profiles.push_back({"external","External shell","external.exe",{},{},{}});disk_config.default_profile="external";
    disk_config.bindings.push_back({"pane.split_right",{"F8",CommandModifier::control|CommandModifier::shift}});
    write_file(serialize_config(disk_config,Platform::windows));
    auto fresh_disk=load_config(lifecycle_path,Platform::windows);CHECK(fresh_disk);
    SettingsPanel reopened;reopened.open(defaults,*fresh_disk.config);
    CHECK(reopened.original()==defaults && reopened.draft()==*fresh_disk.config);
    CHECK(!reopened.dirty() && reopened.loaded_changes_from_disk());
    auto disk_preview=reopened.take_action();CHECK(disk_preview && disk_preview->kind==SettingsActionKind::preview && disk_preview->config==*fresh_disk.config);
    WindowScene loaded_scene;reopened.append_scene(loaded_scene);
    CHECK(std::any_of(loaded_scene.texts.begin(),loaded_scene.texts.end(),[](const auto& t){return t.text=="Loaded changes from disk";}));
    activate(reopened,"font.increase");const auto editing=reopened.draft();CHECK(reopened.dirty());
    reopened.open(defaults,defaults);CHECK(reopened.draft()==editing); // A repeated open never discards edits.
    reopened.cancel();CHECK(reopened.take_action()->config==defaults);
    CHECK(load_config(lifecycle_path,Platform::windows).config==fresh_disk.config); // Cancel never rewrites disk.
    reopened.open(defaults,*fresh_disk.config);click(reopened,"save");
    const auto adopt=reopened.take_action();CHECK(adopt && adopt->kind==SettingsActionKind::save && adopt->config==*fresh_disk.config);
    reopened.persistence_result(true);reopened.open(adopt->config,*fresh_disk.config);
    CHECK(!reopened.loaded_changes_from_disk() && !reopened.dirty() && !reopened.take_action());
    reopened.cancel();reopened.take_action();
    // A syntactically invalid file stays explicit; repair and reread needs no restart.
    write_file("version = 1\n[font]\nsize_dip = nope\n");
    const auto invalid_disk=load_config(lifecycle_path,Platform::windows);CHECK(!invalid_disk && !invalid_disk.errors.empty());CHECK(!reopened.is_open());
    write_file(serialize_config(disk_config,Platform::windows));fresh_disk=load_config(lifecycle_path,Platform::windows);CHECK(fresh_disk);
    reopened.open(defaults,*fresh_disk.config);CHECK(reopened.is_open());reopened.cancel();
    // Custom selection restores the disk palette, while Cancel still restores
    // a different running custom palette rather than silently losing either.
    auto running_custom=defaults;running_custom.theme={"custom","#101010","#EFEFEF","#202020","#778899"};
    auto disk_custom=running_custom;disk_custom.theme.accent="#123456";
    reopened.open(running_custom,disk_custom);click(reopened,"appearance.theme.graphite");click(reopened,"appearance.theme.custom");
    CHECK(reopened.draft().theme==disk_custom.theme);reopened.cancel();CHECK(reopened.take_action()->config==running_custom);
    // Canonical binding order alone must not claim an external change.
    const auto same_disk=parse_config(serialize_config(defaults,Platform::windows),Platform::windows);CHECK(same_disk);
    reopened.open(defaults,*same_disk.config);CHECK(!reopened.loaded_changes_from_disk() && !reopened.take_action());reopened.cancel();

    // Portable discovery candidates have no trusted system identity. Never merge
    // distinct profiles by cmd.exe basename, a claimed Windows ID, or label.
    auto custom_command=defaults;custom_command.profiles.front().executable="C:\\Tools\\cmd.exe";
    Profile candidate_command;candidate_command.id="windows-cmd";candidate_command.label="Command Prompt";
    candidate_command.executable="C:\\OtherWindows\\System32\\cmd.exe";
    const std::vector command_candidates{candidate_command};
    for (const bool alias_with_args:{false,true}) {
        if (alias_with_args) {custom_command.profiles.front().executable="cmd.exe";custom_command.profiles.front().arguments={"/k","echo custom"};}
        SettingsPanel custom_shells;custom_shells.open(custom_command,command_candidates);activate(custom_shells,"nav.shell");
        CHECK(item(custom_shells,"shell:default").enabled && item(custom_shells,"shell:windows-cmd").enabled);
        activate(custom_shells,"shell:windows-cmd");CHECK(custom_shells.draft().profiles.size()==2);
        CHECK(custom_shells.draft().profiles.front()==custom_command.profiles.front());
        CHECK(custom_shells.draft().profiles.back()==candidate_command);
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
    // Four compact tabs retain their hit targets and full semantic identities;
    // narrow painted names fit inside each tab rather than clipping long words.
    for (const auto width:{320.0,480.0,700.0}) {
        SettingsPanel narrow;narrow.resize({width,640});narrow.open(defaults);
        WindowScene scene;narrow.append_scene(scene);
        const std::array<std::string_view,4> nav_ids{"nav.appearance","nav.shell","nav.shortcuts","nav.ribbon"};
        double previous_right=-1;
        for (const auto id:nav_ids) {
            const auto& control=item(narrow,id);CHECK(control.visible);
            CHECK(control.bounds.width>=44 && control.bounds.height>=36);
            CHECK(control.bounds.x>=previous_right);previous_right=control.bounds.x+control.bounds.width;
            const bool abbreviated=control.bounds.width<88;
            const auto expected=id=="nav.appearance"?(abbreviated?"Style":"Appearance"):
                id=="nav.shortcuts"?(abbreviated?"Keys":"Shortcuts"):id=="nav.shell"?"Shell":"Ribbon";
            const auto text=std::find_if(scene.texts.begin(),scene.texts.end(),[&](const auto& t){
                return t.text==expected && near(t.rect.y,control.bounds.y);
            });
            CHECK(text!=scene.texts.end());
            CHECK(text->rect.x>=control.bounds.x && text->rect.x+text->rect.width<=control.bounds.x+control.bounds.width);
        }
        activate(narrow,"nav.shortcuts");CHECK(narrow.section()==SettingsSection::shortcuts);
        activate(narrow,"nav.appearance");CHECK(narrow.section()==SettingsSection::appearance);
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
    SettingsPanel ribbon_panel;ribbon_panel.resize({1000,800});ribbon_panel.open(defaults);
    activate(ribbon_panel,"nav.ribbon");
    activate(ribbon_panel,"ribbon.toggle:session.git");
    CHECK(ribbon_panel.draft().ribbon.hidden[ribbon_index(RibbonBinding::git)]);
    activate(ribbon_panel,"ribbon.down:session.directory");
    CHECK(ribbon_panel.draft().ribbon.order[1]==RibbonBinding::directory);
    activate(ribbon_panel,"ribbon.toggle:clock.local");CHECK(!ribbon_panel.draft().appearance.show_clock);
    WindowScene ribbon_scene;ribbon_panel.append_scene(ribbon_scene);validate_scene(ribbon_scene,1000,800,1);
    const auto ribbon_saved=parse_config(serialize_config(ribbon_panel.draft(),Platform::windows),Platform::windows);
    CHECK(ribbon_saved && ribbon_saved.config->ribbon==ribbon_panel.draft().ribbon);
    for(const auto size:std::array<Size,3>{{{420,360},{640,480},{1400,1000}}}) {
        ribbon_panel.resize(size);WindowScene scene;ribbon_panel.append_scene(scene);validate_scene(scene,size.width,size.height,1);
    }
    ribbon_panel.cancel();const auto ribbon_cancel=ribbon_panel.take_action();
    CHECK(ribbon_cancel && ribbon_cancel->config.ribbon==defaults.ribbon);
    SettingsPanel ribbon_save;ribbon_save.open(defaults);activate(ribbon_save,"nav.ribbon");
    activate(ribbon_save,"ribbon.toggle:workspace.background");activate(ribbon_save,"save");
    auto save_ribbon=ribbon_save.take_action();CHECK(save_ribbon && save_ribbon->kind==SettingsActionKind::save);
    CHECK(save_ribbon->config.ribbon.hidden[ribbon_index(RibbonBinding::background)]);
    ribbon_save.persistence_result(true);CHECK(!ribbon_save.is_open());
    SettingsPanel disabled_ai;auto future=defaults;future.experimental_ai=true;
    disabled_ai.open(defaults,future);activate(disabled_ai,"nav.shortcuts");
    for(const auto& c:disabled_ai.layout().controls)CHECK(c.id!="binding:agent.toggle");
    disabled_ai.open_for_binding(future,"agent.toggle");CHECK(!disabled_ai.capturing_shortcut());
    activate(disabled_ai,"nav.ribbon");for(const auto& c:disabled_ai.layout().controls)CHECK(c.id.find("agent.status")==std::string::npos);
    WindowScene no_ai_scene;disabled_ai.append_scene(no_ai_scene);for(const auto& t:no_ai_scene.texts)CHECK(t.text.find("Agent status")==std::string::npos);
    SettingsPanel enabled_ai;enabled_ai.set_agent_available(true);enabled_ai.open(defaults);activate(enabled_ai,"nav.shortcuts");
    CHECK(std::any_of(enabled_ai.layout().controls.begin(),enabled_ai.layout().controls.end(),[](const auto& c){return c.id=="binding:agent.toggle";}));
    activate(enabled_ai,"nav.ribbon");CHECK(std::any_of(enabled_ai.layout().controls.begin(),enabled_ai.layout().controls.end(),[](const auto& c){return c.id=="ribbon.toggle:agent.status";}));
    auto boundary=defaults;std::swap(boundary.ribbon.order.front(),boundary.ribbon.order[ribbon_index(RibbonBinding::agent)]);
    SettingsPanel boundary_panel;boundary_panel.open(boundary);activate(boundary_panel,"nav.ribbon");
    for(const auto& c:boundary_panel.layout().controls)if(c.id=="ribbon.up:session.git")CHECK(!c.enabled);
});}
