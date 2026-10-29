#pragma once

#include "stormtty/workspace.hpp"
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace stormtty {
enum class Platform { windows, macos };
enum class CommandModifier : unsigned { none = 0, control = 1, shift = 2, alt = 4, command = 8 };
constexpr CommandModifier operator|(CommandModifier a, CommandModifier b) noexcept {
    return static_cast<CommandModifier>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr bool has_modifier(CommandModifier a, CommandModifier b) noexcept {
    return (static_cast<unsigned>(a) & static_cast<unsigned>(b)) == static_cast<unsigned>(b);
}
struct KeyChord {
    std::string key; // normalized ASCII uppercase, or TAB / ENTER / LEFT, etc.
    CommandModifier modifiers{CommandModifier::none};
    auto operator<=>(const KeyChord&) const = default;
};
struct KeyEvent { KeyChord chord; bool composing{}; bool alt_graph{}; bool key_down{true}; };
struct CommandContext {
    std::size_t tabs{}, panes{}; // panes counts the active tab

    bool has_selection{}, clipboard_has_text{}, zoomed{}, session_running{};
    std::size_t total_panes{}; // optional exact workspace count; 0 falls back to panes
    bool supports_workspace_navigator{};
    bool supports_previous_terminal{}, previous_terminal_available{};
    std::string_view previous_terminal_reason{};
    bool supports_pane_context{};
    bool supports_command_watch{},watch_can_arm{},watch_waiting{},watch_unread{};
    bool supports_project_workspaces{}; // Hosts must opt in, not silently no-op.
    std::size_t project_workspaces{};
    bool supports_focus_mode{};
    bool supports_settings_reload{};
    bool supports_agent_panel{};
    bool supports_background_sessions{};
    std::size_t parked_sessions{};
};
struct Command {
    std::string_view id;
    std::string_view label;
    std::string_view description;
};
struct CommandAvailability { bool enabled{}; std::string_view reason; };
struct Binding { std::string command; KeyChord chord; auto operator<=>(const Binding&) const = default; };
// Display-only background title: explicit user names win; only raw known shell
// executable titles are shortened. Custom process titles remain unchanged.
std::string background_session_title(std::string_view user_title, std::string_view process_title,
                                     std::string_view profile_label);
// Bounded Unicode-codepoint subsequence matching. ASCII case-insensitive; other
// codepoints match exactly. Malformed UTF-8 and queries over 64 codepoints fail.
// Candidate fields are bounded to 512 codepoints; no regex or locale dependency.
std::optional<int> command_search_score(std::string_view query, std::string_view label,
                                        std::string_view description = {});
std::vector<const Command*> search_commands(std::string_view query);
std::span<const Command> command_registry() noexcept;
const Command* find_command(std::string_view id) noexcept;
CommandAvailability command_availability(std::string_view id, const CommandContext&) noexcept;
std::vector<Binding> default_bindings(Platform);
// Grammar: Primary+Shift+P, Ctrl+Tab, Cmd+K, F11, etc. Primary maps to Ctrl on
// Windows and Cmd on macOS. Physical key translation remains in the host.
Result<KeyChord> parse_key_chord(std::string_view, Platform);
std::string format_key_chord(const KeyChord&, Platform);
// Returns nothing for composition, AltGr, key-up, disabled commands, OS-reserved
// chords or unmatched input. Callers MUST forward unmatched input to the VT path.
std::optional<std::string_view> resolve_command(const KeyEvent&, std::span<const Binding>,
                                               const CommandContext&, Platform) noexcept;
Status validate_bindings(std::span<const Binding>, Platform, bool allow_terminal_override = false);
bool is_os_reserved(const KeyChord&, Platform) noexcept;
bool is_terminal_reserved(const KeyChord&, Platform) noexcept;
} // namespace stormtty
