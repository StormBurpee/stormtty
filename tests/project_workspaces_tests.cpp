#include "core_test.hpp"
#include "stormtty/project_workspaces.hpp"
#include "stormtty/parked_attention.hpp"

#include <algorithm>
#include <map>
#include <limits>
#include <random>
#include <set>
#include <type_traits>

using namespace stormtty;

static_assert(!std::is_convertible_v<ProjectWorkspaceId, PaneId>);
static_assert(!std::is_convertible_v<ProjectWorkspaceId, SessionId>);
static_assert(!std::is_convertible_v<ProjectPaneReference, ProjectTabReference>);
static_assert(!std::is_copy_constructible_v<ProjectWorkspaceCatalog>);
static_assert(!std::is_move_assignable_v<ProjectWorkspaceCatalog>);

namespace {
struct ProjectSnapshot {
    ProjectWorkspaceId id;
    ProjectWorkspaceMetadata metadata;
    WorkspaceState workspace;
    std::vector<ParkedSession> parked;
    bool operator==(const ProjectSnapshot&) const = default;
};
struct CatalogSnapshot {
    ProjectWorkspaceId active;
    std::vector<ProjectSnapshot> entries;
    bool operator==(const CatalogSnapshot&) const = default;
};
CatalogSnapshot snapshot(const ProjectWorkspaceCatalog& catalog) {
    CatalogSnapshot result{catalog.active_id(), {}};
    for (const auto& project : catalog.entries())
        result.entries.push_back({project.id, project.metadata, project.workspace.state(), project.parked_sessions});
    return result;
}
ProjectWorkspaceId create(ProjectWorkspaceCatalog& catalog, std::string name, Workspace workspace = {}) {
    auto result = catalog.create({std::move(name), "/work/project", "Local"}, std::move(workspace));
    CHECK(result);
    return *result.value;
}
Workspace populated(std::uint64_t first_session) {
    Workspace workspace;
    auto tab = workspace.create_tab({first_session}); CHECK(tab);
    CHECK(workspace.rename_tab(*tab.value, "Build and test"));
    auto pane = workspace.active_pane();
    CHECK(workspace.split(pane, Direction::right, {first_session + 1}));
    CHECK(workspace.set_pane_title(pane, "Compiler output"));
    CHECK(workspace.create_tab({first_session + 2}));
    CHECK(workspace.rename_tab(workspace.active_tab(), "Review"));
    CHECK(workspace.set_zoom(pane, true));
    return workspace;
}

void lifecycle_and_hidden_sessions() {
    ProjectWorkspaceCatalog catalog;
    CHECK(catalog.validate()); CHECK(catalog.entries().empty()); CHECK(!catalog.active_id());
    CHECK(!catalog.active()); CHECK(!catalog.active_workspace()); CHECK(!catalog.workspace({}));
    CHECK(!catalog.find({})); CHECK(!catalog.contains_session({}));
    CHECK(catalog.all_session_ids().empty()); CHECK(catalog.session_ids({999}).empty());

    auto source = populated(100);
    const auto source_state = source.state();
    const auto first = create(catalog, "StormTTY", std::move(source));
    CHECK(catalog.active_id() == first); CHECK(catalog.active()->id == first);
    CHECK(catalog.active_workspace()->state() == source_state);
    CHECK(catalog.session_ids(first) == std::vector<SessionId>({{100}, {101}, {102}}));

    const auto second = create(catalog, "Terminal research", populated(200));
    const auto second_state = catalog.workspace(second)->state();
    CHECK(first != second); CHECK(catalog.entries().size() == 2);
    CHECK(catalog.entries()[0].id == first); CHECK(catalog.entries()[1].id == second);
    CHECK(catalog.active_id() == first); // No accidental navigation on create.
    const auto sessions = catalog.all_session_ids();
    CHECK(sessions == std::vector<SessionId>({{100}, {101}, {102}, {200}, {201}, {202}}));
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(catalog.activate(second)); CHECK(catalog.active_workspace()->state() == second_state);
        CHECK(catalog.contains_session({100})); // Hidden is not unused.
        CHECK(catalog.activate(first)); CHECK(catalog.active_workspace()->state() == source_state);
        CHECK(catalog.contains_session({200}));
        CHECK(catalog.all_session_ids() == sessions); CHECK(catalog.validate());
    }
    // Actors may update a hidden project without redirecting the active project.
    const auto hidden_pane = catalog.workspace(second)->active_pane();
    CHECK(catalog.workspace(second)->set_pane_title(hidden_pane, "Background build complete"));
    CHECK(catalog.active_id() == first); CHECK(catalog.workspace(first)->state() == source_state);

