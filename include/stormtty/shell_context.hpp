#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace stormtty {
// Provisional, application-owned OSC namespace. No authority or executable data.
// Feed complete unknown-OSC callback content, never raw PTY chunks.
inline constexpr std::string_view shell_context_namespace = "7400;stormtty-context;";
inline constexpr std::size_t shell_context_wire_limit = 8192;
inline constexpr std::size_t shell_context_directory_limit = 2048;
inline constexpr std::size_t shell_context_head_limit = 256;
enum class GitHeadKind { none, branch, detached };
struct ShellContext {
    // Exact raw OSC7/OSC9/OSC1337 PWD bytes. An opaque correlation key, not a
    // local path/URI to open. Never render this value directly.
    std::string directory_key;
    std::string display_directory, repository_root, head;
    GitHeadKind git{GitHeadKind::none};
    bool operator==(const ShellContext&) const = default;
};
enum class ShellContextMessageKind { ignored, invalid, clear, snapshot };
struct ShellContextMessage {
    ShellContextMessageKind kind{ShellContextMessageKind::ignored};
    std::optional<ShellContext> context;
};
// Unknown namespaces/versions are ignored. Malformed matching version-one
// packets are invalid. Valid full snapshots own all decoded strings.
ShellContextMessage parse_shell_context_osc(std::string_view content, bool truncated = false);

// Actor-owned latest state; no event queue, I/O, execution, or filesystem access.
// The host copies context() into its thread-safe snapshot. No authority is
// established by a matching directory: arbitrary terminal programs can spoof it.
class ShellContextState {
public:
    // Called in stream order from the existing PWD callback. A change, clear,
    // invalid or oversized PWD invalidates context. No URI normalization.
    bool observe_directory(std::string_view raw_pwd);
    // Emitters send PWD first, then context. Mismatch/invalid packet clears stale
    // context; ignored namespaces/versions leave this state untouched.
    bool apply(const ShellContextMessage&);
    bool reset(); // RIS/session replacement: directory and context unavailable.
    const std::optional<ShellContext>& context() const noexcept { return context_; }
private:
    std::string directory_;
    std::optional<ShellContext> context_;
};
}
