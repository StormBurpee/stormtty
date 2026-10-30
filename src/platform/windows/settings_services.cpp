#include "stormtty/settings_services.hpp"
#include "windows_local_path.hpp"
#include "stormtty/application_layout_services.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace stormtty {
namespace {
constexpr std::size_t max_config_bytes = 1024 * 1024;
using windows_local_path::checked_path;
using windows_local_path::native_path;

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
OpenSnapshot read_snapshot(const std::filesystem::path& path, bool allow_delete = true) {
    OpenSnapshot result;
    result.token.path = path;
    result.handle = Handle(CreateFileW(native_path(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | (allow_delete ? FILE_SHARE_DELETE : 0), nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
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
} // namespace

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

static ConfigSaveResult save_document_bytes(const std::filesystem::path& input, const std::string& bytes, const ConfigFileSnapshot& loaded, HANDLE security_source = INVALID_HANDLE_VALUE) {
    ConfigSaveResult result;
    try {
        if (bytes.size() > max_config_bytes) throw std::runtime_error("Saved document exceeds 1 MiB.");
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
        else if(security_source!=INVALID_HANDLE_VALUE)preserve_dacl(security_source,temporary.handle.get());
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
ConfigSaveResult save_settings_config(const std::filesystem::path& input, const Config& config, const ConfigFileSnapshot& loaded) {
    try {
        if (auto status = validate_config(config, Platform::windows); !status) return {{}, status.error, false};
        const auto bytes = serialize_config(config, Platform::windows);
        const auto roundtrip = parse_config(bytes, Platform::windows);
        if (!roundtrip || serialize_config(*roundtrip.config, Platform::windows) != bytes)
            return {{}, "Settings could not be encoded as valid configuration.", false};
        return save_document_bytes(input, bytes, loaded);
    } catch (const std::exception& error) { return {{}, error.what(), false}; }
}

WorkspaceLibrarySnapshotResult load_workspace_library_snapshot(const std::filesystem::path& input) {
    WorkspaceLibrarySnapshotResult result;
    try {
        const auto path = checked_path(input);
        const auto directories = guard_directories(path, false);
        OpenSnapshot opened;
        opened.token.path = path;
        if (directories.complete) opened = read_snapshot(path);
        if (opened.token.existed) {
            auto parsed = deserialize_workspace_library(opened.token.bytes);
            if (!parsed) { result.error = parsed.error; return result; }
            result.library = std::move(parsed.value);
        } else result.library.emplace();
        result.snapshot = std::move(opened.token);
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
ConfigSaveResult save_workspace_library(const std::filesystem::path& input, const WorkspaceLibrary& library, const ConfigFileSnapshot& loaded) {
    try {
        auto bytes = serialize_workspace_library(library);
        if (!bytes) return {{}, bytes.error, false};
        return save_document_bytes(input, *bytes.value, loaded);
    } catch (const std::exception& error) { return {{}, error.what(), false}; }
}
ApplicationLayoutSnapshotResult load_application_layout_snapshot(const std::filesystem::path& input) {
    ApplicationLayoutSnapshotResult result;
    try {
        const auto path=checked_path(input);const auto directories=guard_directories(path,false);
        OpenSnapshot opened;opened.token.path=path;
        if(directories.complete)opened=read_snapshot(path);
        if(opened.token.existed) {
            auto parsed=deserialize_application_layout(opened.token.bytes);
            if(!parsed){result.error=parsed.error;return result;}
            result.layout=std::move(parsed.value);
        } else result.layout.emplace();
        result.snapshot=std::move(opened.token);
    } catch(const std::exception& error){result.error=error.what();}
    return result;
}
namespace {
bool equal_dacl(HANDLE first,HANDLE second) {
    PACL a{},b{};LocalAllocation first_descriptor,second_descriptor;
    auto status=GetSecurityInfo(first,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&a,nullptr,&first_descriptor.value);
    if(status!=ERROR_SUCCESS)throw io_error("Could not inspect original layout permissions",status);
    status=GetSecurityInfo(second,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&b,nullptr,&second_descriptor.value);
    if(status!=ERROR_SUCCESS)throw io_error("Could not inspect backup layout permissions",status);
    SECURITY_DESCRIPTOR_CONTROL ac{},bc{};DWORD revision{};
    if(!GetSecurityDescriptorControl(first_descriptor.value,&ac,&revision)||!GetSecurityDescriptorControl(second_descriptor.value,&bc,&revision))
        throw io_error("Could not inspect layout permission inheritance");
    if((ac&SE_DACL_PROTECTED)!=(bc&SE_DACL_PROTECTED))return false;
    if(!a||!b)return a==b;
    return a->AclSize==b->AclSize && std::memcmp(a,b,a->AclSize)==0;
}
}
ConfigSaveResult save_application_layout(const std::filesystem::path& input,const ApplicationLayoutDocument& document,const ConfigFileSnapshot& loaded) {
    try {
        // Preflight first: a valid large v1 file may not fit the envelope. Never
        // create a backup or touch the original if complete encoding fails.
        auto bytes=serialize_application_layout(document);if(!bytes)return {{},bytes.error,false};
        const auto path=checked_path(input);
        if(loaded.path!=path)return {{},"Layout snapshot does not match its path.",false};
        bool legacy=false;
        if(loaded.existed) {
            auto parsed=deserialize_application_layout(loaded.bytes);
            if(!parsed)return {{},"Invalid or unsupported original layout; nothing was saved.",false};
            legacy=parsed.value->requires_legacy_backup;
        }
        if(!legacy)return save_document_bytes(path,*bytes.value,loaded);
        const auto directories=guard_directories(path,false);
        if(!directories.complete)return {{},"Original layout changed before migration.",true};
        auto original=read_snapshot(path);
        if(original.token!=loaded)return {{},"Original layout changed before migration.",true};
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(original.handle.get(),&info))throw io_error("Could not inspect original layout");
        if(info.dwFileAttributes&(FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_ENCRYPTED))
            return {{},"Read-only or encrypted layout requires manual migration; nothing was saved.",false};
        const auto backup_path=checked_path(std::filesystem::path(path.wstring()+L".v1-backup"));
        auto backup=read_snapshot(backup_path,false);
        if(!backup.token.existed) {
            ConfigFileSnapshot absent_backup;absent_backup.path=backup_path;
            auto saved=save_document_bytes(backup_path,loaded.bytes,absent_backup,original.handle.get());
            if(!saved)return {{},"Could not create the legacy layout backup: "+saved.error,saved.conflict};
            backup=read_snapshot(backup_path,false);
        }
        // This handle denies new write/delete opens through the final commit.
        // Existing backup bytes and permissions are never silently overwritten.
        if(!backup.token.existed || backup.token.bytes!=loaded.bytes || !equal_dacl(original.handle.get(),backup.handle.get()))
            return {{},"Existing legacy layout backup differs in bytes or permissions; nothing was saved.",true};
        // Classic FileRenameInfo cannot replace an open target, even when this
        // extra source read handle allowed FILE_SHARE_DELETE. Release only it;
        // the verified backup and ancestor guards stay held through commit.
        // The writer reopens/rechecks the exact original token and preserves its
        // current DACL, then closes its own target handles before replacement.
        original.handle.reset();
        return save_document_bytes(path,*bytes.value,loaded);
    } catch(const std::exception& error){return {{},error.what(),false};}
}
AgentSettingsSnapshotResult load_agent_provider_settings(const std::filesystem::path& input) {
    AgentSettingsSnapshotResult result;
    try {
        const auto path=checked_path(input);const auto directories=guard_directories(path,false);
        OpenSnapshot opened;opened.token.path=path;if(directories.complete)opened=read_snapshot(path);
        if(opened.token.existed)result.settings=parse_agent_provider_settings(opened.token.bytes,result.error);
        else result.settings.emplace();
        if(result.settings)result.snapshot=std::move(opened.token);
    }catch(const std::exception& error){result.error=error.what();}
    return result;
}
ConfigSaveResult save_agent_provider_settings(const std::filesystem::path& path,const AgentProviderSettings& settings,const ConfigFileSnapshot& baseline) {
    const auto error=validate_agent_provider_settings(settings);if(!error.empty())return {{},error,false};
    return save_document_bytes(path,serialize_agent_provider_settings(settings),baseline);
}
} // namespace stormtty

#else
namespace stormtty {
SettingsSnapshotResult load_settings_snapshot(const std::filesystem::path&) { return {{}, {}, "Windows settings persistence is unavailable on this platform."}; }
ConfigSaveResult save_settings_config(const std::filesystem::path&, const Config&, const ConfigFileSnapshot&) {
    return {{}, "Windows settings persistence is unavailable on this platform.", false};
}
WorkspaceLibrarySnapshotResult load_workspace_library_snapshot(const std::filesystem::path&) {
    return {{}, {}, "Windows workspace persistence is unavailable on this platform."};
}
ConfigSaveResult save_workspace_library(const std::filesystem::path&, const WorkspaceLibrary&, const ConfigFileSnapshot&) {
    return {{}, "Windows workspace persistence is unavailable on this platform.", false};
}
ApplicationLayoutSnapshotResult load_application_layout_snapshot(const std::filesystem::path&) {
    return {{},{},"Application layout persistence requires the Windows host."};
}
ConfigSaveResult save_application_layout(const std::filesystem::path&,const ApplicationLayoutDocument&,const ConfigFileSnapshot&) {
    return {{},"Application layout persistence requires the Windows host.",false};
}
AgentSettingsSnapshotResult load_agent_provider_settings(const std::filesystem::path&) {return {{},{},"Local agent settings require the Windows host."};}
ConfigSaveResult save_agent_provider_settings(const std::filesystem::path&,const AgentProviderSettings&,const ConfigFileSnapshot&) {return {{},"Local agent settings require the Windows host.",false};}
} // namespace stormtty
#endif
