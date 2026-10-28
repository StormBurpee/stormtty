#include "stormtty/session.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace stormtty;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path utf8_path(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

// Inspect owned VT cells, not an ANSI-stripped transcript. Wide continuation
// cells occupy columns but do not contribute a second character to the text.
std::string row_text(const FrameRow& row) {
    std::string text;
    for (const auto& cell : row.cells) {
        if (cell.span != 0) text += cell.grapheme.empty() ? " " : cell.grapheme;
    }
    return text;
}

std::string frame_text(const FramePtr& frame) {
    std::string text;
    if (frame) for (const auto& row : frame->lines) text += row_text(*row) + '\n';
    return text;
}

std::string cell_description(const Cell& cell) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string text = "utf8=";
    for (std::size_t i = 0; i < std::min<std::size_t>(cell.grapheme.size(), 16); ++i) {
        const auto byte = static_cast<unsigned char>(cell.grapheme[i]);
        text += hex[byte >> 4]; text += hex[byte & 15];
    }
    if (cell.grapheme.size() > 16) text += "...";
    return text + " span=" + std::to_string(cell.span);
}

// Only this fixture's owned VT cells are logged, never environment variables or
// arbitrary PTY history. Bound both geometry and text even on a broken frame.
std::string frame_diagnostic(const FramePtr& frame) {
    if (!frame) return "<no published frame>\n";
    std::string out = "generation=" + std::to_string(frame->generation) + " size=" +
        std::to_string(frame->columns) + "x" + std::to_string(frame->rows) +
        " alternate=" + std::to_string(frame->alternate_screen) + "\n";
    for (std::size_t row = 0; row < std::min<std::size_t>(frame->lines.size(), 32); ++row) {
        out += std::to_string(row) + ": ";
        const auto& cells = frame->lines[row]->cells;
        for (std::size_t column = 0; column < std::min<std::size_t>(cells.size(), 120); ++column) {
            if (cells[column].span == 0) continue;
            if (cells[column].grapheme.empty()) { out += ' '; continue; }
            const auto& grapheme = cells[column].grapheme;
            const auto control = std::any_of(grapheme.begin(), grapheme.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
            out += control ? "<control>" : grapheme.substr(0, 16);
        }
        out += '\n';
        if (out.size() >= 8192) { out.resize(8192); out += "\n<truncated>\n"; break; }
    }
    return out;
}

std::size_t occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0, offset = 0;
    while ((offset = haystack.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

const Cell* find_cell(const FramePtr& frame, std::string_view grapheme) {
    if (frame) for (const auto& row : frame->lines) {
        for (const auto& cell : row->cells) if (cell.grapheme == grapheme) return &cell;
    }
    return nullptr;
}

bool wide_pair(const FramePtr& frame, std::string_view grapheme) {
    if (frame) for (const auto& row : frame->lines) {
        for (std::size_t i = 0; i + 1 < row->cells.size(); ++i) {
            if (row->cells[i].grapheme == grapheme && row->cells[i].span == 2 && row->cells[i + 1].span == 0)
                return true;
        }
    }
    return false;
}

struct BoxRegion { std::size_t top{}, bottom{}; };

std::optional<BoxRegion> active_box(const FramePtr& frame) {
    if (!frame) return std::nullopt;
    for (std::size_t row = 1; row < frame->lines.size(); ++row) {
        if (row_text(*frame->lines[row]).find("StormTTY Ink Workbench") == std::string::npos) continue;
        if (frame->lines[row - 1]->cells.front().grapheme != "╭") return std::nullopt;
        for (std::size_t bottom = row + 1; bottom < frame->lines.size(); ++bottom) {
            if (frame->lines[bottom]->cells.front().grapheme == "╰") return BoxRegion{row - 1, bottom};
        }
    }
    return std::nullopt;
}

std::string box_text(const FramePtr& frame, const BoxRegion& box) {
    std::string text;
    for (std::size_t row = box.top; row <= box.bottom; ++row) text += row_text(*frame->lines[row]) + '\n';
    return text;
}

std::size_t outside_rows(const FramePtr& frame, const BoxRegion& box) {
    std::size_t rows = 0;
    for (std::size_t row = 0; row < frame->lines.size(); ++row) {
        if (row >= box.top && row <= box.bottom) continue;
        if (row_text(*frame->lines[row]).find_first_not_of(' ') != std::string::npos) ++rows;
    }
    return rows;
}

struct WakeCounter {
    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t generation{};
    void signal() {
        { std::lock_guard lock(mutex); ++generation; }
        cv.notify_all();
    }
};

enum class FixtureMode { primary, fullscreen, compact_alternate };

std::string_view mode_name(FixtureMode mode) {
    switch (mode) {
    case FixtureMode::primary: return "primary/default";
    case FixtureMode::fullscreen: return "alternate/fullscreen-incremental";
    case FixtureMode::compact_alternate: return "alternate/compact-incremental";
    }
    return "unknown";
}

class FixtureSession {
public:
    std::shared_ptr<WakeCounter> wake = std::make_shared<WakeCounter>();
    std::shared_ptr<Session> session;
    const std::string scenario;
    const bool expected_alternate;
    const bool expected_fullscreen;

    FixtureSession(const std::string& node, const std::string& fixture, FixtureMode mode,
                   TerminalSize initial_size = {80, 24, 8, 16})
        : scenario(mode_name(mode)), expected_alternate(mode != FixtureMode::primary), expected_fullscreen(mode == FixtureMode::fullscreen) {
        ProcessOptions process;
        process.executable = node;
        process.arguments = {fixture};
        if (expected_alternate) {
            process.arguments.emplace_back("--alternate-screen");
            process.arguments.emplace_back("--incremental");
        }
        if (mode == FixtureMode::fullscreen) process.arguments.emplace_back("--fullscreen");
        const auto directory = utf8_path(fixture).parent_path().u8string();
        process.working_directory.assign(directory.begin(), directory.end());
        // CI remains inherited on purpose. Ink's interactive:true override is
        // safe only because the fixture also requires actual TTY streams.
        // Make color checks deterministic, regardless of the test runner's
        // NO_COLOR/FORCE_COLOR/INK_SCREEN_READER preferences.
        process.environment = {{"NO_COLOR", std::nullopt}, {"FORCE_COLOR", "3"},
            {"INK_SCREEN_READER", std::nullopt}, {"NODE_OPTIONS", std::nullopt}};
        TerminalOptions terminal;
        terminal.size = initial_size;
        terminal.default_foreground = Color{230, 230, 230, 255};
        terminal.default_background = Color{16, 20, 30, 255};
        session = Session::start(std::move(process), terminal, [signal = wake] { signal->signal(); });
    }

    ~FixtureSession() {
        session->request_close();
        // Background test thread only. Reap even on an assertion failure so no
        // Node fixture is left running after a failed test.
        (void)session->wait_closed(5s);
    }

    template<class Predicate>
    FramePtr await(Predicate predicate, std::string_view description, std::chrono::milliseconds timeout = 12s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::optional<std::uint64_t> last_generation;
        std::size_t observed_frames = 0, incomplete_frames = 0, coherent_frames = 0;
        FramePtr last_incomplete;
        std::string last_problem;
        for (;;) {
            std::uint64_t observed;
            { std::lock_guard lock(wake->mutex); observed = wake->generation; }
            const auto frame = session->frame();
            if (frame && last_generation != frame->generation) {
                last_generation = frame->generation;
                ++observed_frames;
                bool coherent = false;
                try {
                    check_live_frame(frame);
                    check(frame->alternate_screen == expected_alternate, "Ink is not in the requested screen mode");
                    if (expected_fullscreen) {
                        const auto box = active_box(frame);
                        check(box && box->top == 0 && box->bottom + 1 == frame->rows,
                            "The fullscreen Ink box does not fill the viewport height");
                    }
                    coherent = true;
                } catch (const std::runtime_error& error) {
                    // A pipe read / ConPTY repaint is not a frame transaction.
                    // Keep all semantic assertions, but allow the next owned
                    // snapshot to finish a redraw. Persistent damage times out.
                    ++incomplete_frames;
                    last_incomplete = frame;
                    last_problem = error.what();
                }
                if (coherent) {
                    ++coherent_frames;
                    if (predicate(frame)) {
                        std::cout << "Ink checkpoint [" << scenario << "] " << description
                            << ": observed=" << observed_frames << " coherent=" << coherent_frames
                            << " incomplete=" << incomplete_frames;
                        if (incomplete_frames) std::cout << " last_incomplete=" << last_problem;
                        std::cout << '\n';
                        return frame;
                    }
                }
            }
            if (session->closed() || std::chrono::steady_clock::now() >= deadline) {
                const auto info = session->info();
                const auto diagnostic = session->diagnostics();
                throw std::runtime_error("phase [" + scenario + "] " + std::string(description) +
                    "\nobserved_frames=" + std::to_string(observed_frames) + " coherent_frames=" + std::to_string(coherent_frames) +
                    " incomplete_frames=" + std::to_string(incomplete_frames) + " last_incomplete=" + last_problem +
                    "\nprovider: " + diagnostic.provider + " advanced_protocols_verified=" + std::to_string(diagnostic.advanced_protocols_verified) +
                    "\nstate: " + std::to_string(static_cast<int>(info.state)) + " error: " + info.error +
                    "\nlast visible frame:\n" + frame_diagnostic(frame) +
                    (last_incomplete && last_incomplete != frame ? "\nlast incomplete frame:\n" + frame_diagnostic(last_incomplete) : ""));
            }
            std::unique_lock lock(wake->mutex);
            wake->cv.wait_until(lock, deadline, [&] { return wake->generation != observed; });
        }
    }

    FramePtr text(std::string_view expected) {
        return await([&](const FramePtr& frame) { return frame_text(frame).find(expected) != std::string::npos; },
            std::string("visible text: ") + std::string(expected));
    }

    void key(Key key) {
        KeyInput input;
        input.key = key;
        check(session->key(input) == SessionSendResult::accepted, "Session rejected key input");
    }

    void selected(std::string_view choice) {
        const auto label = std::string("Selected: ") + std::string(choice);
        const auto marker = std::string("❯ ") + std::string(choice);
        await([&](const FramePtr& frame) {
            const auto box = active_box(frame);
            if (!box) return false;
            // The primary screen may retain earlier output above the current
            // Ink region after resize/reflow. That is not another selection in
            // the live component tree. Alternate screen has no such history.
            const auto live = box_text(frame, *box);
            return (!frame->alternate_screen || outside_rows(frame, *box) == 0) &&
                live.find(label) != std::string::npos && live.find(marker) != std::string::npos &&
                occurrences(live, "❯") == 1;
        }, std::string("keyboard selection: ") + std::string(choice));
    }

    void resize(std::uint16_t columns, std::uint16_t rows) {
        check(session->request_resize({columns, rows, 8, 16}) == SessionSendResult::accepted, "Session rejected resize");
        const auto expected = "Size: " + std::to_string(columns) + "x" + std::to_string(rows);
        const auto frame = await([&](const FramePtr& current) {
            return current->columns == columns && current->rows == rows &&
                frame_text(current).find(expected) != std::string::npos;
        }, std::string("PTY resize and coherent box: ") + expected);
        if (!frame->alternate_screen) {
            const auto historical = outside_rows(frame, *active_box(frame));
            if (historical) std::cout << "NOTE: primary-screen resize retained " << historical
                << " historical rows outside the current Ink box; pristine viewport not claimed\n";
        }
    }

    static void check_live_frame(const FramePtr& frame) {
        check(frame && frame->columns > 0 && frame->rows > 0, "No nonempty frame geometry");
        check(frame->lines.size() == frame->rows, "Frame row count disagrees with geometry");
        for (const auto& row : frame->lines) check(row && row->cells.size() == frame->columns, "Frame cell count disagrees with geometry");
        const auto text = frame_text(frame);
        check(occurrences(text, "StormTTY Ink Workbench") == 1, "Ink redraw left missing/duplicate title rows");
        check(occurrences(text, "Animation:") == 1, "Ink redraw left missing/duplicate animation rows");
        check(text.find("Unicode: café e\xCC\x81 日本 🚀") != std::string::npos, "Unicode content was lost or corrupted");
        check(wide_pair(frame, "日") && wide_pair(frame, "本") && wide_pair(frame, "🚀"), "Wide Unicode lead/continuation cell geometry is wrong");
        check(find_cell(frame, "e\xCC\x81") != nullptr, "Combining accent was not retained in its grapheme");
        check(find_cell(frame, "╭") && find_cell(frame, "╯"), "Ink box borders missing");
        const auto box = active_box(frame);
        check(box.has_value(), "The current Ink box is not bounded by its visible borders");
        const auto width = std::min<std::size_t>(frame->columns, 72);
        check(frame->lines[box->top]->cells[width - 1].grapheme == "╮" &&
            frame->lines[box->bottom]->cells[width - 1].grapheme == "╯", "Ink responsive border width is wrong");
        for (std::size_t row = box->top + 1; row < box->bottom; ++row) {
            const auto& left = frame->lines[row]->cells.front();
            const auto& right = frame->lines[row]->cells[width - 1];
            if (left.grapheme != "│" || right.grapheme != "│") {
                throw std::runtime_error("Ink live box border alignment is wrong: row=" + std::to_string(row) +
                    " expected_columns=0," + std::to_string(width - 1) + " left(" + cell_description(left) +
                    ") right(" + cell_description(right) + ")");
            }
        }
        check(!frame->alternate_screen || outside_rows(frame, *box) == 0,
            "Ink alternate screen contains residual rows outside its live box");
        bool styled_title = false;
        for (const auto& row : frame->lines) {
            if (row_text(*row).find("StormTTY Ink Workbench") == std::string::npos) continue;
            for (const auto& cell : row->cells) {
                if (cell.grapheme == "S") styled_title = cell.style.bold && cell.foreground != frame->foreground;
            }
        }
        check(styled_title, "Ink bold/color styling did not reach visible cells");
    }

    void animation() {
        std::set<std::string> states, bars, spinners;
        std::set<std::uint64_t> generations;
        const auto frame = await([&](const FramePtr& current) {
            for (const auto& row : current->lines) {
                const auto text = row_text(*row);
                const auto start = text.find("Animation: ");
                const auto left = text.find('[', start);
                const auto right = text.find(']', left);
                const auto tick = text.find("Tick: ", right);
                if (start == std::string::npos || left == std::string::npos || right == std::string::npos || tick == std::string::npos) continue;
                states.insert(text.substr(tick));
                bars.insert(text.substr(left, right - left + 1));
                spinners.insert(text.substr(start + 11, left - start - 11));
                generations.insert(current->generation);
            }
            return states.size() >= 4 && bars.size() >= 4 && spinners.size() >= 4 && generations.size() >= 4;
        }, "four distinct coherent spinner/progress frames");
        (void)frame;
    }
};

void exercise(const std::string& node, const std::string& fixture, FixtureMode mode) {
    FixtureSession test(node, fixture, mode);
    const auto first = test.text("Interactive TTY | Size: 80x24");
    check(first->alternate_screen == (mode != FixtureMode::primary), "Ink alternate-screen mode not represented in VT state");
    check(!first->cursor.visible, "Interactive Ink did not hide the editing cursor");
    const auto retained = frame_text(first);
    test.animation();
    check(frame_text(first) == retained, "An immutable published Ink frame changed after a redraw");

    test.key(Key::down);
    test.selected("Borealis");
    test.key(Key::up);
    test.selected("Aurora");
    test.key(Key::down);
    test.selected("Borealis");
    check(test.session->text("stormé") == SessionSendResult::accepted, "Session rejected committed Unicode input");
    test.text("Input: <stormé>");
    test.key(Key::backspace);
    test.text("Input: <storm>");
    check(test.session->paste("世界") == SessionSendResult::accepted, "Session rejected Unicode paste");
    const auto input_frame = test.text("Paste: <世界>");
    check(frame_text(input_frame).find("Input: <storm>") != std::string::npos, "Bracketed paste leaked into ordinary key input");

    test.resize(56, 20);
    test.animation();
    test.resize(96, 28);
    test.animation();
    test.selected("Borealis");
    test.text("Input: <storm>");
    test.text("Paste: <世界>");
    test.key(Key::enter);
    check(test.session->wait_closed(12s), "Ink did not naturally exit/reap after Enter");
    const auto info = test.session->info();
    check(info.state == SessionState::exited && info.exit_code == 0 && info.error.empty(), "Ink process exit was not clean");
    const auto final = test.session->frame();
    check(frame_text(final).find("Ink fixture exited cleanly") != std::string::npos, "Final child output was not drained into VT state");
    check(final->cursor.visible, "Ink did not restore the cursor on exit");
    check(!final->alternate_screen, "Ink did not restore the primary screen on exit");
    check(test.session->text("late") == SessionSendResult::closed, "Exited Ink session accepted input");
    std::cout << mode_name(mode)
        << ": interactive PTY, styled Unicode cells, live animation, key selection/editing, paste, shrink/grow, clean exit passed; provider="
        << test.session->diagnostics().provider << '\n';
}

// Lower-level diagnostic, deliberately outside Session acceptance coverage.
// Process reader copies bounded bytes; all TerminalCore access stays here.
int transport_probe(const std::string& node, const std::string& fixture, const std::string& destination) {
    const auto directory = utf8_path(destination);
    check(directory.is_absolute(), "Capture destination must be absolute");
    std::filesystem::create_directories(directory);
    unsigned failures = 0;
    for (const std::uint16_t columns : {56, 72, 73}) {
        const auto stem = std::to_string(columns) + "x20";
        try {
            struct Capture { std::mutex mutex; std::string bytes; bool overflow{}; };
            const auto capture = std::make_shared<Capture>();
            const auto source_path = directory / (stem + "-stdout-live.bin");
            check(!std::filesystem::exists(source_path), "Refusing to replace an existing stdout capture");
            ProcessOptions options;
            options.executable = node;
            const auto source_utf8 = source_path.u8string();
            options.arguments = {fixture, "--alternate-screen", "--incremental", "--fullscreen",
                "--capture-stdout", std::string(source_utf8.begin(), source_utf8.end())};
            const auto cwd = utf8_path(fixture).parent_path().u8string();
            options.working_directory.assign(cwd.begin(), cwd.end());
            options.grid = {columns, 20};
            options.environment = {{"NO_COLOR", std::nullopt}, {"FORCE_COLOR", "3"},
                {"INK_SCREEN_READER", std::nullopt}, {"NODE_OPTIONS", std::nullopt}};
            auto process = Process::start(std::move(options), {
                [capture](std::string_view bytes, std::stop_token) {
                    std::lock_guard lock(capture->mutex);
                    if (bytes.size() > 1024 * 1024 - capture->bytes.size()) {
                        capture->overflow = true;
                        throw std::runtime_error("Diagnostic transport capture exceeded 1 MiB");
                    }
                    capture->bytes.append(bytes);
                }, {}});
            TerminalOptions terminal;
            terminal.size = {columns, 20, 8, 16};
            terminal.default_foreground = Color{230, 230, 230, 255};
            terminal.default_background = Color{16, 20, 30, 255};
            TerminalCore live(terminal);
            std::size_t parsed = 0;
            // A bounded observation window, not a compatibility wait or pass.
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (std::chrono::steady_clock::now() < deadline) {
                std::string bytes;
                { std::lock_guard lock(capture->mutex); bytes = capture->bytes.substr(parsed); parsed = capture->bytes.size(); }
                live.feed(bytes);
                for (const auto& effect : live.take_effects()) {
                    if (effect.kind == EffectKind::write_pty)
                        check(process->try_send(effect.text) == ProcessSendResult::accepted, "Probe reply rejected");
                }
                std::this_thread::sleep_for(10ms);
            }
            std::string transported;
            { std::lock_guard lock(capture->mutex); transported = capture->bytes; }
            check(std::filesystem::file_size(source_path) <= 1024 * 1024, "Stdout capture file too large");
            std::ifstream source_input(source_path, std::ios::binary);
            const std::string produced{std::istreambuf_iterator<char>(source_input), std::istreambuf_iterator<char>()};
            source_input.close();
            process->request_close();
            check(process->wait_for_exit(5s), "Probe process did not reap");
            { std::lock_guard lock(capture->mutex); check(!capture->overflow, "Transport capture overflowed"); }
            const auto frozen_source_path = directory / (stem + "-stdout.bin");
            check(!std::filesystem::exists(frozen_source_path), "Refusing to replace an existing stdout prefix");
            std::ofstream source_output(frozen_source_path, std::ios::binary);
            source_output.write(produced.data(), static_cast<std::streamsize>(produced.size()));
            source_output.close(); check(bool(source_output), "Could not write stdout prefix");
            // The transport file is the exact pre-close prefix. Stdout may lead
            // it slightly; each is independently replayable at a static size.
            const auto transport_path = directory / (stem + "-transport.bin");
            check(!std::filesystem::exists(transport_path), "Refusing to replace an existing transport capture");
            std::ofstream output(transport_path, std::ios::binary);
            output.write(transported.data(), static_cast<std::streamsize>(transported.size()));
            output.close(); check(bool(output), "Could not write transport capture");
            for (const auto& kind : {"stdout", "transport"}) {
                const auto path = directory / (stem + "-" + kind + ".bin");
                check(std::filesystem::file_size(path) <= 1024 * 1024, "Capture file too large");
                std::ifstream input(path, std::ios::binary);
                const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                check(!bytes.empty(), "Empty probe capture");
                TerminalCore replay(terminal);
                // Child writes use LF. A TTY applies newline processing before
                // the host parser sees them; source replay models that explicitly.
                // Raw capture remains unchanged; transport replay is byte-exact.
                if (std::string_view(kind) == "stdout") replay.feed("\x1b[20h");
                replay.feed(bytes);
                const auto frame = replay.snapshot();
                std::cout << "Transport probe " << stem << " " << kind << (std::string_view(kind) == "stdout" ? " (LNM source replay)" : " (byte-exact replay)") << " bytes=" << bytes.size() << '\n'
                          << frame_diagnostic(frame);
                try {
                    FixtureSession::check_live_frame(frame);
                    const auto box = active_box(frame);
                    check(frame->alternate_screen && box && box->top == 0 && box->bottom + 1 == frame->rows,
                          "Probe fullscreen geometry is wrong");
                    std::cout << "Transport probe " << stem << " " << kind << ": coherent\n";
                } catch (const std::exception& error) {
                    ++failures;
                    std::cout << "Transport probe " << stem << " " << kind << ": FAIL: " << error.what() << '\n';
                }
            }
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "Transport probe " << stem << ": FAIL: " << error.what() << '\n';
        }
    }
    return failures ? 1 : 0;
}

// Diagnostic only: no resize requests and no changes to the fixture or its
// assertions. At 72/73 columns the same 72-column box touches/leaves the margin;
// 56 columns reproduces the failing shrink geometry without an in-flight resize.
int static_margin_probe(const std::string& node, const std::string& fixture) {
    unsigned failures = 0;
    for (const std::uint16_t columns : {56, 72, 73}) {
        const auto size = std::to_string(columns) + "x20";
        try {
            FixtureSession test(node, fixture, FixtureMode::fullscreen, {columns, 20, 8, 16});
            test.text("Interactive TTY | Size: " + size);
            test.animation();
            std::cout << "Static margin probe " << size << ": PASS; provider="
                      << test.session->diagnostics().provider << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "Static margin probe " << size << ": FAIL: " << error.what() << '\n';
        }
    }
    std::cout << "Static margin probe: " << 3 - failures << "/3 passed (startup/animation only; no resize)\n";
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3 || argc == 4 || argc == 5,
            "Usage: ink_session_tests <absolute-node-executable> <absolute-fixture.mjs> [all|primary|fullscreen|compact-alt|host-close|static-margin-probe|transport-probe <absolute-capture-dir>]");
        check(utf8_path(argv[1]).is_absolute(), "Node executable path must be absolute");
        check(utf8_path(argv[2]).is_absolute(), "Ink fixture path must be absolute");
        check(std::filesystem::is_regular_file(utf8_path(argv[1])), "Node executable is missing");
        check(std::filesystem::is_regular_file(utf8_path(argv[2])), "Ink fixture is missing");
        const std::string_view mode = argc >= 4 ? argv[3] : "all";
        if (mode == "transport-probe") {
            check(argc == 5, "transport-probe requires an absolute fresh capture directory");
            return transport_probe(argv[1], argv[2], argv[4]);
        }
        check(argc != 5, "Capture directory is only valid for transport-probe");
        check(mode == "all" || mode == "primary" || mode == "fullscreen" || mode == "compact-alt" || mode == "host-close" || mode == "static-margin-probe",
            "Unknown Ink scenario; use all, primary, fullscreen, compact-alt, host-close, or static-margin-probe");
        if (mode == "static-margin-probe") return static_margin_probe(argv[1], argv[2]);
        if (mode == "all" || mode == "primary") exercise(argv[1], argv[2], FixtureMode::primary);
        if (mode == "all" || mode == "fullscreen") exercise(argv[1], argv[2], FixtureMode::fullscreen);
        // This remains a strict regression, including the known resize gap.
        // It is neither converted to expected-pass nor skipped by 'all'.
        if (mode == "all" || mode == "compact-alt") exercise(argv[1], argv[2], FixtureMode::compact_alternate);
        if (mode == "all" || mode == "host-close") {
            FixtureSession interrupted(argv[1], argv[2], FixtureMode::compact_alternate);
            interrupted.text("Interactive TTY | Size: 80x24");
            interrupted.animation();
            interrupted.session->request_close();
            check(interrupted.session->wait_closed(12s), "Host close did not reap an actively animating Ink process");
            check(interrupted.session->info().state == SessionState::exited, "Host close left Ink in a failed session");
            std::cout << "host close during animation: process and session reaped\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Ink Session FAIL: " << error.what() << '\n';
        return 1;
    }
}
