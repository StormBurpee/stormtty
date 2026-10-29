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
    explicit AgentPanelController(AgentStdioOptions options = {}, std::function<void()> wake = {});
    ~AgentPanelController();
    AgentPanelView view(std::string workspace_id, std::string name, std::string directory) const;
    bool dispatch(const AgentPanelAction&, std::string directory);
    bool attention() const;
    std::string attention_workspace() const;
    bool any_active() const;
    bool active(std::string_view workspace_id) const;
private:
    struct Entry;
    AgentStdioOptions options_;
    std::function<void()> wake_;
    std::map<std::string,std::unique_ptr<Entry>> entries_;
};
}
