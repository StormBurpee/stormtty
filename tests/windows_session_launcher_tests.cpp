#include "core_test.hpp"
#include "stormtty/windows_session_launcher.hpp"
#include "stormtty/windows_shell_profile.hpp"
#include "stormtty/windows_managed_shell.hpp"
#include <algorithm>
#include <array>
#include <cwchar>
using namespace stormtty;
namespace {
// Preserve absent versus explicit-empty values, including on assertion failure.
struct Environment {
    std::wstring name;
    std::optional<std::wstring> original;
    explicit Environment(const wchar_t* key) : name(key) {
        auto* block=GetEnvironmentStringsW();
        if (!block) throw std::runtime_error("environment snapshot failed");
        for (auto* entry=block; *entry; entry+=std::wcslen(entry)+1) {
            const auto* equal=std::wcschr(entry,L'=');
            if (equal && equal-entry==static_cast<std::ptrdiff_t>(name.size()) &&
                CompareStringOrdinal(entry,static_cast<int>(name.size()),name.data(),
                    static_cast<int>(name.size()),TRUE)==CSTR_EQUAL) original=equal+1;
        }
        FreeEnvironmentStringsW(block);
    }
    ~Environment() { SetEnvironmentVariableW(name.c_str(),original?original->c_str():nullptr); }
    void set(const wchar_t* value) { CHECK(SetEnvironmentVariableW(name.c_str(),value)); }
};
const ProcessEnvironmentEntry* entry(const std::vector<ProcessEnvironmentEntry>& values, std::string_view name) {
    for (auto it=values.rbegin(); it!=values.rend(); ++it) if (it->name==name) return &*it;
    return nullptr;
}
template<class F> void fails(F&& operation, std::string_view message) {
    bool caught=false;
    try { operation(); } catch (const std::runtime_error& error) {
        caught=true; CHECK(std::string_view(error.what()).find(message)!=std::string_view::npos);
    }
    CHECK(caught);
}
std::string utf8(const std::filesystem::path& path) {
    auto bytes=path.u8string(); return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};
}
void characterize() {
    auto config=default_config(Platform::windows);
    config.default_profile="saved";
    config.profiles={{"saved","Saved","C:\\trusted\\shell.exe",{"--interactive"},"C:\\profile",{{"EXPLICIT","value"}}}};
    WindowsSessionLaunchOptions options;
    options.shell_runtime_directory=L"C:\\app\\runtimes\\zsh";
    options.shell_runtime_override=false;
    Environment path(L"PATH"); path.set(L"C:\\inherited");
    WindowsSessionLauncher launcher(options);
    CHECK(launcher.managed_profile());
    CHECK(launcher.default_profile_id(config)=="saved"); // Saved choice wins over bundled presence.
    CHECK(!launcher.using_managed_shell(config));
    auto saved=launcher.prepare(config,"","@default","");
    CHECK(saved.process.executable=="C:\\trusted\\shell.exe");
    CHECK(saved.process.working_directory=="C:\\profile");
    CHECK(saved.process.arguments==std::vector<std::string>{"--interactive"});
    CHECK(entry(saved.process.environment,"EXPLICIT")->value=="value");
    CHECK(!saved.managed);
    CHECK(launcher.prepare(config,"saved","missing","C:\\project").process.working_directory=="C:\\project");
    fails([&]{launcher.prepare(config,"missing","saved","");},"Saved shell profile is unavailable");

    auto selected=config;
    selected.default_profile=std::string(managed_zsh_profile_id);
    selected.profiles.push_back(managed_zsh_profile_reference());
    CHECK(launcher.using_managed_shell(selected));
    CHECK(launcher.default_profile_id(selected)==managed_zsh_profile_id);
    options.shell_runtime_override=true;
    CHECK(WindowsSessionLauncher(options).default_profile_id(config)==managed_zsh_profile_id);
    options.shell_runtime_error="missing runtime";
    WindowsSessionLauncher unavailable(options);
    CHECK(!unavailable.managed_profile());
    CHECK(unavailable.prepare(config,"saved","@default","").process.executable==saved.process.executable);
    fails([&]{unavailable.prepare(selected,"","@default","");},"no fallback was launched");

    // Provider changes affect subsequent values only; old actor inputs are owned.
    CHECK(saved.process.provider==ProcessProvider::windows_inbox_basic);
    config.windows_console_backend=WindowsConsoleBackend::bundled_preview;
    auto next=launcher.prepare(config,"saved","@default","");
    CHECK(next.process.provider==ProcessProvider::windows_packaged_experimental);
    CHECK(!next.process.provider_directory.empty());
    CHECK(saved.process.provider==ProcessProvider::windows_inbox_basic);
    options={}; options.console_runtime_directory=L"C:\\explicit-console";
    auto forced=WindowsSessionLauncher(options).prepare(default_config(Platform::windows),"","@default","");
    CHECK(forced.process.provider==ProcessProvider::windows_packaged_experimental);
    CHECK(forced.process.provider_directory=="C:\\explicit-console");

    std::array<wchar_t,MAX_PATH+1> system{};
    CHECK(GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size())));
    const auto trusted=utf8(std::filesystem::path(system.data())/L"cmd.exe");
    WindowsSessionLauncher plain({});
    for (const auto* alias:{"","cmd","CMD","cMd.ExE"}) {
        config.profiles[0].executable=alias;
        config.profiles[0].arguments={"/D","/q"};
        auto cmd=plain.prepare(config,"saved","@default","");
        CHECK(cmd.process.executable==trusted);
        CHECK(cmd.process.arguments==std::vector<std::string>({"/d","/D","/q"}));
        config.profiles[0].arguments={"/q","/d"};
        CHECK(plain.prepare(config,"saved","@default","").process.arguments==config.profiles[0].arguments);
    }
    config.profiles[0].executable="C:\\custom\\cmd.exe";
    CHECK(plain.prepare(config,"saved","@default","").process.executable=="C:\\custom\\cmd.exe");
    options={}; options.smoke_test=true;
    auto smoke=WindowsSessionLauncher(options).prepare(config,"saved","@default","C:\\project");
    CHECK(smoke.process.executable==trusted);
    CHECK(smoke.process.arguments==std::vector<std::string>({"/d","/q"}));
    CHECK(smoke.process.environment.empty()); CHECK(smoke.process.working_directory.empty());
    for (bool projects:{false,true}) {
        options.smoke_projects=projects; options.smoke_windows=!projects;
        CHECK(WindowsSessionLauncher(options).prepare(config,"saved","@default","C:\\project").process.working_directory=="C:\\project");
    }

    config.scrollback_lines=1234;
    config.theme.foreground="#1234aB"; config.theme.background="invalid";
    config.theme.ansi[0]="#654321"; config.theme.ansi[1]="invalid";
    config.appearance.background_opacity=0.2;
    auto colors=plain.prepare(config,"saved","@default","").terminal;
    CHECK(colors.scrollback_lines==1234);
    CHECK(colors.default_foreground==Color{0x12,0x34,0xab,255});
    CHECK(colors.default_background==Color{23,25,29,255});
    CHECK((*colors.ansi)[0]==Color{0x65,0x43,0x21,255});
    CHECK((*colors.ansi)[1]==Color{0,0,0,255});
    CHECK(!colors.resize_pull_scrollback);

    // PATH is captured once at construction; HOME/state is fresh per prepare.
    path.set(L"C:\\later");
    const auto& environment=launcher.managed_profile()->environment;
    CHECK(environment.at("PATH")=="C:\\app\\runtimes\\zsh\\usr\\bin;C:\\inherited");
    Environment home(L"HOME"), drive(L"HOMEDRIVE"), homepath(L"HOMEPATH"), profile(L"USERPROFILE");
    for (const auto* explicit_home:{L"C:\\explicit-\u6d4b\u8bd5",L"/posix/home",L"\\\\server\\share",L""}) {
        home.set(explicit_home);
        const auto values=managed_shell_home_environment();
        CHECK(!entry(values,"HOME"));
        CHECK(entry(values,"STORMTTY_EMPTY_HOME")->value==(*explicit_home?"":"1"));
    }
    home.set(nullptr); drive.set(L"D:"); homepath.set(L"\\native-home"); profile.set(L"C:\\profile-home");
    CHECK(entry(managed_shell_home_environment(),"HOME")->value=="D:\\native-home");
    drive.set(nullptr);
    CHECK(entry(managed_shell_home_environment(),"HOME")->value=="C:\\profile-home");
    profile.set(nullptr); CHECK(!entry(managed_shell_home_environment(),"HOME"));
    home.set(L"");
    // Empty HOME disables state; never touch a real user's state tree in this test.
    auto managed=launcher.prepare(selected,"","@default","C:\\project");
    CHECK(managed.managed);
    CHECK(managed.process.working_directory=="C:\\project");
    CHECK(managed.process.arguments==std::vector<std::string>({"-d","-i"}));
    CHECK(entry(managed.process.environment,"STORMTTY_EMPTY_HOME")->value=="1");
    CHECK(!entry(managed.process.environment,"HOME"));
    CHECK(!entry(managed.process.environment,"STORMTTY_NATIVE_ZSH_STATE")->value);
    CHECK(entry(managed.process.environment,"STORMTTY_NATIVE_CONSOLE_SETUP")->value);
    for (const auto* name:{"MSYS","ENV","BASH_ENV"}) CHECK(!entry(managed.process.environment,name)->value);
    CHECK(managed.process.executable=="C:\\app\\runtimes\\zsh\\usr\\bin\\zsh.exe");
    options={}; options.shell_runtime_directory=L"C:\\app\\runtimes\\zsh"; options.smoke_test=true;
    home.set(L"C:\\explicit");
    fails([&]{WindowsSessionLauncher(options).prepare(selected,"","@default","",{});},"failed revalidation");
}
}
int main() { return run_core_tests("windows session launcher",characterize); }
