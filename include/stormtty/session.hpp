#pragma once
#include "stormtty/process.hpp"
#include "stormtty/terminal.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace stormtty {
enum class SessionState { starting, running, closing, exited, failed };
enum class SessionSendResult { accepted, would_block, closed, too_large };
struct SessionInfo {
    SessionState state{SessionState::starting};
    std::string title, working_directory, error;
    std::optional<std::uint32_t> exit_code;
    std::uint64_t bytes_received{}, bytes_sent{};
};
// Thread-safe host facade. TerminalCore is owned by a dedicated actor; no VT
// method runs on the UI or PTY reader thread. Destruction/close never joins UI.
class Session final {
public:
    using Wake = std::function<void()>; // may run on actor; post a UI wake only
    using SelectionCallback = std::function<void(std::string)>; // actor thread
    static std::shared_ptr<Session> start(ProcessOptions, TerminalOptions, Wake = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    [[nodiscard]] FramePtr frame() const noexcept;
    [[nodiscard]] SessionInfo info() const;
    [[nodiscard]] ProcessDiagnostics diagnostics() const;
    SessionSendResult key(KeyInput);
    SessionSendResult text(std::string); // committed UTF-8, not IME preedit
    SessionSendResult paste(std::string, bool allow_unsafe = false);
    SessionSendResult mouse(MouseInput);
    SessionSendResult focus(bool);
    SessionSendResult select(std::uint16_t start_column, std::uint16_t start_row,
                             std::uint16_t end_column, std::uint16_t end_row,
                             bool rectangle = false);
    SessionSendResult clear_selection();
    SessionSendResult copy_selection(SelectionCallback);
    SessionSendResult scroll(std::int64_t rows);
    SessionSendResult scroll_to_bottom();
    SessionSendResult request_resize(TerminalSize);
    void request_close() noexcept;
    [[nodiscard]] bool closed() const noexcept;
    // Test/background-only convenience; do not call from window callbacks.
    [[nodiscard]] bool wait_closed(std::chrono::milliseconds) const;
private:
    struct Impl;
    explicit Session(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
};
}
