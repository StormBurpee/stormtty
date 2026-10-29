#include "stormtty/windows_shell_state.hpp"
#include "stormtty/process.hpp"
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
    void inspect(HANDLE handle, bool ancestor, bool private_data = true) const {
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
                                               mutations | (private_data ? FILE_READ_DATA : 0); // Runtime may be public-readable.
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

namespace {
class Initialization final : public WindowsProcessInitialization {
    std::filesystem::path root_;
    std::function<bool()> adopt_;
    std::unique_ptr<Handle> mutex_, event_;
    bool owned_{};
    DWORD owner_thread_{};
    void release() noexcept {
        if (owned_ && mutex_ && owner_thread_==GetCurrentThreadId()) (void)ReleaseMutex(mutex_->value);
        owned_=false;
        event_.reset();mutex_.reset();
    }
    static void kernel_security(HANDLE object,const Security& security) {
        PSID owner{};PACL acl{};LocalMemory sd;
        require(GetSecurityInfo(object,SE_KERNEL_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
            &owner,nullptr,&acl,nullptr,reinterpret_cast<PSECURITY_DESCRIPTOR*>(&sd.value))==ERROR_SUCCESS);
        require(security.trusted(owner) && acl && IsValidAcl(acl));
        for(DWORD i=0;i<acl->AceCount;++i) {
            void* raw{};require(GetAce(acl,i,&raw));const auto* header=static_cast<ACE_HEADER*>(raw);
            if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;
            require(header->AceType==ACCESS_ALLOWED_ACE_TYPE);
            const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
            require(security.trusted(const_cast<DWORD*>(&ace->SidStart)));
        }
    }
    static std::string utf8(const std::filesystem::path& path) {
        auto text=path.generic_u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};
    }
public:
    Initialization(std::filesystem::path root,std::function<bool()> adopt):root_(std::move(root)),adopt_(std::move(adopt)){}
    // Before prepare, no mutex is owned. Afterwards only the controller owns
    // this object until finished, including child-launch failures.
    ~Initialization() override { release(); }
    void prepare(std::vector<ProcessEnvironmentEntry>& environment) noexcept override {
        try {
            // Default is secure no-dump completion, NOT ephemeral user state.
            environment.push_back({"STORMTTY_NATIVE_ZSH_NO_DUMP","1"});
            environment.push_back({"STORMTTY_NATIVE_ZSH_INIT_EVENT",std::nullopt});
            const auto state=prepare_managed_shell_state_at(root_,false);
            if(!state) { environment.push_back({"STORMTTY_NATIVE_ZSH_STATE",std::nullopt});return; }
            environment.push_back({"STORMTTY_NATIVE_ZSH_STATE",utf8(state.directory)});
            Security security;
            auto directory=open_checked(state.directory,security,true,false);
            BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(directory.value,&info));
            const auto name=L"Global\\StormTTY-ZshInitMutex-v1-"+std::to_wstring(info.dwVolumeSerialNumber)+L"-"+
                std::to_wstring(info.nFileIndexHigh)+L"-"+std::to_wstring(info.nFileIndexLow);
            SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),security.descriptor.value,FALSE};
            mutex_=std::make_unique<Handle>(CreateMutexExW(&attributes,name.c_str(),0,MUTEX_ALL_ACCESS));
            require(mutex_->value && mutex_->value!=INVALID_HANDLE_VALUE);
            kernel_security(mutex_->value,security);
            const DWORD acquired=WaitForSingleObject(mutex_->value,0);
            if(acquired==WAIT_TIMEOUT) {mutex_.reset();return;} // Never join the shared cache race.
            require(acquired==WAIT_OBJECT_0 || acquired==WAIT_ABANDONED);
            owned_=true;owner_thread_=GetCurrentThreadId();
            const auto uncertain=state.directory/L"completion-init-uncertain";
            if(acquired==WAIT_ABANDONED) {
                // A crashed owner is not a readiness acknowledgement. Persist
                // no-dump mode rather than assuming a possibly dying child is done.
                Handle marker(CreateFileW(native(uncertain).c_str(),GENERIC_WRITE,0,&attributes,
                    CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
                require(marker.value!=INVALID_HANDLE_VALUE || GetLastError()==ERROR_FILE_EXISTS);
            }
            GUID guid{};require(SUCCEEDED(CoCreateGuid(&guid)));
            std::wstring event_name=L"Local\\StormTTY-ZshInit-";
            const auto* bytes=reinterpret_cast<const unsigned char*>(&guid);
            for(std::size_t i=0;i<sizeof(guid);++i){event_name+=L"0123456789abcdef"[bytes[i]>>4];event_name+=L"0123456789abcdef"[bytes[i]&15];}
            event_=std::make_unique<Handle>(CreateEventW(&attributes,TRUE,FALSE,event_name.c_str()));
            require(event_->value && GetLastError()!=ERROR_ALREADY_EXISTS);
            kernel_security(event_->value,security);
            bool fresh=!std::filesystem::exists(state.directory/L"config.zsh") &&
                !std::filesystem::exists(state.directory/L"config.zsh.zwc");
            for(const auto& entry:std::filesystem::directory_iterator(state.directory))
                if(entry.path().filename().wstring().starts_with(L"zcompdump-"))fresh=false;
            const auto completions=state.directory/L"cache/completions";
            if(std::filesystem::exists(completions) && !std::filesystem::is_empty(completions))fresh=false;
            if(!std::filesystem::exists(uncertain)) {
                if(fresh && adopt_) (void)adopt_(); // Failure remains ordinary secure initialization.
                environment.push_back({"STORMTTY_NATIVE_ZSH_NO_DUMP","0"});
            }
            environment.push_back({"STORMTTY_NATIVE_ZSH_INIT_EVENT",std::string(event_name.begin(),event_name.end())});
        } catch(...) {
            // Keep any acquired lease through child exit if preparation could
            // not produce a readiness event; contenders still use no-dump mode.
        }
    }
    std::uintptr_t ready_event() const noexcept override {return event_?reinterpret_cast<std::uintptr_t>(event_->value):0;}
    void ready() noexcept override {release();}
    void finished() noexcept override {release();}
};
}
std::function<bool(std::uintptr_t, bool)> managed_runtime_access_check() {
    auto security=std::make_shared<Security>();
    return [security](std::uintptr_t raw,bool ancestor) noexcept {
        try {security->inspect(reinterpret_cast<HANDLE>(raw),ancestor,false);return true;}
        catch(...) {return false;}
    };
}
std::shared_ptr<WindowsProcessInitialization> managed_shell_initialization_at(
    const std::filesystem::path& root,std::function<bool()> adopt) {
    return std::make_shared<Initialization>(root,std::move(adopt));
}

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
bool publish_managed_completion_seed_at(const std::filesystem::path& local_app_data,
    std::wstring_view filename, std::string_view text) noexcept {
    try {
        require(filename.starts_with(L"zcompdump-") && filename.size() < 64 &&
            filename.size() > 10 && text.size() <= 1024 * 1024);
        for (auto c : filename.substr(10)) require((c >= L'0' && c <= L'9') || c == L'.');
        const auto state = prepare_managed_shell_state_at(local_app_data, false);
        require(static_cast<bool>(state));
        Security security;
        std::vector<Handle> guards;
        auto part = checked_root(state.directory).root_path();
        guards.push_back(open_checked(part, security, true, true));
        for (const auto& component : state.directory.relative_path()) {
            part /= component;
            guards.push_back(open_checked(part, security, true, part != state.directory));
        }
        unsigned count{};
        inspect_tree(state.directory, security, 0, count, guards);
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.descriptor.value, FALSE};
        auto cache = state.directory;
        for (const auto* component : {L"cache", L"completions"}) {
            cache /= component;
            if (!CreateDirectoryW(native(cache).c_str(), &attributes)) require(GetLastError() == ERROR_ALREADY_EXISTS);
            guards.push_back(open_checked(cache, security, true, false));
        }
        require(std::filesystem::is_empty(cache));
        const auto target = state.directory / filename;
        for (const auto& entry : std::filesystem::directory_iterator(state.directory))
            require(!entry.path().filename().wstring().starts_with(L"zcompdump-"));
        // A fixed exclusive temporary file also serializes competing seed adopters.
        // Keep DELETE access for handle-based no-replace publication/cleanup.
        const auto temporary = state.directory / L"completion-seed.tmp";
        Handle file(CreateFileW(native(temporary).c_str(), GENERIC_WRITE | DELETE | READ_CONTROL,
            0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        require(file.value != INVALID_HANDLE_VALUE);
        bool published{};
        try {
            security.inspect(file.value, false);
            DWORD written{};
            require(WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size());
            require(FlushFileBuffers(file.value) != FALSE);
            // Recheck conflicts immediately before publishing. Ordinary shell startup
            // remains outside this prototype's owned fixture until this returns.
            require(std::filesystem::is_empty(cache));
            for (const auto& entry : std::filesystem::directory_iterator(state.directory))
                require(!entry.path().filename().wstring().starts_with(L"zcompdump-"));
            const auto destination = native(target);
            std::vector<BYTE> storage(sizeof(FILE_RENAME_INFO) + destination.size() * sizeof(wchar_t));
            auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
            rename->ReplaceIfExists = FALSE;
            rename->RootDirectory = nullptr;
            rename->FileNameLength = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
            std::copy(destination.begin(), destination.end(), rename->FileName);
            require(SetFileInformationByHandle(file.value, FileRenameInfo, rename, static_cast<DWORD>(storage.size())) != FALSE);
            published = true;
        } catch (...) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            (void)SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
        }
        return published;
    } catch (...) { return false; }
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
