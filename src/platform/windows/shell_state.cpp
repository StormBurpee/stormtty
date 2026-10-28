#include "stormtty/windows_shell_state.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <utility>
#include <vector>

namespace stormtty {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& h) noexcept : value(std::exchange(h.value, INVALID_HANDLE_VALUE)) {}
};
struct LocalMemory {
    void* value{};
    ~LocalMemory() { if (value) LocalFree(value); }
};
void require(bool ok) { if (!ok) throw std::runtime_error("Managed shell state unavailable"); }
std::wstring native(const std::filesystem::path& path) { return L"\\\\?\\" + path.wstring(); }
struct Security {
    std::vector<BYTE> token_user;
    LocalMemory descriptor;
    LocalMemory installer;
    PSID user{};
    Security() {
        HANDLE raw{};
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE);
        Handle token(raw);
        DWORD size{};
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
        require(size > 0);
        token_user.resize(size);
        require(GetTokenInformation(token.value, TokenUser, token_user.data(), size, &size) != FALSE);
        user = reinterpret_cast<TOKEN_USER*>(token_user.data())->User.Sid;
        LocalMemory user_text;
        require(ConvertSidToStringSidW(user, reinterpret_cast<LPWSTR*>(&user_text.value)) != FALSE);
        const std::wstring sddl = L"D:P(A;OICI;FA;;;" + std::wstring(static_cast<wchar_t*>(user_text.value)) +
                                  L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
            reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr) != FALSE);
        // Windows Modules Installer owns standard system ancestors such as C:\\.
        require(ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
                                      &installer.value) != FALSE);
    }
    bool trusted(PSID sid) const {
        return sid && IsValidSid(sid) && (EqualSid(sid, user) || IsWellKnownSid(sid, WinLocalSystemSid) ||
               IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || EqualSid(sid, installer.value));
    }
    void inspect(HANDLE handle, bool ancestor) const {
        PSID owner{}; PACL acl{}; LocalMemory sd;
        require(GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &acl, nullptr, reinterpret_cast<PSECURITY_DESCRIPTOR*>(&sd.value)) == ERROR_SUCCESS);
        require(trusted(owner) && acl && IsValidAcl(acl)); // NULL DACL grants everyone full access.
        constexpr DWORD mutations = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
                                    FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;
        for (DWORD index = 0; index < acl->AceCount; ++index) {
            void* raw{}; require(GetAce(acl, index, &raw) != FALSE);
            const auto* header = static_cast<ACE_HEADER*>(raw);
            // Inherit-only grants still matter inside state: the shell creates
            // new config/history/cache files after this validation.
            if ((header->AceFlags & INHERIT_ONLY_ACE) && ancestor) continue;
            if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
            // Unknown/object/callback grants fail closed; no home-grown AccessCheck.
            require(header->AceType == ACCESS_ALLOWED_ACE_TYPE);
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            DWORD mask = ace->Mask;
            GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
            MapGenericMask(&mask, &mapping);
            // Creating unrelated siblings in ancestors is harmless; replacing
            // an existing directory (DELETE/DELETE_CHILD) or changing it is not.
            const DWORD forbidden = ancestor ? mutations & ~(FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY) :
                                               mutations | FILE_READ_DATA; // Private history and directory listings.
            if (mask & forbidden) require(trusted(const_cast<DWORD*>(&ace->SidStart)));
        }
    }
};
bool reserved_name(std::wstring name) {
    name.resize(name.find(L'.') == std::wstring::npos ? name.size() : name.find(L'.'));
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL" || name == L"CLOCK$" ||
           name == L"CONIN$" || name == L"CONOUT$" ||
           (name.size() == 4 && (name.starts_with(L"COM") || name.starts_with(L"LPT")) &&
            ((name[3] >= L'0' && name[3] <= L'9') || name[3] == L'\u00b9' || name[3] == L'\u00b2' || name[3] == L'\u00b3'));
}
std::filesystem::path checked_root(const std::filesystem::path& input) {
    auto text = input.wstring();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    require(text.size() >= 3 && text.size() < 32000 && text[1] == L':' && text[2] == L'\\' &&
            ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')));
    while (text.size() > 3 && text.back() == L'\\') text.pop_back();
    for (std::size_t begin = 3; begin < text.size();) {
        auto end = text.find(L'\\', begin);
        auto part = text.substr(begin, end == std::wstring::npos ? end : end - begin);
        require(!part.empty() && part != L"." && part != L".." && part.back() != L'.' && part.back() != L' ' && !reserved_name(part));
        require(std::none_of(part.begin(), part.end(), [](wchar_t c) { return c < 32 || std::wstring_view(L"<>:\"|?*").find(c) != std::wstring_view::npos; }));
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    const auto drive = text.substr(0, 3);
    require(GetDriveTypeW(drive.c_str()) == DRIVE_FIXED);
    DWORD flags{};
    require(GetVolumeInformationW(drive.c_str(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) &&
            (flags & FILE_PERSISTENT_ACLS));
    return text;
}
Handle open_checked(const std::filesystem::path& path, const Security& security, bool directory, bool ancestor) {
    Handle handle(CreateFileW(native(path).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(handle.value != INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileType(handle.value) == FILE_TYPE_DISK && GetFileInformationByHandle(handle.value, &info));
    require(!(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT));
    require(static_cast<bool>(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory);
    require(directory || info.nNumberOfLinks == 1);
    security.inspect(handle.value, ancestor);
    return handle;
}
void inspect_tree(const std::filesystem::path& path, const Security& security, unsigned depth, unsigned& count,
                  std::vector<Handle>& guards) {
    require(depth <= 4);
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        require(++count <= 512); // Dedicated state only; never scan arbitrary HOME.
        const DWORD attributes = GetFileAttributesW(native(entry.path()).c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT));
        const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const auto name = entry.path().filename().wstring();
        if (depth == 0) {
            if (name == L"config.zsh" || name == L"history" ||
                (name.starts_with(L"zcompdump-") && !name.ends_with(L".lock"))) require(!directory);
            if (name == L"cache") require(directory);
        }
        guards.push_back(open_checked(entry.path(), security, directory, false));
        if (directory) inspect_tree(entry.path(), security, depth + 1, count, guards);
    }
}
} // namespace
ManagedShellState prepare_managed_shell_state_at(const std::filesystem::path& local_app_data, bool empty_home) noexcept {
    if (empty_home) return {ManagedShellStateStatus::disabled, {}};
    try {
        const auto root = checked_root(local_app_data);
        Security security;
        std::vector<Handle> guards;
        auto part = root.root_path();
        guards.push_back(open_checked(part, security, true, true));
        for (const auto& component : root.relative_path()) {
            part /= component;
            guards.push_back(open_checked(part, security, true, true));
        }
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.descriptor.value, FALSE};
        for (const auto* name : {L"StormTTY", L"managed-zsh-v1"}) {
            part /= name;
            if (!CreateDirectoryW(native(part).c_str(), &attributes)) require(GetLastError() == ERROR_ALREADY_EXISTS);
            guards.push_back(open_checked(part, security, true, false));
        }
        unsigned count{};
        inspect_tree(part, security, 0, count, guards);
        return {ManagedShellStateStatus::ready, part};
    } catch (...) { return {}; } // Fail closed, no path/history logging or repairs.
}
ManagedShellState prepare_managed_shell_state(bool empty_home) noexcept {
    if (empty_home) return {ManagedShellStateStatus::disabled, {}};
    try {
        PWSTR raw{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) return {};
        struct CoString { PWSTR value; ~CoString() { CoTaskMemFree(value); } } owned{raw};
        return prepare_managed_shell_state_at(std::filesystem::path(raw), false);
    } catch (...) { return {}; }
}
} // namespace stormtty
#endif
