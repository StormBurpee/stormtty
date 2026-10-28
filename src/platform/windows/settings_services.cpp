#include "stormtty/settings_services.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace stormtty {
namespace {
constexpr std::size_t max_config_bytes = 1024 * 1024;
constexpr std::size_t max_path_chars = 32700;
constexpr unsigned max_path_components = 128;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) noexcept : value_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE); }
        return *this;
    }
    explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
    HANDLE get() const noexcept { return value_; }
    void reset() noexcept { if (*this) CloseHandle(value_); value_ = INVALID_HANDLE_VALUE; }
private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};
struct RegistryKey {
    HKEY value{};
    ~RegistryKey() { if (value) RegCloseKey(value); }
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
};
struct LocalAllocation {
    PSECURITY_DESCRIPTOR value{};
    ~LocalAllocation() { if (value) LocalFree(value); }
};
std::runtime_error io_error(const char* action, DWORD code = GetLastError()) {
    // Do not echo file contents, environment values, or untrusted paths into UI.
    return std::runtime_error(std::string(action) + " (Windows error " + std::to_string(code) + ").");
}
bool absent(DWORD code) noexcept { return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND; }
std::uint64_t time_value(FILETIME value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
bool reserved_name(std::wstring name) {
    const auto dot = name.find(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL" || name == L"CLOCK$" ||
           name == L"CONIN$" || name == L"CONOUT$" ||
           (name.size() == 4 && (name.starts_with(L"COM") || name.starts_with(L"LPT")) &&
            ((name[3] >= L'0' && name[3] <= L'9') || name[3] == L'\u00b9' || name[3] == L'\u00b2' || name[3] == L'\u00b3'));
}
// Restrict persistence to ordinary local drive paths. No device namespaces,
// current-directory dependence, ADS, relative traversal or Win32 alias names.
std::filesystem::path checked_path(const std::filesystem::path& input) {
    auto value = input.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.size() < 4 || value.size() > max_path_chars ||
        !((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) ||
        value[1] != L':' || value[2] != L'\\')
        throw std::runtime_error("Settings require an absolute local drive path; network and device paths are not supported.");
    unsigned count{};
    for (std::size_t begin = 3; begin <= value.size();) {
        const auto end = value.find(L'\\', begin);
        const auto part = value.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ' ||
            reserved_name(part) || ++count > max_path_components ||
            std::any_of(part.begin(), part.end(), [](wchar_t c) { return c < 32 || std::wstring_view(L"<>:\"|?*").find(c) != std::wstring_view::npos; }))
            throw std::runtime_error("Settings path contains an unsupported or ambiguous component.");
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    if (GetDriveTypeW(value.substr(0, 3).c_str()) == DRIVE_REMOTE)
        throw std::runtime_error("Settings on network drives are not supported for atomic saving.");
    return std::filesystem::path(std::move(value));
}
std::wstring native_path(const std::filesystem::path& path) { return L"\\\\?\\" + path.wstring(); }

// Keep every directory handle open without FILE_SHARE_DELETE, preventing an
// ordinary rename/junction substitution of any validated ancestor during I/O.
struct DirectoryGuard {
    std::vector<Handle> handles;
    bool complete{true};
};
DirectoryGuard guard_directories(const std::filesystem::path& path, bool create) {
    DirectoryGuard guard;
    auto part = path.root_path();
    std::vector<std::filesystem::path> parents{part};
    for (const auto& component : path.parent_path().relative_path()) { part /= component; parents.push_back(part); }
    for (const auto& parent : parents) {
        const auto name = native_path(parent);
        auto open = [&] {
            return Handle(CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        };
        auto handle = open();
        if (!handle && absent(GetLastError())) {
            if (!create) { guard.complete = false; return guard; }
            if (!CreateDirectoryW(name.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                throw io_error("Could not create the settings directory");
            handle = open();
        }
        if (!handle) throw io_error("Could not safely open the settings directory");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.get(), &info)) throw io_error("Could not inspect the settings directory");
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Settings directories must not be symbolic links, junctions, or other reparse points.");
        guard.handles.push_back(std::move(handle));
    }
    return guard;
}
void inspect_file(HANDLE handle, ConfigFileSnapshot& token) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) throw io_error("Could not inspect the settings file");
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1)
        throw std::runtime_error("Settings must be a regular file, not a symbolic link, reparse point, or hard-linked file.");
    const auto size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size > max_config_bytes) throw std::runtime_error("Settings file exceeds the 1 MiB limit.");
    token.existed = true;
    token.file_id = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    token.volume_id = info.dwVolumeSerialNumber;
    token.modified_time = time_value(info.ftLastWriteTime);
    token.bytes.resize(static_cast<std::size_t>(size));
}
struct OpenSnapshot { ConfigFileSnapshot token; Handle handle; };
OpenSnapshot read_snapshot(const std::filesystem::path& path) {
    OpenSnapshot result;
    result.token.path = path;
    result.handle = Handle(CreateFileW(native_path(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!result.handle) {
        if (absent(GetLastError())) return result;
        throw io_error("Could not read the settings file (it may be in use)");
    }
    inspect_file(result.handle.get(), result.token);
    std::size_t done{};
    while (done < result.token.bytes.size()) {
        DWORD read{};
        const DWORD wanted = static_cast<DWORD>(result.token.bytes.size() - done);
        if (!ReadFile(result.handle.get(), result.token.bytes.data() + done, wanted, &read, nullptr))
            throw io_error("Could not read the complete settings file");
        if (!read) throw std::runtime_error("Settings changed while being read; reload and try again.");
        done += read;
    }
    // Size, identity and timestamp must remain stable for the owned byte image.
    auto after = result.token;
    inspect_file(result.handle.get(), after);
    if (after != result.token) throw std::runtime_error("Settings changed while being read; reload and try again.");
    return result;
}
struct TemporaryFile {
    Handle handle;
    bool committed{};
    ~TemporaryFile() {
        if (handle && !committed) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            // Delete by owned handle, never by a path which another process can
            // substitute. We own DELETE access and denied other write/delete opens.
            SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
};
void preserve_dacl(HANDLE original, HANDLE temporary) {
    PACL dacl{};
    LocalAllocation descriptor;
    const auto read = GetSecurityInfo(original, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, &dacl, nullptr, &descriptor.value);
    if (read != ERROR_SUCCESS) throw io_error("Could not preserve settings file permissions", read);
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision{};
    if (!GetSecurityDescriptorControl(descriptor.value, &control, &revision)) throw io_error("Could not inspect settings permissions");
    const SECURITY_INFORMATION flags = DACL_SECURITY_INFORMATION |
        ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION);
    const auto written = SetSecurityInfo(temporary, SE_FILE_OBJECT, flags, nullptr, nullptr, dacl, nullptr);
    if (written != ERROR_SUCCESS) throw io_error("Could not preserve settings file permissions", written);
}
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
    try { return checked_path(std::filesystem::path(text) / L"placeholder").parent_path(); } catch (...) { return {}; }
}
std::optional<std::filesystem::path> known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw{};
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &raw))) return {};
    // Release even if constructing the owned path throws.
    struct CoString { PWSTR value; ~CoString() { CoTaskMemFree(value); } } owned{raw};
    return std::filesystem::path(raw);
}
bool installed_executable(const std::filesystem::path& candidate) {
    try {
        const auto path = checked_path(candidate);
        // Read-only bounded probing, not a recursive scan or PATH search. Reject
        // reparse-point executables; process launch still performs its own checks.
        const auto attrs = GetFileAttributesW(native_path(path).c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    } catch (...) { return false; }
}
void add_first(ShellDiscoveryResult& result, const char* id, const char* label,
               const std::vector<std::filesystem::path>& candidates, bool bash = false) {
    for (const auto& candidate : candidates) {
        if (!installed_executable(candidate)) continue;
        Profile profile;
        profile.id = id; profile.label = label; profile.executable = utf8(candidate.wstring());
        if (profile.executable.empty() || profile.executable.size() > 4096) continue;
        if (bash) { profile.arguments = {"--login", "-i"}; profile.environment.emplace("CHERE_INVOKING", "1"); }
        else if (std::string_view(id) != "windows-cmd") profile.arguments = {"-NoLogo"};
        // Blank cwd inherits the host-selected project/current directory.
        result.profiles.push_back(std::move(profile));
        return;
    }
}
}

