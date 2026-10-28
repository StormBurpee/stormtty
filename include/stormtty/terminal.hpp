#pragma once

#include "stormtty/frame.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace stormtty {
struct TerminalSize {
    std::uint16_t columns{80}, rows{24}, cell_width{8}, cell_height{16};
    bool operator==(const TerminalSize&) const = default;
};
struct TerminalOptions {
    TerminalSize size{};
    // Opaque RGB defaults belong to VT state (including OSC queries/resets), not
    // just window chrome. nullopt uses the engine's initial render default. Alpha
    // must be 255; background opacity is exclusively a renderer policy.
    std::optional<Color> default_foreground;
    std::optional<Color> default_background;
    bool resize_pull_scrollback{false}; // ConPTY must use false; POSIX may opt in.
    std::uint64_t image_budget_bytes{64u * 1024u * 1024u};
    std::uint64_t scrollback_budget_bytes{32u * 1024u * 1024u};
    std::optional<std::size_t> scrollback_lines; // Additional engine page-granularity bound.
    std::size_t apc_limit_bytes{4u * 1024u * 1024u};
    std::size_t clipboard_limit_bytes{1024u * 1024u};
    std::size_t effect_queue_limit_bytes{4u * 1024u * 1024u};
    std::chrono::milliseconds render_hold_timeout{1000};
};
// Observed OSC 133 metadata only: untrusted display hints, never authority or
// service readiness. No command text, history, cwd, or borrowed engine strings.
// Constant-size latest state, not an event queue. Generations are per terminal.
enum class CommandPhase { unavailable, running, completed, interrupted };
struct CommandSnapshot {
    CommandPhase phase{CommandPhase::unavailable};
    std::uint64_t command_generation{};
    // Last explicitly ended command; compare this with a consumer's acknowledged
    // generation to avoid duplicate notifications. Multiple ends may coalesce.
    std::uint64_t completed_generation{};
    std::optional<std::int32_t> last_exit_code; // Last explicit completion only.
    bool operator==(const CommandSnapshot&) const = default;
};
enum class Key {
    unidentified, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z,
    digit0, digit1, digit2, digit3, digit4, digit5, digit6, digit7, digit8, digit9,
    enter, tab, backspace, escape, space, insert, del, home, end, page_up, page_down,
    left, right, up, down, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
    backquote, backslash, bracket_left, bracket_right, comma, equal, minus, period, quote, semicolon, slash,
    numpad0, numpad1, numpad2, numpad3, numpad4, numpad5, numpad6, numpad7, numpad8, numpad9,
    numpad_enter, numpad_add, numpad_subtract, numpad_multiply, numpad_divide, numpad_decimal
};
enum class KeyAction { press, repeat, release };
enum Modifier : std::uint16_t {
    shift = 1u << 0, control = 1u << 1, alt = 1u << 2, super = 1u << 3,
    caps_lock = 1u << 4, num_lock = 1u << 5,
    right_shift = 1u << 6, right_control = 1u << 7, right_alt = 1u << 8, right_super = 1u << 9
};
struct KeyInput {
    Key key{Key::unidentified};
    KeyAction action{KeyAction::press};
    std::uint16_t modifiers{}, consumed_modifiers{};
    std::uint32_t unshifted_codepoint{};
    bool composing{};
    std::string text; // Unmodified layout UTF-8; no C0/DEL. Commit IME once, not preedit.
};
enum class MouseAction { press, release, motion };
enum class MouseButton { none, left, right, middle, wheel_up, wheel_down, wheel_left, wheel_right };
struct MouseInput {
    MouseAction action{MouseAction::motion};
    MouseButton button{MouseButton::none};
    std::uint16_t modifiers{};
    double x{}, y{}; // Pixels from terminal content origin, no chrome padding.
    bool any_button_pressed{};
};
enum class ClipboardLocation { standard, selection, primary };
enum class EffectKind { write_pty, title, working_directory, bell, clipboard_write_denied, clipboard_read_denied };
struct ClipboardItem { std::string mime; std::string bytes; };
struct TerminalEffect {
    EffectKind kind{EffectKind::bell};
    std::string text; // Exact bytes for write_pty; untrusted UTF-8 for title/cwd.
    std::vector<ClipboardItem> clipboard;
    ClipboardLocation clipboard_location{ClipboardLocation::standard};
    bool truncated{};
};
struct DecodedPng {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
};
// Platform decoder must enforce max_decoded_bytes before allocating/decompressing.
// Installed once at process startup, before TerminalCore instances or threads.
using PngDecoder = std::optional<DecodedPng> (*)(std::span<const std::uint8_t>, std::size_t max_decoded_bytes);
void install_png_decoder(PngDecoder decoder, std::size_t max_decoded_bytes = 64u * 1024u * 1024u,
                         std::uint32_t max_dimension = 8192);