    const ProjectWorkspaceCatalog& view = catalog;
    CHECK(view.active_workspace() == view.workspace(first));
    CHECK(view.active_workspace()->state() == source_state);
    CHECK(view.workspace(second)->pane(hidden_pane)->title == "Background build complete");
    CHECK(catalog.rename(first, "StormTTY native"));
    CHECK(catalog.find(first)->metadata.name == "StormTTY native");
    CHECK(catalog.update_metadata(first, {"StormTTY native", "C:\\Work\\StormTTY", "Windows / PowerShell"}));
    CHECK(catalog.workspace(first)->state() == source_state); CHECK(catalog.all_session_ids() == sessions);
    CHECK(catalog.find(first)->metadata.working_directory == "C:\\Work\\StormTTY");
}

void identity_domains() {
    ProjectWorkspaceCatalog one, two;
    const auto a = create(one, "Same display name", populated(1));
    const auto b = create(one, "Same display name", populated(4));
    const auto c = create(two, "Same display name", populated(1));
    CHECK(a != b); CHECK(a != c); CHECK(b != c); CHECK(one.validate()); CHECK(two.validate());
    CHECK(!one.find(c)); CHECK(!two.find(a));
    CHECK(one.workspace(a)->active_pane() == one.workspace(b)->active_pane());
    const ProjectPaneReference ap{a, one.workspace(a)->active_pane()};
    const ProjectPaneReference bp{b, one.workspace(b)->active_pane()};
    const ProjectPaneReference cp{c, two.workspace(c)->active_pane()};
    CHECK(ap); CHECK(ap != bp); CHECK(ap != cp); CHECK(bp != cp);
    CHECK(!ProjectPaneReference{a, {}}); CHECK(!ProjectPaneReference{{}, ap.local});
    CHECK(ProjectSessionReference{a, {1}} != ProjectSessionReference{c, {1}});
    CHECK(ProjectTabReference{a, one.workspace(a)->active_tab()} !=
          ProjectTabReference{b, one.workspace(b)->active_tab()});
    CHECK(ProjectNodeReference{a, one.workspace(a)->state().tabs.front().root} !=
          ProjectNodeReference{b, one.workspace(b)->state().tabs.front().root});
    auto removed = one.remove(a); CHECK(removed);
    auto imported = one.create(removed.value->project.metadata, std::move(removed.value->project.workspace));
    CHECK(imported); CHECK(*imported.value != a); CHECK(*imported.value != b); CHECK(*imported.value != c);
    CHECK(!one.find(a)); CHECK(one.validate());

    std::set<ProjectWorkspaceId> seen{a, b, c, *imported.value};
    for (unsigned i = 0; i < 100; ++i) {
        ProjectWorkspaceCatalog temporary;
        CHECK(seen.insert(create(temporary, "Temporary")).second);
    }
    CHECK(seen.size() == 104); // Catalog destruction never reissues an ID.
}

