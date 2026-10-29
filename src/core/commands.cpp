#include "stormtty/commands.hpp"
#include "stormtty/project_workspaces.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace stormtty {
namespace {
constexpr std::array commands{
    Command{"workspace.navigator", "Switch workspace or terminal…", "Find an existing terminal across open workspaces"},
    Command{"workspace.settings", "Workspace settings…", "Name, icon, colour, shell and saved layouts"},
    Command{"workspace.new", "New workspace…", "Choose a real folder and start its configured shell"},
    Command{"workspace.next", "Next workspace", "Switch projects without restarting hidden sessions"},
    Command{"workspace.previous", "Previous workspace", "Switch projects without restarting hidden sessions"},
    Command{"workspace.close", "Close workspace…", "Close this project's tabs and sessions after confirmation"},
    Command{"workspace.toggle_navigation", "Expand or collapse workspaces", "Toggle the compact project navigation rail"},
    Command{"window.focus_mode", "Toggle focus mode", "Hide project navigation and directory context; keep the terminal"},
    Command{"app.command_palette", "Show commands", "Search every application action and its shortcut"},
    Command{"tab.new", "New tab", "Start a fresh session using the selected profile"},
    Command{"tab.close", "Close tab", "Close all panes in the current tab; host confirms running processes"},
    Command{"tab.next", "Next tab", "Activate the next tab"},
    Command{"tab.previous", "Previous tab", "Activate the previous tab"},
    Command{"tab.rename", "Rename tab", "Set a title; an empty title restores the process title"},
    Command{"tab.move_left", "Move tab left", "Reorder without restarting sessions"},
    Command{"tab.move_right", "Move tab right", "Reorder without restarting sessions"},
    Command{"pane.split_right", "Split right", "Create a session to the right of the current pane"},
    Command{"pane.split_down", "Split below", "Create a session below the current pane"},
    Command{"background.restore", "Restore background terminal…", "Choose a background session to bring to the foreground without restarting it"},
    Command{"pane.background", "Move to background", "Keep this terminal process and output in the workspace footer"},
    Command{"pane.close", "Close pane", "Close the current pane; host confirms running processes"},
    Command{"pane.focus_left", "Focus pane left", "Move focus spatially to the pane on the left"},
    Command{"pane.focus_right", "Focus pane right", "Move focus spatially to the pane on the right"},
    Command{"pane.focus_up", "Focus pane above", "Move focus spatially to the pane above"},
    Command{"pane.focus_down", "Focus pane below", "Move focus spatially to the pane below"},
    Command{"pane.zoom", "Toggle pane zoom", "Fill the tab with this pane, or return to the split arrangement"},
    Command{"pane.move_left", "Move pane left", "Move the existing session beside another pane"},
    Command{"pane.move_right", "Move pane right", "Move the existing session beside another pane"},
    Command{"pane.move_up", "Move pane above", "Move the existing session above another pane"},
    Command{"pane.move_down", "Move pane below", "Move the existing session below another pane"},
    Command{"pane.move_to_tab", "Move pane to tab…", "Choose a destination; the session is not restarted"},
    Command{"pane.new_tab", "Move pane to new tab", "Detach the existing session into a new tab"},
    Command{"pane.swap", "Swap panes…", "Exchange two panes without replacing either session"},
    Command{"pane.resize_left", "Resize pane left", "Move an adjacent divider left by a keyboard step"},
    Command{"pane.resize_right", "Resize pane right", "Move an adjacent divider right by a keyboard step"},
    Command{"pane.resize_up", "Resize pane upward", "Move an adjacent divider upward by a keyboard step"},
    Command{"pane.resize_down", "Resize pane downward", "Move an adjacent divider downward by a keyboard step"},
    Command{"pane.equalize", "Equalize split", "Reset the selected divider to equal fractions"},
    Command{"edit.copy", "Copy selection", "Copy selected terminal text; never replace Ctrl+C interrupt"},
    Command{"edit.paste", "Paste", "Send clipboard text through terminal paste policy and bracketed-paste encoding"},
    Command{"watch.arm", "Watch this command", "Remember this observed command result without moving its terminal"},
    Command{"watch.cancel", "Cancel command watch", "Remove the watch; do not stop the process"},
    Command{"watch.dismiss", "Dismiss watched result", "Acknowledge this terminal’s watched result"},
    Command{"pane.inspect", "Inspect pane context", "Read observed directory, repository and configured start folder"},
    Command{"terminal.find", "Find in terminal", "Search retained terminal text"},
    Command{"settings.reload", "Reload settings", "Read config.toml from disk without restarting terminals; shell choices apply to new sessions"},
    Command{"agent.toggle", "Open or close Codex panel", "Workspace agent transcript, task composer and approvals"},
    Command{"settings.open", "Open settings", "Edit declarative settings and view configuration errors"},
    Command{"window.fullscreen", "Toggle fullscreen", "Enter or leave native fullscreen"}
};
std::string uppercase(std::string_view s) {
    std::string out(s);
    for (auto& c : out) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}
bool valid_key(std::string_view key) {
    if (key.size() == 1) return (key[0] >= 'A' && key[0] <= 'Z') ||
        (key[0] >= '0' && key[0] <= '9') || std::string_view(",./;'-=[]\\`").find(key[0]) != std::string_view::npos;
    constexpr std::array names{"TAB", "ENTER", "ESCAPE", "SPACE", "BACKSPACE", "DELETE", "INSERT",
                               "HOME", "END", "PAGEUP", "PAGEDOWN", "LEFT", "RIGHT", "UP", "DOWN", "PLUS"};
    if (std::find(names.begin(), names.end(), key) != names.end()) return true;
    if (key.size() >= 2 && key.size() <= 3 && key.front() == 'F') {
        unsigned n{};
        for (std::size_t i = 1; i < key.size(); ++i) {
            if (key[i] < '0' || key[i] > '9') return false;
            n = n * 10 + static_cast<unsigned>(key[i] - '0');
        }
        return n >= 1 && n <= 24 && key[1] != '0';
    }
    return false;
}
}
namespace {
// Reject malformed sequences rather than matching partial UTF-8 bytes. Input
// bounds are checked before decoding so even untrusted titles have bounded cost.
std::optional<std::vector<char32_t>> search_text(std::string_view text, std::size_t limit, bool truncate) {
    std::vector<char32_t> out;
    for (std::size_t i=0; i<text.size();) {
        if (out.size()==limit) { if (truncate) return out; return {}; }
        const auto lead=static_cast<unsigned char>(text[i++]);
        char32_t cp=lead; unsigned rest=0;
        if (lead>=0xc2 && lead<=0xdf) { cp=lead&31; rest=1; }
        else if (lead>=0xe0 && lead<=0xef) { cp=lead&15; rest=2; }
        else if (lead>=0xf0 && lead<=0xf4) { cp=lead&7; rest=3; }
        else if (lead>=0x80) return {};
        if (rest>text.size()-i) return {};
        for (unsigned n=0;n<rest;++n) {
            const auto c=static_cast<unsigned char>(text[i++]);
            if ((c&0xc0)!=0x80) return {};
            cp=(cp<<6)|(c&63);
        }
        if ((rest==1 && cp<0x80) || (rest==2 && cp<0x800) ||
            (rest==3 && cp<0x10000) || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return {};
        if (cp>='A' && cp<='Z') cp+='a'-'A';
        out.push_back(cp);
    }
    return out;
}
bool search_separator(char32_t c) { return c==' ' || c=='\t' || c=='\n' || c=='.' || c=='_' || c=='-' || c=='/' || c==':' ; }
std::optional<int> fuzzy_score(const std::vector<char32_t>& query, std::string_view candidate) {
    const auto decoded=search_text(candidate,512,true);
    if (!decoded || decoded->empty()) return {};
    const auto& text=*decoded;
    constexpr int missing=-1000000;
    std::vector<int> previous(text.size(),missing), current(text.size(),missing);
    for (std::size_t q=0;q<query.size();++q) {
        std::fill(current.begin(),current.end(),missing);
        int best=missing;
        for (std::size_t j=0;j<text.size();++j) {
            if (j) best=std::max(best,previous[j-1]+static_cast<int>(j-1));
            if (query[q]!=text[j]) continue;
            const int boundary=(j==0 || search_separator(text[j-1]))?18:0;
            if (!q) current[j]=32+boundary-static_cast<int>(j);
            else {
                if (best>missing/2) current[j]=best-static_cast<int>(j)+32+boundary;
                if (j && previous[j-1]>missing/2) current[j]=std::max(current[j],previous[j-1]+48+boundary);
            }
        }
        previous.swap(current);
    }
    const int score=*std::max_element(previous.begin(),previous.end());
    if (score<=missing/2) return {};
    return score+(query==text?120:0);
}
}
std::optional<int> command_search_score(std::string_view query, std::string_view label, std::string_view description) {
    if (query.size()>256) return {};
    auto decoded=search_text(query,64,false);
    if (!decoded) return {};
    while (!decoded->empty() && (decoded->front()==' ' || decoded->front()=='\t')) decoded->erase(decoded->begin());
    while (!decoded->empty() && (decoded->back()==' ' || decoded->back()=='\t')) decoded->pop_back();
    if (decoded->empty()) return 0;
    const auto primary=fuzzy_score(*decoded,label), secondary=fuzzy_score(*decoded,description);
    if (primary) return *primary+256; // Prefer an action's name to incidental help text.
    if (secondary) return *secondary;
    return {};
}
std::string background_session_title(std::string_view user_title, std::string_view process_title, std::string_view profile_label) {
    auto clean=[](std::string_view value) {
        std::string result(value);
        std::erase_if(result,[](unsigned char c){return c<0x20 || c==0x7f;});
        return result;
    };
    if (!user_title.empty()) return clean(user_title);
    auto title=clean(process_title);
    auto executable=std::string_view(title);
    if (executable.size()>1 && executable.front()=='"' && executable.back()=='"') {
        executable.remove_prefix(1); executable.remove_suffix(1);
    }
    const auto separator=executable.find_last_of("/\\");
    // A complete executable basename or absolute path, not arbitrary prose with
    // a shell-looking suffix. Never interpret this presentation text as a path.
    const bool raw=separator==std::string_view::npos || executable.starts_with("/") || executable.starts_with("\\") ||
        (executable.size()>2 && executable[1]==':' && (executable[2]=='/' || executable[2]=='\\'));
    if (raw) {
        const auto name=uppercase(executable.substr(separator==std::string_view::npos?0:separator+1));
        if (name=="ZSH" || name=="ZSH.EXE") return "Zsh";
        if (name=="PWSH" || name=="PWSH.EXE" || name=="POWERSHELL" || name=="POWERSHELL.EXE") return "PowerShell";
        if (name=="CMD" || name=="CMD.EXE") return "Command Prompt";
        if (name=="BASH" || name=="BASH.EXE") return "Bash";
    }
    if (!title.empty()) return title;
    title=clean(profile_label);
    return title.empty()?"Terminal":title;
}
std::vector<const Command*> search_commands(std::string_view query) {
    std::vector<std::pair<const Command*,int>> ranked;
    for (const auto& command:commands) if (const auto score=command_search_score(query,command.label,command.description)) ranked.push_back({&command,*score});
    std::stable_sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.second>b.second;});
    std::vector<const Command*> result;
    for (const auto& item:ranked) result.push_back(item.first);
    return result;
}
std::span<const Command> command_registry() noexcept { return commands; }
const Command* find_command(std::string_view id) noexcept {
    for (const auto& command : commands) if (command.id == id) return &command;
    return nullptr;
}
CommandAvailability command_availability(std::string_view id, const CommandContext& c) noexcept {
    if (!find_command(id)) return {false, "Unknown command"};
    if (id == "settings.reload") return {c.supports_settings_reload, c.supports_settings_reload ? "" : "Settings reload is not implemented by this host"};
    if (id == "agent.toggle") return {c.supports_agent_panel,c.supports_agent_panel?"":"Native agent panel is not implemented by this host"};
    if (id == "window.focus_mode") return {c.supports_focus_mode, c.supports_focus_mode ? "" : "Focus mode is not implemented by this host"};
    if (id=="workspace.navigator") return {c.supports_workspace_navigator,c.supports_workspace_navigator?"":"Workspace navigator is not implemented by this host"};
    if (id.starts_with("workspace.")) {
        if (!c.supports_project_workspaces) return {false,"Project workspaces are not implemented by this host"};
        if (id=="workspace.next" || id=="workspace.previous") return {c.project_workspaces>1,c.project_workspaces>1?"":"Only one workspace is open"};
        if (id=="workspace.close") return {c.project_workspaces>0,c.project_workspaces>0?"":"No workspace is open"};
        if (id=="workspace.new") return {c.project_workspaces<ProjectWorkspaceCatalog::maximum_projects,c.project_workspaces<ProjectWorkspaceCatalog::maximum_projects?"":"Workspace limit reached"};
        return {true,{}};
    }
    if (id == "tab.new") return {c.tabs < Workspace::maximum_tabs, c.tabs < Workspace::maximum_tabs ? "" : "Tab limit reached"};
    if (id == "app.command_palette" || id == "settings.open" || id == "window.fullscreen") return {true, {}};
    if (id == "background.restore") return {c.supports_background_sessions && c.parked_sessions>0,
        !c.supports_background_sessions ? "Background sessions are not implemented by this host" : c.parked_sessions==0 ? "No background terminals in this workspace" : ""};
    if (!c.tabs || !c.panes) return {false, "No active terminal pane"};
    if(id.starts_with("watch.")) {
        if(!c.supports_command_watch)return {false,"Command watches are not implemented by this host"};
        if(id=="watch.arm")return {c.watch_can_arm,c.watch_can_arm?"":"Available while shell integration reports a running command; dismiss any previous watch first"};
        if(id=="watch.cancel")return {c.watch_waiting,c.watch_waiting?"":"No command is being watched in this terminal"};
        return {c.watch_unread,c.watch_unread?"":"No unread watched result in this terminal"};
    }
    if (id == "pane.inspect") return {c.supports_pane_context,c.supports_pane_context?"":"Pane context inspection is not implemented by this host"};
    if (id == "pane.background") return {c.supports_background_sessions && c.parked_sessions<ProjectWorkspaceCatalog::maximum_parked_sessions, !c.supports_background_sessions ? "Background sessions are not implemented by this host" : c.parked_sessions>=ProjectWorkspaceCatalog::maximum_parked_sessions ? "Background session limit reached" : ""};
    if (id == "edit.copy") return {c.has_selection, c.has_selection ? "" : "Select terminal text first"};
    if (id == "edit.paste") return {c.clipboard_has_text && c.session_running,
        !c.session_running ? "Session is not running" : (!c.clipboard_has_text ? "Clipboard has no text" : "")};
    if (id == "tab.next" || id == "tab.previous" || id == "tab.move_left" || id == "tab.move_right")
        return {c.tabs > 1, c.tabs > 1 ? "" : "Only one tab is open"};
    if (id == "pane.move_to_tab") return {c.tabs > 1, c.tabs > 1 ? "" : "Open another tab first"};
    if (id == "pane.new_tab") return {c.tabs < Workspace::maximum_tabs || c.panes == 1, c.tabs < Workspace::maximum_tabs || c.panes == 1 ? "" : "Tab limit reached"};
    if (id == "pane.split_right" || id == "pane.split_down")
        return {(c.total_panes ? c.total_panes : c.panes) < Workspace::maximum_panes,
                (c.total_panes ? c.total_panes : c.panes) < Workspace::maximum_panes ? "" : "Pane limit reached"};
    if (id.starts_with("pane.focus_") || id.starts_with("pane.move_") || id == "pane.swap" ||
        id.starts_with("pane.resize_") || id == "pane.equalize" || id == "pane.zoom") {
        if (c.panes < 2) return {false, "Split this tab first"};
        if (c.zoomed && (id.starts_with("pane.resize_") || id == "pane.equalize")) return {false, "Return to split before resizing"};
    }
    return {true, {}};
}
Result<KeyChord> parse_key_chord(std::string_view text, Platform platform) {
    if (text.empty() || text.size() > 128) return {{}, "Shortcut is empty or too long"};
    KeyChord chord;
    while (true) {
        const auto separator = text.find('+');
        const auto token = uppercase(text.substr(0, separator));
        if (token.empty()) return {{}, "Shortcut contains an empty part; use PLUS for the plus key"};
        if (separator == std::string_view::npos) {
            if (!valid_key(token)) return {{}, "Unknown shortcut key: " + token};
            chord.key = token; break;
        }
        CommandModifier modifier;
        if (token == "CTRL" || token == "CONTROL") modifier = CommandModifier::control;
        else if (token == "CMD" || token == "COMMAND") modifier = CommandModifier::command;
        else if (token == "SHIFT") modifier = CommandModifier::shift;
        else if (token == "ALT" || token == "OPTION") modifier = CommandModifier::alt;
        else if (token == "PRIMARY") modifier = platform == Platform::windows ? CommandModifier::control : CommandModifier::command;
        else return {{}, "Unknown shortcut modifier: " + token};
        if (has_modifier(chord.modifiers, modifier)) return {{}, "Duplicate shortcut modifier"};
        chord.modifiers = chord.modifiers | modifier;
        text.remove_prefix(separator + 1);
    }
    return {std::move(chord), {}};
}
std::string format_key_chord(const KeyChord& chord, Platform platform) {
    std::string result;
    if (has_modifier(chord.modifiers, CommandModifier::control)) result += "Ctrl+";
    if (has_modifier(chord.modifiers, CommandModifier::command)) result += platform == Platform::macos ? "Cmd+" : "Win+";
    if (has_modifier(chord.modifiers, CommandModifier::alt)) result += platform == Platform::macos ? "Option+" : "Alt+";
    if (has_modifier(chord.modifiers, CommandModifier::shift)) result += "Shift+";
    result += chord.key; return result;
}
std::vector<Binding> default_bindings(Platform platform) {
    std::vector<Binding> out;
    auto add = [&](std::string_view id, std::string_view chord) {
        auto parsed = parse_key_chord(chord, platform); out.push_back({std::string(id), *parsed.value});
    };
    add("app.command_palette", "Primary+Shift+P");
    add("tab.new", platform == Platform::windows ? "Ctrl+Shift+T" : "Cmd+T");
    add("pane.close", platform == Platform::windows ? "Ctrl+Shift+W" : "Cmd+W");
    add("tab.next", "Ctrl+Tab"); add("tab.previous", "Ctrl+Shift+Tab");
    add("terminal.find", platform == Platform::windows ? "Ctrl+Shift+F" : "Cmd+F");
    add("edit.copy", platform == Platform::windows ? "Ctrl+Shift+C" : "Cmd+C");
    add("edit.paste", platform == Platform::windows ? "Ctrl+Shift+V" : "Cmd+V");
    add("settings.open", "Primary+,");
    if(platform==Platform::windows) {add("agent.toggle", "Ctrl+Shift+J");add("workspace.navigator", "Ctrl+Shift+O");}
    return out;
}
bool is_os_reserved(const KeyChord& key, Platform platform) noexcept {
    const auto m = key.modifiers;
    if (platform == Platform::windows) {
        if (has_modifier(m, CommandModifier::command)) return true;
        if (has_modifier(m, CommandModifier::alt) && (key.key == "F4" || key.key == "SPACE" || key.key == "TAB" || key.key == "ESCAPE")) return true;
        if (has_modifier(m, CommandModifier::control) && key.key == "ESCAPE") return true;
        if (has_modifier(m, CommandModifier::control | CommandModifier::alt) && key.key == "DELETE") return true;
    } else {
        if (has_modifier(m, CommandModifier::command) && (key.key == "TAB" || key.key == "SPACE" || key.key == "H" || key.key == "Q")) return true;
        if (has_modifier(m, CommandModifier::control) && (key.key == "LEFT" || key.key == "RIGHT" || key.key == "UP" || key.key == "DOWN")) return true;
    }
    return false;
}
bool is_terminal_reserved(const KeyChord& key, Platform platform) noexcept {
    if (is_os_reserved(key, platform)) return true;
    const auto m = key.modifiers;
    if (has_modifier(m, CommandModifier::command)) return false; // macOS application namespace
    if (has_modifier(m, CommandModifier::control | CommandModifier::alt)) return true; // AltGr
    if (!has_modifier(m, CommandModifier::control)) return true; // text, Alt/Meta, function and navigation keys
    if (has_modifier(m, CommandModifier::shift)) return false;
    return key.key != "TAB" && key.key != ","; // documented application exceptions
}
Status validate_bindings(std::span<const Binding> bindings, Platform platform, bool allow_terminal_override) {
    if (bindings.size() > 256) return Status::failure("Too many keybindings");
    std::set<KeyChord> seen;
    for (const auto& binding : bindings) {
        if (!find_command(binding.command)) return Status::failure("Unknown command: " + binding.command);
        if (!valid_key(binding.chord.key) || static_cast<unsigned>(binding.chord.modifiers) > 15)
            return Status::failure("Invalid keybinding chord");
        if (!seen.insert(binding.chord).second) return Status::failure("Duplicate keybinding: " + format_key_chord(binding.chord, platform));
        if (is_os_reserved(binding.chord, platform)) return Status::failure("Shortcut is reserved by the operating system");
        if (!allow_terminal_override && is_terminal_reserved(binding.chord, platform))
            return Status::failure("Shortcut belongs to terminal input; explicit override is required");
    }
    return {};
}
std::optional<std::string_view> resolve_command(const KeyEvent& event, std::span<const Binding> bindings,
                                               const CommandContext& context, Platform platform) noexcept {
    if (!event.key_down || event.composing || event.alt_graph || is_os_reserved(event.chord, platform)) return {};
    for (const auto& binding : bindings)
        if (binding.chord == event.chord && command_availability(binding.command, context).enabled) return find_command(binding.command)->id;
    return {};
}
} // namespace stormtty
