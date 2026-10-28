#include "stormtty/session.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace stormtty {
namespace {
constexpr std::size_t command_budget = 1024 * 1024;
constexpr std::size_t output_budget = 2 * 1024 * 1024;
constexpr std::size_t write_budget = 4 * 1024 * 1024;
constexpr std::size_t chunk_limit = 32 * 1024;
enum class Op { key, text, paste, mouse, focus, select, clear, copy, scroll, bottom, resize,
                search_start, search_cancel, search_select, search_navigate, default_colors };
bool input_operation(Op op) {
    return op == Op::key || op == Op::text || op == Op::paste || op == Op::mouse || op == Op::focus;
}
bool search_operation(Op op) {
    return op == Op::search_start || op == Op::search_cancel || op == Op::search_select || op == Op::search_navigate;
}
struct Command {
    Op op{};
    std::uint64_t sequence{};
    KeyInput key;
    MouseInput mouse;
    TerminalSize size;
    Color foreground, background;
    std::string text;
    Session::SelectionCallback selection;
    std::int64_t rows{};
    std::uint16_t x1{}, y1{}, x2{}, y2{};
    bool flag{};
    SearchOptions search_options;
    SearchToken search_token;
    SearchDirection search_direction{SearchDirection::backward};
    std::uint64_t search_request{};
    std::size_t search_index{};
    std::size_t bytes() const { return sizeof(Command) + text.size() + key.text.size() + search_options.query.size(); }
};
struct OutputEvent { std::uint64_t sequence{}; std::string bytes; };
struct PendingWrite { std::string bytes; std::size_t offset{}; };
}
struct Session::Impl : std::enable_shared_from_this<Impl> {
    ProcessOptions process_options;
    TerminalOptions terminal_options;
    Wake wake;
    SessionRetentionPolicy retention;
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<Command> commands;
    std::deque<OutputEvent> output;
    std::uint64_t next_sequence{};
    std::size_t command_bytes{}, output_bytes{}; // Excludes the single coalesced default_colors slot.
    std::optional<ProcessExit> exit;
    bool close_requested{}, process_finished_{};
    std::atomic<bool> done{false};
    mutable std::mutex frame_mutex;
    FramePtr latest;
    mutable std::mutex search_mutex;
    SearchSnapshotPtr latest_search;
    std::atomic<std::uint64_t> requested_search{0};
    SessionInfo information;
    std::shared_ptr<Process> process;
    ProcessDiagnostics final_diagnostics;

