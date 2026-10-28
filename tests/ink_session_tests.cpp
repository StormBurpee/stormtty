#include "stormtty/session.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
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

class FixtureSession {
public:
    std::shared_ptr<WakeCounter> wake = std::make_shared<WakeCounter>();
    std::shared_ptr<Session> session;

    FixtureSession(const std::string& node, const std::string& fixture, bool alternate) {
        ProcessOptions process;
        process.executable = node;
        process.arguments = {fixture};
        if (alternate) {
            process.arguments.emplace_back("--alternate-screen");
            process.arguments.emplace_back("--incremental");
        }
        const auto directory = utf8_path(fixture).parent_path().u8string();
        process.working_directory.assign(directory.begin(), directory.end());
        // CI remains inherited on purpose. Ink's interactive:true override is
        // safe only because the fixture also requires actual TTY streams.
        // Make color checks deterministic, regardless of the test runner's
        // NO_COLOR/FORCE_COLOR/INK_SCREEN_READER preferences.
        process.environment = {{"NO_COLOR", std::nullopt}, {"FORCE_COLOR", "3"},
            {"INK_SCREEN_READER", std::nullopt}, {"NODE_OPTIONS", std::nullopt}};
        TerminalOptions terminal;
        terminal.size = {80, 24, 8, 16};
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
        for (;;) {
            std::uint64_t observed;
            { std::lock_guard lock(wake->mutex); observed = wake->generation; }
            const auto frame = session->frame();
            if (frame && predicate(frame)) return frame;
            if (session->closed() || std::chrono::steady_clock::now() >= deadline) {
                const auto info = session->info();
                throw std::runtime_error(std::string(description) + "\nprovider: " + session->diagnostics().provider +
                    "\nstate: " + std::to_string(static_cast<int>(info.state)) + " error: " + info.error +
                    "\nlast visible frame:\n" + frame_text(frame));
            }
            std::unique_lock lock(wake->mutex);
            wake->cv.wait_until(lock, deadline, [&] { return wake->generation != observed; });
        }
    }

    FramePtr text(std::string_view expected) {
        return await([&](const FramePtr& frame) { return frame_text(frame).find(expected) != std::string::npos; },
            std::string("Missing visible text: ") + std::string(expected));
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
        }, "Ink keyboard selection did not move its visible marker");
    }

    void resize(std::uint16_t columns, std::uint16_t rows) {
        check(session->request_resize({columns, rows, 8, 16}) == SessionSendResult::accepted, "Session rejected resize");
        const auto expected = "Size: " + std::to_string(columns) + "x" + std::to_string(rows);
        const auto frame = await([&](const FramePtr& current) {
            return current->columns == columns && current->rows == rows &&
                frame_text(current).find(expected) != std::string::npos;
        }, "PTY resize did not reach Ink useWindowSize and the visible VT frame");
        check_live_frame(frame);
        if (!frame->alternate_screen) {
            const auto historical = outside_rows(frame, *active_box(frame));
            if (historical) std::cout << "NOTE: primary-screen resize retained " << historical
                << " historical rows outside the current Ink box; pristine viewport not claimed\n";
        }
    }

    static void check_live_frame(const FramePtr& frame) {
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
            check(frame->lines[row]->cells.front().grapheme == "│" &&
                frame->lines[row]->cells[width - 1].grapheme == "│", "Ink live box border alignment is wrong");
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
        }, "Ink spinner/progress did not produce four distinct visible frames");
        check_live_frame(frame);
    }
};

void exercise(const std::string& node, const std::string& fixture, bool alternate) {
    FixtureSession test(node, fixture, alternate);
    const auto first = test.text("Interactive TTY | Size: 80x24");
    FixtureSession::check_live_frame(first);
    check(first->alternate_screen == alternate, "Ink alternate-screen mode not represented in VT state");
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
    FixtureSession::check_live_frame(input_frame);

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
    std::cout << (alternate ? "alternate/incremental" : "primary/default")
        << ": interactive PTY, styled Unicode cells, live animation, key selection/editing, paste, shrink/grow, clean exit passed; provider="
        << test.session->diagnostics().provider << '\n';
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3, "Usage: ink_session_tests <absolute-node-executable> <absolute-fixture.mjs>");
        check(utf8_path(argv[1]).is_absolute(), "Node executable path must be absolute");
        check(utf8_path(argv[2]).is_absolute(), "Ink fixture path must be absolute");
        check(std::filesystem::is_regular_file(utf8_path(argv[1])), "Node executable is missing");
        check(std::filesystem::is_regular_file(utf8_path(argv[2])), "Ink fixture is missing");
        exercise(argv[1], argv[2], false);
        exercise(argv[1], argv[2], true);
        {
            FixtureSession interrupted(argv[1], argv[2], true);
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
