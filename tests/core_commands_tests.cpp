#include "core_test.hpp"
#include "stormtty/commands.hpp"
#include "stormtty/terminal.hpp" // Core and VT public headers must coexist.
#include <set>

using namespace stormtty;
int main() { return run_core_tests("commands", [] {
    std::set<std::string_view> ids;
    for (const auto& c : command_registry()) { CHECK(ids.insert(c.id).second); CHECK(!c.label.empty()); CHECK(find_command(c.id) == &c); }
    CHECK(background_session_title({},R"(D:\a\stormtty\runtimes\zsh\usr\bin\zsh.exe)",{})=="Zsh");
    CHECK(background_session_title({},R"("C:\Program Files\PowerShell\7\pwsh.exe")",{})=="PowerShell");
    CHECK(background_session_title({},R"(C:\Windows\System32\cmd.exe)",{})=="Command Prompt");
    CHECK(background_session_title({},"/usr/bin/zsh",{})=="Zsh");
    CHECK(background_session_title("My build", "zsh.exe", "Zsh")=="My build");
    CHECK(background_session_title("zsh.exe", "powershell.exe", {})=="zsh.exe");
    CHECK(background_session_title({},"Build · café", "Zsh")=="Build · café");
    CHECK(background_session_title({},"notes/zsh.exe",{})=="notes/zsh.exe");
    CHECK(background_session_title({},"zsh.exe — build",{})=="zsh.exe — build");
    CHECK(background_session_title({},"/tools/custom.exe",{})=="/tools/custom.exe");
    CHECK(background_session_title({}, {}, "Development")=="Development");
    CHECK(background_session_title({}, {}, {})=="Terminal");
    CHECK(find_command("settings.reload"));
    CHECK(search_commands("reload settings").front()->id=="settings.reload");
    CommandContext reload_context;
    CHECK(!command_availability("settings.reload",reload_context).enabled);
    reload_context.supports_settings_reload=true;
    CHECK(command_availability("settings.reload",reload_context).enabled); // No terminal required.
    CHECK(search_commands("").size()==command_registry().size());
    CHECK(search_commands("  ")==search_commands(""));
    CHECK(search_commands("nwtb").front()->id=="tab.new");
    CHECK(search_commands("splr").front()->id=="pane.split_right");
    CHECK(search_commands("RST BKG")==search_commands("rst bkg"));
    CHECK(search_commands("zzzzzzzzzzzzzzzz").empty());
    CHECK(search_commands("[.*").empty()); // Literal punctuation, not a regex.
    CHECK(search_commands(std::string(65,'a')).empty());
    CHECK(command_search_score("é", "Café"));
    CHECK(!command_search_score("é", "Cafe"));
    CHECK(command_search_score("日本", "日本語 terminal"));
    CHECK(!command_search_score(std::string("\xc3",1),"Café"));
    CHECK(!command_search_score("x",std::string("\xc0\xaf",2)));
    CHECK(!command_search_score("x",std::string("\xed\xa0\x80",3)));
    CHECK(!command_search_score("x",std::string("\xf4\x90\x80\x80",4)));
    CHECK(!command_search_score("z",std::string(512,'a')+"z"));
    CHECK(*command_search_score("fb","foo bar")>*command_search_score("fb","foobar"));
    CHECK(*command_search_score("tab","tab")>*command_search_score("tab","table"));
    CHECK(*command_search_score("tab","tab")>*command_search_score("tab","t a b"));
    for (int i=0;i<20;++i) CHECK(search_commands("pane")==search_commands("pane"));
    for (std::size_t i=0;i<command_registry().size();++i) CHECK(search_commands("")[i]==&command_registry()[i]);
    CommandContext inspect{1,1};
    CHECK(!command_availability("pane.inspect",inspect).enabled);
    inspect.supports_pane_context=true; CHECK(command_availability("pane.inspect",inspect).enabled);
    inspect.panes=0; CHECK(!command_availability("pane.inspect",inspect).enabled);
    CommandContext background{1,1};
    CHECK(!command_availability("pane.background",background).enabled);
    background.supports_background_sessions=true;
    CHECK(command_availability("pane.background",background).enabled);
    background.parked_sessions=256;
    CHECK(!command_availability("pane.background",background).enabled);
    background.parked_sessions=1; background.panes=0; background.tabs=0;
    CHECK(command_availability("background.restore",background).enabled);
    background.supports_background_sessions=false;
    CHECK(!command_availability("background.restore",background).enabled);
    background.supports_background_sessions=true;
    background.parked_sessions=0; background.panes=0;
    CHECK(!command_availability("background.restore",background).enabled);
    CHECK(!command_availability("pane.background",background).enabled);
    CommandContext projects;
    for (const auto* id : {"workspace.new","workspace.next","workspace.previous","workspace.close","workspace.toggle_navigation","window.focus_mode"}) {
        CHECK(!command_availability(id,projects).enabled); CHECK(!command_availability(id,projects).reason.empty());
    }
    projects.supports_project_workspaces=true; projects.supports_focus_mode=true;
    CHECK(command_availability("workspace.new",projects).enabled);
    CHECK(command_availability("window.focus_mode",projects).enabled);
    CHECK(!command_availability("workspace.close",projects).enabled);
    projects.project_workspaces=1;
    CHECK(command_availability("workspace.close",projects).enabled); CHECK(!command_availability("workspace.next",projects).enabled);
    projects.project_workspaces=2;
    CHECK(command_availability("workspace.next",projects).enabled); CHECK(command_availability("workspace.previous",projects).enabled);
    projects.project_workspaces=256; CHECK(!command_availability("workspace.new",projects).enabled);
    for (const auto platform : {Platform::windows,Platform::macos}) for (const auto& binding:default_bindings(platform))
        CHECK((!binding.command.starts_with("workspace.") || (platform==Platform::windows&&binding.command=="workspace.navigator")) && binding.command!="window.focus_mode");
    CHECK(!command_availability("workspace.navigator",projects).enabled);
    projects.supports_workspace_navigator=true;CHECK(command_availability("workspace.navigator",projects).enabled);
    CHECK(!find_command("unknown")); CHECK(!command_availability("unknown", {}).enabled);
    for (const auto platform : {Platform::windows, Platform::macos}) {
        auto bindings = default_bindings(platform); CHECK(validate_bindings(bindings, platform));
        for (const auto& b : bindings) {
            CHECK(!is_os_reserved(b.chord, platform)); CHECK(!is_terminal_reserved(b.chord, platform));
            auto parsed = parse_key_chord(format_key_chord(b.chord, platform), platform); CHECK(parsed); CHECK(*parsed.value == b.chord);
        }
        auto primary = parse_key_chord("Primary+Shift+P", platform); CHECK(primary);
        CHECK(has_modifier(primary.value->modifiers, platform == Platform::windows ? CommandModifier::control : CommandModifier::command));
        CommandContext context{2, 3, false, true, false, true};
        KeyEvent event{*primary.value}; CHECK(resolve_command(event, bindings, context, platform) == "app.command_palette");
        event.composing = true; CHECK(!resolve_command(event, bindings, context, platform));
        event.composing = false; event.alt_graph = true; CHECK(!resolve_command(event, bindings, context, platform));
        event.alt_graph = false; event.key_down = false; CHECK(!resolve_command(event, bindings, context, platform));
        auto copy = *parse_key_chord(platform == Platform::windows ? "Ctrl+Shift+C" : "Cmd+C", platform).value;
        CHECK(!resolve_command({copy}, bindings, context, platform)); context.has_selection = true;
        CHECK(resolve_command({copy}, bindings, context, platform) == "edit.copy");
        CHECK(!resolve_command({*parse_key_chord("Ctrl+C", platform).value}, bindings, context, platform));
        auto duplicate = bindings; duplicate.push_back(bindings.front()); CHECK(!validate_bindings(duplicate, platform));
    }
    for (const auto* bad : {"", "+P", "Ctrl++", "Ctrl+Ctrl+P", "Hyper+P", "F25", "F01", "Unknown", "Ctrl+Primary+P", "Ctrl+Shift+"})
        CHECK(!parse_key_chord(bad, Platform::windows));
    CHECK(parse_key_chord("Ctrl+PLUS", Platform::windows)); CHECK(parse_key_chord("Ctrl+Shift+Left", Platform::windows));
    CHECK(is_os_reserved(*parse_key_chord("Alt+F4", Platform::windows).value, Platform::windows));
    CHECK(is_os_reserved(*parse_key_chord("Cmd+Tab", Platform::macos).value, Platform::macos));
    std::vector<Binding> control_c{{"edit.copy", *parse_key_chord("Ctrl+C", Platform::windows).value}};
    CHECK(!validate_bindings(control_c, Platform::windows)); CHECK(validate_bindings(control_c, Platform::windows, true));
    for (const auto platform:{Platform::windows,Platform::macos}) {
        const auto chord=*parse_key_chord("Alt+]",platform).value;
        CHECK(!is_os_reserved(chord,platform));CHECK(is_terminal_reserved(chord,platform));
        const std::vector<Binding> bindings{{"pane.split_right",chord}};
        CHECK(!validate_bindings(bindings,platform));CHECK(validate_bindings(bindings,platform,true));
    }
    std::vector<Binding> alt_f4{{"pane.close", *parse_key_chord("Alt+F4", Platform::windows).value}};
    CHECK(!validate_bindings(alt_f4, Platform::windows, true));
    CHECK(!command_availability("pane.split_right", {}).enabled);
    CHECK(!command_availability("pane.zoom", {1, 1}).enabled);
    CHECK(command_availability("pane.zoom", {1, 2}).enabled);
    CHECK(!command_availability("pane.resize_left", {1, 2, false, false, true}).enabled);
    CHECK(!command_availability("tab.next", {1, 1}).enabled);
    CHECK(command_availability("tab.new", {}).reason.empty());
    CHECK(!command_availability("pane.split_right", {1, 1, false, false, false, true, Workspace::maximum_panes}).enabled);
    CHECK(command_availability("pane.new_tab", {Workspace::maximum_tabs, 1}).enabled);
}); }
