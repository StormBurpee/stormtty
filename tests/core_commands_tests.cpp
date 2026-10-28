#include "core_test.hpp"
#include "stormtty/commands.hpp"
#include "stormtty/terminal.hpp" // Core and VT public headers must coexist.
#include <set>

using namespace stormtty;
int main() { return run_core_tests("commands", [] {
    std::set<std::string_view> ids;
    for (const auto& c : command_registry()) { CHECK(ids.insert(c.id).second); CHECK(!c.label.empty()); CHECK(find_command(c.id) == &c); }
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
