#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace stormtty {
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
namespace qualification { class InputEvidence; }
#endif

struct ProcessGridSize {
    std::uint16_t columns{80};
    std::uint16_t rows{24};
    bool operator==(const ProcessGridSize&) const = default;
};

struct ProcessEnvironmentEntry {
    std::string name;
    // nullopt removes an inherited variable; an empty string sets an empty value.
    std::optional<std::string> value;
};

#ifdef _WIN32
// Optional launch-owned initialization gate. All methods run on the controller
// worker, never UI/reader threads. No caller supplies one by default.
class WindowsProcessInitialization {
public:
    virtual ~WindowsProcessInitialization() = default;
    virtual void prepare(std::vector<ProcessEnvironmentEntry>&) noexcept = 0;
    [[nodiscard]] virtual std::uintptr_t ready_event() const noexcept = 0;
    virtual void ready() noexcept = 0;
    virtual void finished() noexcept = 0;
};
#endif

enum class ProcessProvider { platform_default, windows_inbox_basic, windows_packaged_experimental };
enum class ProcessState { starting, running, close_requested, draining, exited, failed };
enum class ProcessSendResult { accepted, would_block, closed, too_large };

struct ProcessOptions {
#ifdef _WIN32
    std::shared_ptr<WindowsProcessInitialization> initialization;
#endif
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
    // Isolated synthetic-fixture build only; absent from the shipped ABI.
    std::shared_ptr<qualification::InputEvidence> qualification_input_evidence;
#endif
    // UTF-8. executable must be an absolute path. arguments excludes argv[0].
    // No shell interpolation is performed. For a shell command, launch that shell
    // explicitly. Environment inherits the parent, then applies these overrides.
    std::string executable;
    std::vector<std::string> arguments;
    std::string working_directory;
    std::vector<ProcessEnvironmentEntry> environment;
    ProcessGridSize grid;
    std::size_t input_capacity_bytes{1024 * 1024};
    ProcessProvider provider{ProcessProvider::platform_default};
    // Explicit bundled preview or diagnostic override; never selected by default.
    // Requires the pinned, signature-verified official package in an absolute
    // directory on native AMD64 Windows (not x64 emulation on ARM64).
    std::string provider_directory;
};

struct ProcessExit {
    // Set after a successfully observed child exit (including signal termination).
    std::optional<std::uint32_t> exit_code;
    bool close_requested{};
    std::string error;
};

// Windows controller observations, not terminal/output-stream synchronization.
// Generation zero is the launch grid. Each accepted resize gets a new generation;
// coalescing can skip intermediate generations. Applied advances only after the
// native call succeeds. It does NOT mean a shell has observed the new size or
// that all subsequent output was produced at that size. Counters saturate.
struct WindowsProcessControlDiagnostics {
    std::uint64_t requested_resize_generation{}, applied_resize_generation{};
    ProcessGridSize requested_grid;
    std::optional<ProcessGridSize> applied_grid; // Empty until CreatePseudoConsole succeeds.
    std::uint64_t wait_calls{}, control_wakes{}, child_exit_wakes{}, initialization_wakes{};
    std::uint64_t resize_calls{}, resize_failures{};
};

struct ProcessDiagnostics {
    std::string provider;
    std::string detail;
    bool bundled_provider{};
    // Transport/graphics support is NOT inferred merely from provider selection.
    bool advanced_protocols_verified{};
    std::size_t queued_input_bytes{};
    // Present only for the Windows event-driven controller. POSIX keeps its
    // existing implementation and reports nullopt, never fabricated zero data.
    std::optional<WindowsProcessControlDiagnostics> windows_control{};
};

struct ProcessCallbacks {
    // Called on the reader worker, in byte order. The view is borrowed only for
    // the call. Consume/copy ALL bytes before returning; never queue unboundedly.
    // Blocking here backpressures the child without dropping terminal bytes.
    // Honor cancellation when waiting on a downstream queue: it means explicit
    // session shutdown, not permission to drop bytes while the session runs.
    // Do not join/wait for this Process from either callback. Exceptions are caught
    // and cause a failed session. Capture weak/shared state, never a raw UI pointer.
    std::function<void(std::string_view, std::stop_token)> output;
    // Once, on the controller worker, after output has stopped and resources are
    // reaped. May run after Process destruction; dispatch UI work asynchronously.
    std::function<void(ProcessExit)> exited;
};

class Process final {
public:
    // Returns immediately; child/PTY creation and cleanup run off the caller thread.
    // Launch failures are reported through exited, not synchronously. Allocation,
    // control-event or worker creation can throw before a Process is returned.
    static std::unique_ptr<Process> start(ProcessOptions, ProcessCallbacks);
    ~Process(); // Requests close; never joins a worker or blocks on OS cleanup.
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Nonblocking, all-or-none enqueue; accepted writes retain call order.
    // Caller retains/retries bytes on would_block. No arbitrary VT-byte eviction.
    ProcessSendResult try_send(std::string_view bytes);
    // Latest valid grid wins; OS resize is performed on the controller worker.
    bool request_resize(ProcessGridSize);
    // Idempotent and nonblocking; closes only this session/owned process group.
    void request_close() noexcept;
    [[nodiscard]] ProcessState state() const noexcept;
    [[nodiscard]] ProcessDiagnostics diagnostics() const;
    // Test/background-worker convenience only; never call from UI or callbacks.
    [[nodiscard]] bool wait_for_exit(std::chrono::milliseconds timeout) const;

private:
    struct Impl;
    explicit Process(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
};

} // namespace stormtty
