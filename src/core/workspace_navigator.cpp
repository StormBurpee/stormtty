#include "stormtty/workspace_navigator.hpp"

namespace stormtty {

void WorkspaceNavigator::begin(bool background_only,
                               std::optional<ProjectWorkspaceId> activity_project) {
    *this = WorkspaceNavigator{};
    background_only_ = background_only;
    activity_project_ = activity_project;
    visible_ = true;
}

void WorkspaceNavigator::close() {
    if (!visible_) {
        return;
    }
    visible_ = false;
    switch_pressed_ = false;
    activity_project_.reset();
}

bool WorkspaceNavigator::is_open() const {
    return visible_;
}

bool WorkspaceNavigator::background_only() const {
    return background_only_;
}

std::optional<ProjectWorkspaceId> WorkspaceNavigator::activity_project() const {
    return activity_project_;
}

void WorkspaceNavigator::replace_catalog(std::vector<NavigatorWorkspace> entries,
                                         ProjectWorkspaceId active_project,
                                         SessionId active_session) {
    if (!visible_) {
        return;
    }
    const auto selected = target();
    if (activity_project_) {
        refresh(std::move(entries), active_project, active_session);
    } else {
        open(std::move(entries), selected ? selected->project : active_project,
             selected ? selected->session : active_session);
    }
    search(editor_.text());
}

TerminalFindQuery& WorkspaceNavigator::editor() {
    return editor_;
}

const TerminalFindQuery& WorkspaceNavigator::editor() const {
    return editor_;
}

void WorkspaceNavigator::apply_query() {
    search(editor_.text());
}

unsigned WorkspaceNavigator::focus() const {
    return focus_;
}

void WorkspaceNavigator::set_focus(unsigned focus) {
    focus_ = focus;
}

void WorkspaceNavigator::arm_switch() {
    switch_pressed_ = true;
    pressed_target_ = target();
}

void WorkspaceNavigator::cancel_switch() {
    switch_pressed_ = false;
    pressed_target_.reset();
}

bool WorkspaceNavigator::switch_pressed() const {
    return switch_pressed_;
}

bool WorkspaceNavigator::take_switch() {
    const bool activate = switch_pressed_ && pressed_target_ == target();
    cancel_switch();
    return activate;
}

} // namespace stormtty
