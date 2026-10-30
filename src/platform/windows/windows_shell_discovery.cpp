#include "stormtty/settings_services.hpp"
#include "shell_discovery_policy.hpp"
#include "windows_local_path.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

namespace stormtty {
namespace {
using windows_local_path::checked_path;
using windows_local_path::native_path;
using windows_local_path::max_path_chars;

struct RegistryKey {
    HKEY value{};
    ~RegistryKey() { if (value) RegCloseKey(value); }
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
};

std::string utf8(std::wstring_view value) {
    if (value.empty() || value.size() > max_path_chars) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr)) return {};
    return result;
}

std::optional<std::filesystem::path> registry_directory(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view) {
    RegistryKey opened;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &opened.value) != ERROR_SUCCESS) return {};
    DWORD type{}, count{};
    if (RegQueryValueExW(opened.value, value, nullptr, &type, nullptr, &count) != ERROR_SUCCESS ||
        type != REG_SZ || count < sizeof(wchar_t) || count > (max_path_chars + 1) * sizeof(wchar_t) || count % sizeof(wchar_t)) return {};
    std::wstring text(count / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(opened.value, value, nullptr, &type, reinterpret_cast<BYTE*>(text.data()), &count) != ERROR_SUCCESS || type != REG_SZ) return {};
    const auto end = text.find(L'\0');
    if (end == std::wstring::npos || end == 0 || end + 1 != count / sizeof(wchar_t)) return {};
    text.resize(end);
    while (!text.empty() && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    if (text.empty()) return {};
    try {
        return checked_path(std::filesystem::path(text) / L"placeholder").parent_path();
    } catch (...) {
        return {};
    }
}

std::optional<std::filesystem::path> known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw{};
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &raw))) return {};
    // Release even if constructing the owned path throws.
    struct CoString {
        PWSTR value;
        ~CoString() { CoTaskMemFree(value); }
    } owned{raw};
    return std::filesystem::path(raw);
}

bool installed_executable(const std::filesystem::path& candidate) {
    try {
        const auto path = checked_path(candidate);
        // Read-only bounded probing, not a recursive scan or PATH search. Reject
        // reparse-point executables; process launch still performs its own checks.
        const auto attrs = GetFileAttributesW(native_path(path).c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    } catch (...) {
        return false;
    }
}

void add_first(ShellDiscoveryResult& result, const char* id, const char* label,
               const std::vector<std::filesystem::path>& candidates, bool bash = false) {
    shell_discovery_policy::add_first(result, id, label, candidates, bash,
        [](const std::filesystem::path& candidate) {
            return installed_executable(candidate) ? utf8(candidate.wstring()) : std::string{};
        });
}

} // namespace

ShellDiscoveryResult discover_windows_shells(std::span<const Profile> configured) {
    ShellDiscoveryResult result;
    try {
        std::vector<wchar_t> system(max_path_chars + 1);
        const auto count = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        if (count && count < system.size()) {
            const std::filesystem::path root(std::wstring(system.data(), count));
            add_first(result, "windows-cmd", "Command Prompt", {root / L"cmd.exe"});
            // This candidate was just created from GetSystemDirectoryW, not an
            // arbitrary caller's Profile ID or basename. Match the host's exact
            // builtin alias policy, including /d injection, before hiding it.
            shell_discovery_policy::hide_represented_cmd(result, configured);
            add_first(result, "windows-powershell", "Windows PowerShell", {root / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe"});
        }
        std::vector<std::filesystem::path> powershell, git;
        // Fixed stable MSI registration keys only; do not enumerate arbitrary
        // uninstall entries, preview installs, PATH, App Paths, or user cwd.
        constexpr const wchar_t* releases[] = {
            L"SOFTWARE\\Microsoft\\PowerShellCore\\InstalledVersions\\31ab5147-9a97-4452-8443-d9709f0516e1",
            L"SOFTWARE\\Microsoft\\PowerShellCore\\InstalledVersions\\1d00683b-0f84-4db8-a64f-2f98ad42fe06"
        };
        for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            for (const auto* release : releases) {
                if (auto directory = registry_directory(HKEY_LOCAL_MACHINE, release, L"InstallLocation", view)) {
                    powershell.push_back(*directory / L"pwsh.exe");
                }
            }
            for (const auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
                if (auto directory = registry_directory(root, L"SOFTWARE\\GitForWindows", L"InstallPath", view)) {
                    git.push_back(*directory / L"bin" / L"bash.exe");
                }
            }
        }
        for (const auto* id : {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86}) {
            if (auto directory = known_folder(*id)) {
                powershell.push_back(*directory / L"PowerShell" / L"7" / L"pwsh.exe");
                git.push_back(*directory / L"Git" / L"bin" / L"bash.exe");
            }
        }
        if (auto directory = known_folder(FOLDERID_LocalAppData)) {
            git.push_back(*directory / L"Programs" / L"Git" / L"bin" / L"bash.exe");
        }
        add_first(result, "powershell-7", "PowerShell 7", powershell);
        add_first(result, "git-bash", "Git Bash", git, true);
    } catch (const std::exception&) {
        result.warnings.emplace_back("Some installed shells could not be checked. Existing profiles are unchanged.");
    }
    return result;
}

} // namespace stormtty
#else
namespace stormtty {
ShellDiscoveryResult discover_windows_shells(std::span<const Profile>) {
    return {{}, {"Windows shell discovery is unavailable on this platform."}};
}
} // namespace stormtty
#endif
