#pragma once
#ifdef _WIN32
#include <filesystem>
#include <string_view>
#include <functional>
#include <cstdint>
#include <memory>

namespace stormtty {
class WindowsProcessInitialization;
enum class ManagedShellStateStatus { ready, disabled, unavailable };
struct ManagedShellState {
    ManagedShellStateStatus status{ManagedShellStateStatus::unavailable};
    std::filesystem::path directory;
    explicit operator bool() const noexcept { return status == ManagedShellStateStatus::ready; }
};
// The caller supplies the resolved explicit-empty-HOME decision, not an ambient
// "trusted" flag. Never changes HOME, CWD, environment, or existing permissions.
ManagedShellState prepare_managed_shell_state(bool empty_home) noexcept;
// Same validation for an explicit local app-data root; used by native tests.
// This does not trust the supplied root: every ancestor and existing entry is checked.
ManagedShellState prepare_managed_shell_state_at(const std::filesystem::path& local_app_data,
                                                bool empty_home) noexcept;
// Diagnostic-only caller; no production launch invokes this function.
// Exclusive no-replace text publication after fresh ACL/tree validation.
bool publish_managed_completion_seed_at(const std::filesystem::path& local_app_data,
    std::wstring_view filename, std::string_view text) noexcept;
// Public-readable runtime policy: current user/System/Administrators/installer
// ownership, no untrusted mutation grants. Construct once per bounded traversal.
// Caller must retain the already-opened, non-reparse file/directory handle.
std::function<bool(std::uintptr_t, bool)> managed_runtime_access_check();
// Inactive unless explicitly assigned to ProcessOptions::initialization.
// The callback must be backed by a build-trusted seed identity; it is attempted
// only for new completion state, under the cross-process initialization lease.
std::shared_ptr<WindowsProcessInitialization> managed_shell_initialization_at(
    const std::filesystem::path& local_app_data, std::function<bool()> adopt);
} // namespace stormtty
#endif
