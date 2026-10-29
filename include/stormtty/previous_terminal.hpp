#pragma once
#include "stormtty/command_watch.hpp"
#include "stormtty/commands.hpp"

namespace stormtty {
// Per-window, memory-only committed model selections. Never observe transient
// input focus: overlays/deactivation are not terminal navigation. Incarnations,
// not pane/tab positions, survive moves and reject recycled actor identities.
class PreviousTerminal {
public:
    void commit(WatchIdentity actor) noexcept {
        if (!actor || actor == current_) return;
        previous_ = current_;
        current_ = actor;
    }
    [[nodiscard]] WatchIdentity current() const noexcept { return current_; }
    [[nodiscard]] WatchIdentity previous() const noexcept { return previous_; }
private:
    WatchIdentity current_{}, previous_{};
};
enum class PreviousTerminalState { none, available, parked, missing, dormant, stale };
inline CommandAvailability previous_terminal_availability(PreviousTerminalState state) noexcept {
    switch (state) {
    case PreviousTerminalState::available: return {true,{}};
    case PreviousTerminalState::parked: return {false,"Previous terminal is in the background; restore it explicitly"};
    case PreviousTerminalState::missing: return {false,"Previous terminal is closed or no longer available"};
    case PreviousTerminalState::dormant: return {false,"Previous terminal has no retained actor; nothing will be launched"};
    case PreviousTerminalState::stale: return {false,"Previous terminal actor has changed"};
    default: return {false,"Select another terminal first"};
    }
}
} // namespace stormtty
