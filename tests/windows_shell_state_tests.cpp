#include "core_test.hpp"
#include "stormtty/windows_shell_state.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <winioctl.h>
#include <cstring>
#include <fstream>
#include <vector>
using namespace stormtty;
namespace {
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        PWSTR value{};
        CHECK(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value)));
        root = std::filesystem::path(value) / (L"StormTTY-state-test-\u00e9-\u6d4b\u8bd5-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        CoTaskMemFree(value);
        CHECK(std::filesystem::create_directory(root));
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
void write(const std::filesystem::path& path) {
    std::ofstream stream(path, std::ios::binary); stream << "# test\n"; stream.close(); CHECK(stream.good());
}
void unsafe_acl(const std::filesystem::path& path, bool read_only = false) {
    PSECURITY_DESCRIPTOR descriptor{};
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(read_only ?
        L"D:P(A;OICI;FR;;;WD)(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)" :
        L"D:P(A;OICI;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr));
    BOOL present{}, defaulted{}; PACL acl{};
    CHECK(GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted));
    const auto result = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(descriptor); CHECK(result == ERROR_SUCCESS);
}
void unsafe_inheritance(const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR original{}; PACL old_acl{};
    CHECK(GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_acl, nullptr, &original) == ERROR_SUCCESS);
    PSID everyone{}; CHECK(ConvertStringSidToSidW(L"S-1-1-0", &everyone));
    EXPLICIT_ACCESSW grant{};
    grant.grfAccessPermissions = FILE_ALL_ACCESS; grant.grfAccessMode = GRANT_ACCESS;
    grant.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT | INHERIT_ONLY;
    grant.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    grant.Trustee.ptstrName = static_cast<LPWSTR>(everyone);
    PACL acl{};
    const auto built = SetEntriesInAclW(1, &grant, old_acl, &acl);
    LocalFree(original); LocalFree(everyone); CHECK(built == ERROR_SUCCESS);
    const auto set = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(acl); CHECK(set == ERROR_SUCCESS);
}
void junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    CHECK(CreateDirectoryW(link.c_str(), nullptr));
    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    // Mount-point reparse data needs no symbolic-link privilege; exercise this
    // rejection on ordinary Windows CI rather than silently skipping it.
    struct MountPoint { DWORD tag; WORD length, reserved, substitute_offset, substitute_length,
        print_offset, print_length; wchar_t names[1]; };
    const std::wstring substitute = L"\\??\\" + target.wstring(), print = target.wstring();
    const auto bytes = 16 + (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    std::vector<BYTE> buffer(bytes);
    auto* data = reinterpret_cast<MountPoint*>(buffer.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT; data->length = static_cast<WORD>(bytes - 8);
    data->substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data->print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data->print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::memcpy(data->names, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(reinterpret_cast<BYTE*>(data->names) + data->print_offset, print.c_str(), (print.size() + 1) * sizeof(wchar_t));
    DWORD returned{};
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                                    nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle); CHECK(ok);
}
}
int main() { return run_core_tests("windows_shell_state (Windows native)", [] {
    const auto disabled = prepare_managed_shell_state(true);
    CHECK(disabled.status == ManagedShellStateStatus::disabled && disabled.directory.empty());
    CHECK(prepare_managed_shell_state_at(L"invalid", true).status == ManagedShellStateStatus::disabled);
    CHECK(!prepare_managed_shell_state_at(L"relative", false));
    CHECK(!prepare_managed_shell_state_at(L"\\\\server\\share", false));
    CHECK(!prepare_managed_shell_state_at(L"C:\\x\\..\\y", false));
    // Default uses the native known folder, never ambient LOCALAPPDATA/HOME.
    CHECK(prepare_managed_shell_state(false));
    Fixture fixture;
    const auto result = prepare_managed_shell_state_at(fixture.root, false);
    CHECK(result && result.directory == fixture.root / L"StormTTY" / L"managed-zsh-v1");
    CHECK(result.directory.wstring().find(L"\u6d4b\u8bd5") != std::wstring::npos);
    PSECURITY_DESCRIPTOR descriptor{};
    CHECK(GetNamedSecurityInfoW(const_cast<wchar_t*>(result.directory.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor) == ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision{};
    CHECK(GetSecurityDescriptorControl(descriptor, &control, &revision));
    LocalFree(descriptor);
    CHECK(control & SE_DACL_PROTECTED);
    const auto config = result.directory / L"config.zsh";
    write(config);
    const auto isolated_root=fixture.root/L"independent-smoke";
    CHECK(std::filesystem::create_directory(isolated_root));
    const auto isolated=prepare_managed_shell_state_at(isolated_root,false);
    CHECK(isolated && isolated.directory!=result.directory);
    CHECK(!std::filesystem::exists(isolated.directory/L"config.zsh"));
    CHECK(!std::filesystem::exists(isolated.directory/L"history"));
    CHECK(std::filesystem::exists(config)); // Never migrated or removed user/other-run state.
    CHECK(prepare_managed_shell_state_at(fixture.root, false));
    // Ordinary shell-created descendants inherit the private native DACL.
    const auto cache = result.directory / L"cache";
    CHECK(std::filesystem::create_directory(cache));
    const auto dump = cache / L".zcompdump.zwc";
    write(dump);
    CHECK(prepare_managed_shell_state_at(fixture.root, false));
    const auto alias = result.directory / L"config-hardlink";
    CHECK(CreateHardLinkW(alias.c_str(), config.c_str(), nullptr));
    CHECK(!prepare_managed_shell_state_at(fixture.root, false));
    CHECK(std::filesystem::remove(alias));
    CHECK(prepare_managed_shell_state_at(fixture.root, false));
    unsafe_acl(config, true);
    CHECK(!prepare_managed_shell_state_at(fixture.root, false));
    CHECK(std::filesystem::remove(config));
    write(config);
    unsafe_acl(config);
    CHECK(!prepare_managed_shell_state_at(fixture.root, false));
    std::ifstream preserved(config, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(preserved), {}};
    CHECK(bytes == "# test\n"); // Exact bytes, not overwritten/repaired.
    preserved.close();
    CHECK(std::filesystem::remove(config));
    CHECK(prepare_managed_shell_state_at(fixture.root, false));
    unsafe_acl(dump);
    CHECK(!prepare_managed_shell_state_at(fixture.root, false));
    CHECK(std::filesystem::remove(dump));
    const auto linked = result.directory / L"linked-cache";
    junction(linked, cache);
    CHECK(!prepare_managed_shell_state_at(fixture.root, false));
    CHECK(RemoveDirectoryW(linked.c_str()));
    CHECK(prepare_managed_shell_state_at(fixture.root, false));
    const auto linked_root = fixture.root / L"linked-root";
    junction(linked_root, cache);
    CHECK(!prepare_managed_shell_state_at(linked_root, false));
    CHECK(!std::filesystem::exists(cache / L"StormTTY"));
    CHECK(RemoveDirectoryW(linked_root.c_str()));
    CHECK(std::filesystem::create_directory(config));
    CHECK(!prepare_managed_shell_state_at(fixture.root, false));
    CHECK(std::filesystem::remove(config));
    CHECK(prepare_managed_shell_state_at(fixture.root, false));
    const auto inherited = result.directory / L"unsafe-inheritance";
    CHECK(std::filesystem::create_directory(inherited));
    unsafe_inheritance(inherited);
    CHECK(!prepare_managed_shell_state_at(fixture.root, false));
    CHECK(std::filesystem::remove(inherited));
    CHECK(prepare_managed_shell_state_at(fixture.root, false));
    // Never repair an unsafe existing directory, including an ancestor.
    unsafe_acl(result.directory);
    CHECK(!prepare_managed_shell_state_at(fixture.root, false));
    CHECK(prepare_managed_shell_state_at(fixture.root, true).status == ManagedShellStateStatus::disabled);
}); }
#else
int main() { return 1; }
#endif
