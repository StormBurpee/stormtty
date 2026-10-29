#pragma once
#include "stormtty/agent_panel.hpp"
#include "stormtty/agent_stdio_transport.hpp"
#include <functional>
#include <map>
#include <memory>
namespace stormtty {
// Native host adapter. Configuration is inert: only the explicit Connect action
// launches a child. Worker owns launch/teardown; UI owns action dispatch.
class AgentPanelController {
public:
    using LaunchFactory=std::function<std::unique_ptr<AgentStdioTransport>(const AgentStdioOptions&,std::shared_ptr<AgentService>,std::string&)>;
    explicit AgentPanelController(AgentStdioOptions options = {}, std::function<void()> wake = {}, std::vector<WorkspaceAgentAssociation> restored = {}, LaunchFactory launch = {});
    ~AgentPanelController();
    AgentPanelView view(std::string workspace_id, std::string name, std::string directory) const;
    bool dispatch(const AgentPanelAction&, std::string directory);
    std::vector<WorkspaceAgentAssociation> associations() const;
    bool can_reconfigure() const;
    bool reconfigure(const AgentProviderSettings&);
    bool attention() const;
    std::string attention_workspace() const;
    bool any_active() const;
    bool active(std::string_view workspace_id) const;
private:
    struct Entry;
    AgentStdioOptions options_;
    LaunchFactory launch_;
    std::uint64_t settings_generation_{};
    std::function<void()> wake_;
    std::map<std::string,std::unique_ptr<Entry>> entries_;
};
}