    Impl(ProcessOptions p, TerminalOptions t, Wake w, SessionRetentionPolicy policy)
        : process_options(std::move(p)), terminal_options(t), wake(std::move(w)), retention(policy) {}
    void publish(FramePtr frame) {
        FramePtr retired;
        { std::lock_guard lock(frame_mutex); retired = std::exchange(latest, std::move(frame)); }
        // Release potentially large rows/images outside the short mailbox lock.
    }
    void publish_search(SearchSnapshotPtr value) {
        SearchSnapshotPtr retired;
        { std::lock_guard lock(search_mutex); retired = std::exchange(latest_search, std::move(value)); }
    }
    SearchSubmission enqueue_search(SearchOptions options, bool cancel) {
        if (options.query.size() > 4096) return {SessionSendResult::too_large, 0};
        Command command; command.op = cancel ? Op::search_cancel : Op::search_start;
        command.search_options = std::move(options);
        const auto amount = command.bytes();
        std::uint64_t request{};
        {
            std::lock_guard lock(mutex);
            if (close_requested || done.load()) return {SessionSendResult::closed, 0};
            std::size_t retired{};
            for (const auto& pending : commands) if (search_operation(pending.op)) retired += pending.bytes();
            if (amount > command_budget - (command_bytes - retired)) return {SessionSendResult::would_block, 0};
            for (auto it = commands.begin(); it != commands.end();) {
                if (search_operation(it->op)) it = commands.erase(it); else ++it;
            }
            command_bytes -= retired;
            request = requested_search.fetch_add(1, std::memory_order_acq_rel) + 1;
            command.search_request = request; command.sequence = ++next_sequence;
            command_bytes += amount; commands.push_back(std::move(command));
        }
        changed.notify_all();
        return {SessionSendResult::accepted, request};
    }
    void notify() noexcept { try { if (wake) wake(); } catch (...) {} }
    void set_error(std::string value) {
        { std::lock_guard lock(mutex); information.error = std::move(value); }
        notify();
    }
    SessionSendResult enqueue_default_colors(Color foreground, Color background) {
        if (foreground.a != 255 || background.a != 255)
            throw std::invalid_argument("terminal default colors must be opaque RGB; apply opacity in the renderer");
        Command command; command.op = Op::default_colors;
        command.foreground = foreground; command.background = background;
        {
            std::lock_guard lock(mutex);
            if (close_requested || done.load() ||
                (exit && retention == SessionRetentionPolicy::final_frame_only)) return SessionSendResult::closed;
            // One separately budgeted display slot. Replace only pending display
            // updates, preserving all input/output and the newest update's order.
            std::erase_if(commands, [](const auto& pending) { return pending.op == Op::default_colors; });
            command.sequence = ++next_sequence;
            commands.push_back(std::move(command));
        }
        changed.notify_all();
        return SessionSendResult::accepted;
    }
    SessionSendResult enqueue(Command command) {
        const auto amount = command.bytes();
        if (amount > command_budget) return SessionSendResult::too_large;
        {
            std::lock_guard lock(mutex);
            if (close_requested || done.load() || (input_operation(command.op) && (exit || process_finished_)))
                return SessionSendResult::closed;
            if (amount > command_budget - command_bytes) return SessionSendResult::would_block;
            command.sequence = ++next_sequence;
            command_bytes += amount;
            commands.push_back(std::move(command));
        }
        changed.notify_all();
        return SessionSendResult::accepted;
    }
    void accept_output(std::string_view bytes, std::stop_token stop) {
        for (std::size_t offset = 0; offset < bytes.size();) {
            const auto count = std::min(chunk_limit, bytes.size() - offset);
            std::unique_lock lock(mutex);
            if (!changed.wait(lock, stop, [&] {
                    return done.load() || close_requested || count <= output_budget - output_bytes;
                }) || done.load() || close_requested) return;
            output.push_back({++next_sequence, std::string(bytes.substr(offset, count))});
            output_bytes += count;
            information.bytes_received += count;
            offset += count;
            lock.unlock();
            changed.notify_all();
        }
    }
    void close() noexcept {
        { std::lock_guard lock(mutex); close_requested = true; }
        changed.notify_all();
    }
    void run() noexcept {
        try { actor(); }
        catch (const std::exception& e) { fail(e.what()); }
        catch (...) { fail("Unexpected terminal session failure"); }
        // A queued copy callback may own the final Session reference. Dispose
        // closures outside mutex: releasing that owner reenters request_close.
        // Also retire commands never reached before a final-frame-only exit.
        std::deque<Command> retired;
        {
            std::lock_guard lock(mutex);
            close_requested = true;
            retired.swap(commands); command_bytes = 0;
        }
        retired.clear();
        // Publish under the same mutex as wait predicates, avoiding a lost wake
        // between a predicate check and condition-variable sleep.
        { std::lock_guard lock(mutex); done.store(true, std::memory_order_release); }
        changed.notify_all();
        notify();
    }
    void fail(std::string reason) noexcept {
        std::shared_ptr<Process> child;
        std::deque<Command> retired;
        {
            std::lock_guard lock(mutex);
            information.state = SessionState::failed;
            information.error = std::move(reason);
            close_requested = true;
            child = process;
            retired.swap(commands); command_bytes = 0;
            output.clear(); output_bytes = 0;
        }
        changed.notify_all();
        if (child) {
            child->request_close();
            // This is the background actor, never the UI/callback. A failed
            // session is not called closed until its transport has reaped.
            while (!child->wait_for_exit(std::chrono::milliseconds(250))) {}
        }
        { std::lock_guard lock(mutex); process.reset(); process_finished_ = true; }
        changed.notify_all();
    }
    void publish_process_result(const ProcessExit& result) {
        // Process exit callbacks run only after the reader stopped and OS
        // resources were reaped. Retain diagnostics, not a transport owner.
        auto diagnostic = process ? process->diagnostics() : ProcessDiagnostics{};
        std::shared_ptr<Process> retired;
        {
            std::lock_guard lock(mutex);
            information.exit_code = result.exit_code;
            if (!result.error.empty()) information.error = result.error;
            information.state = result.error.empty() ? SessionState::exited : SessionState::failed;
            final_diagnostics = std::move(diagnostic);
            retired = std::move(process);
            process_finished_ = true;
        }
        changed.notify_all();
        notify();
    }
    void apply_document_command(TerminalCore& core, Command& c, bool& dirty,
                                TerminalCore::Clock::time_point& next_search_step) {
        switch (c.op) {
        case Op::default_colors: core.set_default_colors(c.foreground, c.background); dirty = true; break;
        case Op::select: core.select(c.x1, c.y1, c.x2, c.y2, c.flag); dirty = true; break;
        case Op::clear: core.clear_selection(); dirty = true; break;
        case Op::copy: {
            auto text = core.selection_text();
            if (text.size() > 16u * 1024u * 1024u) throw std::invalid_argument("Selection exceeds clipboard safety limit");
            if (c.selection) c.selection(std::move(text));
            break;
        }
        case Op::scroll: core.scroll_viewport(c.rows); dirty = true; break;
        case Op::bottom: core.scroll_to_bottom(); dirty = true; break;
        case Op::resize:
            core.resize(c.size);
            dirty = true; break;
        case Op::search_start:
            if (c.search_request == requested_search.load()) {
                core.begin_search(c.search_request, std::move(c.search_options));
                publish_search(core.search_snapshot()); next_search_step = TerminalCore::Clock::now(); notify();
            }
            break;
        case Op::search_cancel:
            if (c.search_request == requested_search.load()) {
                core.cancel_search(c.search_request); publish_search(core.search_snapshot()); notify();
            }
            break;
        case Op::search_select:
            if (c.search_token.request_id != requested_search.load()) break;
            dirty = core.select_search_match(c.search_token, c.search_index) == SearchApplyResult::applied || dirty;
            publish_search(core.search_snapshot()); notify(); break;
        case Op::search_navigate:
            if (c.search_token.request_id != requested_search.load()) break;
            dirty = core.navigate_search(c.search_token, c.search_direction, c.flag) == SearchApplyResult::applied || dirty;
            publish_search(core.search_snapshot()); notify(); break;
        default: break; // Pending input is rejected/discarded after process exit.
        }
    }
    void step_search(TerminalCore& core, TerminalCore::Clock::time_point now,
                     TerminalCore::Clock::time_point& next_search_step) {
        if (core.search_needs_work() && now >= next_search_step &&
            core.search_snapshot()->token.request_id == requested_search.load(std::memory_order_acquire)) {
            const auto found = core.search_step();
            if (found->token.request_id == requested_search.load(std::memory_order_acquire)) {
                publish_search(found); notify();
            }
            next_search_step = TerminalCore::Clock::now() + std::chrono::milliseconds(5);
        }
    }
    void retain_document(TerminalCore& core) {
        // No Process, terminal animation deadlines or periodic polling here.
        // A retained actor wakes only for a user operation, bounded active search
        // work, one pending display publication, or explicit disposal.
        bool dirty = false;
        auto last_frame = TerminalCore::Clock::now(), next_search_step = last_frame;
        for (;;) {
            const auto requested = requested_search.load(std::memory_order_acquire);
            if (requested && requested != core.search_snapshot()->token.request_id) core.cancel_search(requested);
            std::deque<Command> batch;
            bool dispose{};
            {
                std::lock_guard lock(mutex);
                dispose = close_requested;
                if (dispose) { batch.swap(commands); command_bytes = 0; }
                else for (unsigned i = 0; i < 8 && !commands.empty(); ++i) {
                    if (commands.front().op != Op::default_colors) command_bytes -= commands.front().bytes();
                    batch.push_back(std::move(commands.front())); commands.pop_front();
                }
            }
            if (dispose) {
                core.cancel_search(requested_search.load()); publish_search(core.search_snapshot());
                return;
            }
            if (!batch.empty()) notify(); // Host may retry a bounded display queue.
            for (auto& c : batch) {
                try { apply_document_command(core, c, dirty, next_search_step); }
                catch (const std::invalid_argument& e) { set_error(e.what()); }
            }
            const auto now = TerminalCore::Clock::now();
            step_search(core, now, next_search_step);
            if (dirty && now - last_frame >= std::chrono::milliseconds(8)) {
                publish(core.snapshot(now)); last_frame = now; dirty = false;
                // Display-only operations must never turn retained protocol
                // effects into writes or start another process.
                (void)core.take_effects(); notify();
            }
            if (!batch.empty()) continue;
            auto until = TerminalCore::Clock::time_point::max();
            if (dirty) until = last_frame + std::chrono::milliseconds(8);
            if (core.search_needs_work()) until = std::min(until, next_search_step);
            std::unique_lock lock(mutex);
            const auto ready = [&] { return !commands.empty() || close_requested; };
            if (until == TerminalCore::Clock::time_point::max()) changed.wait(lock, ready);
            else changed.wait_until(lock, until, ready);
        }
    }
    void actor() {
        TerminalCore core(terminal_options); // Construct and destroy on this actor.
        publish(core.snapshot());
        publish_search(core.search_snapshot());
        notify();
        const auto weak = weak_from_this();
        auto child = Process::start(process_options, {
            [weak](std::string_view bytes, std::stop_token stop) {
                if (auto state = weak.lock()) state->accept_output(bytes, stop);
            },
            [weak](ProcessExit value) {
                if (auto state = weak.lock()) {
                    { std::lock_guard lock(state->mutex); state->exit = std::move(value); }
                    state->changed.notify_all();
                }
            }
        });
        { std::lock_guard lock(mutex); process = std::move(child); }
        std::deque<PendingWrite> writes;
        std::size_t write_bytes = 0;
        bool closing = false, dirty = false;
        auto last_frame = TerminalCore::Clock::now();
        auto next_search_step = last_frame;
        auto append = [&](std::string bytes) {
            if (bytes.empty()) return;
            if (bytes.size() > write_budget - write_bytes)
                throw std::runtime_error("Terminal input backlog exceeded its safety limit");
            write_bytes += bytes.size();
            writes.push_back({std::move(bytes), 0});
        };
        auto effects = [&] {
            for (auto& effect : core.take_effects()) {
                if (effect.kind == EffectKind::write_pty) { append(std::move(effect.text)); continue; }
                std::lock_guard lock(mutex);
                if (effect.kind == EffectKind::title) information.title = std::move(effect.text);
                else if (effect.kind == EffectKind::working_directory)
                    information.working_directory = std::move(effect.text);
                // Clipboard protocol requests are denied by the adapter; never
                // copy their payloads into the OS clipboard or execute titles.
            }
        };
        for (;;) {
            // Cancel superseded work before consuming any more search units,
            // even if the newest typing command is behind bounded PTY output.
            const auto requested = requested_search.load(std::memory_order_acquire);
            if (requested && requested != core.search_snapshot()->token.request_id)
                core.cancel_search(requested);
            std::deque<Command> batch;
            std::string bytes;
            std::optional<ProcessExit> child_exit;
            bool request_close_now = false;
            {
                std::lock_guard lock(mutex);
                request_close_now = close_requested;
                for (unsigned i = 0; i < 8 && !commands.empty() &&
                     (output.empty() || commands.front().sequence < output.front().sequence); ++i) {
                    if (commands.front().op != Op::default_colors) command_bytes -= commands.front().bytes();
                    batch.push_back(std::move(commands.front())); commands.pop_front();
                }
                if (!output.empty() && (commands.empty() || output.front().sequence < commands.front().sequence)) {
                    output_bytes -= output.front().bytes.size();
                    bytes = std::move(output.front().bytes); output.pop_front();
                }
                // Exit is consumed only after all bytes delivered before EOF.
                if (exit && output.empty() && bytes.empty()) child_exit = exit;
            }
            changed.notify_all(); // Reader backpressure and UI queue capacity.
            if (!batch.empty()) notify();
            if (request_close_now && !closing) {
                closing = true;
                { std::lock_guard lock(mutex); information.state = SessionState::closing; }
                process->request_close();
                core.cancel_search(requested_search.load()); publish_search(core.search_snapshot());
                writes.clear(); write_bytes = 0; // Explicit close, never normal overload.
                notify();
            }
            if (!closing) {
                for (auto& c : batch) {
                    try {
                        if (!input_operation(c.op)) {
                            apply_document_command(core, c, dirty, next_search_step);
                            if (c.op == Op::resize) process->request_resize({c.size.columns, c.size.rows});
                        } else switch (c.op) {
                        case Op::key: append(core.encode_key(c.key)); core.scroll_to_bottom(); dirty = true; break;
                        case Op::text: { KeyInput key; key.text = std::move(c.text); append(core.encode_key(key)); core.scroll_to_bottom(); dirty = true; break; }
                        case Op::paste: append(core.encode_paste(c.text, c.flag)); core.scroll_to_bottom(); dirty = true; break;
                        case Op::mouse: append(core.encode_mouse(c.mouse)); break;
                        case Op::focus: append(core.encode_focus(c.flag)); break;
                        default: break;
                        }
                    } catch (const std::invalid_argument& e) { set_error(e.what()); }
                }
            }
            if (!bytes.empty() && !closing) { core.feed(bytes); dirty = true; }
            effects();
            bool progress = false;
            while (!closing && !writes.empty()) {
                auto& pending = writes.front();
                const auto count = std::min({chunk_limit, process_options.input_capacity_bytes,
                                             pending.bytes.size() - pending.offset});
                if (count == 0) throw std::runtime_error("Process input capacity must be positive");
                const auto sent = process->try_send(std::string_view(pending.bytes).substr(pending.offset, count));
                if (sent == ProcessSendResult::would_block) break;
                if (sent == ProcessSendResult::closed) { writes.clear(); write_bytes = 0; break; }
                if (sent == ProcessSendResult::too_large) throw std::runtime_error("Process rejected bounded input chunk");
                pending.offset += count; write_bytes -= count; progress = true;
                { std::lock_guard lock(mutex); information.bytes_sent += count; }
                if (pending.offset == pending.bytes.size()) writes.pop_front();
            }
            const auto now = TerminalCore::Clock::now();
            if (!closing && !child_exit) step_search(core, now, next_search_step);
            if (child_exit) core.finish_output(); // Reveal final held bytes and freeze animations.
            const auto deadline = core.next_deadline();
            if ((dirty && now - last_frame >= std::chrono::milliseconds(8)) ||
                (deadline && now >= *deadline) || child_exit) {
                publish(core.snapshot(now));
                last_frame = now; dirty = false; effects(); notify();
            }
            if (child_exit) {
                const bool retain = retention == SessionRetentionPolicy::retain_document && !closing;
                if (!retain && core.search_needs_work()) {
                    core.cancel_search(requested_search.load()); publish_search(core.search_snapshot());
                }
                writes.clear(); write_bytes = 0;
                // Completed copy callbacks can own the last Session reference.
                // Release this live-loop batch outside mutex before entering an
                // indefinitely retained document; otherwise its closures keep
                // Session alive and prevent destructor-triggered disposal.
                batch.clear();
                publish_process_result(*child_exit);
                if (retain) retain_document(core); // Rechecks close_requested before any operation.
                return;
            }
            const bool starting = process->state() == ProcessState::starting;
            {
                bool state_changed = false;
                { std::lock_guard lock(mutex);
                  if (!closing && information.state == SessionState::starting && process->state() == ProcessState::running) {
                      information.state = SessionState::running; state_changed = true;
                  }
                }
                if (state_changed) notify();
            }
            if (!batch.empty() || !bytes.empty() || progress) continue;
            auto until = TerminalCore::Clock::time_point::max();
            if (dirty) until = last_frame + std::chrono::milliseconds(8);
            if (deadline) until = std::min(until, *deadline);
            if (!writes.empty()) until = std::min(until, now + std::chrono::milliseconds(5));
            if (starting) until = std::min(until, now + std::chrono::milliseconds(20));
            if (!closing && core.search_needs_work()) until = std::min(until, next_search_step);
            std::unique_lock lock(mutex);
            const auto ready = [&] { return !commands.empty() || !output.empty() || exit.has_value() || (close_requested && !closing); };
            if (until == TerminalCore::Clock::time_point::max()) changed.wait(lock, ready);
            else changed.wait_until(lock, until, ready);
        }
    }
};
Session::Session(std::shared_ptr<Impl> value) : impl_(std::move(value)) {}
std::shared_ptr<Session> Session::start(ProcessOptions process, TerminalOptions terminal, Wake wake, SessionRetentionPolicy retention) {
    if (retention != SessionRetentionPolicy::final_frame_only && retention != SessionRetentionPolicy::retain_document)
        throw std::invalid_argument("Unknown session retention policy");
    if (!process.input_capacity_bytes) throw std::invalid_argument("Process input capacity must be positive");
    process.grid = {terminal.size.columns, terminal.size.rows};
    const auto add_default = [&](std::string name, std::string value) {
        const auto equal = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
#ifdef _WIN32
                const auto upper = [](unsigned char c) { return c >= 'a' && c <= 'z' ? static_cast<unsigned char>(c - 'a' + 'A') : c; };
                if (upper(a[i]) != upper(b[i])) return false;
#else
                if (a[i] != b[i]) return false;
#endif
            }
            return true;
        };
        if (std::none_of(process.environment.begin(), process.environment.end(), [&](const auto& e) { return equal(e.name, name); }))
            process.environment.push_back({std::move(name), std::move(value)});
    };
    add_default("TERM", "xterm-256color");
    add_default("COLORTERM", "truecolor");
    add_default("TERM_PROGRAM", "stormtty");
    add_default("TERM_PROGRAM_VERSION", "0.1.0-dev");
    auto state = std::make_shared<Impl>(std::move(process), terminal, std::move(wake), retention);
    auto session = std::shared_ptr<Session>(new Session(state));
    std::thread([state] { state->run(); }).detach();
    return session;
}
Session::~Session() { request_close(); }
FramePtr Session::frame() const noexcept { std::lock_guard lock(impl_->frame_mutex); return impl_->latest; }
SessionInfo Session::info() const { std::lock_guard lock(impl_->mutex); return impl_->information; }
ProcessDiagnostics Session::diagnostics() const {
    std::shared_ptr<Process> process;
    { std::lock_guard lock(impl_->mutex); process = impl_->process; if (!process) return impl_->final_diagnostics; }
    return process->diagnostics();
}
SessionSendResult Session::key(KeyInput value) { Command c; c.op = Op::key; c.key = std::move(value); return impl_->enqueue(std::move(c)); }
SessionSendResult Session::text(std::string value) { Command c; c.op = Op::text; c.text = std::move(value); return impl_->enqueue(std::move(c)); }
SessionSendResult Session::paste(std::string value, bool allow) { Command c; c.op = Op::paste; c.text = std::move(value); c.flag = allow; return impl_->enqueue(std::move(c)); }
SessionSendResult Session::mouse(MouseInput value) { Command c; c.op = Op::mouse; c.mouse = value; return impl_->enqueue(std::move(c)); }
SessionSendResult Session::focus(bool value) { Command c; c.op = Op::focus; c.flag = value; return impl_->enqueue(std::move(c)); }
SessionSendResult Session::select(std::uint16_t x1, std::uint16_t y1, std::uint16_t x2, std::uint16_t y2, bool rectangle) {
    Command c; c.op = Op::select; c.x1 = x1; c.y1 = y1; c.x2 = x2; c.y2 = y2; c.flag = rectangle; return impl_->enqueue(std::move(c));
}
SessionSendResult Session::clear_selection() { Command c; c.op = Op::clear; return impl_->enqueue(std::move(c)); }
SessionSendResult Session::copy_selection(SelectionCallback value) { Command c; c.op = Op::copy; c.selection = std::move(value); return impl_->enqueue(std::move(c)); }
SessionSendResult Session::scroll(std::int64_t value) { Command c; c.op = Op::scroll; c.rows = value; return impl_->enqueue(std::move(c)); }
SessionSendResult Session::scroll_to_bottom() { Command c; c.op = Op::bottom; return impl_->enqueue(std::move(c)); }
SessionSendResult Session::request_resize(TerminalSize value) { Command c; c.op = Op::resize; c.size = value; return impl_->enqueue(std::move(c)); }
SessionSendResult Session::set_default_colors(Color foreground, Color background) { return impl_->enqueue_default_colors(foreground, background); }
SearchSubmission Session::search(SearchOptions options) { return impl_->enqueue_search(std::move(options), false); }
SearchSubmission Session::cancel_search() { return impl_->enqueue_search({}, true); }
SearchSnapshotPtr Session::search_snapshot() const { std::lock_guard lock(impl_->search_mutex); return impl_->latest_search; }
SessionSendResult Session::select_search_match(SearchToken token, std::size_t index) {
    Command c; c.op = Op::search_select; c.search_token = token; c.search_index = index; return impl_->enqueue(std::move(c));
}
SessionSendResult Session::navigate_search(SearchToken token, SearchDirection direction, bool wrap) {
    Command c; c.op = Op::search_navigate; c.search_token = token; c.search_direction = direction; c.flag = wrap;
    return impl_->enqueue(std::move(c));
}
void Session::request_close() noexcept { impl_->close(); }
bool Session::closed() const noexcept { return impl_->done.load(std::memory_order_acquire); }
bool Session::process_finished() const noexcept {
    std::lock_guard lock(impl_->mutex); return impl_->process_finished_;
}
bool Session::wait_for_process_exit(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex); return impl_->changed.wait_for(lock, timeout, [&] { return impl_->process_finished_; });
}
bool Session::wait_closed(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex); return impl_->changed.wait_for(lock, timeout, [&] { return impl_->done.load(); });
}
} // namespace stormtty