void deliberate_removal() {
    ProjectWorkspaceCatalog catalog;
    const auto a = create(catalog, "One", populated(10));
    const auto b = create(catalog, "Two", populated(20));
    const auto c = create(catalog, "Three", populated(30));
    const auto d = create(catalog, "Four");
    CHECK(catalog.activate(b));
    const auto b_state = catalog.workspace(b)->state();
    const auto preview = catalog.session_ids(b);
    const auto before = snapshot(catalog);
    // Canceling a host confirmation is simply no mutation.
    CHECK(snapshot(catalog) == before);
    auto removed = catalog.remove(b); CHECK(removed);
    CHECK(removed.value->project.id == b); CHECK(removed.value->project.metadata.name == "Two");
    CHECK(removed.value->project.workspace.state() == b_state); CHECK(removed.value->sessions == preview);
    CHECK(removed.value->sessions == std::vector<SessionId>({{20}, {21}, {22}}));
    CHECK(catalog.active_id() == c); CHECK(!catalog.find(b)); CHECK(!catalog.contains_session({20}));
    CHECK(catalog.entries()[0].id == a); CHECK(catalog.entries()[1].id == c); CHECK(catalog.entries()[2].id == d);
    CHECK(catalog.remove(a)); CHECK(catalog.active_id() == c); // Hidden removal preserves focus.
    CHECK(catalog.activate(d));
    auto empty = catalog.remove(d); CHECK(empty); CHECK(empty.value->sessions.empty());
    CHECK(catalog.active_id() == c); // Last row falls back to previous row.
    CHECK(catalog.remove(c)); CHECK(!catalog.active_id()); CHECK(!catalog.active_workspace());
    CHECK(catalog.entries().empty()); CHECK(catalog.validate()); CHECK(catalog.all_session_ids().empty());
    // Detached state still has its associations after the catalog becomes empty.
    CHECK(removed.value->project.workspace.state() == b_state);
    const auto next = create(catalog, "Next"); CHECK(next != a); CHECK(next != b); CHECK(next != c); CHECK(next != d);
    CHECK(catalog.active_id() == next);
}

void failures_are_transactional() {
    ProjectWorkspaceCatalog catalog;
    const auto id = create(catalog, "Native", populated(1000));
    auto before = snapshot(catalog);
    CHECK(!catalog.activate({})); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.activate({9999999})); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.rename({}, "Orphan")); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.remove({})); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.remove({9999999})); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.update_metadata({}, {"Orphan", {}, {}})); CHECK(snapshot(catalog) == before);
    const std::vector<std::string> bad_names{
        "", "   ", "line\nbreak", "tab\tname", "escape\x1b", "delete\x7f",
        std::string("bad\0name", 8), std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
        std::string(ProjectWorkspaceCatalog::maximum_name_bytes + 1, 'a')};
    for (const auto& name : bad_names) {
        CHECK(!catalog.rename(id, name)); CHECK(snapshot(catalog) == before);
        CHECK(!catalog.create({name, {}, {}})); CHECK(snapshot(catalog) == before);
    }
    CHECK(!catalog.update_metadata(id, {"Native", "bad\ndirectory", "Local"})); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.update_metadata(id, {"Native", "/work", "bad\tlabel"})); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.create({"Native", std::string(ProjectWorkspaceCatalog::maximum_directory_bytes + 1, 'x'), {}}));
    CHECK(snapshot(catalog) == before);
    CHECK(!catalog.create({"Native", {}, std::string(ProjectWorkspaceCatalog::maximum_environment_bytes + 1, 'x')}));
    CHECK(snapshot(catalog) == before);
    CHECK(!catalog.create({"Native", std::string("\xf4\x90\x80\x80", 4), {}})); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.create({"Native", {}, std::string("\xe2\x82", 2)})); CHECK(snapshot(catalog) == before);

    // Workspace accepts a session once; catalog also rejects duplicate imports.
    Workspace duplicate; CHECK(duplicate.create_tab({1001}));
    const auto duplicate_state = duplicate.state();
    CHECK(!catalog.create({"Duplicate session", {}, {}}, duplicate));
    CHECK(snapshot(catalog) == before); CHECK(duplicate.state() == duplicate_state);
    const auto other = create(catalog, "Other", populated(2000));
    // Existing Workspace mutation is deliberately retained, not wrapped in a
    // second tab/split API. Cross-project binding is a host precondition checked
    // by validate(), and is readily detected and repaired without process work.
    const auto other_pane = catalog.workspace(other)->active_pane();
    const auto original_session = catalog.workspace(other)->pane(other_pane)->session;
    CHECK(catalog.workspace(other)->bind_session(other_pane, {1000}));
    CHECK(catalog.workspace(other)->validate()); CHECK(!catalog.validate());
    CHECK(catalog.workspace(other)->bind_session(other_pane, original_session)); CHECK(catalog.validate());
}

