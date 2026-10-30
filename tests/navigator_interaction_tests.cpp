#include "navigator_interaction_legacy.hpp"

#include <iostream>
#include <stdexcept>

using namespace stormtty;

namespace {

unsigned checks{};

void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

std::vector<NavigatorWorkspace> catalog(WatchIdentity identity = {1, 7, 3},
                                        bool parked = false) {
    return {
        {{4}, "Work", "/work", {
            {{7}, "Build", "/work", "Running", parked, true, identity},
            {{8}, "Logs", "/logs", "Exited", true, false, {1, 8, 5}}
        }},
        {{9}, "Other", "/other", {
            {{10}, "Server", "/other", "Running", false, false, {1, 10, 6}}
        }}
    };
}

template<class Navigator>
void characterize() {
    Navigator nav;
    check(!nav.is_open(), "navigator starts visible");
    for (const bool background : {false, true}) {
        for (const bool activity : {false, true}) {
            const std::optional<ProjectWorkspaceId> scope = activity
                ? std::optional<ProjectWorkspaceId>{ProjectWorkspaceId{4}}
                : std::nullopt;
            nav.begin(background, scope);
            check(nav.is_open() && nav.background_only() == background &&
                nav.activity_project() == scope, "opening scope changed");
            check(nav.focus() == 0 && nav.editor().text().empty() && nav.entries().empty(),
                "opening did not reset query, focus and catalog");
            nav.replace_catalog(catalog(), {4}, {7});
            const auto initial = nav.target();
            check(initial && initial->session == SessionId{7}, "initial active selection changed");
            nav.arm_switch();
            check(nav.switch_pressed(), "switch press not visible");
            nav.cancel_switch();
            check(!nav.switch_pressed() && !nav.take_switch(), "capture cancellation retained press");
            nav.arm_switch();
            auto reordered = catalog();
            std::reverse(reordered[0].terminals.begin(), reordered[0].terminals.end());
            nav.replace_catalog(reordered, {9}, {10});
            check(nav.target() == initial, "refresh lost stable selected identity");
            check(nav.activity_project() == scope, "refresh replaced frozen scope");
            check(nav.take_switch() && !nav.take_switch(), "unchanged target press not one-shot");
            nav.arm_switch();
            nav.select_terminal(0); // Reordered first row is the other actor.
            check(nav.target() != initial && !nav.take_switch(), "press authorized another target");
            nav.set_focus(2);
            check(nav.editor().insert("Logs"), "query edit failed");
            nav.apply_query();
            check(nav.target() && nav.target()->session == SessionId{8}, "query not applied");
            nav.arm_switch();
            nav.close();
            check(!nav.is_open() && !nav.activity_project() && nav.background_only() == background,
                "close changed scope reset policy");
            check(nav.editor().text() == "Logs" && nav.focus() == 2 && !nav.entries().empty(),
                "close erased presentation/editor state");
            check(!nav.take_switch(), "close retained activation eligibility");
            const auto closed_target = nav.target();
            nav.replace_catalog({}, {9}, {});
            check(nav.target() == closed_target, "closed refresh changed retained model");
            nav.begin(background, scope);
            check(nav.editor().text().empty() && nav.focus() == 0 && nav.entries().empty() &&
                !nav.take_switch(), "reopening retained old interaction");
        }
    }

    nav.begin(false, ProjectWorkspaceId{4});
    nav.replace_catalog(catalog(), {4}, {7});
    nav.arm_switch();
    nav.replace_catalog(catalog({1, 7, 4}), {4}, {7});
    check(nav.selection_stale() && !nav.target() && nav.terminal(),
        "activity accepted replacement incarnation");
    check(!nav.take_switch(), "old valid press accepted newly stale target");
    // A stale activity view still has a displayed terminal. Both null targets
    // compare equal: release must invoke commit so its existing stale notice is
    // shown. An optional-target return would silently suppress that behavior.
    nav.arm_switch();
    check(nav.take_switch(), "null-target press suppressed stale commit attempt");
    nav.replace_catalog(catalog(), {4}, {7});
    nav.apply_query();
    check(nav.selection_stale() && !nav.target(), "unchanged query cleared sticky stale guard");
    nav.select_terminal(0);
    check(nav.target() && !nav.selection_stale(), "explicit row failed to recover selection");
    nav.arm_switch();
    nav.replace_catalog(catalog({1, 7, 3}, true), {4}, {7});
    check(nav.selection_stale() && !nav.take_switch(), "placement mutation authorized old press");
    check(nav.editor().insert("Logs"), "activity search edit failed");
    nav.apply_query();
    check(nav.target() && nav.target()->session == SessionId{8}, "changed query did not recover");
    nav.replace_catalog({}, {4}, {});
    nav.replace_catalog(catalog(), {4}, {7});
    check(nav.selection_stale() && !nav.target(), "repopulation cleared stale guard");

    nav.begin(false, {});
    nav.replace_catalog(catalog(), {4}, {7});
    nav.replace_catalog(catalog({1, 7, 4}), {4}, {7});
    check(!nav.selection_stale() && nav.target()->identity == WatchIdentity{1, 7, 4},
        "ordinary refresh acquired activity stale policy");
    nav.set_focus(1);
    nav.move(true, 1);
    check(nav.target() && nav.target()->project == ProjectWorkspaceId{9},
        "workspace preview lost selection");
    nav.editor().select_all();
    check(nav.editor().insert("NO_MATCH_713"), "empty result query edit failed");
    nav.apply_query();
    check(!nav.target(), "no-match query retained target");
    check(nav.focus() == 1, "filter changed control focus");
}

} // namespace

int main() {
    try {
        characterize<test::LegacyNavigatorInteraction>();
        std::cout << "navigator interaction: " << checks << " checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
