#pragma once
#include "stormtty/settings_services.hpp"
#include <functional>
#include <mutex>
#include <thread>
namespace stormtty {
// OS-known per-user local folder; never an ambient environment/repository path.
std::filesystem::path local_agent_settings_path();
struct AgentSetupCompletion {
    bool saved{};
    AgentProviderSettings settings;
    std::optional<ConfigFileSnapshot> snapshot;
    std::string error;
};
// Native settings worker only; never calls a provider or touches executable paths.
class AgentSetupService {
public:
    AgentSetupService(std::filesystem::path path,std::function<void()> wake);
    ~AgentSetupService();
    void load();
    bool save(const AgentProviderSettings&);
    std::optional<AgentSetupCompletion> take_completion();
    bool busy() const {return busy_;}
    bool ready() const {return baseline_.has_value();}
private:
    std::filesystem::path path_;
    std::function<void()> wake_;
    std::optional<ConfigFileSnapshot> baseline_;
    std::mutex mutex_;
    std::optional<AgentSetupCompletion> completion_;
    std::thread worker_;
    bool busy_{};
};
}