void bounds_and_unbound_workspaces() {
    ProjectWorkspaceCatalog catalog;
    ProjectWorkspaceMetadata maximum{
        std::string(ProjectWorkspaceCatalog::maximum_name_bytes, 'n'),
        std::string(ProjectWorkspaceCatalog::maximum_directory_bytes, 'd'),
        std::string(ProjectWorkspaceCatalog::maximum_environment_bytes, 'e')};
    auto first = catalog.create(maximum); CHECK(first); CHECK(catalog.find(*first.value)->metadata == maximum);
    CHECK(catalog.update_metadata(*first.value, {"开发 — Native", "/Users/dev/项目", "macOS — Local"}));
    CHECK(catalog.update_metadata(*first.value, {"Default directory", {}, {}}));
    Workspace unbound; CHECK(unbound.create_tab()); CHECK(unbound.split(unbound.active_pane(), Direction::down));
    for (std::size_t i = 1; i < ProjectWorkspaceCatalog::maximum_projects; ++i)
        CHECK(catalog.create({"Unbound " + std::to_string(i), {}, {}}, unbound));
    CHECK(catalog.validate()); CHECK(catalog.all_session_ids().empty()); CHECK(!catalog.contains_session({}));
    CHECK(catalog.entries().size() == ProjectWorkspaceCatalog::maximum_projects);
    const auto before = snapshot(catalog);
    CHECK(!catalog.create({"Over limit", {}, {}})); CHECK(snapshot(catalog) == before);
    CHECK(catalog.remove(catalog.entries()[20].id));
    CHECK(catalog.create({"Available again", {}, {}})); CHECK(catalog.validate());
}

