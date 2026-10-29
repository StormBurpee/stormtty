#pragma once
#include "stormtty/agent_panel.hpp"
#include "stormtty/agent_stdio_transport.hpp"
#include "stormtty/agent_discovery.hpp"
#include <functional>
#include <map>
#include <memory>
namespace stormtty {
// Native host adapter. Configuration is inert. Panel opening can launch a safely
// discovered status daemon; overrides require explicit Connect. Starting a model
// task always requires the separate Start action. Worker owns launch/teardown.
class AgentPanelController {
public:
    using LaunchFactory=std::function<std::unique_ptr<AgentStdioTransport>(const AgentStdioOptions&,std::shared_ptr<AgentService>,std::string&)>;
    using DiscoveryFactory=std::function<AgentDiscoveryResult()>;
    explicit AgentPanelController(AgentStdioOptions options = {}, std::function<void()> wake = {}, std::vector<WorkspaceAgentAssociation> restored = {}, LaunchFactory launch = {}, DiscoveryFactory discover = {});
    ~AgentPanelController();
    AgentPanelView view(std::string workspace_id, std::string name, std::string directory) const;
    bool dispatch(const AgentPanelAction&, std::string directory);
    // Call once when opening the panel: discovers known installations and checks
    // status only. An advanced override still requires explicit Connect.
    bool opened(std::string workspace_id, std::string directory);
    // UI-thread catalog operation: no process stop/join here. Call after close
    // confirmation and before removing the workspace model. False leaves it intact.
    bool retire(std::string_view workspace_id);
    static constexpr std::size_t maximum_retiring = 16;
    std::size_t retiring_count() const;
    std::vector<WorkspaceAgentAssociation> associations() const;
    bool can_reconfigure() const;
    bool reconfigure(const AgentProviderSettings&);
    bool attention() const;
    std::string attention_workspace() const;
    bool any_active() const;
    bool active(std::string_view workspace_id) const;
private:
    struct Entry;
    struct Reaper;
    AgentStdioOptions options_;
    LaunchFactory launch_;
    DiscoveryFactory discover_;
    std::uint64_t settings_generation_{};
    std::function<void()> wake_;
    std::map<std::string,std::unique_ptr<Entry>> entries_;
    std::unique_ptr<Reaper> reaper_;
};
}
