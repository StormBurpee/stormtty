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
#include <barrier>
#include <thread>
#include <array>
#include <exception>
#include <functional>
using namespace stormtty;
namespace {
struct Directory {
    std::filesystem::path path=std::filesystem::current_path()/(L"layout-migration-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    Directory(){CHECK(std::filesystem::create_directory(path));}
    ~Directory(){std::error_code error;std::filesystem::remove_all(path,error);}
};
void serialized_settings_windows(const Directory& directory) {
    const auto path=directory.path/L"two-settings-editors.stormtty";
    auto baseline=load_settings_snapshot(path);CHECK(baseline);CHECK(!baseline.snapshot->existed);
    auto first=*baseline.config,second=first;first.font.size_dip=18;second.font.size_dip=22;
    ConfigSaveResult results[2];std::barrier start(3);
    std::thread a([&]{start.arrive_and_wait();results[0]=save_settings_config(path,first,*baseline.snapshot);});
    std::thread b([&]{start.arrive_and_wait();results[1]=save_settings_config(path,second,*baseline.snapshot);});
    start.arrive_and_wait();a.join();b.join();
    CHECK(static_cast<bool>(results[0])!=static_cast<bool>(results[1]));
    CHECK(results[0]?results[1].conflict:results[0].conflict);
    auto saved=load_settings_snapshot(path);CHECK(saved);
    CHECK(saved.config->font.size_dip==(results[0]?18:22));
    // Stale editor is still stale after retry; coordinator never silently reloads.
    const auto winner_bytes=saved.snapshot->bytes;
    const auto retry=save_settings_config(path,results[0]?second:first,*baseline.snapshot);CHECK(!retry);CHECK(retry.conflict);
    const auto unchanged=load_settings_snapshot(path);CHECK(unchanged);CHECK(unchanged.snapshot->bytes==winner_bytes);
}
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
// These synthetic same-path fixtures exercise the shared commit authority.
// Production adapters still use their separate host-selected document paths.
struct ReadObservation {
    std::optional<ConfigFileSnapshot> snapshot;
    std::string error;
};

struct DocumentAdapter {
    const wchar_t* name;
    std::string encoded;
    std::function<ConfigSaveResult(const std::filesystem::path&, const ConfigFileSnapshot&)> save;
    std::function<ReadObservation(const std::filesystem::path&)> load;
};

std::array<DocumentAdapter, 4> document_adapters() {
    auto config = default_config(Platform::windows);
    config.font.size_dip = 19;
    WorkspaceLibrary library;
    AgentProviderSettings agent;
    ApplicationLayoutDocument layout;
    layout.clean_shutdown = false;
    const auto workspace_bytes = serialize_workspace_library(library);
    const auto layout_bytes = serialize_application_layout(layout);
    CHECK(workspace_bytes);
    CHECK(layout_bytes);
    return {{
        {L"settings", serialize_config(config, Platform::windows),
            [config](const auto& path, const auto& baseline) {
                return save_settings_config(path, config, baseline);
            },
            [](const auto& path) {
                auto loaded = load_settings_snapshot(path);
                return ReadObservation{std::move(loaded.snapshot), std::move(loaded.error)};
            }},
        {L"workspace", *workspace_bytes.value,
            [library](const auto& path, const auto& baseline) {
                return save_workspace_library(path, library, baseline);
            },
            [](const auto& path) {
                auto loaded = load_workspace_library_snapshot(path);
                return ReadObservation{std::move(loaded.snapshot), std::move(loaded.error)};
            }},
        {L"agent", serialize_agent_provider_settings(agent),
            [agent](const auto& path, const auto& baseline) {
                return save_agent_provider_settings(path, agent, baseline);
            },
            [](const auto& path) {
                auto loaded = load_agent_provider_settings(path);
                return ReadObservation{std::move(loaded.snapshot), std::move(loaded.error)};
            }},
        {L"layout", *layout_bytes.value,
            [layout](const auto& path, const auto& baseline) {
                return save_application_layout(path, layout, baseline);
            },
            [](const auto& path) {
                auto loaded = load_application_layout_snapshot(path);
                return ReadObservation{std::move(loaded.snapshot), std::move(loaded.error)};
            }}
    }};
}

void cross_adapter_saves(const Directory& directory) {
    const auto adapters = document_adapters();
    constexpr std::array<std::array<std::size_t, 2>, 3> pairs{{{0, 1}, {0, 2}, {1, 3}}};
    const auto existing_bytes = serialize_application_layout(ApplicationLayoutDocument{});
    CHECK(existing_bytes);
    for (const auto& pair : pairs) {
        const auto& first = adapters[pair[0]];
        const auto& second = adapters[pair[1]];
        CHECK(first.encoded != second.encoded);
        for (const bool existed : {false, true}) {
            for (unsigned repetition = 0; repetition < 3; ++repetition) {
                const auto name = std::wstring(first.name) + L"-" + second.name +
                    (existed ? L"-existing-" : L"-missing-") + std::to_wstring(repetition);
                const auto path = directory.path / (name + L".stormtty");
                if (existed) write(path, *existing_bytes.value);
                // A valid envelope lets the layout adapter validate the same
                // baseline without involving legacy migration in this lock test.
                const auto loaded = load_application_layout_snapshot(path);
                CHECK(loaded);
                CHECK(loaded.snapshot->existed == existed);
                const auto baseline = *loaded.snapshot;
                std::array<ConfigSaveResult, 2> results;
                std::array<std::exception_ptr, 2> exceptions;
                std::barrier start(3);
                const auto save = [&](std::size_t index, const DocumentAdapter& adapter) {
                    start.arrive_and_wait();
                    try {
                        results[index] = adapter.save(path, baseline);
                    } catch (...) {
                        exceptions[index] = std::current_exception();
                    }
                };
                std::thread a([&] { save(0, first); });
                std::thread b([&] { save(1, second); });
                start.arrive_and_wait();
                a.join();
                b.join();
                for (const auto& exception : exceptions) {
                    if (exception) std::rethrow_exception(exception);
                }
                CHECK(static_cast<bool>(results[0]) != static_cast<bool>(results[1]));
                const std::size_t winner = results[0] ? 0 : 1;
                const auto& winning_adapter = winner == 0 ? first : second;
                const auto& losing_adapter = winner == 0 ? second : first;
                const auto& failed = results[1 - winner];
                CHECK(!results[winner].conflict);
                CHECK(failed.conflict);
                CHECK(!failed.snapshot);
                CHECK(failed.error == "Settings changed on disk. Nothing was saved; close and reopen Settings to review the new file.");
                CHECK(read(path) == winning_adapter.encoded);
                const auto reread = winning_adapter.load(path);
                CHECK(reread.error.empty());
                CHECK(reread.snapshot == results[winner].snapshot);
                CHECK(reread.snapshot->bytes == winning_adapter.encoded);
                CHECK(!std::filesystem::exists(backup(path)));
                const auto retry = losing_adapter.save(path, baseline);
                CHECK(!retry);
                CHECK(retry.conflict);
                CHECK(!retry.snapshot);
                CHECK(winning_adapter.load(path).snapshot == reread.snapshot);
            }
        }
    }
    for (const auto& item : std::filesystem::recursive_directory_iterator(directory.path)) {
        CHECK(!item.path().filename().wstring().starts_with(L".stormtty-save-"));
    }
}

void public_read_lifetimes(const Directory& directory) {
    // Public loads are synchronous: without production hooks this observes
    // admission and release, not the exact internal parse-time guard interval.
    for (const auto& adapter : document_adapters()) {
        const auto parent = directory.path / (std::wstring(L"read-") + adapter.name);
        CHECK(std::filesystem::create_directory(parent));
        const auto path = parent / L"document.stormtty";
        write(path, adapter.encoded);
        {
            OwnedHandle writer;
            writer.value = CreateFileW(path.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            CHECK(writer.value != INVALID_HANDLE_VALUE);
            const auto blocked = adapter.load(path);
            CHECK(!blocked.snapshot);
            CHECK(!blocked.error.empty());
        }
        const auto loaded = adapter.load(path);
        CHECK(loaded.snapshot);
        CHECK(loaded.error.empty());
        CHECK(loaded.snapshot->bytes == adapter.encoded);
        const auto original = *loaded.snapshot;
        const auto moved_parent = directory.path / (std::wstring(L"moved-") + adapter.name);
        // Moving the ancestor proves no directory guard leaked past return.
        CHECK(MoveFileExW(parent.c_str(), moved_parent.c_str(), 0));
        const auto moved_path = moved_parent / path.filename();
        {
            OwnedHandle writer;
            // No sharing: a leaked file read handle would also refuse this open.
            writer.value = CreateFileW(moved_path.c_str(), GENERIC_WRITE, 0,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            CHECK(writer.value != INVALID_HANDLE_VALUE);
        }
        write(moved_path, "invalid document\n");
        CHECK(*loaded.snapshot == original);
        CHECK(loaded.snapshot->bytes == adapter.encoded);
        const auto malformed = adapter.load(moved_path);
        CHECK(!malformed.snapshot);
        CHECK(!malformed.error.empty());
        // Parse-failure returns must release both file and ancestor guards too.
        CHECK(MoveFileExW(moved_parent.c_str(), parent.c_str(), 0));
        CHECK(DeleteFileW(path.c_str()));
        CHECK(RemoveDirectoryW(parent.c_str()));
    }
}

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
    auto saved=save_application_layout(path,loaded.layout->document,*loaded.snapshot);
    if(!saved)std::cerr<<"initial legacy migration failed: conflict="<<saved.conflict<<" error="<<saved.error<<'\n';
    CHECK(saved);
    CHECK(read(backup(path))==*old.value);
    auto current=load_application_layout_snapshot(path);CHECK(current);CHECK(!current.layout->requires_legacy_backup);
    CHECK(current.layout->document.library.presets.size()==1);
    current.layout->document.clean_shutdown=false;
    auto updated=save_application_layout(path,current.layout->document,*current.snapshot);
    if(!updated)std::cerr<<"envelope update failed: conflict="<<updated.conflict<<" error="<<updated.error<<'\n';
    CHECK(updated);
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
int main() {
    return run_core_tests("application_layout_services (Windows native)", [] {
        Directory directory;
        migration_and_conflicts(directory);
        malformed_and_bounds(directory);
        permission_guards(directory);
        serialized_settings_windows(directory);
        cross_adapter_saves(directory);
        public_read_lifetimes(directory);
    });
}
#else
int main(){std::cout<<"SKIP application_layout_services: Windows runtime required\n";return 0;}
#endif
