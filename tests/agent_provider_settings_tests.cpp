#include "core_test.hpp"
#include "stormtty/agent_provider_settings.hpp"
using namespace stormtty;
int main(){return run_core_tests("agent_provider_settings",[]{
    AgentProviderSettings valid{"C:\\Program Files\\Codex\\codex.exe","chatgpt:person@example.com"};
    CHECK(validate_agent_provider_settings(valid).empty());std::string error;
    CHECK(parse_agent_provider_settings(serialize_agent_provider_settings(valid),error)==valid);
    CHECK(parse_agent_provider_settings(serialize_agent_provider_settings({}),error)==AgentProviderSettings{});
    for(const auto* path:{"codex.exe","C:codex.exe","\\\\host\\share\\codex.exe","C:\\..\\codex.exe","C:\\Tools\\codex.cmd","C:\\Tools\\codex.exe --login","C:\\Tools\\codex.exe:stream","C:\\Tools\\\\codex.exe"}) {
        auto candidate=valid;candidate.executable=path;CHECK(!validate_agent_provider_settings(candidate).empty());
    }
    auto candidate=valid;candidate.executable="C:\\Tools\\\xE2\x80\xAE" "codex.exe";CHECK(!validate_agent_provider_settings(candidate).empty());
    candidate=valid;candidate.account_reference="sk-not-an-account";CHECK(!validate_agent_provider_settings(candidate).empty());
    candidate.account_reference="chatgpt:no-email";CHECK(!validate_agent_provider_settings(candidate).empty());
    candidate.account_reference="chatgpt:a@b\nsecret=token";CHECK(!validate_agent_provider_settings(candidate).empty());
    CHECK(!parse_agent_provider_settings(serialize_agent_provider_settings(valid)+"token=secret\n",error));
    CHECK(!parse_agent_provider_settings("stormtty-agent-settings:2\nexecutable=\naccount=\n",error));
});}