// Literal UTF-8 search uses the pinned engine's ASCII case-insensitive matching.
// Non-ASCII bytes are exact; regex, Unicode normalization/folding and a case
// toggle are deliberately not advertised. Budgets bound each query, not PTY IO.
struct SearchOptions {
    std::string query; // Valid UTF-8, at most 4096 bytes; empty means idle.
    std::size_t max_matches{1024}; // 1..4096; excessive matches report limited.
    std::size_t memory_budget_bytes{8u * 1024u * 1024u}; // Engine allocator: 64 KiB..64 MiB.
    std::uint32_t max_work_units_per_step{2}; // 1..16, not a wall-clock deadline.
    std::uint64_t max_total_work_units{100000}; // 1..1000000 over this request.
};
enum class SearchStatus { idle, running, complete, cancelled, limited, error };
enum class SearchDirection { forward, backward }; // Toward newer / older text.
enum class SearchApplyResult { applied, stale, not_ready, no_match, boundary };
struct SearchToken {
    std::uint64_t request_id{}, revision{};
    std::uint64_t generation{}; // Core-assigned; protects even reused caller request IDs.
    bool operator==(const SearchToken&) const = default;
};
struct SearchPoint {
    std::uint16_t column{};
    std::uint32_t row{}; // Absolute screen row, including scrollback.
    bool operator==(const SearchPoint&) const = default;
};
struct SearchMatch {
    SearchPoint start{}, end{}; // Inclusive cell endpoints; no borrowed refs.
    bool operator==(const SearchMatch&) const = default;
};
struct SearchSnapshot {
    SearchToken token{};
    std::uint64_t update_id{}, work_units{};
    SearchStatus status{SearchStatus::idle};
    bool alternate_screen{};
    // This is an upstream semantic constraint, not an editable fake case toggle.
    static constexpr bool ascii_case_insensitive = true;
    std::string query, message;
    std::size_t matches_found{}; // Provisional engine count until complete; live updates may change it.
    std::vector<SearchMatch> matches; // Newest first, only published when complete.
    std::optional<std::size_t> selected_index;
    std::optional<SearchApplyResult> last_apply_result;
};
using SearchSnapshotPtr = std::shared_ptr<const SearchSnapshot>;

// One actor thread owns each TerminalCore, including construction/destruction.
// No mutexes/OS types; all methods except the static safety check require that
// thread. Exceptions report allocation/API/budget errors to the session actor;
// C callbacks never throw. feed never silently drops PTY bytes on overload.
// A callback/effect-budget failure is fatal: subsequent operations rethrow.
// Search-specific limits/errors use SearchStatus and do not poison the terminal.
// Geometry is limited to 1048576 cells; session queues must bound retained frames.
// Supported: VT/Unicode, alt-screen, reflow, DEC2026, selection, negotiated
// key/mouse/paste/focus, direct Kitty graphics + animation, owned snapshots.
// Clipboard requests are copied for audit/UI and synchronously DENIED (the C
// callback reply cannot outlive its callback). No external file/shm graphics,
// SIXEL, iTerm images, URL execution, IME platform services, or PNG decoder built in.
class TerminalCore final {
public:
    using Clock = std::chrono::steady_clock;
    explicit TerminalCore(TerminalOptions options = {});
    ~TerminalCore();
    TerminalCore(const TerminalCore&) = delete;
    TerminalCore& operator=(const TerminalCore&) = delete;
    TerminalCore(TerminalCore&&) = delete;
    TerminalCore& operator=(TerminalCore&&) = delete;

    void feed(std::string_view bytes);
    void feed(std::span<const std::uint8_t> bytes);
    void resize(TerminalSize size);
    // Actor-only appearance update. Changes configured opaque RGB defaults,
    // preserving explicit ANSI/RGB cells and terminal-origin OSC overrides.
    // OSC 110/111 resets subsequently resolve to these latest defaults.
    void set_default_colors(Color foreground, Color background);
    // Encoders return exact PTY bytes. Empty means intentionally suppressed.
    std::string encode_key(const KeyInput& event);
    std::string encode_mouse(const MouseInput& event);
    std::string encode_focus(bool focused);
    // Unsafe paste throws invalid_argument unless the user approved these bytes.
    std::string encode_paste(std::string_view text, bool allow_unsafe = false);
    static bool paste_is_safe(std::string_view text) noexcept;

    void select(std::uint16_t start_column, std::uint16_t start_row,
                std::uint16_t end_column, std::uint16_t end_row, bool rectangle = false);
    void clear_selection();
    // Preflight output length before allocating; cap is a clipboard safety bound.
    std::string selection_text(std::size_t max_bytes = 16u * 1024u * 1024u);
    void scroll_viewport(std::int64_t rows);
    void scroll_to_bottom();
    // Irreversibly finish the output stream: release a pending synchronized
    // update and freeze image animations. Display/selection/search/resize remain
    // available, feed rejects later bytes, and next_deadline becomes empty.
    // Idempotent and actor-only; does not destroy scrollback or published frames.
    void finish_output();
    // Tick animations/hold timeout and build only on demand, not per input byte.
    FramePtr snapshot(Clock::time_point now = Clock::now());
    // Wake at this deadline (or next input), never idle-poll animations.
    std::optional<Clock::time_point> next_deadline() const;
    bool render_held() const;
    TerminalSize size() const;
    std::vector<TerminalEffect> take_effects();
    [[nodiscard]] CommandSnapshot command_snapshot() const; // Owned value, owner thread.

    // Actor-only incremental real-scrollback search. A work unit is one bounded
    // engine feed/tick or a batch of <=64 coordinate conversions, not milliseconds.
    // No ghostty_search_run or unbounded selection/copy API is used. Owned result
    // arrays and temporary reference arrays are additionally bounded by max_matches.
    void begin_search(std::uint64_t request_id, SearchOptions options);
    // Stops search work/results, but does not erase a separately owned terminal selection.
    void cancel_search(std::uint64_t request_id);
    SearchSnapshotPtr search_step();
    SearchSnapshotPtr search_snapshot() const;
    bool search_needs_work() const;
    std::uint64_t revision() const;
    // Output, eviction, reset, and resize invalidate tokens. Streaming clients
    // must retry against a fresh completed snapshot, never reuse stale positions.
    SearchApplyResult select_search_match(SearchToken token, std::size_t index);
    SearchApplyResult navigate_search(SearchToken token, SearchDirection direction, bool wrap = true);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace stormtty
