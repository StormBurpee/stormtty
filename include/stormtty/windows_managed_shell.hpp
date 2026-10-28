#pragma once
// Shared by the Windows host and the native path-qualification probe.
#ifdef _WIN32
#include <windows.h>
#include "stormtty/process.hpp"
#include <cwchar>
#include <stdexcept>
#include <utility>

namespace stormtty {
inline std::vector<ProcessEnvironmentEntry> managed_shell_home_environment() {
    // One wide snapshot: GetEnvironmentVariableA in the runtime's early passwd
    // lookup can irreversibly lose Unicode before its UTF-8 environment exists.
    // Do not use getenv, mutate the parent, or treat HOME="" as absent.
    struct Snapshot {
        LPWCH value{GetEnvironmentStringsW()};
        ~Snapshot() { if (value) FreeEnvironmentStringsW(value); }
    } snapshot;
    if (!snapshot.value) throw std::runtime_error("Could not read managed shell environment.");
    const auto read=[&](std::wstring_view name)->std::optional<std::wstring> {
        for (auto* entry=snapshot.value; *entry; entry+=std::wcslen(entry)+1) {
            const auto* equal=std::wcschr(entry,L'=');
            if (!equal || equal-entry!=static_cast<std::ptrdiff_t>(name.size())) continue;
            if (CompareStringOrdinal(entry,static_cast<int>(name.size()),name.data(),
                                     static_cast<int>(name.size()),TRUE)==CSTR_EQUAL)
                return std::wstring(equal+1);
        }
        return std::nullopt;
    };
    const auto home=read(L"HOME");
    std::vector<ProcessEnvironmentEntry> result{{"STORMTTY_EMPTY_HOME",home && home->empty()?"1":""}};
    if (home) return result; // Explicit Windows/POSIX/UNC/empty HOME is authoritative.
    std::optional<std::wstring> fallback;
    const auto drive=read(L"HOMEDRIVE"), path=read(L"HOMEPATH");
    if (drive && !drive->empty() && path && !path->empty()) fallback=*drive+*path;
    else if (const auto profile=read(L"USERPROFILE"); profile && !profile->empty()) fallback=*profile;
    if (!fallback) return result; // Leave account-database fallback to nsswitch.
    const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,fallback->data(),
        static_cast<int>(fallback->size()),nullptr,0,nullptr,nullptr);
    if (size<=0) throw std::runtime_error("Invalid UTF-16 managed shell home.");
    std::string utf8(static_cast<std::size_t>(size),'\0');
    if (WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,fallback->data(),
        static_cast<int>(fallback->size()),utf8.data(),size,nullptr,nullptr)!=size)
        throw std::runtime_error("Could not encode managed shell home.");
    // The process transport creates a Unicode Windows environment block; MSYS2
    // then performs its ordinary Windows-to-POSIX conversion. No path rewriting.
    result.push_back({"HOME",std::move(utf8)});
    return result;
}
} // namespace stormtty
#endif
