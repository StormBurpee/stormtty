#pragma once
#include "stormtty/session.hpp"
#include "stormtty/command_watch.hpp"
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

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

// One entry per retained actor in the active workspace's watched-or-parked
// union. The host validates incarnation/membership and deduplicates placements.
// These are terminal/result counts, not jobs, progress or process liveness.
inline unsigned activity_priority(const CommandWatch* watch, SessionState state, bool parked_unread) {
    if(state==SessionState::failed) return 6;
    if(watch && watch->unread()) {
        if(watch->status()!=WatchStatus::completed || !watch->exit_code()) return 4;
        return *watch->exit_code()!=0?5:3;
    }
    if(parked_unread) return 3;
    return watch && watch->status()==WatchStatus::watching?2:1;
}
struct WorkspaceActivitySummary {
    std::size_t terminals{}, background{}, unread_terminals{}, running{}, completed{}, nonzero{}, unknown{}, unavailable{}, background_finished{}, failed{};
    std::optional<std::int32_t> nonzero_code;
    void add(bool parked, std::optional<std::uint64_t> parked_completion, const CommandWatch* watch, SessionState state) {
        if(watch && watch->status()==WatchStatus::idle) watch=nullptr;
        if(!parked && !watch) return;
        ++terminals;
        if(parked) ++background;
        if(parked_completion || (watch && watch->unread())) ++unread_terminals;
        if(state==SessionState::failed) ++failed;
        // Only an exact completed Watch duplicates this observed parked D.
        // An older result or unavailable Watch must not hide a later real D.
        if(parked_completion && !(watch && watch->status()==WatchStatus::completed && watch->generation()==*parked_completion)) ++background_finished;
        if(!watch) return;
        switch(watch->status()) {
        case WatchStatus::watching: ++running; break;
        case WatchStatus::completed:
            if(!watch->exit_code()) ++unknown;
            else if(*watch->exit_code()!=0) {++nonzero;nonzero_code=watch->exit_code();}
            else ++completed;
            break;
        case WatchStatus::completion_unconfirmed:
        case WatchStatus::interrupted:
        case WatchStatus::unavailable: ++unavailable; break;
        case WatchStatus::idle: break;
        }
    }
    [[nodiscard]] std::size_t unread() const noexcept {return std::min(terminals,unread_terminals);}
    [[nodiscard]] bool has_watches() const noexcept {return running+completed+nonzero+unknown+unavailable!=0;}
    // Whole, prioritized tokens: the host measures them, never clips an exit
    // code into another value. Omitted detail is available in the scoped list.
    [[nodiscard]] std::vector<std::string> labels() const {
        std::vector<std::string> out;
        if(failed) out.push_back(std::to_string(failed)+(failed==1?" terminal failed":" terminals failed"));
        if(nonzero) out.push_back(nonzero==1?"Watch · exit "+std::to_string(*nonzero_code):std::to_string(nonzero)+" nonzero Watch results");
        if(unknown) out.push_back(unknown==1?"Watch · finished · status unknown":std::to_string(unknown)+" Watch results · status unknown");
        if(unavailable) out.push_back(unavailable==1?"Watch · result unavailable":std::to_string(unavailable)+" Watch results unavailable");
        if(completed) out.push_back(completed==1?"Watch · complete · exit 0":std::to_string(completed)+" Watch results · exit 0");
        if(background_finished) out.push_back(std::to_string(background_finished)+(background_finished==1?" background completion":" background completions"));
        if(running) out.push_back(running==1?"Watch · running":std::to_string(running)+" Watches running");
        if(background) out.push_back("Background "+std::to_string(background));
        return out;
    }
    [[nodiscard]] std::string accessible_label() const {
        std::string result="Workspace activity";
        for(const auto& token:labels()) result+=" · "+token;
        return result;
    }
    template<class Measure> [[nodiscard]] std::string label(double width, Measure measure) const {
        const auto tokens=labels();
        std::string result;
        for(const auto& token:tokens) {
            const auto candidate=result.empty()?token:result+" · "+token;
            if(measure(candidate)>width) break;
            result=candidate;
        }
        if(result.empty() && terminals) result=background==terminals&&!has_watches()?"Background":"Activity";
        return measure(result)<=width?result:std::string{};
    }
};

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
