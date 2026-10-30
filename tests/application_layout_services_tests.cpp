#include "core_test.hpp"
#include "stormtty/application_layout_services.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <cstring>
#include <fstream>
#include <iterator>
using namespace stormtty;
namespace {
struct Directory {
    std::filesystem::path path=std::filesystem::current_path()/(L"layout-migration-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    Directory(){CHECK(std::filesystem::create_directory(path));}
    ~Directory(){std::error_code error;std::filesystem::remove_all(path,error);}
};
void write(const std::filesystem::path& path,std::string_view bytes) {
    std::ofstream file(path,std::ios::binary|std::ios::trunc);CHECK(file.good());file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));file.close();CHECK(file.good());
}
std::string read(const std::filesystem::path& path) {std::ifstream file(path,std::ios::binary);CHECK(file.good());return {std::istreambuf_iterator<char>(file),{}};}
std::filesystem::path backup(const std::filesystem::path& path){return std::filesystem::path(path.wstring()+L".v1-backup");}
WorkspaceLibrary legacy() {
    ProjectWorkspaceCatalog catalog;CHECK(catalog.create({"Migration","C:/work","Local"}));
    auto snapshot=capture_workspace_snapshot(catalog);CHECK(snapshot);
    WorkspaceLibrary library;library.last_session=*snapshot.value;
    CHECK(save_workspace_preset(library,"Preserved",*snapshot.value));return library;
}
struct OwnedHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~OwnedHandle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
};
void protect_for_current_user(const std::filesystem::path& path) {
    OwnedHandle token;CHECK(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value));
    DWORD size{};GetTokenInformation(token.value,TokenUser,nullptr,0,&size);CHECK(size>0);
    std::vector<std::byte> user(size);CHECK(GetTokenInformation(token.value,TokenUser,user.data(),size,&size));
    EXPLICIT_ACCESSW access{};access.grfAccessPermissions=FILE_ALL_ACCESS;access.grfAccessMode=SET_ACCESS;
    access.grfInheritance=NO_INHERITANCE;access.Trustee.TrusteeForm=TRUSTEE_IS_SID;access.Trustee.TrusteeType=TRUSTEE_IS_USER;
    access.Trustee.ptstrName=reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
    PACL acl{};CHECK(SetEntriesInAclW(1,&access,nullptr,&acl)==ERROR_SUCCESS);
    const auto result=SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr);
    LocalFree(acl);CHECK(result==ERROR_SUCCESS);
}
std::pair<bool,std::vector<std::byte>> permissions(const std::filesystem::path& path) {
    PACL acl{};PSECURITY_DESCRIPTOR descriptor{};
    CHECK(GetNamedSecurityInfoW(path.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&acl,nullptr,&descriptor)==ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL control{};DWORD revision{};
    const bool inspected=GetSecurityDescriptorControl(descriptor,&control,&revision)!=FALSE;
    std::vector<std::byte> bytes;if(acl){bytes.resize(acl->AclSize);std::memcpy(bytes.data(),acl,acl->AclSize);}
    LocalFree(descriptor);CHECK(inspected);return {(control&SE_DACL_PROTECTED)!=0,std::move(bytes)};
}
void permission_guards(const Directory& directory) {
    auto encoded=serialize_workspace_library(legacy());CHECK(encoded);
    const auto path=directory.path/L"private.stormtty";write(path,*encoded.value);protect_for_current_user(path);
    const auto expected=permissions(path);CHECK(expected.first);
    auto loaded=load_application_layout_snapshot(path);CHECK(loaded);
    CHECK(save_application_layout(path,loaded.layout->document,*loaded.snapshot));
    CHECK(permissions(backup(path))==expected);CHECK(permissions(path)==expected);
    const auto mismatch=directory.path/L"permission-mismatch.stormtty";write(mismatch,*encoded.value);protect_for_current_user(mismatch);
    write(backup(mismatch),*encoded.value);CHECK(permissions(mismatch)!=permissions(backup(mismatch)));
    auto baseline=load_application_layout_snapshot(mismatch);CHECK(baseline);
    CHECK(!save_application_layout(mismatch,baseline.layout->document,*baseline.snapshot));
    CHECK(read(mismatch)==*encoded.value);CHECK(read(backup(mismatch))==*encoded.value);
    const auto held=directory.path/L"write-held.stormtty";write(held,*encoded.value);write(backup(held),*encoded.value);
    auto before=load_application_layout_snapshot(held);CHECK(before);
    OwnedHandle writer;writer.value=CreateFileW(backup(held).c_str(),GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(writer.value!=INVALID_HANDLE_VALUE);
    CHECK(!save_application_layout(held,before.layout->document,*before.snapshot));CHECK(read(held)==*encoded.value);
}
void migration_and_conflicts(const Directory& directory) {
    auto library=legacy();auto old=serialize_workspace_library(library);CHECK(old);
    const auto path=directory.path/L"workspaces.stormtty";write(path,*old.value);
    auto loaded=load_application_layout_snapshot(path);CHECK(loaded);CHECK(loaded.layout->requires_legacy_backup);
    CHECK(!std::filesystem::exists(backup(path)));
    auto saved=save_application_layout(path,loaded.layout->document,*loaded.snapshot);CHECK(saved);
    CHECK(read(backup(path))==*old.value);
    auto current=load_application_layout_snapshot(path);CHECK(current);CHECK(!current.layout->requires_legacy_backup);
    CHECK(current.layout->document.library.presets.size()==1);
    current.layout->document.clean_shutdown=false;
    auto updated=save_application_layout(path,current.layout->document,*current.snapshot);CHECK(updated);
    CHECK(read(backup(path))==*old.value);CHECK(!load_application_layout_snapshot(path).layout->document.clean_shutdown);
    auto stale=save_application_layout(path,loaded.layout->document,*loaded.snapshot);CHECK(!stale);CHECK(stale.conflict);
    const auto refused=directory.path/L"differing.stormtty";write(refused,*old.value);write(backup(refused),"Do not overwrite this backup");
    auto baseline=load_application_layout_snapshot(refused);CHECK(baseline);
    auto denied=save_application_layout(refused,baseline.layout->document,*baseline.snapshot);CHECK(!denied);CHECK(denied.conflict);
    CHECK(read(refused)==*old.value);CHECK(read(backup(refused))=="Do not overwrite this backup");
    const auto reusable=directory.path/L"reusable.stormtty";write(reusable,*old.value);write(backup(reusable),*old.value);
    auto reuse=load_application_layout_snapshot(reusable);CHECK(reuse);CHECK(save_application_layout(reusable,reuse.layout->document,*reuse.snapshot));
    const auto changed=directory.path/L"changed.stormtty";write(changed,*old.value);
    auto prior=load_application_layout_snapshot(changed);CHECK(prior);write(changed,"External edit");
    auto conflict=save_application_layout(changed,prior.layout->document,*prior.snapshot);CHECK(!conflict);CHECK(conflict.conflict);
    CHECK(read(changed)=="External edit");CHECK(!std::filesystem::exists(backup(changed)));
    const auto readonly=directory.path/L"readonly.stormtty";write(readonly,*old.value);
    auto locked=load_application_layout_snapshot(readonly);CHECK(locked);CHECK(SetFileAttributesW(readonly.c_str(),FILE_ATTRIBUTE_READONLY));
    auto blocked=save_application_layout(readonly,locked.layout->document,*locked.snapshot);
    CHECK(SetFileAttributesW(readonly.c_str(),FILE_ATTRIBUTE_NORMAL));CHECK(!blocked);
    CHECK(!std::filesystem::exists(backup(readonly)));CHECK(read(readonly)==*old.value);
}
void malformed_and_bounds(const Directory& directory) {
    const auto missing=directory.path/L"missing"/L"layout.stormtty";
    auto empty=load_application_layout_snapshot(missing);CHECK(empty);CHECK(!empty.snapshot->existed);CHECK(!std::filesystem::exists(missing.parent_path()));
    CHECK(save_application_layout(missing,empty.layout->document,*empty.snapshot));CHECK(!std::filesystem::exists(backup(missing)));
    const auto unknown=directory.path/L"unknown.stormtty";const std::string unknown_bytes="STORMTTY-APPLICATION-LAYOUT 99\n";write(unknown,unknown_bytes);
    CHECK(!load_application_layout_snapshot(unknown));ConfigFileSnapshot forged;forged.path=unknown;forged.existed=true;forged.bytes=unknown_bytes;
    CHECK(!save_application_layout(unknown,{},forged));CHECK(read(unknown)==unknown_bytes);CHECK(!std::filesystem::exists(backup(unknown)));
    auto large=legacy();large.presets.clear();
    for(int i=0;i<20;++i) {
        auto part=legacy().last_session.projects[0];part.metadata.working_directory=std::string(32000,'a');large.last_session.projects.push_back(std::move(part));
    }
    auto encoded=serialize_workspace_library(large);CHECK(encoded);
    const auto path=directory.path/L"large.stormtty";write(path,*encoded.value);
    auto loaded=load_application_layout_snapshot(path);CHECK(loaded);
    CHECK(!save_application_layout(path,loaded.layout->document,*loaded.snapshot));
    CHECK(read(path)==*encoded.value);CHECK(!std::filesystem::exists(backup(path)));
    const auto linked=directory.path/L"linked.stormtty";auto normal=serialize_workspace_library(legacy());CHECK(normal);write(linked,*normal.value);
    const auto target=directory.path/L"unrelated-backup";write(target,*normal.value);
    CHECK(CreateHardLinkW(backup(linked).c_str(),target.c_str(),nullptr));
    auto before=load_application_layout_snapshot(linked);CHECK(before);CHECK(!save_application_layout(linked,before.layout->document,*before.snapshot));
    CHECK(read(linked)==*normal.value);CHECK(read(target)==*normal.value);
    for(const auto& item:std::filesystem::recursive_directory_iterator(directory.path))CHECK(!item.path().filename().wstring().starts_with(L".stormtty-save-"));
}
}
int main(){return run_core_tests("application_layout_services (Windows native)",[]{Directory directory;migration_and_conflicts(directory);malformed_and_bounds(directory);permission_guards(directory);});}
#else
int main(){std::cout<<"SKIP application_layout_services: Windows runtime required\n";return 0;}
#endif
