#pragma once
#include <functional>
#include <string>
#include <vector>
namespace stormtty {
enum class AgentDiscoveryPlatform { windows, macos, linux };
struct AgentDiscoveryOptions {
    AgentDiscoveryPlatform platform{AgentDiscoveryPlatform::windows};
    bool arm64{};
    // OS-known installation locations, NOT PATH, current directory or repository input.
    std::vector<std::string> native_candidates, npm_node_modules, trusted_roots;
};
struct AgentDiscoveryFilesystem {
    // canonical returns an empty string when absent/unreadable. No execution.
    std::function<std::string(const std::string&)> canonical;
    std::function<bool(const std::string&)> exists;
    std::function<bool(const std::string&, AgentDiscoveryPlatform)> native_executable;
};
struct AgentDiscoveryResult { std::string executable, detail, working_directory; };
AgentDiscoveryResult discover_codex(const AgentDiscoveryOptions&, const AgentDiscoveryFilesystem&);
// Bounded known locations only; does not inspect auth files or execute a process.
AgentDiscoveryResult discover_codex();
}
