#pragma once
#include "stormtty/agent_service.hpp"
#include <memory>
#include <functional>
#include <string>
namespace stormtty {
struct AgentStdioOptions {
    // User-selected absolute executable. Never resolved through PATH or a shell.
    std::string executable, working_directory, runtime_version, account_reference;
    // Separate authorization to launch the provider process. Does not authorize
    // a thread/turn, login, sandbox installation or account configuration.
    bool user_confirmed_launch{};
    // Worker callback; never stop/join this transport from within the callback.
    std::function<void()> state_changed;
};
class AgentStdioTransport {
public:
    static std::unique_ptr<AgentStdioTransport> launch(const AgentStdioOptions&, std::shared_ptr<AgentService>, std::string& error);
    ~AgentStdioTransport();
    AgentStdioTransport(const AgentStdioTransport&)=delete;
    AgentStdioTransport& operator=(const AgentStdioTransport&)=delete;
    void stop();
private:
    struct Impl;
    explicit AgentStdioTransport(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace stormtty
