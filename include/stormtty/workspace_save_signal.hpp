#pragma once
#include <utility>

namespace stormtty {
// UI-thread signal. Only persisted-model mutations mark this; rendering,
// terminal output, typing, hover and clocks must not participate. A host timer
// consumes many mutations once, without inspecting/copying the workspace.
class WorkspaceSaveSignal {
public:
    void mark_changed() noexcept { dirty_ = true; }
    bool take_pending() noexcept { return std::exchange(dirty_, false); }
    bool pending() const noexcept { return dirty_; }
private:
    bool dirty_{};
};
} // namespace stormtty
