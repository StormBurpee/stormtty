#pragma once
// Windows host policy only. The process transport still validates every byte
// and verifies the actual console host before launching the user's shell.
#ifdef _WIN32
#include <windows.h>
#include "stormtty/config.hpp"
#include "stormtty/process.hpp"
#include <filesystem>
#include <stdexcept>

namespace stormtty {
inline bool native_amd64_console_supported() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    return info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
#else
    return false;
#endif
}
inline std::filesystem::path bundled_console_runtime_directory() {
    std::wstring module(32768,L'\0');
    const DWORD count=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    if (!count || count>=module.size()) throw std::runtime_error("Could not locate the app-owned console runtime.");
    module.resize(count);
    return std::filesystem::path(module).parent_path()/L"runtimes"/L"conpty"/L"1.25.260930003"/L"x64";
}
inline bool bundled_console_runtime_present() noexcept {
    if (!native_amd64_console_supported()) return false;
    try {
        const auto root=bundled_console_runtime_directory();
        return std::filesystem::is_regular_file(root/L"conpty.dll") &&
            std::filesystem::is_regular_file(root/L"OpenConsole.exe");
    } catch (...) { return false; }
}
inline void select_windows_console_provider(ProcessOptions& process, WindowsConsoleBackend choice,
                                            const std::filesystem::path& cli_override={}) {
    process.provider_directory.clear();
    if (cli_override.empty() && choice==WindowsConsoleBackend::system) {
        process.provider=ProcessProvider::windows_inbox_basic;
        return;
    }
    process.provider=ProcessProvider::windows_packaged_experimental;
    // Even unavailable runtimes reach the async Process failure path: keep the
    // window and Settings alive so the user can explicitly return to System.
    const auto path=(cli_override.empty()?bundled_console_runtime_directory():cli_override).wstring();
    const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data(),static_cast<int>(path.size()),nullptr,0,nullptr,nullptr);
    if (!count) throw std::runtime_error("Could not encode the console runtime path.");
    process.provider_directory.resize(static_cast<std::size_t>(count));
    if (WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data(),static_cast<int>(path.size()),process.provider_directory.data(),count,nullptr,nullptr)!=count)
        throw std::runtime_error("Could not encode the console runtime path.");
}
} // namespace stormtty
#endif
