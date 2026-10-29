#pragma once
#include "stormtty/process.hpp"
#include "stormtty/terminal.hpp"
#include <chrono>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace stormtty {
enum class SessionState { starting, running, closing, exited, failed };
// Opt-in document retention does not retain/restart an exited child process.
// Existing callers keep their immutable final viewport and dispose the actor.
enum class SessionRetentionPolicy { final_frame_only, retain_document };
enum class SessionSendResult { accepted, would_block, closed, too_large };
struct SearchSubmission {
    SessionSendResult result{SessionSendResult::closed};
    std::uint64_t request_id{};
};
struct SessionInfo {
    SessionState state{SessionState::starting};
    std::string title, working_directory, error;
    std::optional<std::uint32_t> exit_code;
    std::uint64_t bytes_received{}, bytes_sent{};
    std::optional<ShellContext> shell_context; // Untrusted owned labels, never action authority.
    CommandSnapshot command; // Independent observed shell command, not process status.
};
// Thread-safe host facade. TerminalCore is owned by a dedicated actor; no VT
// method runs on the UI or PTY reader thread. Destruction/close never joins UI.
class Session final {
public:
    using Wake = std::function<void()>; // may run on actor; post a UI wake only
    using SelectionCallback = std::function<void(std::string)>; // actor thread
    static std::shared_ptr<Session> start(ProcessOptions, TerminalOptions, Wake = {},
        SessionRetentionPolicy = SessionRetentionPolicy::final_frame_only);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    [[nodiscard]] FramePtr frame() const noexcept;
    [[nodiscard]] SessionInfo info() const;
    [[nodiscard]] ProcessDiagnostics diagnostics() const;
    // PTY-generating operations return closed once transport exit is observed,
    // even while an opted-in read-only terminal document remains available.
    SessionSendResult key(KeyInput);
    SessionSendResult text(std::string); // committed UTF-8, not IME preedit
    SessionSendResult paste(std::string, bool allow_unsafe = false);
    SessionSendResult mouse(MouseInput);
    SessionSendResult focus(bool);
#ifdef STORMTTY_QUALIFICATION_INPUT_EVIDENCE
    SessionSendResult qualification_typing_phase(bool arm);
#endif
    SessionSendResult select(std::uint16_t start_column, std::uint16_t start_row,
                             std::uint16_t end_column, std::uint16_t end_row,
                             bool rectangle = false);
    SessionSendResult clear_selection();
    SessionSendResult copy_selection(SelectionCallback);
    SessionSendResult scroll(std::int64_t rows);
    SessionSendResult scroll_to_bottom();
    // A retained exited document resizes/reflows locally, with no OS/PTY call.
    SessionSendResult request_resize(TerminalSize);
    // Display-only: one pending update, latest preview/cancel wins. Shares actor
    // ordering with input/output but never writes PTY bytes or consumes input
    // queue capacity. Non-opaque colors throw invalid_argument before enqueue.
    // A retained exited document may update its display defaults; closing or
    // disposed sessions return closed. No process is launched or written to.
    SessionSendResult set_default_colors(Color foreground, Color background, std::optional<std::array<Color,16>> ansi = {});
    // Latest typing request wins; superseded pending search/navigation commands
    // are coalesced, and in-progress work is cancelled on the actor. Results are
    // an owned mailbox; reading it never scans terminal contents on the UI thread.
    // Retain-document sessions can search after exit; final-frame-only sessions
    // dispose VT on exit and then return closed. Existing search budgets apply.
    SearchSubmission search(SearchOptions);
    SearchSubmission cancel_search();
    [[nodiscard]] SearchSnapshotPtr search_snapshot() const;
    SessionSendResult select_search_match(SearchToken, std::size_t index);
    SessionSendResult navigate_search(SearchToken, SearchDirection, bool wrap = true);
    // Explicit disposal, including a retained document. Idempotent, nonblocking;
    // a still-running child is closed and reaped before actor completion.
    void request_close() noexcept;
    [[nodiscard]] bool closed() const noexcept; // Actor/document is disposed.
    // True after the terminal process result (including launch/session failure).
    // Natural exit publishes all final output/frame before this condition. It
    // does not imply that a retained document is disposed.
    [[nodiscard]] bool process_finished() const noexcept;
    // Test/background-only conveniences; never call from window callbacks.
    [[nodiscard]] bool wait_for_process_exit(std::chrono::milliseconds) const;
    [[nodiscard]] bool wait_closed(std::chrono::milliseconds) const;
private:
    struct Impl;
    explicit Session(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
};
}