void parked_ownership_and_identity() {
    ProjectWorkspaceCatalog catalog;
    Workspace source;
    CHECK(source.create_tab({41}, "shell-profile"));
    const auto old_pane = source.active_pane();
    CHECK(source.set_pane_title(old_pane, "Build output"));
    const auto a = create(catalog, "One", source);
    const auto b = create(catalog, "Two", populated(100));
    const auto b_before = catalog.workspace(b)->state();
    CHECK(catalog.activate(b));
    const auto sessions = catalog.all_session_ids();
    CHECK(catalog.park({a, old_pane}).value == std::optional<SessionId>{{41}});
    CHECK(catalog.active_id() == b);
    CHECK(catalog.find(a)); CHECK(catalog.workspace(a)->state().tabs.empty());
    CHECK(catalog.workspace(a)->validate()); CHECK(catalog.validate());
    CHECK(catalog.find(a)->parked_sessions == std::vector<ParkedSession>({{{41}, "shell-profile", "Build output"}}));
    CHECK(catalog.all_session_ids() == sessions); CHECK(catalog.contains_session({41}));
    CHECK(catalog.session_ids(a) == std::vector<SessionId>({{41}}));
    const auto before = snapshot(catalog);
    CHECK(!catalog.park({a, old_pane})); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.park({{}, old_pane})); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.reattach_to_new_tab({b, {41}})); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.reattach_to_new_tab({a, {}})); CHECK(snapshot(catalog) == before);
    CHECK(!catalog.reattach_to_split({a, {41}}, {b, catalog.workspace(b)->active_pane()}, Direction::right));
    CHECK(snapshot(catalog) == before);
    CHECK(!catalog.reattach_to_split({a, {41}}, {a, old_pane}, Direction::right));
    CHECK(snapshot(catalog) == before);
    Workspace duplicate; CHECK(duplicate.create_tab({41}));
    CHECK(!catalog.create({"Duplicate", {}, {}}, duplicate)); CHECK(snapshot(catalog) == before);

    auto restored = catalog.reattach_to_new_tab({a, {41}}); CHECK(restored);
    const auto new_pane = catalog.workspace(a)->active_pane();
    CHECK(new_pane != old_pane);
    const auto* pane = catalog.workspace(a)->pane(new_pane);
    CHECK(pane->session == SessionId{41}); CHECK(pane->profile == "shell-profile"); CHECK(pane->title == "Build output");
    CHECK(catalog.find(a)->parked_sessions.empty()); CHECK(catalog.all_session_ids() == sessions);
    CHECK(catalog.workspace(b)->state() == b_before); CHECK(catalog.active_id() == b);

    CHECK(catalog.workspace(a)->split(new_pane, Direction::down, {42}));
    const auto target = catalog.workspace(a)->active_pane();
    CHECK(catalog.park({a, new_pane}));
    auto failure = snapshot(catalog);
    CHECK(!catalog.reattach_to_split({a, {41}}, {a, target}, Direction::right, 0));
    CHECK(snapshot(catalog) == failure);
    CHECK(!catalog.reattach_to_split({a, {41}}, {a, target}, static_cast<Direction>(999)));
    CHECK(snapshot(catalog) == failure);
    CHECK(!catalog.reattach_to_split({a, {41}}, {a, target}, Direction::down, std::numeric_limits<double>::quiet_NaN()));
    CHECK(snapshot(catalog) == failure);
    // Legacy mutable access may violate catalog ownership; transfers reject it.
    CHECK(catalog.workspace(a)->bind_session(target, {41})); CHECK(!catalog.validate());
    failure = snapshot(catalog);
    CHECK(!catalog.reattach_to_new_tab({a, {41}})); CHECK(!catalog.park({a, target}));
    CHECK(!catalog.discard_parked({a, {41}})); CHECK(snapshot(catalog) == failure);
    CHECK(catalog.workspace(a)->bind_session(target, {42})); CHECK(catalog.validate());
    auto split = catalog.reattach_to_split({a, {41}}, {a, target}, Direction::left, 0.3);
    CHECK(split); CHECK(catalog.workspace(a)->pane(*split.value)->session == SessionId{41});
    CHECK(catalog.workspace(a)->pane(*split.value)->title == "Build output");
    CHECK(catalog.park({a, *split.value}));
    const auto serialized = serialize_workspace(*catalog.workspace(a));
    auto metadata_only = deserialize_workspace(serialized); CHECK(metadata_only);
    CHECK(metadata_only.value->state().panes.size() == 1);
    CHECK(!metadata_only.value->pane(metadata_only.value->active_pane())->session);
    failure = snapshot(catalog);
    CHECK(!catalog.discard_parked({b, {41}})); CHECK(!catalog.discard_parked({a, {42}}));
    CHECK(snapshot(catalog) == failure);
    auto discarded = catalog.discard_parked({a, {41}}); CHECK(discarded);
    CHECK(discarded.value->session == SessionId{41}); CHECK(!catalog.contains_session({41}));
    CHECK(catalog.park({a, target}));
    CHECK(catalog.active_id() == b); CHECK(catalog.find(a));
    const auto preview = catalog.session_ids(a);
    auto removed = catalog.remove(a); CHECK(removed);
    CHECK(removed.value->sessions == preview); CHECK(preview == std::vector<SessionId>({{42}}));
    CHECK(removed.value->project.workspace.state().panes.empty());
    CHECK(removed.value->project.parked_sessions.size() == 1); CHECK(!catalog.contains_session({42}));
    CHECK(catalog.validate());
}

