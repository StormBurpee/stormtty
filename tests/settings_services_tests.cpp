#include "core_test.hpp"
#include "stormtty/settings_services.hpp"
#include "stormtty/settings_panel.hpp"
#include "stormtty/settings_reload.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

using namespace stormtty;
namespace {
struct TestDirectory {
    std::filesystem::path path;
    TestDirectory() {
        path = std::filesystem::current_path() /
            (L"settings-services-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        CHECK(std::filesystem::create_directory(path));
    }
    ~TestDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}
void write(const std::filesystem::path& path, std::string_view value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output.good());
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.close(); CHECK(output.good());
}
bool has_temporaries(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (entry.path().filename().wstring().starts_with(L".stormtty-save-")) return true;
    return false;
}
void check_rejected(const std::filesystem::path& path, const Config& config) {
    auto loaded = load_settings_snapshot(path);
    CHECK(!loaded); CHECK(!loaded.error.empty());
    ConfigFileSnapshot fabricated; fabricated.path = path;
    auto saved = save_settings_config(path, config, fabricated);
    CHECK(!saved); CHECK(!saved.error.empty());
}
}
int main() { return run_core_tests("settings_services (Windows native)", [] {
    TestDirectory directory;
    const auto path = directory.path / L"new-directory" / L"config-\u00e9.stormtty";
    const auto workspace_path = directory.path / L"workspaces.stormtty";
    auto workspace_missing = load_workspace_library_snapshot(workspace_path);
    CHECK(workspace_missing); CHECK(!workspace_missing.snapshot->existed);
    ProjectWorkspaceCatalog projects;
    CHECK(projects.create({"Project", "C:/work", "Local", "code", "#abcdef", "default"}));
    auto workspace_capture = capture_workspace_snapshot(projects); CHECK(workspace_capture);
    auto library = *workspace_missing.library;
    library.last_session = *workspace_capture.value;
    CHECK(save_workspace_preset(library, "Development", *workspace_capture.value));
    auto workspace_saved = save_workspace_library(workspace_path, library, *workspace_missing.snapshot);
    CHECK(workspace_saved); CHECK(!has_temporaries(directory.path));
    auto workspace_loaded = load_workspace_library_snapshot(workspace_path); CHECK(workspace_loaded);
    CHECK(serialize_workspace_library(*workspace_loaded.library).value == serialize_workspace_library(library).value);
    CHECK(save_workspace_library(workspace_path, library, *workspace_missing.snapshot).conflict);
    const auto workspace_bytes = read(workspace_path);
    auto invalid_library = library; invalid_library.last_session.active_index = 99;
    CHECK(!save_workspace_library(workspace_path, invalid_library, *workspace_saved.snapshot));
    CHECK(read(workspace_path) == workspace_bytes);
    write(workspace_path, "invalid"); CHECK(!load_workspace_library_snapshot(workspace_path));
    CHECK(save_workspace_library(workspace_path, library, *workspace_saved.snapshot).conflict);
    CHECK(!has_temporaries(directory.path));
    const auto defaults = default_config(Platform::windows);
    auto missing = load_settings_snapshot(path);
    CHECK(missing); CHECK(!missing.snapshot->existed); CHECK(*missing.config == defaults);
    CHECK(!std::filesystem::exists(path.parent_path())); // read never creates directories
    auto config = defaults;
    config.font.size_dip = 17.5;
    config.font.family = "Cascadia Mono";
    Profile bash;
    bash.id = "git-bash"; bash.label = "Git Bash";
    bash.executable = "C:\\Program Files\\Git\\bin\\bash.exe";
    bash.arguments = {"--login", "-i"}; bash.environment["CHERE_INVOKING"] = "1";
    config.profiles.push_back(bash); config.default_profile = bash.id;
    auto saved = save_settings_config(path, config, *missing.snapshot);
    CHECK(saved); CHECK(saved.snapshot->existed); CHECK(read(path) == serialize_config(config, Platform::windows));
    CHECK(!has_temporaries(directory.path));
    auto loaded = load_settings_snapshot(path);
    CHECK(loaded); CHECK(*loaded.snapshot == *saved.snapshot); CHECK(loaded.config->profiles == config.profiles);
    CHECK(loaded.config->default_profile == "git-bash");
    CHECK(loaded.config->font.size_dip == 17.5);
    const auto original = read(path);
    SettingsReload reload;
    CHECK(reload.begin(false,false));
    auto reloaded=reload.complete(reload.generation(),load_settings_snapshot(path));
    CHECK(reloaded.config==loaded.config); CHECK(read(path)==original);
    CHECK(reload.begin(false,false));
    CHECK(!reload.complete(reload.generation(),load_settings_snapshot(directory.path / "absent.toml")).config);
    write(path,"version = 1\n[font]\nsize_dip = broken\n");
    CHECK(reload.begin(false,false));
    CHECK(!reload.complete(reload.generation(),load_settings_snapshot(path)).config);
    CHECK(read(path)!=""); // The failure never writes defaults over the user's bytes.
    write(path,original);


    // Invalid drafts must never overwrite the existing file or create a temp.
    auto invalid = config; invalid.font.size_dip = 999;
    CHECK(!save_settings_config(path, invalid, *loaded.snapshot)); CHECK(read(path) == original);
    // The parser supplies the built-in default profile. A value object that
    // would acquire an extra profile on reload is not a lossless save draft.
    auto non_roundtrip = config; non_roundtrip.profiles.erase(non_roundtrip.profiles.begin());
    CHECK(validate_config(non_roundtrip, Platform::windows));
    CHECK(!save_settings_config(path, non_roundtrip, *loaded.snapshot)); CHECK(read(path) == original);
    CHECK(!has_temporaries(directory.path));
    auto wrong_path = *loaded.snapshot; wrong_path.path = directory.path / L"different.stormtty";
    CHECK(!save_settings_config(path, config, wrong_path)); CHECK(read(path) == original);

    // A stale token never overwrites an ordinary external edit, removal, or
    // replacement (even when the external replacement contains identical bytes).
    write(path, original + "\n# independent editor\n");
    auto conflict = save_settings_config(path, config, *loaded.snapshot);
    CHECK(!conflict); CHECK(conflict.conflict); CHECK(read(path).ends_with("# independent editor\n"));
    CHECK(!has_temporaries(directory.path));
    auto refreshed = load_settings_snapshot(path); CHECK(refreshed);
    std::filesystem::remove(path);
    CHECK(save_settings_config(path, config, *refreshed.snapshot).conflict); CHECK(!std::filesystem::exists(path));
    auto absent_again = load_settings_snapshot(path); CHECK(absent_again);
    write(path, original);
    CHECK(save_settings_config(path, config, *absent_again.snapshot).conflict); CHECK(read(path) == original);
    auto before_replace = load_settings_snapshot(path); CHECK(before_replace);
    const auto replacement = directory.path / L"replacement.stormtty";
    write(replacement, original);
    CHECK(MoveFileExW(replacement.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING));
    CHECK(save_settings_config(path, config, *before_replace.snapshot).conflict);

    // A successful token can be reused for a second Save without hidden reload.
    auto baseline = load_settings_snapshot(path); CHECK(baseline);
    config.appearance.background_opacity = 0.8;
    auto second = save_settings_config(path, config, *baseline.snapshot); CHECK(second);
    config.font.size_dip = 18;
    auto third = save_settings_config(path, config, *second.snapshot); CHECK(third);
    CHECK(load_settings_snapshot(path).config->font.size_dip == 18);
    CHECK(!has_temporaries(directory.path));

    // Actual external-file editor lifecycle: fresh disk draft, separate running
    // rollback baseline, explicit adoption, and unchanged optimistic tokens.
    const auto editor_path=directory.path/L"editor-lifecycle.stormtty";
    auto running=defaults;SettingsPanel editor;
    auto external=running;external.font.size_dip=22;external.theme=*find_builtin_theme("daylight");
    external.profiles.push_back({"external","External shell","external.exe",{},{},{}});external.default_profile="external";
    external.bindings.push_back({"pane.split_right",{"F8",CommandModifier::control|CommandModifier::shift}});
    const auto commit_action=[&] {
        editor.key({{"ENTER",CommandModifier::control}});
        const auto action=editor.take_action();CHECK(action && action->kind==SettingsActionKind::save);return *action;
    };
    // Own Save -> reread -> reopen has no false "loaded from disk" status.
    auto editor_missing=load_settings_snapshot(editor_path);CHECK(editor_missing);
    auto editor_own=save_settings_config(editor_path,running,*editor_missing.snapshot);CHECK(editor_own);
    auto editor_fresh=load_settings_snapshot(editor_path);CHECK(editor_fresh);
    editor.open(running,*editor_fresh.config);CHECK(!editor.loaded_changes_from_disk() && !editor.take_action());editor.cancel();editor.take_action();
    // Valid external edits can open immediately. Preview is reversible; shell and
    // shortcut adoption remains the caller's explicit successful-Save operation.
    write(editor_path,serialize_config(external,Platform::windows));
    editor_fresh=load_settings_snapshot(editor_path);CHECK(editor_fresh);editor.open(running,*editor_fresh.config);
    CHECK(editor.loaded_changes_from_disk() && !editor.dirty());
    const auto loaded_preview=editor.take_action();CHECK(loaded_preview && loaded_preview->kind==SettingsActionKind::preview);
    CHECK(editor.original()==running && loaded_preview->config.default_profile=="external");
    editor.cancel();const auto rollback=editor.take_action();CHECK(rollback && rollback->config==running);
    CHECK(read(editor_path)==serialize_config(external,Platform::windows));
    // Reopen+Save adopts that disk value using the same exact freshly read token.
    editor_fresh=load_settings_snapshot(editor_path);CHECK(editor_fresh);editor.open(running,*editor_fresh.config);
    const auto adopt_external=commit_action();auto adopted=save_settings_config(editor_path,adopt_external.config,*editor_fresh.snapshot);CHECK(adopted);
    editor.persistence_result(true);running=adopt_external.config;
    editor_fresh=load_settings_snapshot(editor_path);CHECK(editor_fresh);editor.open(running,*editor_fresh.config);
    CHECK(!editor.loaded_changes_from_disk() && !editor.dirty());editor.cancel();editor.take_action();
    // A change DURING editing still conflicts. Failed Save preserves the draft;
    // Cancel then reopen obtains a new token without restart or silent overwrite.
    editor_fresh=load_settings_snapshot(editor_path);CHECK(editor_fresh);editor.open(running,*editor_fresh.config);
    for (unsigned i=0;i<256 && editor.focused_control()!="font.increase";++i) editor.key({{"TAB"}});
    CHECK(editor.focused_control()=="font.increase");editor.key({{"ENTER"}});const auto unsaved=editor.draft();
    auto newer=external;newer.font.size_dip=28;write(editor_path,serialize_config(newer,Platform::windows));
    const auto stale_action=commit_action();const auto stale_save=save_settings_config(editor_path,stale_action.config,*editor_fresh.snapshot);
    CHECK(!stale_save && stale_save.conflict);editor.persistence_result(false,stale_save.error);
    CHECK(editor.is_open() && editor.dirty() && editor.draft()==unsaved);
    CHECK(read(editor_path)==serialize_config(newer,Platform::windows));
    editor.cancel();CHECK(editor.take_action()->config==running);
    editor_fresh=load_settings_snapshot(editor_path);CHECK(editor_fresh);editor.open(running,*editor_fresh.config);
    CHECK(editor.draft().font.size_dip==28 && editor.loaded_changes_from_disk());
    const auto recovered_action=commit_action();const auto recovered=save_settings_config(editor_path,recovered_action.config,*editor_fresh.snapshot);CHECK(recovered);
    editor.persistence_result(true);running=recovered_action.config;
    // Invalid-file reads do not create an editor/token. Repair permits next open.
    write(editor_path,"version = 1\n[font]\nsize_dip = invalid\n");
    const auto invalid_read=load_settings_snapshot(editor_path);CHECK(!invalid_read && !invalid_read.snapshot && !invalid_read.error.empty());CHECK(!editor.is_open());
    write(editor_path,serialize_config(running,Platform::windows));editor_fresh=load_settings_snapshot(editor_path);CHECK(editor_fresh);
    editor.open(running,*editor_fresh.config);CHECK(editor.is_open() && !editor.dirty());editor.cancel();editor.take_action();
    CHECK(!has_temporaries(directory.path));

    // Read-only files and concurrent writer handles fail safely.
    auto writable = load_settings_snapshot(path); CHECK(writable);
    const auto unchanged = read(path);
    CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
    CHECK(!save_settings_config(path, config, *writable.snapshot)); CHECK(read(path) == unchanged);
    CHECK(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
    HANDLE busy = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(busy != INVALID_HANDLE_VALUE);
    auto busy_result = save_settings_config(path, config, *writable.snapshot);
    CloseHandle(busy);
    CHECK(!busy_result); CHECK(read(path) == unchanged); CHECK(!has_temporaries(directory.path));
    // This reader permits our snapshot reads but prevents the final rename.
    // It exercises cleanup AFTER the staged file was written and flushed.
    HANDLE reader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(reader != INVALID_HANDLE_VALUE);
    auto rename_blocked = save_settings_config(path, config, *writable.snapshot);
    CloseHandle(reader);
    CHECK(!rename_blocked); CHECK(read(path) == unchanged); CHECK(!has_temporaries(directory.path));

    // Hard links are refused, including on Save; no alias can be overwritten.
    const auto hardlink = directory.path / L"alias.stormtty";
    CHECK(CreateHardLinkW(hardlink.c_str(), path.c_str(), nullptr));
    CHECK(!load_settings_snapshot(path)); CHECK(!save_settings_config(path, config, *writable.snapshot));
    CHECK(read(path) == unchanged); CHECK(read(hardlink) == unchanged);
    CHECK(DeleteFileW(hardlink.c_str()));

    // Symlink creation may require Developer Mode/admin rights. Exercise file
    // and directory-reparse refusal where available, report an explicit skip otherwise.
    const auto link = directory.path / L"linked.stormtty";
    if (CreateSymbolicLinkW(link.c_str(), path.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        CHECK(!load_settings_snapshot(link));
        ConfigFileSnapshot link_token; link_token.path = link;
        CHECK(!save_settings_config(link, config, link_token)); CHECK(read(path) == unchanged);
        CHECK(DeleteFileW(link.c_str()));
    } else {
        const auto error = GetLastError();
        CHECK(error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED);
        std::cout << "SKIP file symlink fixture: Windows privilege/filesystem unavailable\n";
    }
    const auto linked_directory = directory.path / L"linked-directory";
    if (CreateSymbolicLinkW(linked_directory.c_str(), path.parent_path().c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        const auto linked_path = linked_directory / path.filename();
        CHECK(!load_settings_snapshot(linked_path));
        ConfigFileSnapshot link_token; link_token.path = linked_path;
        CHECK(!save_settings_config(linked_path, config, link_token)); CHECK(read(path) == unchanged);
        CHECK(RemoveDirectoryW(linked_directory.c_str()));
    } else {
        const auto error = GetLastError();
        CHECK(error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED);
        std::cout << "SKIP directory symlink fixture: Windows privilege/filesystem unavailable\n";
    }

    for (const auto* unsafe : {L"relative.stormtty", L"C:relative.stormtty", L"\\\\server\\share\\config.stormtty", L"\\\\?\\C:\\config.stormtty",
                              L"C:\\safe\\..\\config.stormtty", L"C:\\safe\\config.stormtty:stream", L"C:\\safe\\NUL", L"C:\\safe\\name. "})
        check_rejected(unsafe, config);
    const auto malformed = directory.path / L"malformed.stormtty";
    write(malformed, "version = 99\n"); CHECK(!load_settings_snapshot(malformed)); CHECK(read(malformed) == "version = 99\n");
    write(malformed, std::string(1024 * 1024 + 1, 'x')); CHECK(!load_settings_snapshot(malformed));
    const auto directory_target = directory.path / L"directory-target";
    CHECK(std::filesystem::create_directory(directory_target)); check_rejected(directory_target, config);

    // Discovery must return real absolute executable candidates without running
    // them, touching defaults/custom profiles, querying WSL, or changing cwd.
    const auto old_cwd = std::filesystem::current_path();
    const auto discovered = discover_windows_shells();
    CHECK(std::filesystem::current_path() == old_cwd);
    CHECK(discovered.profiles.size() <= 4);
    std::set<std::string> ids;
    for (const auto& profile : discovered.profiles) {
        CHECK(ids.insert(profile.id).second); CHECK(profile.working_directory.empty());
        const std::u8string encoded(profile.executable.begin(), profile.executable.end());
        const std::filesystem::path native(encoded);
        CHECK(native.is_absolute()); CHECK(std::filesystem::is_regular_file(native));
        CHECK(profile.id != "wsl");
        auto candidate = defaults; candidate.profiles.push_back(profile); CHECK(validate_config(candidate, Platform::windows));
        if (profile.id == "git-bash") {
            CHECK(native.filename() == L"bash.exe"); CHECK(native.parent_path().filename() == L"bin");
            CHECK(profile.arguments == std::vector<std::string>({"--login", "-i"}));
            CHECK(profile.environment.at("CHERE_INVOKING") == "1");
        }
    }
    CHECK(ids.contains("windows-cmd"));
    const auto has_cmd=[](const ShellDiscoveryResult& result) {
        return std::any_of(result.profiles.begin(),result.profiles.end(),[](const auto& p){return p.id=="windows-cmd";});
    };
    const auto cmd=std::find_if(discovered.profiles.begin(),discovered.profiles.end(),[](const auto& p){return p.id=="windows-cmd";});
    CHECK(cmd->arguments==std::vector<std::string>{"/d"});
    const auto preserved=defaults;
    CHECK(!has_cmd(discover_windows_shells(defaults.profiles)));
    auto equivalent=defaults.profiles;
    equivalent.front().arguments={"/d"}; equivalent.front().executable="CMD.EXE";
    CHECK(!has_cmd(discover_windows_shells(equivalent)));
    auto distinct=defaults.profiles; distinct.front().arguments={"/q"};
    CHECK(has_cmd(discover_windows_shells(distinct)));
    distinct=defaults.profiles; distinct.front().environment["CUSTOM"]="1";
    CHECK(has_cmd(discover_windows_shells(distinct)));
    distinct=defaults.profiles; distinct.front().working_directory="C:\\custom";
    CHECK(has_cmd(discover_windows_shells(distinct)));
    distinct=defaults.profiles; distinct.front().executable="C:\\custom\\cmd.exe";
    CHECK(has_cmd(discover_windows_shells(distinct)));
    CHECK(defaults==preserved);
    CHECK(defaults.default_profile == "default" && defaults.profiles.size() == 1);
    CHECK(!has_temporaries(directory.path));
}); }
#else
int main() {
    std::cout << "SKIP settings_services: Windows runtime required (portable header compilation only)\n";
    return 0;
}
#endif
