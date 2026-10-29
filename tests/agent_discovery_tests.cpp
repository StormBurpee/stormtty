#include "core_test.hpp"
#include "stormtty/agent_discovery.hpp"
#include <algorithm>
#include <map>
#include <set>
using namespace stormtty;
int main(){return run_core_tests("agent_discovery",[]{
    AgentDiscoveryOptions o; o.platform=AgentDiscoveryPlatform::windows;
    o.trusted_roots={"C:/Users/A/AppData/Roaming/npm","C:/Users/A/.codex/packages/standalone"};
    o.npm_node_modules={"C:/Users/A/AppData/Roaming/npm/node_modules"};
    const std::string native=o.npm_node_modules[0]+"/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/bin/codex.exe";
    std::map<std::string,std::string> files;std::set<std::string> executables,markers;std::vector<std::string> probes;
    AgentDiscoveryFilesystem fs{
        [&](const std::string& p){probes.push_back(p);auto i=files.find(p);return i==files.end()?std::string{}:i->second;},
        [&](const std::string& p){return markers.contains(p);},
        [&](const std::string& p,AgentDiscoveryPlatform){return executables.contains(p);}};
    CHECK(discover_codex(o,fs).executable.empty());
    files[native]=native;executables.insert(native);
    auto found=discover_codex(o,fs);CHECK(found.executable==native);CHECK(found.working_directory==native.substr(0,native.rfind('/')));
    for(const auto& probe:probes)CHECK(!probe.ends_with(".cmd")&&!probe.ends_with(".js"));
    markers.insert("C:/Users/A/AppData/Roaming/npm/.git");CHECK(discover_codex(o,fs).executable.empty());markers.clear();
    files[native]="C:/repo/codex.exe";executables.insert(files[native]);CHECK(discover_codex(o,fs).executable.empty());
    files[native]="//server/share/codex.exe";executables.insert(files[native]);CHECK(discover_codex(o,fs).executable.empty());
    files[native]=native;executables.clear();CHECK(discover_codex(o,fs).executable.empty()); // shim/text is not a native image
    executables.insert(native);o.arm64=true;CHECK(discover_codex(o,fs).executable.empty());o.arm64=false;
    o.native_candidates={"codex.exe","./codex.exe","C:codex.exe","//server/share/codex.exe","C:/Users/A/AppData/Roaming/npm/../repo/codex.exe"};
    probes.clear();CHECK(discover_codex(o,fs).executable==native);CHECK(std::find(probes.begin(),probes.end(),native)!=probes.end());
    for(const auto& unsafe:o.native_candidates)CHECK(std::find(probes.begin(),probes.end(),unsafe)==probes.end());
    const std::string nested=o.npm_node_modules[0]+"/@openai/codex/node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/bin/codex.exe";
    files[nested]=nested;executables.insert(nested);
    CHECK(discover_codex(o,fs).executable==nested); // nested wins over an older hoisted package
    files.erase(native);CHECK(discover_codex(o,fs).executable==nested); // normal unhoisted global npm
    files[nested]="//server/share/codex.exe";executables.insert(files[nested]);CHECK(discover_codex(o,fs).executable.empty());
    files[nested]="C:/repo/codex.exe";executables.insert(files[nested]);CHECK(discover_codex(o,fs).executable.empty());
    files[nested]=nested;markers.insert(o.npm_node_modules[0]+"/@openai/codex/.git");CHECK(discover_codex(o,fs).executable.empty());markers.clear();
    AgentDiscoveryOptions mac;mac.platform=AgentDiscoveryPlatform::macos;mac.arm64=true;
    mac.native_candidates={"/Users/A/.local/bin/codex"};mac.trusted_roots={"/Users/A/.local/bin","/Users/A/.codex/packages/standalone"};
    const auto standalone=std::string("/Users/A/.codex/packages/standalone/releases/0.165.0/bin/codex");
    files[mac.native_candidates[0]]=standalone;executables.insert(standalone);CHECK(discover_codex(mac,fs).executable==standalone);
    markers.insert("/Users/A/.codex/.git");CHECK(discover_codex(mac,fs).executable.empty());markers.clear();
    mac.native_candidates={"/Users/A/.local/bin/codex"};files[mac.native_candidates[0]]="/Users/A/project/codex";executables.insert(files[mac.native_candidates[0]]);CHECK(discover_codex(mac,fs).executable.empty());
    mac.native_candidates.clear();mac.npm_node_modules={"/usr/local/lib/node_modules"};mac.trusted_roots={"/usr/local"};
    const std::string legacy="/usr/local/lib/node_modules/@openai/codex/vendor/aarch64-apple-darwin/codex/codex";
    files[legacy]=legacy;executables.insert(legacy);CHECK(discover_codex(mac,fs).executable==legacy);
    const std::string nested_mac="/usr/local/lib/node_modules/@openai/codex/node_modules/@openai/codex-darwin-arm64/vendor/aarch64-apple-darwin/bin/codex";
    files[nested_mac]=nested_mac;executables.insert(nested_mac);CHECK(discover_codex(mac,fs).executable==nested_mac);

});}
