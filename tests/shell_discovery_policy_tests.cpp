#include "core_test.hpp"
#include "../src/platform/windows/shell_discovery_policy.hpp"

using namespace stormtty;
namespace {

ShellDiscoveryResult candidate(const char* id = "windows-cmd", bool bash = false) {
    ShellDiscoveryResult result;
    shell_discovery_policy::add_first(result, id, "Detected shell", {"fixed/path"}, bash,
        [](const auto&) { return std::string("C:/verified/shell.exe"); });
    return result;
}

void bounded_first_candidate() {
    ShellDiscoveryResult result;
    std::vector<std::filesystem::path> visited;
    const std::vector<std::filesystem::path> paths{"missing", "oversized", "valid", "later"};
    shell_discovery_policy::add_first(result, "powershell-7", "PowerShell 7", paths, false,
        [&](const auto& path) {
            visited.push_back(path);
            if (path == "missing") return std::string{};
            if (path == "oversized") return std::string(4097, 'x');
            return std::string(4096, 'v');
        });
    CHECK(visited == std::vector<std::filesystem::path>({"missing", "oversized", "valid"}));
    CHECK(result.profiles.size() == 1);
    CHECK(result.profiles[0].id == "powershell-7");
    CHECK(result.profiles[0].label == "PowerShell 7");
    CHECK(result.profiles[0].executable == std::string(4096, 'v'));
    CHECK(result.profiles[0].arguments == std::vector<std::string>{"-NoLogo"});
    CHECK(result.profiles[0].environment.empty());
    CHECK(result.profiles[0].working_directory.empty());
    CHECK(result.warnings.empty());

    const auto saved = result.profiles;
    shell_discovery_policy::add_first(result, "missing", "Missing", paths, false,
        [](const auto&) { return std::string{}; });
    CHECK(result.profiles == saved);
    shell_discovery_policy::add_first(result, "unicode", "Unicode", {"fixed/path"}, false,
        [](const auto&) { return std::string("C:/\xc3\xa9/pwsh.exe"); });
    CHECK(result.profiles.back().executable == "C:/\xc3\xa9/pwsh.exe");
}

void launch_policy() {
    const auto cmd = candidate();
    CHECK(cmd.profiles[0].arguments == std::vector<std::string>{"/d"});
    const auto bash = candidate("git-bash", true);
    CHECK(bash.profiles[0].arguments == std::vector<std::string>({"--login", "-i"}));
    CHECK(bash.profiles[0].environment.size() == 1);
    CHECK(bash.profiles[0].environment.at("CHERE_INVOKING") == "1");
    CHECK(bash.profiles[0].working_directory.empty());
    const auto powershell = candidate("windows-powershell");
    CHECK(powershell.profiles[0].arguments == std::vector<std::string>{"-NoLogo"});
}

void cmd_equivalence() {
    for (const auto* alias : {"cmd", "cmd.exe", "CMD", "CmD.ExE"}) {
        for (const auto& arguments : {std::vector<std::string>{}, std::vector<std::string>{"/d"}}) {
            Profile configured;
            configured.id = "custom-id";
            configured.executable = alias;
            configured.arguments = arguments;
            const std::vector<Profile> profiles{configured};
            auto discovered = candidate();
            shell_discovery_policy::hide_represented_cmd(discovered, profiles);
            CHECK(discovered.profiles.empty());
            CHECK(profiles == std::vector<Profile>{configured});
        }
    }
    Profile configured;
    configured.id = "windows-cmd";
    configured.executable = "cmd";
    for (unsigned variant = 0; variant < 8; ++variant) {
        auto distinct = configured;
        switch (variant) {
        case 0: distinct.executable = "C:/Windows/System32/cmd.exe"; break;
        case 1: distinct.executable = "my-cmd.exe"; break;
        case 2: distinct.arguments = {"/D"}; break;
        case 3: distinct.arguments = {"/d", "/q"}; break;
        case 4: distinct.arguments = {"/q", "/d"}; break;
        case 5: distinct.arguments = {"/d", "/d"}; break;
        case 6: distinct.environment.emplace("CUSTOM", "value"); break;
        case 7: distinct.working_directory = "C:/work"; break;
        }
        auto discovered = candidate();
        shell_discovery_policy::hide_represented_cmd(discovered, std::vector<Profile>{distinct});
        CHECK(discovered.profiles.size() == 1);
    }
    auto non_cmd = candidate("powershell-7");
    shell_discovery_policy::hide_represented_cmd(non_cmd, std::vector<Profile>{configured});
    CHECK(non_cmd.profiles.size() == 1);
}

} // namespace

int main() {
    return run_core_tests("shell_discovery_policy", [] {
        bounded_first_candidate();
        launch_policy();
        cmd_equivalence();
    });
}