ShellDiscoveryResult discover_windows_shells() {
    ShellDiscoveryResult result;
    try {
        std::vector<wchar_t> system(max_path_chars + 1);
        const auto count = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        if (count && count < system.size()) {
            const std::filesystem::path root(std::wstring(system.data(), count));
            add_first(result, "windows-cmd", "Command Prompt", {root / L"cmd.exe"});
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
            for (const auto* release : releases)
                if (auto directory = registry_directory(HKEY_LOCAL_MACHINE, release, L"InstallLocation", view)) powershell.push_back(*directory / L"pwsh.exe");
            for (const auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
                if (auto directory = registry_directory(root, L"SOFTWARE\\GitForWindows", L"InstallPath", view)) git.push_back(*directory / L"bin" / L"bash.exe");
        }
        for (const auto* id : {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86}) {
            if (auto directory = known_folder(*id)) {
                powershell.push_back(*directory / L"PowerShell" / L"7" / L"pwsh.exe");
                git.push_back(*directory / L"Git" / L"bin" / L"bash.exe");
            }
        }
        if (auto directory = known_folder(FOLDERID_LocalAppData)) git.push_back(*directory / L"Programs" / L"Git" / L"bin" / L"bash.exe");
        add_first(result, "powershell-7", "PowerShell 7", powershell);
        add_first(result, "git-bash", "Git Bash", git, true);
    } catch (const std::exception&) {
        result.warnings.emplace_back("Some installed shells could not be checked. Existing profiles are unchanged.");
    }
    return result;
}

SettingsSnapshotResult load_settings_snapshot(const std::filesystem::path& input) {
    SettingsSnapshotResult result;
    try {
        const auto path = checked_path(input);
        const auto directories = guard_directories(path, false);
        // Do not traverse an ancestor that did not exist when guarded: another
        // process could create a junction there before an unguarded file open.
        OpenSnapshot opened;
        opened.token.path = path;
        if (directories.complete) opened = read_snapshot(path);
        if (opened.token.existed) {
            auto parsed = parse_config(opened.token.bytes, Platform::windows);
            if (!parsed) {
                const auto& error = parsed.errors.front();
                result.error = "Settings are invalid at line " + std::to_string(error.line) + ": " + error.message;
                return result;
            }
            result.config = std::move(parsed.config);
        } else result.config = default_config(Platform::windows);
        result.snapshot = std::move(opened.token);
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}

ConfigSaveResult save_settings_config(const std::filesystem::path& input, const Config& config, const ConfigFileSnapshot& loaded) {
    ConfigSaveResult result;
    try {
        if (auto status = validate_config(config, Platform::windows); !status) { result.error = std::move(status.error); return result; }
        const auto bytes = serialize_config(config, Platform::windows);
        const auto roundtrip = parse_config(bytes, Platform::windows);
        if (bytes.size() > max_config_bytes || !roundtrip || serialize_config(*roundtrip.config, Platform::windows) != bytes)
            throw std::runtime_error("Settings could not be encoded as valid bounded UTF-8 configuration.");
        const auto path = checked_path(input);
        if (loaded.path != path || (!loaded.existed && (!loaded.bytes.empty() || loaded.file_id || loaded.volume_id || loaded.modified_time)))
            throw std::runtime_error("Settings snapshot does not match the selected configuration path. Reload settings.");
        static std::mutex save_mutex;
        const std::lock_guard serialize_saves(save_mutex);
        const auto directories = guard_directories(path, true);
        auto current = read_snapshot(path);
        const auto conflict = [&] {
            result.conflict = true;
            result.error = "Settings changed on disk. Nothing was saved; close and reopen Settings to review the new file.";
        };
        if (current.token != loaded) { conflict(); return result; }
        if (current.handle) {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(current.handle.get(), &info)) throw io_error("Could not inspect settings before saving");
            if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) throw std::runtime_error("Settings file is read-only. Nothing was saved.");
            if (info.dwFileAttributes & FILE_ATTRIBUTE_ENCRYPTED)
                throw std::runtime_error("Encrypted settings files require manual editing to preserve encryption. Nothing was saved.");
        }
        TemporaryFile temporary;
        static std::atomic_uint64_t sequence{};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            const auto filename = L".stormtty-save-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence) + L".tmp";
            const auto candidate = path.parent_path() / filename;
            temporary.handle = Handle(CreateFileW(native_path(candidate).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE | WRITE_DAC,
                FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr));
            if (temporary.handle) break;
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) throw io_error("Could not create the temporary settings file");
        }
        if (!temporary.handle) throw std::runtime_error("Could not reserve a unique temporary settings file.");
        if (current.handle) preserve_dacl(current.handle.get(), temporary.handle.get());
        std::size_t done{};
        while (done < bytes.size()) {
            DWORD written{};
            if (!WriteFile(temporary.handle.get(), bytes.data() + done, static_cast<DWORD>(bytes.size() - done), &written, nullptr))
                throw io_error("Could not write settings; the original file is unchanged");
            if (!written) throw std::runtime_error("Could not complete the settings write; the original file is unchanged.");
            done += written;
        }
        FILETIME modified{};
        GetSystemTimeAsFileTime(&modified);
        if (!SetFileTime(temporary.handle.get(), nullptr, nullptr, &modified) || !FlushFileBuffers(temporary.handle.get()))
            throw io_error("Could not flush settings; the original file is unchanged");
        ConfigFileSnapshot next;
        next.path = path;
        inspect_file(temporary.handle.get(), next);
        next.bytes = bytes;
        // Allocate everything that could throw BEFORE commit. Rename-by-handle
        // keeps the staged file exclusively owned and has a single commit point;
        // unlike ReplaceFileW, it has no documented partial-rename failure mode.
        const auto destination = native_path(path);
        const auto length = destination.size() * sizeof(wchar_t);
        std::vector<std::byte> rename_storage(sizeof(FILE_RENAME_INFO) + length);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(rename_storage.data());
        rename->ReplaceIfExists = loaded.existed ? TRUE : FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(length);
        std::memcpy(rename->FileName, destination.data(), length);
        auto recheck = read_snapshot(path);
        if (recheck.token != loaded) { conflict(); return result; }
        // A Win32 rename is not compare-and-swap: the final identity/byte check
        // detects ordinary external edits, not a hostile racing rename. See docs.
        recheck.handle.reset();
        current.handle.reset();
        result.snapshot.emplace(std::move(next));
        if (!SetFileInformationByHandle(temporary.handle.get(), FileRenameInfo, rename, static_cast<DWORD>(rename_storage.size()))) {
            const auto error = GetLastError();
            result.snapshot.reset();
            if (!loaded.existed && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)) { conflict(); return result; }
            throw io_error("Could not atomically replace settings; the original file is unchanged", error);
        }
        temporary.committed = true;
        // No fallible I/O or allocations after commit. The returned owned token
        // describes this file; any later external edit will conflict next time.
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
} // namespace stormtty

#else
namespace stormtty {
ShellDiscoveryResult discover_windows_shells() { return {{}, {"Windows shell discovery is unavailable on this platform."}}; }
SettingsSnapshotResult load_settings_snapshot(const std::filesystem::path&) { return {{}, {}, "Windows settings persistence is unavailable on this platform."}; }
ConfigSaveResult save_settings_config(const std::filesystem::path&, const Config&, const ConfigFileSnapshot&) {
    return {{}, "Windows settings persistence is unavailable on this platform.", false};
}
} // namespace stormtty
#endif
