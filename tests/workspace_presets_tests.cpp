#include "core_test.hpp"
#include "stormtty/workspace_presets.hpp"
#include "stormtty/workspace_save_signal.hpp"
#include <random>
using namespace stormtty;
int main() {
    return run_core_tests("workspace_presets", [] {
        WorkspaceSaveSignal save_signal;
        CHECK(!save_signal.pending()); CHECK(!save_signal.take_pending());
        for (int i=0;i<100000;++i) save_signal.mark_changed();
        CHECK(save_signal.pending()); CHECK(save_signal.take_pending());
        for (int i=0;i<100000;++i) CHECK(!save_signal.take_pending());
        save_signal.mark_changed(); CHECK(save_signal.take_pending()); CHECK(!save_signal.pending());
        ProjectWorkspaceCatalog live;
        Workspace layout;
        const auto tab = layout.create_tab({42}, "zsh"); CHECK(tab);
        CHECK(layout.rename_tab(*tab.value, "Build"));
        CHECK(layout.set_pane_title(layout.active_pane(), "private process title"));
        CHECK(layout.split(layout.active_pane(), Direction::right, {43}, "powershell", 0.6));
        const auto first = live.create({"Storm", "C:/Users/Storm/workspace", "Local", "code", "#6aB8ff", "zsh"}, layout); CHECK(first);
        const auto second = live.create({"Docs", "/Users/storm/docs", "Local", "book", "#ffcc00", "zsh"}); CHECK(second);
        CHECK(live.activate(*second.value));
        auto saved = capture_workspace_snapshot(live); CHECK(saved);
        CHECK(saved.value->active_index == 1);
        CHECK(saved.value->projects[0].metadata.persistent_id.size() == 32);
        for (const auto& [id, pane] : saved.value->projects[0].layout.state().panes) {
            (void)id; CHECK(!pane.session); CHECK(pane.title.empty());
        }
        CHECK(saved.value->projects[0].layout.state().tabs[0].title == "Build");
        WorkspaceLibrary library; library.last_session = *saved.value;
        CHECK(save_workspace_preset(library, "Development", *saved.value));
        CHECK(save_workspace_preset(library, "Docs", *saved.value));
        auto encoded = serialize_workspace_library(library); CHECK(encoded);
        CHECK(encoded.value->find("private process title") == std::string::npos);
        auto parsed = deserialize_workspace_library(*encoded.value); CHECK(parsed);
        CHECK(serialize_workspace_library(*parsed.value).value == encoded.value);
        ProjectWorkspaceCatalog restored;
        CHECK(restore_workspace_snapshot(restored, parsed.value->last_session));
        CHECK(restored.entries().size() == 2); CHECK(restored.active()->metadata.name == "Docs");
        CHECK(restored.entries()[0].metadata == live.entries()[0].metadata);
        CHECK(restored.entries()[0].id != live.entries()[0].id);
        CHECK(restored.all_session_ids().empty());
        CHECK(!restore_workspace_snapshot(live, parsed.value->last_session));
        CHECK(live.all_session_ids().size() == 2);
        CHECK(!save_workspace_preset(library, "Development", *saved.value));
        CHECK(serialize_workspace_library(library).value == encoded.value);
        CHECK(save_workspace_preset(library, "Development", {}, true));
        CHECK(library.presets.size() == 2 && library.presets[0].snapshot.projects.empty());
        CHECK(remove_workspace_preset(library, "Docs")); CHECK(!remove_workspace_preset(library, "Missing"));
        auto invalid = *saved.value;
        invalid.active_index = 99; CHECK(!restore_workspace_snapshot(restored, invalid));
        CHECK(restored.entries().size() == 2);
        invalid = *saved.value; invalid.projects[0].metadata.icon = "../../icon.svg"; CHECK(!validate_workspace_snapshot(invalid));
        invalid = *saved.value; invalid.projects[0].metadata.accent = "#fff"; CHECK(!validate_workspace_snapshot(invalid));
        invalid = *saved.value; invalid.projects[0].metadata.name = "bad\x1b"; CHECK(!validate_workspace_snapshot(invalid));
        invalid = *saved.value; invalid.projects[1].metadata.persistent_id = invalid.projects[0].metadata.persistent_id; CHECK(!validate_workspace_snapshot(invalid));
        invalid = *saved.value; invalid.projects[0].layout.bind_session(invalid.projects[0].layout.active_pane(), {22}); CHECK(!validate_workspace_snapshot(invalid));
        CHECK(!deserialize_workspace_library(*encoded.value + "junk"));
        CHECK(!deserialize_workspace_library("STORMTTY-WORKSPACES 2\n"));
        CHECK(!deserialize_workspace_library(std::string(WorkspaceLibrary::maximum_bytes + 1, 'x')));
        for (std::size_t size = 0; size + 1 < encoded.value->size(); ++size) CHECK(!deserialize_workspace_library(std::string_view(*encoded.value).substr(0, size)));
        std::mt19937 random(53);
        for (int i = 0; i < 300; ++i) {
            auto corrupt = *encoded.value;
            corrupt[random() % corrupt.size()] = static_cast<char>(random() % 256);
            auto result = deserialize_workspace_library(corrupt);
            if (result) CHECK(serialize_workspace_library(*result.value));
        }
        auto escaped = *saved.value;
        escaped.projects[0].metadata.working_directory = std::string(ProjectWorkspaceCatalog::maximum_directory_bytes, '\\');
        WorkspaceLibrary escaped_library; escaped_library.last_session = escaped;
        auto escaped_bytes = serialize_workspace_library(escaped_library); CHECK(escaped_bytes);
        CHECK(deserialize_workspace_library(*escaped_bytes.value));
        // The established v1 individual-layout format remains readable unchanged.
        CHECK(deserialize_workspace(serialize_workspace(layout)));
    });
}