void parked_attention_placement_lifecycle() {
    ProjectWorkspaceCatalog catalog;
    Workspace initial; CHECK(initial.create_tab({41}));
    const auto project=create(catalog,"Attention",std::move(initial));
    const auto pane=catalog.workspace(project)->active_pane();
    const auto parked=catalog.park({project,pane}); CHECK(parked);
    SessionInfo info; info.state=SessionState::running;
    info.command={CommandPhase::completed,1,1,0};
    std::map<SessionId,ParkedAttention> attention;
    attention.emplace(*parked.value,ParkedAttention(info));
    CHECK(!attention.at(*parked.value).unread_completion());
    info.command={CommandPhase::completed,2,2,0};
    CHECK(attention.at(*parked.value).observe(info)==ParkedNotice::command_finished);
    const auto before=snapshot(catalog);
    // A canceled confirmation performs no placement or attention mutation.
    const bool confirmed=false;
    if (confirmed) { CHECK(catalog.discard_parked({project,*parked.value})); attention.erase(*parked.value); }
    CHECK(snapshot(catalog)==before && attention.at(*parked.value).unread_completion());
    for(std::size_t i=0;i<Workspace::maximum_tabs;++i) CHECK(catalog.workspace(project)->create_tab());
    auto restore=[&] {
        const auto result=catalog.reattach_to_new_tab({project,*parked.value});
        if(result)attention.erase(*parked.value);
        return bool(result);
    };
    CHECK(!restore()); CHECK(attention.at(*parked.value).unread_completion());
    CHECK(catalog.workspace(project)->close_tab(catalog.workspace(project)->state().tabs.back().id));
    CHECK(restore()); CHECK(attention.empty());
    const auto again=catalog.park({project,catalog.workspace(project)->active_pane()}); CHECK(again);
    attention.emplace(*again.value,ParkedAttention(info));
    CHECK(!attention.at(*again.value).unread_completion());
    CHECK(attention.at(*again.value).observe(info)==ParkedNotice::none);
    CHECK(catalog.discard_parked({project,*again.value}));
    std::erase_if(attention,[&](const auto& item){return !catalog.contains_session(item.first);});
    CHECK(attention.empty()); CHECK(catalog.validate());
}

void parked_bounds_and_rollback() {
    ProjectWorkspaceCatalog catalog;
    const auto id = create(catalog, "Capacity");
    for (std::size_t i = 0; i < ProjectWorkspaceCatalog::maximum_parked_sessions; ++i) {
        CHECK(catalog.workspace(id)->create_tab({i + 1}));
        CHECK(catalog.park({id, catalog.workspace(id)->active_pane()}));
    }
    CHECK(catalog.workspace(id)->create_tab({10000}));
    const auto before = snapshot(catalog);
    CHECK(!catalog.park({id, catalog.workspace(id)->active_pane()}));
    CHECK(snapshot(catalog) == before); CHECK(catalog.validate());
    CHECK(catalog.discard_parked({id, {1}}));
    CHECK(catalog.park({id, catalog.workspace(id)->active_pane()}));
    CHECK(catalog.workspace(id)->create_tab()); // Unbound is not a parked session.
    const auto unbound = snapshot(catalog);
    CHECK(!catalog.park({id, catalog.workspace(id)->active_pane()}));
    CHECK(snapshot(catalog) == unbound);
    // At the tab limit a reattach fails without consuming parked ownership/IDs.
    for (std::size_t i = 1; i < Workspace::maximum_tabs; ++i) CHECK(catalog.workspace(id)->create_tab());
    const auto full = snapshot(catalog);
    CHECK(!catalog.reattach_to_new_tab({id, {2}})); CHECK(snapshot(catalog) == full);
    CHECK(catalog.reattach_to_split({id, {2}}, {id, catalog.workspace(id)->active_pane()}, Direction::right));
    CHECK(catalog.validate());
    // A split that exceeds maximum tree depth is rejected after staged ID allocation.
    auto target = catalog.workspace(id)->active_pane();
    while (true) {
        auto next = catalog.workspace(id)->split(target, Direction::right);
        if (!next) break;
        target = *next.value;
    }
    const auto deep = snapshot(catalog);
    CHECK(!catalog.reattach_to_split({id, {3}}, {id, target}, Direction::right));
    CHECK(snapshot(catalog) == deep);
    auto removed = catalog.remove(id); CHECK(removed);
    CHECK(removed.value->sessions.size() == ProjectWorkspaceCatalog::maximum_parked_sessions);
    CHECK(catalog.entries().empty()); CHECK(catalog.validate());
}

