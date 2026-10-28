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

struct ProcessGridSize {
    std::uint16_t columns{80};
    std::uint16_t rows{24};
};

struct ProcessEnvironmentEntry {
    std::string name;
    // nullopt removes an inherited variable; an empty string sets an empty value.
    std::optional<std::string> value;
};

enum class ProcessProvider { platform_default, windows_inbox_basic };
enum class ProcessState { starting, running, close_requested, draining, exited, failed };
enum class ProcessSendResult { accepted, would_block, closed, too_large };

struct ProcessOptions {
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
};

struct ProcessExit {
    // Set after a successfully observed child exit (including signal termination).
    std::optional<std::uint32_t> exit_code;
    bool close_requested{};
    std::string error;
};

struct ProcessDiagnostics {
    std::string provider;
    std::string detail;
    bool bundled_provider{};
    // Transport/graphics support is NOT inferred merely from provider selection.
    bool advanced_protocols_verified{};
    std::size_t queued_input_bytes{};
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
    // Returns immediately; all OS creation and cleanup runs off the caller thread.
    // Launch failures are reported through exited, not synchronously. Allocation
    // or worker-creation failure can throw before a Process has been returned.
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
