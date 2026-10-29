#include "stormtty/workspace_presets.hpp"
#include "text.hpp"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>

namespace stormtty {
namespace {
bool valid_name(std::string_view name) {
    return !name.empty() && name.size() <= 256 && detail::valid_utf8(name) &&
        name.find_first_not_of(' ') != std::string_view::npos &&
        std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
Status append_snapshot(ProjectWorkspaceCatalog& catalog, const WorkspaceSnapshot& saved) {
    if (saved.projects.size() > ProjectWorkspaceCatalog::maximum_projects ||
        (saved.projects.empty() ? saved.active_index != 0 : saved.active_index >= saved.projects.size()))
        return Status::failure("Invalid saved workspace count or active selection");
    for (std::size_t index = 0; index < saved.projects.size(); ++index) {
        const auto& project = saved.projects[index];
        if (project.metadata.persistent_id.empty()) return Status::failure("Saved workspace identity is missing");
        for (const auto& [id, pane] : project.layout.state().panes) {
            (void)id;
            if (pane.session) return Status::failure("Saved layouts cannot contain live sessions");
        }
        auto created = catalog.create(project.metadata, project.layout);
        if (!created) return Status::failure(created.error);
        if (index == saved.active_index) catalog.activate(*created.value);
    }
    return {};
}
bool write_snapshot(std::ostream& out, const WorkspaceSnapshot& snapshot) {
    out << "snapshot " << snapshot.projects.size() << ' ' << snapshot.active_index << '\n';
    for (const auto& project : snapshot.projects) {
        const auto& m = project.metadata;
        out << "project " << std::quoted(m.name) << ' ' << std::quoted(m.working_directory) << ' '
            << std::quoted(m.environment_label) << ' ' << std::quoted(m.icon) << ' '
            << std::quoted(m.accent) << ' ' << std::quoted(m.preferred_profile) << ' ' << std::quoted(m.persistent_id) << '\n';
        const auto bytes = serialize_workspace(project.layout);
        if (bytes.size() + static_cast<std::size_t>(out.tellp()) > WorkspaceLibrary::maximum_bytes) return false;
        out << "layout " << bytes.size() << '\n' << bytes;
    }
    return true;
}
bool quoted(std::istream& in, std::string& value) {
    in >> std::ws;
    if (in.peek() != '"') return false;
    return static_cast<bool>(in >> std::quoted(value));
}
bool finished(std::istream& row) { row >> std::ws; return row.eof(); }
bool record(std::istream& input, std::istringstream& row, std::string_view expected) {
    std::string line, kind;
    if (!std::getline(input, line) || line.size() > 131072) return false;
    row.clear(); row.str(line); row.imbue(std::locale::classic());
    return (row >> kind) && kind == expected;
}
bool read_snapshot(std::istream& input, WorkspaceSnapshot& snapshot) {
    std::istringstream row;
    std::size_t count{};
    if (!record(input, row, "snapshot") || !(row >> count >> snapshot.active_index) || !finished(row) ||
        count > ProjectWorkspaceCatalog::maximum_projects) return false;
    for (std::size_t index = 0; index < count; ++index) {
        SavedProjectWorkspace project;
        auto& m = project.metadata;
        if (!record(input, row, "project") || !quoted(row, m.name) || !quoted(row, m.working_directory) ||
            !quoted(row, m.environment_label) || !quoted(row, m.icon) || !quoted(row, m.accent) ||
            !quoted(row, m.preferred_profile) || !quoted(row, m.persistent_id) || !finished(row)) return false;
        std::size_t length{};
        if (!record(input, row, "layout") || !(row >> length) || !finished(row) ||
            !length || length > WorkspaceLibrary::maximum_bytes) return false;
        std::string bytes(length, '\0');
        if (!input.read(bytes.data(), static_cast<std::streamsize>(length))) return false;
        auto parsed = deserialize_workspace(bytes);
        if (!parsed) return false;
        project.layout = std::move(*parsed.value);
        snapshot.projects.push_back(std::move(project));
    }
    return static_cast<bool>(validate_workspace_snapshot(snapshot));
}
} // namespace

Result<WorkspaceSnapshot> capture_workspace_snapshot(const ProjectWorkspaceCatalog& catalog) {
    if (auto status = catalog.validate(); !status) return {{}, status.error};
    WorkspaceSnapshot result;
    for (const auto& project : catalog.entries()) {
        auto state = project.workspace.state();
        for (auto& [id, pane] : state.panes) {
            (void)id;
            pane.session = {};
            pane.title.clear(); // transient process titles are not user layout intent
        }
        auto layout = Workspace::from_state(std::move(state));
        if (!layout) return {{}, layout.error};
        if (project.id == catalog.active_id()) result.active_index = result.projects.size();
        result.projects.push_back({project.metadata, std::move(*layout.value)});
    }
    return {std::move(result), {}};
}
Status validate_workspace_snapshot(const WorkspaceSnapshot& snapshot) {
    ProjectWorkspaceCatalog prepared;
    return append_snapshot(prepared, snapshot);
}
Status restore_workspace_snapshot(ProjectWorkspaceCatalog& catalog, const WorkspaceSnapshot& snapshot) {
    ProjectWorkspaceCatalog prepared;
    if (auto status = append_snapshot(prepared, snapshot); !status) return status;
    return catalog.replace_unbound(prepared);
}
Result<std::string> serialize_workspace_library(const WorkspaceLibrary& library) {
    if (library.presets.size() > WorkspaceLibrary::maximum_presets) return {{}, "Too many workspace presets"};
    if (auto status = validate_workspace_snapshot(library.last_session); !status) return {{}, status.error};
    std::set<std::string> names;
    for (const auto& preset : library.presets) {
        if (!valid_name(preset.name) || !names.insert(preset.name).second) return {{}, "Invalid or duplicate workspace preset name"};
        if (auto status = validate_workspace_snapshot(preset.snapshot); !status) return {{}, status.error};
    }
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "STORMTTY-WORKSPACES 1\nrestore " << library.restore_on_launch << '\n';
    if (!write_snapshot(out, library.last_session)) return {{}, "Workspace library exceeds 1 MiB"};
    for (const auto& preset : library.presets) {
        out << "preset " << std::quoted(preset.name) << '\n';
        if (!write_snapshot(out, preset.snapshot)) return {{}, "Workspace library exceeds 1 MiB"};
        if (out.tellp() > static_cast<std::streamoff>(WorkspaceLibrary::maximum_bytes)) return {{}, "Workspace library exceeds 1 MiB"};
    }
    out << "end\n";
    auto bytes = out.str();
    if (bytes.size() > WorkspaceLibrary::maximum_bytes) return {{}, "Workspace library exceeds 1 MiB"};
    return {std::move(bytes), {}};
}
Result<WorkspaceLibrary> deserialize_workspace_library(std::string_view bytes) {
    auto fail = []() -> Result<WorkspaceLibrary> { return {{}, "Invalid or unsupported workspace library"}; };
    if (bytes.size() > WorkspaceLibrary::maximum_bytes || !detail::valid_document_text(bytes)) return fail();
    std::istringstream input{std::string(bytes)}, row;
    input.imbue(std::locale::classic());
    std::string line;
    if (!std::getline(input, line) || line != "STORMTTY-WORKSPACES 1") return fail();
    WorkspaceLibrary library;
    unsigned restore{};
    if (!record(input, row, "restore") || !(row >> restore) || restore > 1 || !finished(row)) return fail();
    library.restore_on_launch = restore != 0;
    if (!read_snapshot(input, library.last_session)) return fail();
    bool end{};
    while (std::getline(input, line)) {
        if (line == "end") { end = true; break; }
        row.clear(); row.str(line);
        std::string kind, name;
        if (!(row >> kind) || kind != "preset" || !quoted(row, name) || !finished(row) ||
            library.presets.size() >= WorkspaceLibrary::maximum_presets) return fail();
        WorkspaceSnapshot snapshot;
        if (!read_snapshot(input, snapshot)) return fail();
        library.presets.push_back({std::move(name), std::move(snapshot)});
    }
    if (!end || input.peek() != std::char_traits<char>::eof() || !serialize_workspace_library(library)) return fail();
    return {std::move(library), {}};
}
Status save_workspace_preset(WorkspaceLibrary& library, std::string name, WorkspaceSnapshot snapshot, bool replace) {
    auto next = library;
    auto it = std::find_if(next.presets.begin(), next.presets.end(), [&](const auto& p) { return p.name == name; });
    if (it != next.presets.end()) {
        if (!replace) return Status::failure("Workspace preset already exists");
        it->snapshot = std::move(snapshot);
    } else next.presets.push_back({std::move(name), std::move(snapshot)});
    auto encoded = serialize_workspace_library(next);
    if (!encoded) return Status::failure(encoded.error);
    library = std::move(next);
    return {};
}
Status remove_workspace_preset(WorkspaceLibrary& library, std::string_view name) {
    const auto it = std::find_if(library.presets.begin(), library.presets.end(), [&](const auto& p) { return p.name == name; });
    if (it == library.presets.end()) return Status::failure("Workspace preset does not exist");
    library.presets.erase(it);
    return {};
}
} // namespace stormtty
