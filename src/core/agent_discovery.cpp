#include "stormtty/agent_discovery.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif
namespace stormtty {
namespace {
std::string slash(std::string p) { std::replace(p.begin(),p.end(),'\\','/'); return p; }
bool safe(const std::string& p, bool windows) {
    if(p.empty()||p.size()>32768||p.starts_with("//"))return false;
    const std::size_t start=windows?3:1;
    if(windows) {if(p.size()<4||!((p[0]>='A'&&p[0]<='Z')||(p[0]>='a'&&p[0]<='z'))||p[1]!=':'||p[2]!='/')return false;}
    else if(p[0]!='/')return false;
    if(std::any_of(p.begin(),p.end(),[](unsigned char c){return c<32||c==127;}))return false;
    if(p.find_first_of("\"<>|?*",start)!=std::string::npos||p.find(':',start)!=std::string::npos)return false;
    for(auto at=start;at<p.size();) {auto end=p.find('/',at);if(end==std::string::npos)end=p.size();const auto part=p.substr(at,end-at);if(part.empty()||part=="."||part==".."||(windows&&(part.back()=='.'||part.back()==' ')))return false;at=end+1;}
    return true;
}
std::string comparison(std::string s,bool windows) {if(windows)for(auto& c:s)if(c>='A'&&c<='Z')c=static_cast<char>(c+32);return s;}
bool contained(const std::string& p,const std::vector<std::string>& roots,bool windows) {
    const auto v=comparison(p,windows);
    for(auto root:roots){root=slash(root);if(!safe(root,windows))continue;root=comparison(root,windows);if(v.starts_with(root+"/"))return true;}
    return false;
}
bool repository(const std::string& p,const AgentDiscoveryFilesystem& fs) {
    auto parent=p;
    while(true){const auto end=parent.rfind('/');if(end==std::string::npos||end==0)break;parent.resize(end);if(fs.exists(parent+"/.git"))return true;}
    return false;
}
std::string utf8(const std::filesystem::path& p) { const auto s=p.generic_u8string();return {reinterpret_cast<const char*>(s.data()),s.size()}; }
#ifdef _WIN32
std::string folder(REFKNOWNFOLDERID id) {PWSTR raw=nullptr;if(FAILED(SHGetKnownFolderPath(id,KF_FLAG_DONT_VERIFY,nullptr,&raw)))return {};const auto result=utf8(std::filesystem::path(raw));CoTaskMemFree(raw);return result;}
#endif
}
AgentDiscoveryResult discover_codex(const AgentDiscoveryOptions& o,const AgentDiscoveryFilesystem& fs) {
    if(!fs.canonical||!fs.exists||!fs.native_executable)return {{},"Codex discovery is unavailable.",{}};
    const bool windows=o.platform==AgentDiscoveryPlatform::windows;
    const std::string platform=windows?"win32":o.platform==AgentDiscoveryPlatform::macos?"darwin":"linux";
    const std::string triple=std::string(o.arm64?"aarch64":"x86_64")+(windows?"-pc-windows-msvc":o.platform==AgentDiscoveryPlatform::macos?"-apple-darwin":"-unknown-linux-musl");
    const std::string binary=windows?"codex.exe":"codex";
    auto candidates=o.native_candidates;
    for(const auto& raw:o.npm_node_modules) {
        const auto root=slash(raw);if(!safe(root,windows)||!contained(root+"/placeholder",o.trusted_roots,windows))continue;
        const auto platform_package="codex-"+platform+(o.arm64?"-arm64":"-x64");
        // The official wrapper uses createRequire(import.meta.url).resolve(), so
        // global npm can keep the optional platform package inside codex's own
        // node_modules rather than hoisting it to the global node_modules root.
        // Prefer that bounded nested location as Node does; never execute its shim.
        for(const auto& package_root:{root+"/@openai/codex/node_modules/@openai/"+platform_package,
                                      root+"/@openai/"+platform_package,root+"/@openai/codex"}) {
            candidates.push_back(package_root+"/vendor/"+triple+"/bin/"+binary);
            candidates.push_back(package_root+"/vendor/"+triple+"/codex/"+binary);
        }
    }
    if(candidates.size()>128)return {{},"Too many Codex installation locations.",{}};
    for(auto candidate:candidates) {
        candidate=slash(candidate);
        if(!safe(candidate,windows)||!contained(candidate,o.trusted_roots,windows)||repository(candidate,fs))continue;
        auto resolved=slash(fs.canonical(candidate));
        if(!safe(resolved,windows)||!contained(resolved,o.trusted_roots,windows)||repository(resolved,fs))continue;
        if(fs.native_executable(resolved,o.platform))return {resolved,"Installed Codex found. Checking sign-in does not start a task.",resolved.substr(0,resolved.rfind('/'))};
    }
    return {{},"Codex was not found in supported installation locations. Install Codex, then retry, or choose a native executable in Advanced.",{}};
}
AgentDiscoveryResult discover_codex() {
    AgentDiscoveryOptions o;
#if defined(_M_ARM64) || defined(__aarch64__)
    o.arm64=true;
#endif
    std::string home;
#ifdef _WIN32
    o.platform=AgentDiscoveryPlatform::windows;
    home=folder(FOLDERID_Profile);const auto local=folder(FOLDERID_LocalAppData),roaming=folder(FOLDERID_RoamingAppData),programs=folder(FOLDERID_ProgramFiles);
    if(!local.empty()){o.trusted_roots.push_back(local+"/Programs/OpenAI/Codex");o.native_candidates.push_back(local+"/Programs/OpenAI/Codex/bin/codex.exe");}
    if(!roaming.empty()){o.trusted_roots.push_back(roaming+"/npm");o.npm_node_modules.push_back(roaming+"/npm/node_modules");}
    if(!programs.empty()){o.trusted_roots.push_back(programs+"/nodejs");o.npm_node_modules.push_back(programs+"/nodejs/node_modules");}
#else
#ifdef __APPLE__
    o.platform=AgentDiscoveryPlatform::macos;
#else
    o.platform=AgentDiscoveryPlatform::linux;
#endif
    std::array<char,16384> buffer{};passwd pw{},*result=nullptr;
    if(getpwuid_r(getuid(),&pw,buffer.data(),buffer.size(),&result)==0&&result&&pw.pw_dir)home=pw.pw_dir;
    for(const auto* prefix:{"/opt/homebrew","/usr/local"}){o.trusted_roots.push_back(prefix);o.native_candidates.push_back(std::string(prefix)+"/bin/codex");o.npm_node_modules.push_back(std::string(prefix)+"/lib/node_modules");}
    if(!home.empty()){o.trusted_roots.push_back(home+"/.local/bin");o.native_candidates.push_back(home+"/.local/bin/codex");o.trusted_roots.push_back(home+"/.npm-global");o.npm_node_modules.push_back(home+"/.npm-global/lib/node_modules");}
#endif
    if(!home.empty()){const auto root=home+"/.codex/packages/standalone";o.trusted_roots.push_back(root);const auto binary=o.platform==AgentDiscoveryPlatform::windows?"codex.exe":"codex";o.native_candidates.push_back(root+"/current/bin/"+binary);o.native_candidates.push_back(root+"/current/"+binary);}
    AgentDiscoveryFilesystem fs;
    fs.canonical=[](const std::string& p){std::error_code ec;auto result=std::filesystem::canonical(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(p.data()),p.size())),ec);return ec?std::string{}:utf8(result);};
    fs.exists=[](const std::string& p){std::error_code ec;return std::filesystem::exists(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(p.data()),p.size())),ec)||static_cast<bool>(ec);};
    fs.native_executable=[](const std::string& p,AgentDiscoveryPlatform platform){
        const auto path=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(p.data()),p.size()));std::error_code ec;if(!std::filesystem::is_regular_file(path,ec)||ec)return false;
#ifndef _WIN32
        if(access(path.c_str(),X_OK)!=0)return false;
#endif
        std::ifstream file(path,std::ios::binary);std::array<unsigned char,4> bytes{};file.read(reinterpret_cast<char*>(bytes.data()),4);if(file.gcount()!=4)return false;
        if(platform==AgentDiscoveryPlatform::windows)return bytes[0]=='M'&&bytes[1]=='Z';
        if(platform==AgentDiscoveryPlatform::linux)return bytes==std::array<unsigned char,4>{0x7f,'E','L','F'};
        return bytes==std::array<unsigned char,4>{0xcf,0xfa,0xed,0xfe}||bytes==std::array<unsigned char,4>{0xfe,0xed,0xfa,0xcf}||bytes==std::array<unsigned char,4>{0xca,0xfe,0xba,0xbe}||bytes==std::array<unsigned char,4>{0xbe,0xba,0xfe,0xca};
    };
    return discover_codex(o,fs);
}
}
