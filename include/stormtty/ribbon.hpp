#pragma once
#include <array>
#include <algorithm>
#include <optional>
#include <string_view>

namespace stormtty {
// Closed, portable data-binding registry. No callbacks, scripts, paths or commands.
enum class RibbonBinding { directory, git, status, background, agent, clock };
struct RibbonBindingInfo { RibbonBinding binding; std::string_view id, label; int priority; double minimum, preferred; };
inline constexpr std::array<RibbonBindingInfo,6> ribbon_bindings{{
    {RibbonBinding::directory,"session.directory","Directory",80,96,320},
    {RibbonBinding::git,"session.git","Git branch",60,88,180},
    {RibbonBinding::status,"session.status","Session status",90,64,80},
    {RibbonBinding::background,"workspace.background","Background terminals",95,64,380},
    {RibbonBinding::agent,"agent.status","Agent status",100,88,160},
    {RibbonBinding::clock,"clock.local","Clock",20,44,44}
}};
inline std::optional<RibbonBinding> ribbon_binding(std::string_view id) {
    for(const auto& entry:ribbon_bindings) if(entry.id==id) return entry.binding;
    return {};
}
inline constexpr std::size_t ribbon_index(RibbonBinding value) {return static_cast<std::size_t>(value);}
struct RibbonConfig {
    std::array<RibbonBinding,6> order{RibbonBinding::directory,RibbonBinding::git,RibbonBinding::status,RibbonBinding::background,RibbonBinding::agent,RibbonBinding::clock};
    std::array<bool,6> hidden{};
    auto operator<=>(const RibbonConfig&) const = default;
};
inline bool valid_ribbon(const RibbonConfig& config) {
    std::array<bool,6> seen{};
    for(auto item:config.order) {const auto i=ribbon_index(item);if(i>=seen.size() || seen[i]) return false;seen[i]=true;}
    return true;
}
// Host must construct this anew for the active workspace, never retain stale context.
// Attention is essential and cannot be hidden by a declarative preference.
struct RibbonHostData {
    bool agent_available{}; bool agent_attention{}; bool session_error{};
    // Opt-in compact workspace summary: one Navigator target, not per-terminal chips.
    bool compact_background{};
    bool background_attention{}; // Unread explicit command completion, not shell liveness.
};
}
