#pragma once
// Shared verifier. Production callers must use the build-anchored entry point.
#include "stormtty/windows_shell_state.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <utility>
namespace completion_seed {
namespace fs = std::filesystem;
inline void check(bool value) { if (!value) throw std::runtime_error("seed rejected"); }
struct Handle {
    HANDLE h{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE v):h(v){}
    Handle(Handle&& other) noexcept:h(std::exchange(other.h, INVALID_HANDLE_VALUE)){}
    Handle(const Handle&)=delete;
    ~Handle(){if(h!=INVALID_HANDLE_VALUE) CloseHandle(h);}
};
inline std::string utf8(const fs::path& p) {const auto s=p.generic_u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
struct VerificationDiagnostic {unsigned stage{}, object{}, win32_error{};};
inline Handle open(const fs::path& p, bool directory, VerificationDiagnostic* diagnostic=nullptr) {
    Handle h(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if(h.h==INVALID_HANDLE_VALUE && diagnostic)diagnostic->win32_error=GetLastError();
    check(h.h!=INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION info{};
    check(GetFileInformationByHandle(h.h,&info) && GetFileType(h.h)==FILE_TYPE_DISK);
    check(!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT));
    check(static_cast<bool>(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory);
    check(directory || info.nNumberOfLinks==1);
    return h;
}
inline std::string read(const fs::path& p, std::size_t limit, VerificationDiagnostic* diagnostic=nullptr) {
    auto handle=open(p,false,diagnostic);
    LARGE_INTEGER size{};check(GetFileSizeEx(handle.h,&size) && size.QuadPart>=0 && static_cast<unsigned long long>(size.QuadPart)<=limit);
    std::string s(static_cast<std::size_t>(size.QuadPart),'\0'); DWORD count{};
    check(ReadFile(handle.h,s.data(),static_cast<DWORD>(s.size()),&count,nullptr) && count==s.size());return s;
}
inline fs::path path(std::string_view s) {return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()),s.size()));}
inline bool hex(std::string_view s) {return s.size()==64 && s.find_first_not_of("0123456789abcdef")==s.npos;}
struct Algorithm {
    BCRYPT_ALG_HANDLE h{};
    Algorithm(){check(BCryptOpenAlgorithmProvider(&h,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0);}
    ~Algorithm(){if(h) BCryptCloseAlgorithmProvider(h,0);}
};
inline std::string hash(HANDLE file, Algorithm& algorithm, unsigned long long& total) {
    struct Hash {BCRYPT_HASH_HANDLE h{};~Hash(){if(h) BCryptDestroyHash(h);}} hash;
    check(BCryptCreateHash(algorithm.h,&hash.h,nullptr,0,nullptr,0,0)>=0);
    unsigned char buffer[65536];DWORD count{};
    while(true){check(ReadFile(file,buffer,sizeof(buffer),&count,nullptr));if(!count)break;
        total+=count;check(total<=1024ull*1024*1024);check(BCryptHashData(hash.h,buffer,count,0)>=0);}
    unsigned char digest[32];check(BCryptFinishHash(hash.h,digest,sizeof(digest),0)>=0);
    std::string result;for(auto c:digest){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;
}
inline std::string hash_bytes(std::string_view bytes, Algorithm& algorithm) {
    struct Hash {BCRYPT_HASH_HANDLE h{};~Hash(){if(h) BCryptDestroyHash(h);}} hash;
    check(bytes.size()<=8*1024*1024 && BCryptCreateHash(algorithm.h,&hash.h,nullptr,0,nullptr,0,0)>=0);
    check(BCryptHashData(hash.h,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),static_cast<ULONG>(bytes.size()),0)>=0);
    unsigned char digest[32];check(BCryptFinishHash(hash.h,digest,sizeof(digest),0)>=0);
    std::string result;for(auto c:digest){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;
}
inline std::string posix(const fs::path& p) {
    auto s=utf8(fs::absolute(p));check(s.size()>3 && s[1]==':' && s[2]=='/' && s.find_first_of("\r\n\t")==s.npos);
    char drive=s[0];if(drive>='A'&&drive<='Z')drive=static_cast<char>(drive-'A'+'a');
    return std::string("/")+drive+s.substr(2);
}
inline bool adopt(const fs::path& runtime, const fs::path& package, const fs::path& local,
                  const fs::path& state, std::string_view expected_identity = {}, VerificationDiagnostic* diagnostic=nullptr) noexcept {
    const auto stage=[&](unsigned value){if(diagnostic)diagnostic->stage=value;};
    const auto object=[&](unsigned value){if(diagnostic)diagnostic->object=value;};
    try {
        // Hold each runtime ancestor against replacement, then every inventoried
        // object against writes/deletion while validating and publishing.
        stage(1);std::function<bool(std::uintptr_t,bool)> access;
        if(!expected_identity.empty()) {
            check(hex(expected_identity));
            access=stormtty::managed_runtime_access_check();
        }
        const auto inspect=[&](HANDLE handle,bool ancestor){
            if(access)check(access(reinterpret_cast<std::uintptr_t>(handle),ancestor));
        };
        std::vector<Handle> guards;
        stage(2);auto ancestor=fs::absolute(runtime).root_path();guards.push_back(open(ancestor,true,diagnostic));stage(3);inspect(guards.back().h,true);
        unsigned index{};
        for(const auto& part:fs::absolute(runtime).relative_path()){
            object(++index);stage(4);ancestor/=part;guards.push_back(open(ancestor,true,diagnostic));stage(5);inspect(guards.back().h,ancestor!=fs::absolute(runtime));
        }
        object(0);stage(6);const auto identity=read(package/L"identity.txt",8*1024*1024,diagnostic);
        stage(7);Algorithm algorithm;
        if(!expected_identity.empty())check(hex(expected_identity) && hash_bytes(identity,algorithm)==expected_identity);
        stage(8);std::vector<std::string> lines;std::size_t offset{};
        while(offset<identity.size()){const auto end=identity.find('\n',offset);check(end!=identity.npos);lines.push_back(identity.substr(offset,end-offset));offset=end+1;}
        check(lines.size()>6 && lines[0]=="STORMTTY_SEED_V1");
        const auto version=lines[1];check(!version.empty() && version.size()<32 && version.find_first_not_of("0123456789.")==version.npos);
        check(lines[2].size()==40 && lines[2].find_first_not_of("0123456789abcdef")==lines[2].npos && hex(lines[3]));
        check(!lines[4].empty() && lines[4].find_first_not_of("0123456789")==lines[4].npos);
        const auto path_count=std::stoul(lines[4]);check(path_count>0 && path_count<=256 && lines.size()>5+path_count);
        std::string fpath;unsigned caches{};
        for(std::size_t i=5;i<5+path_count;++i){const auto& token=lines[i];std::string path;
            if(token=="@C"){++caches;path=posix(state)+"/cache/completions";}
            else if(token.starts_with("@R/"))path=posix(runtime)+token.substr(2);
            else if(token.starts_with("@U/usr/"))path=token.substr(2);
            else check(false);
            check(path.find_first_of("\r\n\t")==path.npos && path.find("/../")==path.npos);
            if(!fpath.empty())fpath+=' ';fpath+=path;
        }
        check(caches==1);
        std::map<std::string,std::string> expected;
        for(std::size_t i=5+path_count;i<lines.size();++i){const auto& row=lines[i];std::string name,digest;
            if(row.starts_with("D\t")){name=row.substr(2);digest="D";}
            else {check(row.size()>67 && row.starts_with("F\t") && row[66]=='\t');digest=row.substr(2,64);check(hex(digest));name=row.substr(67);}
            check(!name.empty() && name[0]!='/' && name.find_first_of("\\:\r\n\t")==name.npos);
            for(const auto& component:path(name))check(component!=".." && component!=".");
            check(expected.emplace(name,digest).second);
        }
        check(expected.size()<=65536);
        unsigned long long total{};std::set<std::string> actual;
        index=0;
        for(const auto& entry:fs::recursive_directory_iterator(runtime)){
            object(++index);stage(9);
            const auto name=utf8(entry.path().lexically_relative(runtime));auto found=expected.find(name);check(found!=expected.end());
            const bool directory=found->second=="D";auto handle=open(entry.path(),directory,diagnostic);
            // Validate each directory BEFORE iterator increment descends into it.
            stage(10);inspect(handle.h,false);
            stage(11);
            if(!directory)check(hash(handle.h,algorithm,total)==found->second);
            guards.push_back(std::move(handle));actual.insert(name);
        }
        stage(12);check(actual.size()==expected.size());
        object(0);stage(13);
        // Independently reconstruct the managed pre-compinit order. An identity
        // with reordered paths is not accepted merely because files match.
        std::vector<std::string> ordered{
            "@R/stormtty/vendor/oh-my-zsh/functions", "@R/stormtty/vendor/oh-my-zsh/completions",
            "@R/stormtty/vendor/oh-my-zsh/custom/functions", "@R/stormtty/vendor/oh-my-zsh/custom/completions", "@C"};
        for(const auto& base: {std::string("usr/share/zsh/functions"), "usr/share/zsh/"+version+"/functions"}) {
            std::vector<std::string> children;
            if(fs::is_directory(runtime/path(base))) for(const auto& entry:fs::recursive_directory_iterator(runtime/path(base))) {
                const auto relative=entry.path().lexically_relative(runtime/path(base));bool hidden{};
                for(const auto& component:relative)if(utf8(component).starts_with("."))hidden=true;
                if(!hidden && entry.is_directory())children.push_back("@U/"+utf8(entry.path().lexically_relative(runtime)));
            }
            std::sort(children.begin(),children.end());ordered.insert(ordered.end(),children.begin(),children.end());
        }
        check(ordered.size()==path_count && std::equal(ordered.begin(),ordered.end(),lines.begin()+5));
        stage(14);auto seed_handle=open(package/L"seed.dump",false,diagnostic);unsigned long long seed_size{};
        stage(15);check(hash(seed_handle.h,algorithm,seed_size)==lines[3] && seed_size<=1024*1024);
        // Read executable text from the SAME locked handle whose hash matched,
        // never reopen a path that a package-ancestor rename could redirect.
        stage(16);LARGE_INTEGER zero{};check(SetFilePointerEx(seed_handle.h,zero,nullptr,FILE_BEGIN));
        std::string text(static_cast<std::size_t>(seed_size),'\0');DWORD read_count{};
        check(ReadFile(seed_handle.h,text.data(),static_cast<DWORD>(text.size()),&read_count,nullptr) && read_count==text.size());
        check(text.starts_with("#files: ") && text.find("version: "+version+"\n")!=text.npos);
        text+="\n#omz revision: "+lines[2]+"\n#omz fpath: "+fpath+"\n";
        // Private-cache contents/existing executable caches are checked under
        // native directory guards by the publisher. Any rejection is fallback.
        stage(17);const bool published=stormtty::publish_managed_completion_seed_at(local,path("zcompdump-"+version).wstring(),text);
        if(published)stage(18);return published;
    } catch(...) {return false;}
}
// The anchor must come from an app-compiled build output, never environment,
// user configuration, or a checksum sidecar adjacent to the package.
inline bool adopt_build_trusted(const fs::path& runtime,const fs::path& package,const fs::path& local,
    const fs::path& state,std::string_view compiled_identity,VerificationDiagnostic* diagnostic=nullptr) noexcept {
    if(!hex(compiled_identity))return false;
    return adopt(runtime,package,local,state,compiled_identity,diagnostic);
}
} // namespace completion_seed
