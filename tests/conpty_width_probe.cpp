// Qualification only. Never linked into StormTTY; no VT rewriting.
#include "stormtty/process.hpp"
#include "stormtty/terminal.hpp"
#define NOMINMAX
#include <windows.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
using namespace stormtty;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::wstring wide(std::string_view s) {
    const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    check(n > 0, "Invalid UTF-8"); std::wstring w(static_cast<std::size_t>(n), L'\0');
    check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n) == n, "UTF-8 decode failed"); return w;
}
std::string utf8(std::wstring_view s) {
    const auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    check(n > 0, "Invalid UTF-16"); std::string b(static_cast<std::size_t>(n), '\0');
    check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), b.data(), n, nullptr, nullptr) == n, "UTF-16 encode failed"); return b;
}
struct File {
    HANDLE h{INVALID_HANDLE_VALUE};
    explicit File(const std::filesystem::path& path) : h(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)) { check(h != INVALID_HANDLE_VALUE, "Evidence file must be new and writable"); }
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    void write(std::string_view b) { DWORD n{}; check(WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &n, nullptr) && n == b.size(), "Evidence write failed"); }
};
int child(const std::filesystem::path& evidence, unsigned columns, bool file_route) {
    File log(evidence);
    auto source_path = evidence; source_path.replace_extension(".stdout.bin"); File source(source_path);
    const auto out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD original{}, input{};
    check(GetConsoleMode(out, &original), "Child stdout is not console");
    check(GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &input), "Child stdin is not console");
    log.write("route=" + std::string(file_route ? "WriteFile_UTF8" : "WriteConsoleW_libuv_style") + "\noriginal_output_mode=" + std::to_string(original) + "\noriginal_input_mode=" + std::to_string(input) + "\noriginal_output_cp=" + std::to_string(GetConsoleOutputCP()) + "\noriginal_input_cp=" + std::to_string(GetConsoleCP()) + "\n");
    // libuv enables VT without changing the other output mode bits. UTF-8 CP is
    // explicit for the comparison route; WriteConsoleW itself ignores the CP.
    check(SetConsoleMode(out, original | ENABLE_VIRTUAL_TERMINAL_PROCESSING), "Enable VT failed");
    check(SetConsoleOutputCP(CP_UTF8) && SetConsoleCP(CP_UTF8), "UTF-8 codepage setup failed");
    DWORD effective{}; check(GetConsoleMode(out, &effective), "Read effective mode failed");
    log.write("effective_output_mode=" + std::to_string(effective) + "\neffective_output_cp=" + std::to_string(GetConsoleOutputCP()) + "\neffective_input_cp=" + std::to_string(GetConsoleCP()) + "\nlabel\tx\ty\tbuffer_width\twindow_width\n");
    auto sample = [&](std::string_view label) {
        CONSOLE_SCREEN_BUFFER_INFO info{}; check(GetConsoleScreenBufferInfo(out, &info), "GetConsoleScreenBufferInfo failed");
        check(info.srWindow.Right - info.srWindow.Left + 1 == static_cast<int>(columns), "Unexpected console width");
        log.write(std::string(label) + "\t" + std::to_string(info.dwCursorPosition.X) + "\t" + std::to_string(info.dwCursorPosition.Y) + "\t" + std::to_string(info.dwSize.X) + "\t" + std::to_string(info.srWindow.Right - info.srWindow.Left + 1) + "\n");
    };
    auto emit = [&](std::string_view bytes) {
        source.write(bytes);
        DWORD n{};
        if (file_route) check(WriteFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr) && n == bytes.size(), "Console WriteFile failed");
        else { const auto w = wide(bytes); check(WriteConsoleW(out, w.data(), static_cast<DWORD>(w.size()), &n, nullptr) && n == w.size(), "WriteConsoleW failed"); }
    };
    emit("\x1b[2J\x1b[H"); sample("initial");
    emit(std::string(columns, 'A')); sample("ascii_exact_fill");
    std::this_thread::sleep_for(150ms);
    emit("\r\n"); sample("ascii_crlf");
    // These are exactly the fixture's Unicode atoms; combining mark stays a
    // distinct write/sample so its cursor contribution is directly observable.
    const std::array<std::string_view, 12> atoms{"│ ", "Unicode: ", "caf", "é", " ", "e", "́", " ", "日", "本", " ", "🚀"};
    const std::array<std::string_view, 12> labels{"left_border", "label", "caf", "precomposed_e", "space1", "base_e", "combining_acute", "space2", "cjk_day", "cjk_origin", "space3", "rocket"};
    for (std::size_t i = 0; i < atoms.size(); ++i) { emit(atoms[i]); sample(labels[i]); }
    emit(std::string(columns - 26, ' ')); sample("unicode_padding");
    emit("\x1b[36m│\x1b[39m"); sample("unicode_segmented_border");
    std::this_thread::sleep_for(150ms); emit("\r\n"); sample("unicode_segmented_crlf");
    std::string row = "\x1b[36m│\x1b[39m Unicode: café é 日本 🚀";
    row += std::string(columns - 26, ' '); row += "\x1b[36m│\x1b[39m";
    emit(row); sample("unicode_batched_border");
    std::this_thread::sleep_for(150ms); emit("\r\n"); sample("unicode_batched_crlf");
    // Original fixture caps the box at 72 columns. Preserve that separate
    // non-exact-fill control at width 73 instead of conflating the two.
    if (columns == 73) {
        std::string capped = "\x1b[36m│\x1b[39m Unicode: café é 日本 🚀";
        capped += std::string(46, ' '); capped += "\x1b[36m│\x1b[39m";
        emit(capped); sample("fixture_capped72_border");
        std::this_thread::sleep_for(150ms); emit("\r\n"); sample("fixture_capped72_crlf");
    }
    // No EL is supplied by this child. Any EL in transport is provider-emitted.
    std::this_thread::sleep_for(300ms);
    check(FlushFileBuffers(log.h), "Flush evidence failed"); return 0;
}
void save(const std::filesystem::path& p, std::string_view b) { File f(p); f.write(b); }
struct Capture { std::mutex mutex; std::string bytes; std::optional<ProcessExit> exit; };
int parent(const std::filesystem::path& directory, const std::string& provider) {
    check(directory.is_absolute() && !std::filesystem::exists(directory), "Capture directory must be absolute and fresh");
    check(std::filesystem::create_directories(directory), "Create capture directory failed");
    std::wstring exe(32768, L'\0'); const auto count = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    check(count > 0 && count < exe.size(), "Get executable path failed"); exe.resize(count);
    unsigned failures{};
    for (const std::uint16_t columns : {56, 72, 73}) for (const bool file_route : {false, true}) {
        const auto stem = std::to_string(columns) + (file_route ? "-writefile" : "-writeconsole");
        auto capture = std::make_shared<Capture>();
        ProcessOptions options; options.executable = utf8(exe); options.grid = {columns, 20};
        options.arguments = {"--child", utf8((directory / (stem + "-cursor.tsv")).wstring()), std::to_string(columns), file_route ? "file" : "console"};
        if (!provider.empty()) { options.provider = ProcessProvider::windows_packaged_experimental; options.provider_directory = provider; }
        auto process = Process::start(std::move(options), {
            [capture](std::string_view b, std::stop_token) { std::lock_guard lock(capture->mutex); check(b.size() <= 1024 * 1024 - capture->bytes.size(), "Transport capture exceeds 1 MiB"); capture->bytes.append(b); },
            [capture](ProcessExit exit) { std::lock_guard lock(capture->mutex); capture->exit = std::move(exit); }});
        TerminalOptions opts; opts.size = {columns, 20, 8, 16}; TerminalCore core(opts);
        std::size_t consumed{};
        auto feed = [&] {
            std::string bytes; { std::lock_guard lock(capture->mutex); bytes = capture->bytes.substr(consumed); consumed = capture->bytes.size(); }
            core.feed(bytes);
            for (const auto& effect : core.take_effects()) if (effect.kind == EffectKind::write_pty)
                check(process->try_send(effect.text) == ProcessSendResult::accepted, "Terminal reply rejected");
        };
        bool exited{};
        try {
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            do { feed(); exited = process->wait_for_exit(10ms); } while (!exited && std::chrono::steady_clock::now() < deadline);
            if (!exited) { process->request_close(); exited = process->wait_for_exit(5s); ++failures; }
            feed();
        } catch (...) { process->request_close(); (void)process->wait_for_exit(5s); throw; }
        const auto diagnostics = process->diagnostics();
        std::string bytes; std::optional<ProcessExit> result;
        { std::lock_guard lock(capture->mutex); bytes = capture->bytes; result = capture->exit; }
        save(directory / (stem + "-transport.bin"), bytes);
        const auto frame = core.snapshot();
        std::string viewport;
        for (const auto& row : frame->lines) {
            for (const auto& cell : row->cells) if (cell.span != 0)
                viewport += cell.grapheme.empty() ? " " : cell.grapheme;
            viewport += '\n';
        }
        save(directory / (stem + "-ghostty-viewport.txt"), viewport);
        std::string meta = "provider=" + diagnostics.provider + "\ndetail=" + diagnostics.detail + "\nbundled_verified=" + std::to_string(diagnostics.bundled_provider) + "\nreaped=" + std::to_string(exited) + "\nbytes=" + std::to_string(bytes.size()) + "\n";
        if (result) meta += "exit_code=" + (result->exit_code ? std::to_string(*result->exit_code) : "unknown") + "\nerror=" + result->error + "\n";
        save(directory / (stem + "-metadata.txt"), meta);
        if (!exited || !result || result->exit_code != 0 || !result->error.empty() || bytes.empty()) ++failures;
        std::cout << stem << ": " << meta;
    }
    std::cout << "Diagnostic collection failures=" << failures << "; this is not an Ink compatibility pass\n";
    return failures ? 1 : 0;
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--child") {
            const auto columns = static_cast<unsigned>(std::stoul(argv[3]));
            check(columns == 56 || columns == 72 || columns == 73, "Invalid child grid");
            check(std::string_view(argv[4]) == "file" || std::string_view(argv[4]) == "console", "Invalid route");
            return child(wide(argv[2]), columns, std::string_view(argv[4]) == "file");
        }
        check(argc == 2 || (argc == 4 && std::string_view(argv[2]) == "--packaged-provider"), "Usage: conpty_width_probe <fresh-absolute-dir> [--packaged-provider <verified-dir>]");
        return parent(wide(argv[1]), argc == 4 ? argv[3] : "");
    } catch (const std::exception& error) {
        // Child diagnostics must never alter the measured console stream.
        if (argc > 1 && std::string_view(argv[1]) == "--child") return 90;
        std::cerr << error.what() << '\n'; return 1;
    }
}