void operation_trace() {
    ProjectWorkspaceCatalog catalog;
    std::mt19937 rng(0x50724f6a);
    std::uint64_t next_session = 1;
    std::set<ProjectWorkspaceId> ever_issued;
    for (unsigned step = 0; step < 1500; ++step) {
        if (catalog.entries().empty() || (catalog.entries().size() < 18 && rng() % 4 == 0)) {
            const auto previous_active = catalog.active_id();
            auto source = populated(next_session); next_session += 3;
            const auto before = source.state();
            const auto id = create(catalog, "Project " + std::to_string(step), std::move(source));
            CHECK(ever_issued.insert(id).second); CHECK(catalog.workspace(id)->state() == before);
            CHECK(catalog.active_id() == (previous_active ? previous_active : id));
        } else {
            const auto index = static_cast<std::size_t>(rng() % catalog.entries().size());
            const auto id = catalog.entries()[index].id;
            const auto before = snapshot(catalog);
            const auto sessions = catalog.all_session_ids();
            switch (rng() % 5) {
            case 0:
                CHECK(catalog.activate(id)); CHECK(catalog.active_id() == id);
                for (const auto& previous : before.entries)
                    CHECK(catalog.workspace(previous.id)->state() == previous.workspace);
                CHECK(catalog.all_session_ids() == sessions);
                break;
            case 1:
                CHECK(catalog.rename(id, "Task group " + std::to_string(step)));
                CHECK(catalog.active_id() == before.active); CHECK(catalog.all_session_ids() == sessions);
                CHECK(catalog.workspace(id)->state() == before.entries[index].workspace);
                break;
            case 2: {
                const auto affected = catalog.session_ids(id);
                auto removed = catalog.remove(id); CHECK(removed);
                CHECK(removed.value->project.workspace.state() == before.entries[index].workspace);
                CHECK(removed.value->sessions == affected); CHECK(!catalog.find(id));
                for (const auto session : affected) CHECK(!catalog.contains_session(session));
                for (const auto& previous : before.entries)
                    if (previous.id != id) CHECK(catalog.workspace(previous.id)->state() == previous.workspace);
                break;
            }
            case 3:
                CHECK(!catalog.rename(id, "\x1b[31m")); CHECK(snapshot(catalog) == before);
                CHECK(!catalog.remove({})); CHECK(snapshot(catalog) == before);
                break;
            default:
                CHECK(catalog.update_metadata(id, {"Context", "/work/next", "Local"}));
                CHECK(catalog.workspace(id)->state() == before.entries[index].workspace);
                CHECK(catalog.all_session_ids() == sessions);
                break;
            }
        }
        CHECK(catalog.validate());
        std::set<SessionId> observed;
        for (const auto& project : catalog.entries())
            for (const auto& [pane_id, pane] : project.workspace.state().panes) {
                (void)pane_id;
                CHECK(pane.session); CHECK(observed.insert(pane.session).second);
            }
        const auto all = catalog.all_session_ids();
        CHECK(std::vector<SessionId>(observed.begin(), observed.end()) == all);
    }
}
} // namespace

int main() {
    return run_core_tests("project_workspaces", [] {
        lifecycle_and_hidden_sessions(); identity_domains(); deliberate_removal();
        failures_are_transactional(); bounds_and_unbound_workspaces(); operation_trace();
        parked_ownership_and_identity(); parked_bounds_and_rollback(); parked_attention_placement_lifecycle();
    });
}
