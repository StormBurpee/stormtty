#pragma once
#include "stormtty/session.hpp"
#include <algorithm>
#include <cstddef>
#include <string>

namespace stormtty {
// Presentation only. A live shell does not prove that a command is running.
// Terminal lifecycle wins over stale OSC metadata after exit/close/failure.
inline std::string terminal_status_label(const SessionInfo& info) {
    switch (info.state) {
    case SessionState::starting: return "Terminal starting";
    case SessionState::closing: return "Terminal closing";
    case SessionState::failed: return "Terminal failed";
    case SessionState::exited:
        return "Terminal exited" + (info.exit_code ? " · exit " + std::to_string(*info.exit_code) : "");
    case SessionState::running: break;
    }
    switch (info.command.phase) {
    case CommandPhase::running: return "Command running";
    case CommandPhase::interrupted: return "Command interrupted";
    case CommandPhase::completed:
        if (info.command.completed_generation &&
            info.command.completed_generation == info.command.command_generation)
            return "Command finished" + (info.command.last_exit_code && *info.command.last_exit_code != 0
                ? " · exit " + std::to_string(*info.command.last_exit_code) : "");
        break;
    case CommandPhase::unavailable: break;
    }
    return "Terminal open";
}

// Construct anew from ONLY the active workspace's parked placements. Count
// terminals with unread observed completions, not commands, progress or liveness.
// The attention map remains host-owned; rendering never acknowledges it.
struct WorkspaceBackgroundSummary {
    std::size_t terminals{}, unread_terminals{};
    void add(bool unread_completion) noexcept {
        ++terminals;
        if (unread_completion) ++unread_terminals;
    }
    [[nodiscard]] std::size_t unread() const noexcept { return std::min(terminals, unread_terminals); }
    [[nodiscard]] std::string label() const { return "Background " + std::to_string(terminals); }
    [[nodiscard]] std::string completion_label() const {
        return unread() ? std::to_string(unread()) + " finished" : std::string{};
    }
    [[nodiscard]] std::string accessible_label() const {
        auto result = label();
        if (unread()) result += " · " + std::to_string(unread()) +
            (unread() == 1 ? " terminal has an unread command completion" : " terminals have unread command completions");
        return result;
    }
};
} // namespace stormtty
