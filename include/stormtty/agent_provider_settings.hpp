#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace stormtty {
// Machine-local metadata only. Never tokens, credentials or portable workspace data.
struct AgentProviderSettings {
    std::string executable, account_reference;
    bool operator==(const AgentProviderSettings&) const = default;
};
std::string validate_agent_provider_settings(const AgentProviderSettings&);
std::string serialize_agent_provider_settings(const AgentProviderSettings&);
std::optional<AgentProviderSettings> parse_agent_provider_settings(std::string_view, std::string& error);
}
