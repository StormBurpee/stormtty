#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <imm.h>
#include <wincodec.h>
#include "stormtty/native_window.hpp"
#include "stormtty/renderer.hpp"
#include "stormtty/session.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <deque>
#include <mutex>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace stormtty {
namespace {
constexpr UINT wake_message = WM_APP + 41;
constexpr UINT_PTR cursor_timer = 1;
constexpr UINT_PTR smoke_timeout_timer = 2;
constexpr std::string_view smoke_marker="__STORMTTY_NATIVE_SMOKE_OK__";
constexpr std::size_t clipboard_limit = 1024 * 1024;

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return L"\ufffd";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("Invalid UTF-16 text received from Windows.");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
Color system_color(int index) { const auto c = GetSysColor(index); return {GetRValue(c), GetGValue(c), GetBValue(c), 255}; }
Color color_hex(std::string_view text, Color fallback) {
    if (text.size() != 7 || text[0] != '#') return fallback;
    unsigned value{};
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i]; const int n = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (n < 0) return fallback;
        value = value * 16 + static_cast<unsigned>(n);
    }
    return {static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value), 255};
}
std::uint16_t terminal_modifiers() {
    std::uint16_t result{};
    const auto pressed = [](int key) { return (GetKeyState(key) & 0x8000) != 0; };
    if (pressed(VK_SHIFT)) result |= Modifier::shift;
    if (pressed(VK_CONTROL)) result |= Modifier::control;
    if (pressed(VK_MENU)) result |= Modifier::alt;
    if (pressed(VK_LWIN) || pressed(VK_RWIN)) result |= Modifier::super;
    if (pressed(VK_RSHIFT)) result |= Modifier::right_shift;
    if (pressed(VK_RCONTROL)) result |= Modifier::right_control;
    if (pressed(VK_RMENU)) result |= Modifier::right_alt;
    if (GetKeyState(VK_CAPITAL) & 1) result |= Modifier::caps_lock;
    if (GetKeyState(VK_NUMLOCK) & 1) result |= Modifier::num_lock;
    return result;
}
Key physical_key(WPARAM vk, LPARAM flags) {
    if (vk >= 'A' && vk <= 'Z') return static_cast<Key>(static_cast<int>(Key::a) + static_cast<int>(vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<Key>(static_cast<int>(Key::digit0) + static_cast<int>(vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12) return static_cast<Key>(static_cast<int>(Key::f1) + static_cast<int>(vk - VK_F1));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return static_cast<Key>(static_cast<int>(Key::numpad0) + static_cast<int>(vk - VK_NUMPAD0));
    switch (vk) {
    case VK_RETURN: return flags & (1LL << 24) ? Key::numpad_enter : Key::enter;
    case VK_TAB: return Key::tab; case VK_BACK: return Key::backspace; case VK_ESCAPE: return Key::escape;
    case VK_SPACE: return Key::space; case VK_INSERT: return Key::insert; case VK_DELETE: return Key::del;
    case VK_HOME: return Key::home; case VK_END: return Key::end; case VK_PRIOR: return Key::page_up; case VK_NEXT: return Key::page_down;
    case VK_LEFT: return Key::left; case VK_RIGHT: return Key::right; case VK_UP: return Key::up; case VK_DOWN: return Key::down;
    case VK_OEM_3: return Key::backquote; case VK_OEM_5: case VK_OEM_102: return Key::backslash;
    case VK_OEM_4: return Key::bracket_left; case VK_OEM_6: return Key::bracket_right;
    case VK_OEM_COMMA: return Key::comma; case VK_OEM_PLUS: return Key::equal; case VK_OEM_MINUS: return Key::minus;
    case VK_OEM_PERIOD: return Key::period; case VK_OEM_7: return Key::quote; case VK_OEM_1: return Key::semicolon; case VK_OEM_2: return Key::slash;
    case VK_ADD: return Key::numpad_add; case VK_SUBTRACT: return Key::numpad_subtract; case VK_MULTIPLY: return Key::numpad_multiply;
    case VK_DIVIDE: return Key::numpad_divide; case VK_DECIMAL: return Key::numpad_decimal;
    default: return Key::unidentified;
    }
}
bool nontext_key(WPARAM key) {
    return key == VK_RETURN || key == VK_TAB || key == VK_BACK || key == VK_ESCAPE ||
           (key >= VK_PRIOR && key <= VK_DOWN) || key == VK_INSERT || key == VK_DELETE || (key >= VK_F1 && key <= VK_F12);
}
std::string chord_key(WPARAM key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::string(1, static_cast<char>(key));
    if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1);
    switch (key) {
    case VK_TAB: return "TAB"; case VK_RETURN: return "ENTER"; case VK_ESCAPE: return "ESCAPE";
    case VK_LEFT: return "LEFT"; case VK_RIGHT: return "RIGHT"; case VK_UP: return "UP"; case VK_DOWN: return "DOWN";
    case VK_SPACE: return "SPACE"; case VK_BACK: return "BACKSPACE"; case VK_DELETE: return "DELETE";
    case VK_INSERT: return "INSERT"; case VK_HOME: return "HOME"; case VK_END: return "END";
    case VK_PRIOR: return "PAGEUP"; case VK_NEXT: return "PAGEDOWN"; case VK_ADD: return "PLUS";
    case VK_OEM_COMMA: return ","; case VK_OEM_PLUS: return "="; case VK_OEM_MINUS: return "-";
    case VK_OEM_PERIOD: return "."; case VK_OEM_1: return ";"; case VK_OEM_2: return "/";
    case VK_OEM_3: return "`"; case VK_OEM_4: return "["; case VK_OEM_5: case VK_OEM_102: return "\\";
    case VK_OEM_6: return "]"; case VK_OEM_7: return "'"; default: return {};
    }
}
struct WakeState {
    std::atomic<HWND> window{};
    std::atomic<bool> posted{};
    std::mutex mutex;
    std::optional<std::string> clipboard;
    std::optional<std::string> error;
    void wake() noexcept {
        if (auto hwnd = window.load(std::memory_order_acquire)) {
            if (!posted.exchange(true, std::memory_order_acq_rel) && !PostMessageW(hwnd, wake_message, 0, 0)) posted.store(false, std::memory_order_release);
        }
    }
};
struct OwnedMenu {
    HMENU value{CreatePopupMenu()};
    ~OwnedMenu() { if (value) DestroyMenu(value); }
    operator HMENU() const noexcept { return value; }
};
struct ClipboardLock {
    bool open{};
    explicit ClipboardLock(HWND window) : open(OpenClipboard(window) != FALSE) {}
    ~ClipboardLock() { if (open) CloseClipboard(); }
};
struct ImmContext {
    HWND window{}; HIMC context{};
    explicit ImmContext(HWND hwnd) : window(hwnd), context(ImmGetContext(hwnd)) {}
    ~ImmContext() { if (context) ImmReleaseContext(window, context); }
};
struct WindowClass {
    HINSTANCE instance{GetModuleHandleW(nullptr)}; ATOM atom{};
    ~WindowClass() { if (atom) UnregisterClassW(L"StormTTY.NativeWindow", instance); }
};

class NativeWindow final {
public:
    explicit NativeWindow(Config config, NativeRunOptions options) : config_(std::move(config)), options_(std::move(options)), wake_(std::make_shared<WakeState>()) {}
    ~NativeWindow() {
        wake_->window.store(nullptr, std::memory_order_release);
        for (auto& [id, session] : sessions_) { (void)id; session->request_close(); }
        if (hwnd_) DestroyWindow(hwnd_);
    }
    int run();
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM) noexcept;
    LRESULT message(UINT, WPARAM, LPARAM);
    bool key_message(MSG&);
    void create();
    void settings_changed();
    void refresh_layout();
    void update_sessions();
    void sync_focus();
    void changed(bool geometry = false);
    void render();
    bool smoke_frame_ready() const;
    void smoke_progress(bool presented = false);
    void smoke_finish(std::string_view error = {});
    void capture_frame();
    void submit(std::function<SessionSendResult()> action, std::size_t bytes = 64);
    void drain_input();
    WindowScene scene();
    void dispatch(std::string_view command);
    CommandContext command_context() const;
    Session* active_session() const;
    Session* session_for(PaneId) const;
    SessionId new_session();
    void new_tab();
    void split(Direction);
    void close_pane(PaneId);
    void close_tab(TabId);
    bool confirm_close(std::string_view what);
    void retire_unused_sessions();
    void activate(PaneId);
    void terminal_text(std::string, Key key = Key::unidentified, std::uint16_t modifiers = 0);
    void terminal_mouse(Point, MouseAction, MouseButton, bool);
    void copy();
    void paste();
    void set_clipboard(std::string_view);
    void toggle_palette();
    std::vector<const Command*> palette_commands() const;
    void popup_menu(Point, bool pane_actions = false);
    void pointer_down(Point, bool double_click);
    void pointer_move(Point);
    void pointer_up(Point);
    void cancel_drag();
    void position_ime();
    void composition(LPARAM);
    void caret_schedule();
    void toggle_fullscreen();
    void show_error(std::string_view);
    std::string pane_title(PaneId) const;
    std::string tab_title(TabId) const;
    PixelRect pixels(Rect r) const { return {static_cast<float>(r.x * scale_), static_cast<float>(r.y * scale_), static_cast<float>(r.width * scale_), static_cast<float>(r.height * scale_)}; }
    Point point(LPARAM lparam, bool screen = false) const {
        POINT value{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (screen) ScreenToClient(hwnd_, &value);
        return {value.x / scale_, value.y / scale_};
    }
    Rect content_bounds(const PanePlacement& p) const {
        const double header = layout_.panes.size() > 1 ? 24.0 : 0.0;
        return {p.bounds.x + 12, p.bounds.y + header + 10, std::max(0.0, p.bounds.width - 24), std::max(0.0, p.bounds.height - header - 20)};
    }
    Config config_;
    NativeRunOptions options_;
    bool smoke_command_sent_{}, smoke_marker_presented_{}, smoke_finished_{};
    int exit_code_{};
    Workspace workspace_;
    std::map<SessionId, std::shared_ptr<Session>> sessions_;
    std::map<SessionId, TerminalSize> sizes_;
    std::map<SessionId, SessionState> session_states_;
    SessionId focused_session_{};
    std::uint64_t next_session_{1};
    std::shared_ptr<WakeState> wake_;
    HWND hwnd_{};
    std::unique_ptr<Renderer> renderer_;
    double scale_{1}, width_{1100}, height_{720};
    UINT dpi_{96};
    ChromeLayout chrome_;
    Rect zoom_return_;
    Layout layout_;
    std::uint64_t chrome_generation_{1};
    bool dirty_{true}, minimized_{}, maximized_{}, active_{true}, high_contrast_{}, reduced_motion_{}, in_size_move_{}, render_busy_{};
    bool palette_open_{}, rename_open_{}, composing_{}, cursor_on_{true}, cursor_timer_active_{}, fullscreen_{};
    WINDOWPLACEMENT restore_placement_{};
    LONG_PTR restore_style_{};
    std::string query_, preedit_, notice_;
    std::size_t palette_index_{};
    TabId rename_tab_{};
    Utf16Input text_input_;
    WPARAM text_vk_{}; LPARAM text_flags_{};
    std::uint16_t text_modifiers_{};
    std::array<bool, 256> consumed_keys_{};
    Point hover_{-1,-1}, drag_origin_{};
    enum class Drag { none, divider, tab, pane, selection } drag_{Drag::none};
    bool drag_started_{};
    NodeId drag_node_{}; TabId drag_tab_{}; PaneId drag_pane_{};
    DividerPlacement drag_divider_{};
    PaneId selection_pane_{};
    std::uint16_t selection_column_{}, selection_row_{};
    std::optional<Rect> drop_preview_;
    PaneId drop_target_{};
    std::optional<Direction> drop_direction_;
    int wheel_remainder_{};
    int horizontal_wheel_remainder_{};
    PaneId mouse_capture_pane_{};
    MouseButton mouse_capture_button_{MouseButton::none};
    struct PendingInput { std::function<SessionSendResult()> action; std::size_t bytes; };
    std::deque<PendingInput> pending_input_;
    std::size_t pending_bytes_{};
    bool backdrop_{};
};

void NativeWindow::create() {
    hwnd_ = CreateWindowExW(0, L"StormTTY.NativeWindow", L"StormTTY", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) throw std::runtime_error("Could not create the native StormTTY window.");
    wake_->window.store(hwnd_, std::memory_order_release);
    dpi_ = GetDpiForWindow(hwnd_); scale_ = dpi_ / 96.0;
    RendererOptions options;
    options.force_warp = options_.smoke_test;
    options.font.family = config_.font.family;
    options.font.size_dip = static_cast<float>(config_.font.size_dip);
    options.font.ligatures = config_.font.ligatures;
    renderer_ = std::make_unique<Renderer>(hwnd_, options);
    settings_changed();
    new_tab();
    SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(1100 * scale_), static_cast<int>(720 * scale_), SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    refresh_layout();
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    if (options_.smoke_test && !SetTimer(hwnd_,smoke_timeout_timer,20000,nullptr)) throw std::runtime_error("Could not start the native smoke deadline.");
}
int NativeWindow::run() {
    WindowClass klass;
    WNDCLASSEXW definition{}; definition.cbSize=sizeof(definition);
    definition.style = CS_DBLCLKS;
    definition.lpfnWndProc = procedure;
    definition.hInstance = klass.instance;
    definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    definition.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    definition.lpszClassName = L"StormTTY.NativeWindow";
    klass.atom = RegisterClassExW(&definition);
    if (!klass.atom) throw std::runtime_error("Could not register the native window class.");
    install_windows_png_decoder();
    create();
    for (;;) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                int code=static_cast<int>(message.wParam);
                // Explicit test-mode post-loop cleanup only. No WndProc or
                // normal UI close ever waits for process/actor shutdown.
                if (options_.smoke_test) for (const auto& [id,session]:sessions_) {
                    (void)id; if (!session->wait_closed(std::chrono::seconds(5))) code=3;
                }
                return code;
            }
            if (message.hwnd == hwnd_ && key_message(message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (dirty_ && !minimized_) render();
        // No ambient loop. GPU readiness is included only while a dirty frame is
        // blocked; otherwise only actual Windows/session work wakes this thread.
        HANDLE ready = !minimized_ && dirty_ && render_busy_ && renderer_ ? static_cast<HANDLE>(renderer_->frame_latency_wait_handle()) : nullptr;
        const DWORD result = MsgWaitForMultipleObjectsEx(ready ? 1 : 0, ready ? &ready : nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED) throw std::runtime_error("The native window wait failed.");
    }
}
LRESULT CALLBACK NativeWindow::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* self = reinterpret_cast<NativeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<NativeWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->hwnd_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    try { return self->message(message, wparam, lparam); }
    catch (const std::exception& error) {
        // An exception cannot cross a Win32 ABI. Keep the real session alive;
        // expose the failure rather than replacing its contents with demo text.
        try { self->notice_ = error.what(); self->dirty_ = true; ++self->chrome_generation_; InvalidateRect(window, nullptr, FALSE); }
        catch (...) { PostMessageW(window, WM_CLOSE, 0, 0); }
    } catch (...) { PostMessageW(window, WM_CLOSE, 0, 0); }
    return 0;
}
void NativeWindow::settings_changed() {
    HIGHCONTRASTW contrast{}; contrast.cbSize=sizeof(contrast);
    high_contrast_ = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    reduced_motion_ = !animations || config_.appearance.reduced_motion==ReducedMotionPreference::enabled;
    if (config_.appearance.reduced_motion==ReducedMotionPreference::disabled && animations) reduced_motion_=false;
    DWORD transparency = 1, size = sizeof(transparency);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &size);
    const BOOL dark = config_.theme.name != "daylight" && !high_contrast_;
    DwmSetWindowAttribute(hwnd_, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark));
    const DWORD material = high_contrast_ || !transparency || config_.appearance.backdrop==BackdropPreference::none ? 1 : 2;
    backdrop_ = SUCCEEDED(DwmSetWindowAttribute(hwnd_, static_cast<DWMWINDOWATTRIBUTE>(38), &material, sizeof(material))) && material == 2;
    MARGINS margins = backdrop_ ? MARGINS{-1,-1,-1,-1} : MARGINS{1,1,1,1};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    if (renderer_) renderer_->set_background_opacity(backdrop_ ? static_cast<float>(config_.appearance.background_opacity) : 1.0f);
    changed();
}
void NativeWindow::changed(bool geometry) {
    ++chrome_generation_; dirty_ = true;
    sync_focus();
    if (geometry && renderer_) refresh_layout();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}
void NativeWindow::refresh_layout() {
    RECT bounds{}; GetClientRect(hwnd_, &bounds);
    width_ = (bounds.right - bounds.left) / scale_; height_ = (bounds.bottom - bounds.top) / scale_;
    maximized_ = IsZoomed(hwnd_) != FALSE;
    std::vector<TabId> tabs;
    for (const auto& tab : workspace_.state().tabs) tabs.push_back(tab.id);
    const auto caption_width = std::max(46.0, GetSystemMetricsForDpi(SM_CXSIZE, dpi_) / scale_);
    chrome_ = layout_chrome(width_, height_, tabs, workspace_.active_tab(), caption_width, fullscreen_ ? 0 : 40);
    zoom_return_={};
    if (const auto* tab=workspace_.tab(workspace_.active_tab()); tab && tab->zoomed_pane && chrome_.drag.width>=180 && !fullscreen_) zoom_return_={chrome_.drag.x+4,6,120,28};
    layout_ = workspace_.layout(workspace_.active_tab(), {0, chrome_.band.height, width_, std::max(0.0, height_ - chrome_.band.height)}, {1.0 / scale_, 7, 80, 48});
    if (renderer_) {
        renderer_->resize(static_cast<std::uint32_t>(std::max(0L, bounds.right)), static_cast<std::uint32_t>(std::max(0L, bounds.bottom)), static_cast<float>(dpi_));
        const auto metrics = renderer_->metrics();
        for (const auto& placement : layout_.panes) {
            const auto content = pixels(content_bounds(placement));
            TerminalSize size;
            size.columns = static_cast<std::uint16_t>(std::clamp(content.width / metrics.width, 1.0f, 1000.0f));
            size.rows = static_cast<std::uint16_t>(std::clamp(content.height / metrics.height, 1.0f, 1000.0f));
            size.cell_width = metrics.width; size.cell_height = metrics.height;
            if (sizes_[placement.session] != size) {
                sizes_[placement.session] = size;
                if (auto found = sessions_.find(placement.session); found != sessions_.end()) {
                    auto session = found->second;
                    submit([session,size] { return session->request_resize(size); });
                }
            }
        }
    }
    position_ime();
}
void NativeWindow::submit(std::function<SessionSendResult()> action, std::size_t bytes) {
    if (pending_input_.empty()) {
        const auto result = action();
        if (result == SessionSendResult::accepted || result == SessionSendResult::closed) return;
        if (result == SessionSendResult::too_large) { show_error("Input exceeds the session limit; nothing was sent."); return; }
    }
    if (pending_input_.size() >= 65536 || bytes > 4 * 1024 * 1024 - pending_bytes_) {
        show_error("Input is backpressured. This keystroke or paste was not accepted; wait for the shell to catch up.");
        MessageBeep(MB_ICONWARNING); return;
    }
    pending_bytes_ += bytes;
    pending_input_.push_back({std::move(action), bytes});
}
void NativeWindow::drain_input() {
    // Bound per-dispatch work so a flooding shell cannot starve window messages.
    std::size_t count = 0;
    while (!pending_input_.empty() && count++ < 256) {
        const auto result = pending_input_.front().action();
        if (result == SessionSendResult::would_block) break;
        if (result == SessionSendResult::too_large) show_error("Queued input exceeded the session limit and was rejected.");
        pending_bytes_ -= pending_input_.front().bytes;
        pending_input_.pop_front();
    }
    if (!pending_input_.empty() && count >= 256) wake_->wake();
}
Session* NativeWindow::session_for(PaneId pane) const {
    const auto* model = workspace_.pane(pane);
    if (!model) return nullptr;
    auto found = sessions_.find(model->session);
    return found == sessions_.end() ? nullptr : found->second.get();
}
Session* NativeWindow::active_session() const { return session_for(workspace_.active_pane()); }
SessionId NativeWindow::new_session() {
    ProcessOptions process;
    for (const auto& profile : config_.profiles) if (profile.id == config_.default_profile) {
        process.executable = profile.executable; process.arguments = profile.arguments; process.working_directory = profile.working_directory;
        for (const auto& [name, value] : profile.environment) process.environment.push_back({name, value});
        break;
    }
    // The built-in alias is resolved only against the trusted system directory,
    // never the current directory/PATH. Other configured executables must be
    // absolute and are validated by the process transport.
    std::string alias=process.executable;
    for (auto& character:alias) if (character>='A' && character<='Z') character=static_cast<char>(character-'A'+'a');
    if (options_.smoke_test || alias.empty() || alias=="cmd.exe" || alias=="cmd") {
        std::array<wchar_t, MAX_PATH + 1> directory{};
        const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        if (!length || length >= directory.size()) throw std::runtime_error("Could not discover the Windows system shell.");
        process.executable = narrow(std::wstring(directory.data(), length) + L"\\cmd.exe");
        if (options_.smoke_test) { process.arguments={"/d","/q"}; process.environment.clear(); process.working_directory.clear(); }
        else if (std::find(process.arguments.begin(),process.arguments.end(),"/d")==process.arguments.end()) process.arguments.insert(process.arguments.begin(),"/d"); // Do not unexpectedly run cmd AutoRun registry code.
    }
    TerminalOptions terminal;
    terminal.scrollback_lines=config_.scrollback_lines;
    // VT default colors belong to the engine, including OSC queries/resets.
    // Translucency is exclusively a renderer policy; terminal colors stay opaque.
    terminal.default_foreground=color_hex(config_.theme.foreground,{236,238,242,255});
    terminal.default_background=color_hex(config_.theme.background,{23,25,29,255});
    const SessionId id{next_session_++};
    std::weak_ptr<WakeState> weak = wake_;
    auto session = Session::start(std::move(process), terminal, [weak] { if (auto state = weak.lock()) state->wake(); });
    sessions_.emplace(id, std::move(session));
    return id;
}
void NativeWindow::new_tab() {
    const auto id = new_session();
    auto result = workspace_.create_tab(id, config_.default_profile);
    if (!result) { sessions_.at(id)->request_close(); show_error(result.error); retire_unused_sessions(); return; }
    changed(true);
}
void NativeWindow::split(Direction direction) {
    const auto active = workspace_.active_pane();
    if (!active) return;
    const auto id = new_session();
    auto result = workspace_.split(active, direction, id, config_.default_profile);
    if (!result) { sessions_.at(id)->request_close(); show_error(result.error); retire_unused_sessions(); return; }
    activate(*result.value); changed(true);
}
void NativeWindow::retire_unused_sessions() {
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
        const bool used = std::any_of(workspace_.state().panes.begin(), workspace_.state().panes.end(), [&](const auto& item) { return item.second.session == iterator->first; });
        if (!used) { iterator->second->request_close(); sizes_.erase(iterator->first); iterator = sessions_.erase(iterator); }
        else ++iterator;
    }
}
bool NativeWindow::confirm_close(std::string_view what) {
    const auto message = widen("Close " + std::string(what) + "?\n\nIts shell or child programs may still be running. Unsaved work can be lost.");
    return MessageBoxW(hwnd_, message.c_str(), L"StormTTY", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}
void NativeWindow::close_pane(PaneId pane) {
    auto* session = session_for(pane);
    if (session && !session->closed() && !confirm_close("this pane")) return;
    workspace_.close_pane(pane); retire_unused_sessions();
    if (workspace_.state().tabs.empty()) { DestroyWindow(hwnd_); return; }
    changed(true);
}
void NativeWindow::close_tab(TabId tab) {
    bool live = false;
    for (auto pane : workspace_.pane_order(tab)) if (auto* session = session_for(pane); session && !session->closed()) live = true;
    if (live && !confirm_close("this tab and its panes")) return;
    workspace_.close_tab(tab); retire_unused_sessions();
    if (workspace_.state().tabs.empty()) { DestroyWindow(hwnd_); return; }
    changed(true);
}
void NativeWindow::sync_focus() {
    SessionId desired{};
    if (active_ && !palette_open_ && !rename_open_) if (const auto* pane=workspace_.pane(workspace_.active_pane())) desired=pane->session;
    if (desired==focused_session_) return;
    const auto previous=focused_session_; focused_session_=desired;
    if (sessions_.contains(previous)) { auto old=sessions_.at(previous); submit([old] { return old->focus(false); }); }
    if (sessions_.contains(desired)) { auto current=sessions_.at(desired); submit([current] { return current->focus(true); }); }
}
void NativeWindow::activate(PaneId pane) {
    if (workspace_.active_pane() == pane) return;
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    if (!workspace_.focus(pane)) return;
    text_input_.reset(); cursor_on_=true; caret_schedule(); changed(true);
}
void NativeWindow::update_sessions() {
    std::vector<std::pair<PaneId,std::string>> titles;
    for (const auto& [pane_id,pane]:workspace_.state().panes) {
        auto found=sessions_.find(pane.session);
        if (found==sessions_.end()) continue;
        const auto info=found->second->info();
        if (!info.title.empty() && pane.title!=info.title) titles.emplace_back(pane_id,info.title);
        if (!info.error.empty() && notice_!=info.error) { notice_=info.error; ++chrome_generation_; }
        if (!session_states_.contains(pane.session) || session_states_[pane.session]!=info.state) { session_states_[pane.session]=info.state; ++chrome_generation_; }
    }
    // Model mutations transactionally replace state; never retain iterators or
    // Pane references across them.
    for (auto& [pane,title]:titles) { workspace_.set_pane_title(pane,std::move(title)); ++chrome_generation_; }
    std::optional<std::string> clipboard,error;
    { std::lock_guard lock(wake_->mutex); clipboard.swap(wake_->clipboard); error.swap(wake_->error); }
    if (clipboard) set_clipboard(*clipboard);
    if (error) show_error(*error);
    drain_input(); dirty_=true; caret_schedule(); position_ime();
    if (options_.smoke_test) smoke_progress();
}
void NativeWindow::show_error(std::string_view error) { notice_ = std::string(error); changed(); }

std::string NativeWindow::pane_title(PaneId id) const {
    const auto* pane = workspace_.pane(id);
    if (!pane) return {};
    std::string result = pane->title;
    if (result.empty()) {
        for (const auto& profile : config_.profiles) if (profile.id == pane->profile) { result = profile.label; break; }
        if (result.empty()) result = "Terminal";
    }
    result.erase(std::remove_if(result.begin(), result.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }), result.end());
    if (result.size() > 256) { result.resize(256); while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xc0) == 0x80) result.pop_back(); }
    if (auto* session = session_for(id); session && session->closed()) result += " · exited";
    return result;
}
std::string NativeWindow::tab_title(TabId id) const {
    const auto* tab = workspace_.tab(id);
    if (!tab) return {};
    return tab->title.empty() ? pane_title(tab->active_pane) : tab->title;
}
CommandContext NativeWindow::command_context() const {
    CommandContext context;
    context.tabs = workspace_.state().tabs.size();
    context.total_panes = workspace_.state().panes.size();
    context.panes = workspace_.pane_order(workspace_.active_tab()).size();
    context.clipboard_has_text = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
    if (const auto* tab = workspace_.tab(workspace_.active_tab())) context.zoomed = tab->zoomed_pane.has_value();
    if (auto* session = active_session()) {
        context.session_running = !session->closed();
        if (auto frame = session->frame()) for (const auto& row : frame->lines) if (row && row->selection) { context.has_selection = true; break; }
    }
    return context;
}
CommandAvailability host_availability(std::string_view id, const CommandContext& context) {
    if (id == "terminal.find") return {false, "Retained-text search is not implemented yet"};
    if (id == "settings.open") return {false, "Use --config path; an in-app settings editor is not implemented yet"};
    return command_availability(id, context);
}
void NativeWindow::toggle_palette() {
    palette_open_ = !palette_open_; rename_open_ = false; query_.clear(); palette_index_ = 0;
    composing_ = false; preedit_.clear();
    if (ImmContext context(hwnd_); context.context) ImmNotifyIME(context.context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    caret_schedule(); changed();
}
std::vector<const Command*> NativeWindow::palette_commands() const {
    std::string query = query_;
    for (auto& c : query) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    std::vector<const Command*> result;
    for (const auto& command : command_registry()) {
        std::string haystack = std::string(command.label) + " " + std::string(command.description);
        for (auto& c : haystack) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (query.empty() || haystack.find(query) != std::string::npos) result.push_back(&command);
    }
    return result;
}
void NativeWindow::dispatch(std::string_view command) {
    if (drag_!=Drag::none || mouse_capture_pane_) cancel_drag();
    if (!host_availability(command, command_context()).enabled) { show_error(host_availability(command, command_context()).reason); return; }
    notice_.clear();
    const auto pane = workspace_.active_pane();
    const auto tab = workspace_.active_tab();
    const Rect viewport{0, chrome_.band.height, width_, std::max(0.0, height_ - chrome_.band.height)};
    auto report = [this](Status status) { if (!status) show_error(status.error); };
    if (command == "app.command_palette") { toggle_palette(); return; }
    if (command == "tab.new") { new_tab(); return; }
    if (command == "tab.close") { close_tab(tab); return; }
    if (command == "pane.close") { close_pane(pane); return; }
    if (command == "pane.split_right") { split(Direction::right); return; }
    if (command == "pane.split_down") { split(Direction::down); return; }
    if (command == "edit.copy") { copy(); return; }
    if (command == "edit.paste") { paste(); return; }
    if (command == "window.fullscreen") { toggle_fullscreen(); return; }
    if (command == "tab.rename") {
        rename_open_ = true; palette_open_ = false; rename_tab_ = tab; query_ = workspace_.tab(tab)->title; changed(); return;
    }
    if (command == "tab.next" || command == "tab.previous" || command == "tab.move_left" || command == "tab.move_right") {
        const auto& tabs = workspace_.state().tabs;
        const auto found = std::find_if(tabs.begin(), tabs.end(), [tab](const Tab& item) { return item.id == tab; });
        const auto index = static_cast<std::size_t>(std::distance(tabs.begin(), found));
        const bool previous = command == "tab.previous" || command == "tab.move_left";
        const auto next = (index + tabs.size() + (previous ? tabs.size() - 1 : 1)) % tabs.size();
        if (command.starts_with("tab.move_")) report(workspace_.reorder_tab(tab, next));
        else activate(tabs[next].active_pane);
    } else if (command == "pane.zoom") report(workspace_.toggle_zoom());
    else if (command == "pane.new_tab") {
        auto result = workspace_.move_pane_to_new_tab(pane);
        if (!result) show_error(result.error);
    } else if (command == "pane.move_to_tab" || command == "pane.swap") {
        OwnedMenu menu;
        if (!menu) throw std::runtime_error("Could not create the pane destination menu.");
        std::vector<PaneId> destinations;
        if (command == "pane.move_to_tab") {
            for (const auto& candidate : workspace_.state().tabs) if (candidate.id != tab) {
                destinations.push_back(candidate.active_pane);
                AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(destinations.size()), widen(tab_title(candidate.id)).c_str());
            }
        } else for (const auto target : workspace_.pane_order(tab)) if (target != pane) {
            destinations.push_back(target); AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(destinations.size()), widen(pane_title(target)).c_str());
        }
        POINT position{static_cast<LONG>(width_ * scale_ / 2), static_cast<LONG>(80 * scale_)}; ClientToScreen(hwnd_, &position);
        const auto selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY, position.x, position.y, hwnd_, nullptr);
        if (selected && static_cast<std::size_t>(selected) <= destinations.size()) {
            if (command == "pane.swap") report(workspace_.swap_panes(pane, destinations[selected - 1]));
            else report(workspace_.move_pane(pane, destinations[selected - 1], Direction::right));
        }
    } else if (command.starts_with("pane.focus_") || command.starts_with("pane.move_")) {
        const auto direction = command.ends_with("left") ? Direction::left : command.ends_with("right") ? Direction::right : command.ends_with("up") ? Direction::up : Direction::down;
        auto status = workspace_.focus_direction(direction, viewport, {1/scale_,7,80,48});
        const auto target = workspace_.active_pane();
        workspace_.focus(pane);
        if (!status) show_error(status.error);
        else if (command.starts_with("pane.focus_")) activate(target);
        else report(workspace_.move_pane(pane, target, direction));
    } else if (command.starts_with("pane.resize_") || command == "pane.equalize") {
        const auto placement = std::find_if(layout_.panes.begin(), layout_.panes.end(), [pane](const PanePlacement& p) { return p.pane == pane; });
        const DividerPlacement* best = nullptr;
        const bool horizontal = command.ends_with("left") || command.ends_with("right");
        if (placement != layout_.panes.end()) for (const auto& divider : layout_.dividers) {
            const Point center{placement->bounds.x + placement->bounds.width / 2, placement->bounds.y + placement->bounds.height / 2};
            if (!divider.split_bounds.contains(center)) continue;
            if (command != "pane.equalize" && (divider.axis == Axis::horizontal) != horizontal) continue;
            if (!best || divider.split_bounds.width * divider.split_bounds.height < best->split_bounds.width * best->split_bounds.height) best = &divider;
        }
        if (best) {
            if (command == "pane.equalize") report(workspace_.equalize_split(best->node));
            else report(workspace_.resize_split(best->node, best->applied_ratio + ((command.ends_with("left") || command.ends_with("up")) ? -0.05 : 0.05), viewport, {1/scale_,7,80,48}));
        } else show_error("There is no adjacent divider in that direction.");
    }
    changed(true);
}
WindowScene NativeWindow::scene() {
    WindowScene result;
    const Color background = high_contrast_ ? system_color(COLOR_WINDOW) : color_hex(config_.theme.background, {23,25,29,255});
    Color chrome = high_contrast_ ? system_color(COLOR_BTNFACE) : color_hex(config_.theme.chrome, {34,37,43,255});
    const bool daylight=config_.theme.name=="daylight";
    Color raised = high_contrast_ ? system_color(COLOR_WINDOW) : daylight?Color{225,230,235,255}:Color{48,52,61,255};
    const Color text = high_contrast_ ? system_color(COLOR_BTNTEXT) : color_hex(config_.theme.foreground, {236,238,242,255});
    const Color secondary = high_contrast_ ? text : daylight?Color{88,101,114,255}:Color{164,173,185,255};
    const Color accent = high_contrast_ ? system_color(COLOR_WINDOWTEXT) : color_hex(config_.theme.accent, {167,196,210,255});
    const Color seam = high_contrast_ ? text : Color{116,131,145,255};
    result.background = background; result.chrome_generation = chrome_generation_;
    auto rectangle = [&](Rect bounds, Color fill, double radius = 0, Color stroke = {}, double stroke_width = 0) {
        result.primitives.push_back({pixels(bounds), fill, stroke, static_cast<float>(radius * scale_), static_cast<float>(stroke_width * scale_), ChromePrimitiveKind::rectangle});
    };
    auto line = [&](double x1, double y1, double x2, double y2, Color color, double width = 1) {
        result.primitives.push_back({pixels({x1,y1,x2-x1,y2-y1}), {}, color, 0, static_cast<float>(width * scale_), ChromePrimitiveKind::line});
    };
    auto label = [&](Rect bounds, std::string value, Color color, float size = 13, bool bold = false) {
        result.texts.push_back({pixels(bounds), std::move(value), color, size, bold, "Segoe UI Variable Text"});
    };
    for (const auto& placement : layout_.panes) {
        if (auto* session = session_for(placement.pane)) {
            auto frame=session->frame();
            const bool cursor_visible=!frame || !frame->cursor.blinking || cursor_on_;
            result.panes.push_back({placement.pane.value,pixels(content_bounds(placement)),std::move(frame),active_ && placement.pane==workspace_.active_pane() && !palette_open_ && !rename_open_,cursor_visible,reduced_motion_ || cursor_on_});
        }
        if (layout_.panes.size() > 1) {
            const Rect header{placement.bounds.x, placement.bounds.y, placement.bounds.width, 24};
            rectangle(header, chrome);
            if (placement.pane == workspace_.active_pane()) rectangle({header.x + 8,header.y + 7,2,10}, accent);
            label({header.x + 16,header.y + 3,std::max(0.0,header.width - 48),18}, pane_title(placement.pane), placement.pane == workspace_.active_pane() ? text : secondary, 12);
            label({header.x + header.width - 28,header.y + 1,24,22}, "···", secondary, 16);
        }
    }
    for (const auto& divider : layout_.dividers) rectangle(divider.bounds, divider.hit_bounds.contains(hover_) || drag_node_ == divider.node ? accent : seam);
    if (!fullscreen_) {
        if (backdrop_) chrome.a = 248;
        rectangle(chrome_.band, chrome);
        auto button = [&](Rect bounds, bool close = false) {
            if (bounds.contains(hover_)) rectangle(bounds, close ? Color{196,43,28,255} : raised, close ? 0 : 4);
        };
        button(chrome_.menu); button(chrome_.new_tab); button(chrome_.palette);
        if (zoom_return_.width) {
            button(zoom_return_);
            label({zoom_return_.x+8,zoom_return_.y+5,zoom_return_.width-16,20},"Return to split",accent,12);
        }
        for (const auto& tab : chrome_.tabs) {
            const bool selected = tab.tab == workspace_.active_tab();
            if (selected || tab.bounds.contains(hover_)) rectangle(tab.bounds, selected ? background : raised, 6);
            if (chrome_.compact) label({tab.bounds.x+6,tab.bounds.y+7,std::max(0.0,tab.bounds.width-8),20},"▾",text,13);
            else label({tab.bounds.x + 10,tab.bounds.y + 7,std::max(0.0,tab.bounds.width - 40),20}, tab_title(tab.tab), selected ? text : secondary, 13, selected);
            if (!chrome_.compact && (selected || tab.bounds.contains(hover_))) {
                const double x = tab.close.x + tab.close.width / 2, y = tab.close.y + tab.close.height / 2;
                if (tab.close.contains(hover_)) rectangle(tab.close, raised, 4);
                line(x - 3,y - 3,x + 3,y + 3,secondary); line(x + 3,y - 3,x - 3,y + 3,secondary);
            }
        }
        for (int row = 0; row < 3; ++row) line(chrome_.menu.x + 8,chrome_.menu.y + 10 + row * 5,chrome_.menu.x + 20,chrome_.menu.y + 10 + row * 5,secondary,1.2);
        const double nx = chrome_.new_tab.x + 14, ny = chrome_.new_tab.y + 16;
        line(nx-5,ny,nx+5,ny,secondary,1.3); line(nx,ny-5,nx,ny+5,secondary,1.3);
        const double px = chrome_.palette.x + 8, py = chrome_.palette.y + 10;
        for (int y=0;y<2;++y) for (int x=0;x<2;++x) rectangle({px+x*7,py+y*7,4,4},secondary,1);
        button(chrome_.minimize); button(chrome_.maximize); button(chrome_.close,true);
        const double cy = chrome_.band.height / 2;
        auto center = [](Rect r) { return r.x+r.width/2; };
        const double minx = center(chrome_.minimize), maxx = center(chrome_.maximize), closex = center(chrome_.close);
        line(minx-5,cy+3,minx+5,cy+3,text);
        if (maximized_) {
            rectangle({maxx-3,cy-5,8,8}, {0,0,0,0}, 0, text, 1);
            rectangle({maxx-5,cy-3,8,8}, chrome, 0, text, 1);
        } else rectangle({maxx-5,cy-5,10,10}, {0,0,0,0}, 0, text,1);
        line(closex-4,cy-4,closex+4,cy+4,text); line(closex+4,cy-4,closex-4,cy+4,text);
    }
    if (drop_preview_) {
        rectangle(*drop_preview_, {167,196,210,30}, 0, accent,2);
        const auto bounds=*drop_preview_; const auto tag_width=std::min(132.0,std::max(0.0,bounds.width-16));
        const Rect tag{bounds.x+(bounds.width-tag_width)/2,bounds.y+std::max(0.0,(bounds.height-28)/2),tag_width,28};
        rectangle(tag,raised,4);
        label({tag.x+8,tag.y+5,std::max(0.0,tag.width-16),20},drop_direction_?"Move pane here":"Swap panes",text,12);
    }
    if (drag_ == Drag::tab && drag_started_) {
        for (const auto& tab : chrome_.tabs) if (tab.bounds.contains(hover_)) rectangle({tab.bounds.x,4,2,32},accent);
    }
    if (!preedit_.empty() && composing_) {
        for (const auto& placement : layout_.panes) if (placement.pane == workspace_.active_pane()) {
            if (auto* session = active_session(); session && session->frame()) {
                const auto frame = session->frame(); const auto content = content_bounds(placement); const auto metrics = renderer_->metrics();
                const double x = content.x + frame->cursor.column * metrics.width / scale_;
                const double y = content.y + frame->cursor.row * metrics.height / scale_;
                const double w = std::min(320.0,std::max(80.0,width_-x));
                rectangle({x,y,w,metrics.height/scale_},background);
                label({x,y,w,metrics.height/scale_},preedit_,text,static_cast<float>(config_.font.size_dip));
                line(x,y+metrics.height/scale_-1,x+w,y+metrics.height/scale_-1,accent);
            }
        }
    }
    if (!notice_.empty()) {
        rectangle({12,std::max(44.0,height_-56),std::max(0.0,width_-24),44},raised,6);
        label({24,std::max(49.0,height_-51),std::max(0.0,width_-64),34},notice_,text,12);
    }
    if (palette_open_ || rename_open_) {
        const double w = std::min(560.0,std::max(0.0,width_-32)), x = (width_-w)/2, y=std::min(72.0,std::max(0.0,height_-80));
        const auto commands = palette_commands();
        const std::size_t first = palette_index_ >= 8 ? palette_index_-7 : 0;
        const auto rows = rename_open_ ? std::size_t{0} : std::min<std::size_t>(8,commands.size()-std::min(first,commands.size()));
        rectangle({x,y,w,64+rows*44.0},raised,8,seam,1);
        label({x+16,y+12,w-32,22},query_.empty() ? (rename_open_ ? "Type a tab name; Enter saves, Escape cancels" : "Search commands…") : query_, query_.empty()?secondary:text,14);
        line(x+12,y+47,x+w-12,y+47,seam);
        for (std::size_t row=0;row<rows;++row) {
            const auto index=first+row; const auto& command=*commands[index];
            const auto available=host_availability(command.id,command_context());
            const double top=y+55+row*44;
            if (index==palette_index_) rectangle({x+7,top-2,w-14,42},background,4,accent,1);
            label({x+16,top,w-170,20},std::string(command.label),available.enabled?text:secondary,13);
            label({x+16,top+20,w-32,17},std::string(available.enabled?command.description:available.reason),secondary,11);
            for (const auto& binding : config_.bindings) if (binding.command==command.id) {
                label({x+w-150,top,134,20},format_key_chord(binding.chord,Platform::windows),secondary,11); break;
            }
        }
        if (!rename_open_ && commands.empty()) label({x+16,y+50,w-32,20},"No matching commands",secondary,12);
    }
    return result;
}
void NativeWindow::render() {
    if (!renderer_ || minimized_) return;
    const auto result = renderer_->render(scene());
    render_busy_ = result == RenderResult::busy;
    if (!render_busy_) dirty_ = false;
    if (options_.smoke_test) smoke_progress(result==RenderResult::presented);
    // Occlusion is retried on actual exposure/activation/output, not a timer.
}

void NativeWindow::terminal_text(std::string text, Key key, std::uint16_t modifiers) {
    if (text.empty()) return;
    if (palette_open_ || rename_open_) {
        if (query_.size() + text.size() <= 1024) query_ += text;
        palette_index_ = 0; changed(); return;
    }
    const auto* pane = workspace_.pane(workspace_.active_pane());
    if (!pane || !sessions_.contains(pane->session)) return;
    auto session = sessions_.at(pane->session);
    const auto bytes = text.size();
    if (key == Key::unidentified && !modifiers) submit([session,text = std::move(text)] { return session->text(text); },bytes);
    else {
        KeyInput input;
        input.key = key; input.modifiers = modifiers;
        input.action=(text_flags_ & (1LL<<30))?KeyAction::repeat:KeyAction::press;
        if (key>=Key::a && key<=Key::z) input.unshifted_codepoint=static_cast<std::uint32_t>('a'+static_cast<int>(key)-static_cast<int>(Key::a));
        else if (key>=Key::digit0 && key<=Key::digit9) input.unshifted_codepoint=static_cast<std::uint32_t>('0'+static_cast<int>(key)-static_cast<int>(Key::digit0));
        // Ctrl+A etc arrive as C0 WM_CHAR units. Ghostty encodes the physical
        // key+modifiers; its text field must contain only printable layout text.
        if (text.size()!=1 || (static_cast<unsigned char>(text[0])>=0x20 && text[0]!=0x7f)) input.text = std::move(text);
        if (modifiers & Modifier::right_alt) input.consumed_modifiers = Modifier::alt | Modifier::control | Modifier::right_alt;
        submit([session,input = std::move(input)] { return session->key(input); },bytes+64);
    }
    cursor_on_ = true;
}
bool NativeWindow::key_message(MSG& message) {
    const bool down = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
    const bool up = message.message == WM_KEYUP || message.message == WM_SYSKEYUP;
    if (!down && !up) return false;
    const auto vk = message.wParam;
    const auto modifiers = terminal_modifiers();
    const bool altgr = (modifiers & Modifier::right_alt) && (modifiers & Modifier::control);
    // Preserve OS-reserved shortcuts before application or terminal routing.
    if ((modifiers & Modifier::super) || ((modifiers & Modifier::alt) && !altgr && (vk == VK_F4 || vk == VK_SPACE))) return false;
    if (up && vk < consumed_keys_.size() && consumed_keys_[vk]) { consumed_keys_[vk] = false; return true; }
    if (composing_ || vk == VK_PROCESSKEY) return false;
    if (down && vk == VK_ESCAPE && drag_ != Drag::none) { cancel_drag(); consumed_keys_[vk] = true; return true; }
    if (palette_open_ || rename_open_) {
        if (up) return true;
        if (vk == VK_ESCAPE) { palette_open_ = rename_open_ = false; query_.clear(); changed(); caret_schedule(); return true; }
        if (vk == VK_BACK) {
            if (!query_.empty()) { auto start=query_.size()-1; while (start>0 && (static_cast<unsigned char>(query_[start]) & 0xc0)==0x80) --start; query_.resize(start); }
            palette_index_ = 0; changed(); return true;
        }
        if (vk == VK_RETURN) {
            if (rename_open_) { workspace_.rename_tab(rename_tab_,query_); rename_open_ = false; changed(); }
            else {
                const auto commands = palette_commands();
                if (!commands.empty()) {
                    const auto command = std::string(commands[std::min(palette_index_,commands.size()-1)]->id);
                    if (host_availability(command,command_context()).enabled) { palette_open_=false; query_.clear(); dispatch(command); }
                }
            }
            caret_schedule(); return true;
        }
        if (!rename_open_ && (vk == VK_UP || vk == VK_DOWN)) {
            const auto commands = palette_commands();
            if (!commands.empty()) palette_index_ = (palette_index_ + commands.size() + (vk == VK_UP ? commands.size()-1 : 1)) % commands.size();
            changed(); return true;
        }
        // Text remains on the OS TranslateMessage/WM_CHAR path for dead keys.
        text_vk_ = vk; text_flags_ = message.lParam; text_modifiers_ = modifiers;
        return false;
    }
    if (down) {
        CommandModifier app_modifiers = CommandModifier::none;
        if (modifiers & Modifier::control) app_modifiers = app_modifiers | CommandModifier::control;
        if (modifiers & Modifier::shift) app_modifiers = app_modifiers | CommandModifier::shift;
        if (modifiers & Modifier::alt) app_modifiers = app_modifiers | CommandModifier::alt;
        const KeyEvent event{{chord_key(vk),app_modifiers},false,altgr,true};
        if (const auto command = resolve_command(event, config_.bindings, command_context(), Platform::windows)) {
            if (vk < consumed_keys_.size()) consumed_keys_[vk] = true;
            dispatch(*command); return true;
        }
    }
    const auto* pane = workspace_.pane(workspace_.active_pane());
    if (!pane || !sessions_.contains(pane->session)) return false;
    const Key key = physical_key(vk,message.lParam);
    if (key == Key::unidentified) return false;
    if (up || nontext_key(vk)) {
        auto session = sessions_.at(pane->session);
        KeyInput input;
        input.key = key; input.modifiers = modifiers;
        input.action = up ? KeyAction::release : (message.lParam & (1LL << 30) ? KeyAction::repeat : KeyAction::press);
        submit([session,input] { return session->key(input); });
        if (down) cursor_on_ = true;
        return true; // Do not also manufacture WM_CHAR for Enter/Tab/Backspace.
    }
    text_vk_ = vk; text_flags_ = message.lParam; text_modifiers_ = modifiers;
    return false;
}
void NativeWindow::copy() {
    const auto* pane = workspace_.pane(workspace_.active_pane());
    if (!pane || !sessions_.contains(pane->session)) return;
    auto session = sessions_.at(pane->session);
    std::weak_ptr<WakeState> weak = wake_;
    submit([session,weak] { return session->copy_selection([weak](std::string text) {
        if (auto state=weak.lock(); state && state->window.load(std::memory_order_acquire)) {
            {
                std::lock_guard lock(state->mutex);
                if (text.size()>clipboard_limit) state->error="Selection exceeds the 1 MiB clipboard limit; nothing was copied.";
                else state->clipboard=std::move(text);
            }
            state->wake();
        }
    }); });
}
void NativeWindow::set_clipboard(std::string_view text) {
    if (text.empty()) return;
    const auto wide=widen(text);
    const auto bytes=(wide.size()+1)*sizeof(wchar_t);
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if (!memory) { show_error("Could not allocate clipboard text."); return; }
    void* target=GlobalLock(memory);
    if (!target) { GlobalFree(memory); show_error("Could not lock clipboard text."); return; }
    std::memcpy(target,wide.c_str(),bytes); GlobalUnlock(memory);
    ClipboardLock clipboard(hwnd_);
    if (!clipboard.open || !EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT,memory)) {
        GlobalFree(memory); show_error("Clipboard is busy; the selection was not copied.");
    }
    // Successful SetClipboardData transfers ownership to Windows.
}
void NativeWindow::paste() {
    std::string text;
    {
        ClipboardLock clipboard(hwnd_);
        if (!clipboard.open) { show_error("Clipboard is busy; nothing was pasted."); return; }
        const HANDLE data=GetClipboardData(CF_UNICODETEXT);
        if (!data) return;
        const auto bytes=GlobalSize(data);
        if (bytes > clipboard_limit*2+2) { show_error("Paste exceeds the 1 MiB safety limit; nothing was sent."); return; }
        const auto* wide=static_cast<const wchar_t*>(GlobalLock(data));
        if (!wide) return;
        const std::size_t capacity=bytes/sizeof(wchar_t);
        std::size_t length=0; while (length<capacity && wide[length]) ++length;
        try {
            if (length==capacity) { GlobalUnlock(data); show_error("Clipboard text is not terminated; nothing was pasted."); return; }
            text=narrow(std::wstring_view(wide,length));
        } catch (...) { GlobalUnlock(data); throw; }
        GlobalUnlock(data);
    }
    if (text.size()>clipboard_limit) { show_error("Paste exceeds the 1 MiB safety limit; nothing was sent."); return; }
    const bool unsafe=!TerminalCore::paste_is_safe(text);
    if (unsafe && MessageBoxW(hwnd_,L"This paste contains line breaks or terminal control characters. It may execute commands in the active shell.\n\nPaste it anyway?",L"Review paste",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) return;
    const auto* pane=workspace_.pane(workspace_.active_pane());
    if (!pane || !sessions_.contains(pane->session)) return;
    auto session=sessions_.at(pane->session); const auto size=text.size();
    submit([session,text=std::move(text),unsafe] { return session->paste(text,unsafe); },size);
}
void NativeWindow::terminal_mouse(Point point, MouseAction action, MouseButton button, bool pressed) {
    for (const auto& placement:layout_.panes) {
        const bool captured=mouse_capture_pane_ && placement.pane==mouse_capture_pane_;
        if (mouse_capture_pane_ ? !captured : !placement.bounds.contains(point)) continue;
        const auto content=content_bounds(placement);
        const auto* pane=workspace_.pane(placement.pane);
        if (!pane || !sessions_.contains(pane->session)) return;
        auto session=sessions_.at(pane->session);
        const auto frame=session->frame();
        if (!frame || !frame->mouse_reporting || (!content.contains(point) && !captured)) {
            if (action==MouseAction::release && captured) {
                mouse_capture_pane_={}; mouse_capture_button_=MouseButton::none;
                if (GetCapture()==hwnd_) ReleaseCapture();
            }
            return;
        }
        const bool physical_button=button==MouseButton::left || button==MouseButton::right || button==MouseButton::middle;
        if (action==MouseAction::press && physical_button) {
            mouse_capture_pane_=placement.pane; mouse_capture_button_=button; SetCapture(hwnd_);
        }
        MouseInput input{action,button,terminal_modifiers(),(point.x-content.x)*scale_,(point.y-content.y)*scale_,pressed};
        submit([session,input] { return session->mouse(input); });
        if (action==MouseAction::release && physical_button) {
            mouse_capture_pane_={}; mouse_capture_button_=MouseButton::none;
            if (GetCapture()==hwnd_) ReleaseCapture();
        }
        return;
    }
}
void NativeWindow::popup_menu(Point point, bool pane_actions) {
    const auto context=command_context();
    OwnedMenu menu;
    if (!menu) return;
    std::vector<std::string_view> commands=pane_actions ? std::vector<std::string_view>{"pane.split_right","pane.split_down","pane.zoom","pane.new_tab","pane.swap","pane.move_to_tab","pane.close"} :
        std::vector<std::string_view>{"tab.new","pane.split_right","pane.split_down","tab.rename","tab.next","tab.previous","app.command_palette","window.fullscreen","tab.close"};
    for (std::size_t i=0;i<commands.size();++i) {
        const auto* command=find_command(commands[i]);
        if (command) AppendMenuW(menu,MF_STRING | (host_availability(command->id,context).enabled?MF_ENABLED:MF_GRAYED),i+1,widen(command->label).c_str());
    }
    POINT screen{static_cast<LONG>(point.x*scale_),static_cast<LONG>(point.y*scale_)}; ClientToScreen(hwnd_,&screen);
    const auto selected=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY,screen.x,screen.y,hwnd_,nullptr);
    if (selected && static_cast<std::size_t>(selected)<=commands.size()) dispatch(commands[selected-1]);
}
void NativeWindow::pointer_down(Point point, bool double_click) {
    SetFocus(hwnd_);
    notice_.clear();
    if (palette_open_ || rename_open_) {
        const double w=std::min(560.0,std::max(0.0,width_-32)),x=(width_-w)/2,y=std::min(72.0,std::max(0.0,height_-80));
        const auto commands=palette_commands(); const std::size_t first=palette_index_>=8?palette_index_-7:0;
        if (!rename_open_ && Rect{x,y+53,w,352}.contains(point)) {
            const auto index=first+static_cast<std::size_t>((point.y-y-53)/44);
            if (index<commands.size()) { palette_index_=index; const auto id=std::string(commands[index]->id); if (host_availability(id,command_context()).enabled) { palette_open_=false; dispatch(id); } }
        } else if (!Rect{x,y,w,64+std::min<std::size_t>(8,commands.size())*44.0}.contains(point)) { palette_open_=rename_open_=false; query_.clear(); }
        changed(); return;
    }
    if (zoom_return_.contains(point)) { dispatch("pane.zoom"); return; }
    if (chrome_.menu.contains(point)) { popup_menu({point.x,40}); return; }
    if (chrome_.new_tab.contains(point)) { if (GetKeyState(VK_SHIFT)&0x8000) popup_menu({point.x,40}); else new_tab(); return; }
    if (chrome_.palette.contains(point)) { toggle_palette(); return; }
    for (const auto& tab:chrome_.tabs) if (tab.bounds.contains(point)) {
        if (chrome_.compact) {
            OwnedMenu picker; if (!picker) return;
            std::vector<PaneId> panes;
            for (const auto& item:workspace_.state().tabs) {
                panes.push_back(item.active_pane);
                AppendMenuW(picker,MF_STRING|(item.id==workspace_.active_tab()?MF_CHECKED:0),panes.size(),widen(tab_title(item.id)).c_str());
            }
            POINT position{static_cast<LONG>(tab.bounds.x*scale_),static_cast<LONG>(40*scale_)}; ClientToScreen(hwnd_,&position);
            const auto chosen=TrackPopupMenuEx(picker,TPM_RETURNCMD|TPM_NONOTIFY,position.x,position.y,hwnd_,nullptr);
            if (chosen && static_cast<std::size_t>(chosen)<=panes.size()) activate(panes[chosen-1]);
            return;
        }
        if (tab.close.contains(point)) { close_tab(tab.tab); return; }
        if (double_click) { activate(workspace_.tab(tab.tab)->active_pane); dispatch("tab.rename"); return; }
        activate(workspace_.tab(tab.tab)->active_pane);
        drag_=Drag::tab; drag_tab_=tab.tab; drag_origin_=point; SetCapture(hwnd_); return;
    }
    for (const auto& divider:layout_.dividers) if (divider.hit_bounds.contains(point)) {
        if (double_click) { workspace_.equalize_split(divider.node); changed(true); return; }
        drag_=Drag::divider; drag_node_=divider.node; drag_divider_=divider; drag_origin_=point; SetCapture(hwnd_); return;
    }
    for (const auto& placement:layout_.panes) if (placement.bounds.contains(point)) {
        const auto pane=placement.pane;
        const auto bounds=placement.bounds;
        activate(pane);
        if (layout_.panes.size()>1 && point.y<bounds.y+24) {
            if (point.x>=bounds.x+bounds.width-32) { popup_menu(point,true); return; }
            if (double_click) { workspace_.toggle_zoom(); changed(true); return; }
            drag_=Drag::pane; drag_pane_=pane; drag_origin_=point; SetCapture(hwnd_); return;
        }
        auto* session=active_session(); auto frame=session?session->frame():nullptr;
        if (frame && frame->mouse_reporting && !(GetKeyState(VK_SHIFT)&0x8000)) {
            SetCapture(hwnd_); terminal_mouse(point,MouseAction::press,MouseButton::left,true); return;
        }
        for (const auto& current:layout_.panes) if (current.pane==pane) {
            const auto content=content_bounds(current); const auto metrics=renderer_->metrics();
            selection_column_=static_cast<std::uint16_t>(std::clamp((point.x-content.x)*scale_/metrics.width,0.0,frame && frame->columns?static_cast<double>(frame->columns-1):0.0));
            selection_row_=static_cast<std::uint16_t>(std::clamp((point.y-content.y)*scale_/metrics.height,0.0,frame && frame->rows?static_cast<double>(frame->rows-1):0.0));
            selection_pane_=pane; drag_=Drag::selection; drag_origin_=point; SetCapture(hwnd_);
            const auto* model=workspace_.pane(pane); if (model && sessions_.contains(model->session)) { auto owned=sessions_.at(model->session); submit([owned] { return owned->clear_selection(); }); }
            return;
        }
    }
}
void NativeWindow::pointer_move(Point point) {
    const auto old=hover_; hover_=point;
    TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,hwnd_,0}; TrackMouseEvent(&tracking);
    if (drag_!=Drag::none && !drag_started_) {
        const auto dx=GetSystemMetricsForDpi(SM_CXDRAG,dpi_)/scale_,dy=GetSystemMetricsForDpi(SM_CYDRAG,dpi_)/scale_;
        drag_started_=std::abs(point.x-drag_origin_.x)>dx || std::abs(point.y-drag_origin_.y)>dy;
    }
    if (drag_==Drag::divider) {
        const auto& split=drag_divider_.split_bounds;
        const double extent=drag_divider_.axis==Axis::horizontal?split.width:split.height;
        const double position=drag_divider_.axis==Axis::horizontal?point.x-split.x:point.y-split.y;
        const double gap=1/scale_;
        if (extent>gap) workspace_.resize_split(drag_node_,(position-gap/2)/(extent-gap),{0,chrome_.band.height,width_,height_-chrome_.band.height},{1/scale_,7,80,48});
        changed(true); return;
    }
    if (drag_==Drag::pane && drag_started_) {
        drop_preview_.reset(); drop_target_={}; drop_direction_.reset();
        for (const auto& pane:layout_.panes) if (pane.pane!=drag_pane_ && pane.bounds.contains(point)) {
            drop_target_=pane.pane;
            const auto b=pane.bounds;
            const double x=(point.x-b.x)/std::max(1.0,b.width),y=(point.y-b.y)/std::max(1.0,b.height);
            if (x<.25) { drop_direction_=Direction::left; drop_preview_=Rect{b.x,b.y,b.width/2,b.height}; }
            else if (x>.75) { drop_direction_=Direction::right; drop_preview_=Rect{b.x+b.width/2,b.y,b.width/2,b.height}; }
            else if (y<.25) { drop_direction_=Direction::up; drop_preview_=Rect{b.x,b.y,b.width,b.height/2}; }
            else if (y>.75) { drop_direction_=Direction::down; drop_preview_=Rect{b.x,b.y+b.height/2,b.width,b.height/2}; }
            else drop_preview_=b;
            break;
        }
        changed(); return;
    }
    if (drag_==Drag::selection && drag_started_) {
        for (const auto& pane:layout_.panes) if (pane.pane==selection_pane_) {
            const auto* model=workspace_.pane(pane.pane); if (!model || !sessions_.contains(model->session)) break;
            auto session=sessions_.at(model->session); auto frame=session->frame(); if (!frame || !frame->columns || !frame->rows) break;
            const auto content=content_bounds(pane); const auto metrics=renderer_->metrics();
            const auto column=static_cast<std::uint16_t>(std::clamp((point.x-content.x)*scale_/metrics.width,0.0,static_cast<double>(frame->columns-1)));
            const auto row=static_cast<std::uint16_t>(std::clamp((point.y-content.y)*scale_/metrics.height,0.0,static_cast<double>(frame->rows-1)));
            const auto start_column=selection_column_,start_row=selection_row_; const bool rectangle=(GetKeyState(VK_MENU)&0x8000)!=0;
            submit([session,start_column,start_row,column,row,rectangle] { return session->select(start_column,start_row,column,row,rectangle); }); break;
        }
        return;
    }
    if (drag_==Drag::none) {
        const auto held=(GetKeyState(VK_LBUTTON)&0x8000)?MouseButton::left:(GetKeyState(VK_RBUTTON)&0x8000)?MouseButton::right:(GetKeyState(VK_MBUTTON)&0x8000)?MouseButton::middle:MouseButton::none;
        terminal_mouse(point,MouseAction::motion,held,held!=MouseButton::none);
    }
    // Hover causes damage only when a chrome region changes, not for every pixel
    // of mouse motion over a terminal with reporting disabled.
    if (point.y<chrome_.band.height || old.y<chrome_.band.height || drag_==Drag::tab || std::any_of(layout_.dividers.begin(),layout_.dividers.end(),[&](const DividerPlacement& d) { return d.hit_bounds.contains(point)||d.hit_bounds.contains(old); })) changed();
}
void NativeWindow::pointer_up(Point point) {
    if (drag_==Drag::pane && drag_started_ && drop_target_) {
        const auto status=drop_direction_?workspace_.move_pane(drag_pane_,drop_target_,*drop_direction_):workspace_.swap_panes(drag_pane_,drop_target_);
        if (!status) show_error(status.error);
    } else if (drag_==Drag::tab && drag_started_) {
        // Cross-window tear-off is intentionally not faked. A drag outside the
        // band cancels and retains the one existing owner/session.
        if (chrome_.band.contains(point)) for (std::size_t i=0;i<chrome_.tabs.size();++i) if (chrome_.tabs[i].bounds.contains(point)) { workspace_.reorder_tab(drag_tab_,i); break; }
    } else if (drag_==Drag::none) terminal_mouse(point,MouseAction::release,MouseButton::left,false);
    cancel_drag(); changed(true);
}
void NativeWindow::cancel_drag() {
    drag_=Drag::none; drag_started_=false; drag_node_={}; drag_tab_={}; drag_pane_={}; drop_target_={}; drop_preview_.reset(); drop_direction_.reset();
    if (GetCapture()==hwnd_) ReleaseCapture();
    changed();
}

template<class T> struct WicObject {
    T* value{};
    ~WicObject() { if (value) value->Release(); }
    T** put() noexcept { return &value; }
    T* operator->() const noexcept { return value; }
};
void NativeWindow::capture_frame() {
    if (options_.capture_path.empty()) return;
    auto pixels=renderer_->read_pixels();
    RECT bounds{}; GetClientRect(hwnd_,&bounds);
    const auto width=static_cast<UINT>(bounds.right),height=static_cast<UINT>(bounds.bottom);
    if (!width || !height || pixels.size()!=static_cast<std::size_t>(width)*height*4) throw std::runtime_error("Native capture returned invalid framebuffer dimensions.");
    auto check=[](HRESULT result) { if (FAILED(result)) throw std::runtime_error("Could not encode the native CI framebuffer PNG."); };
    WicObject<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IWICImagingFactory),reinterpret_cast<void**>(factory.put())));
    WicObject<IWICStream> stream;
    check(factory->CreateStream(stream.put()));
    check(stream->InitializeFromFilename(options_.capture_path.c_str(),GENERIC_WRITE));
    WicObject<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()));
    check(encoder->Initialize(stream.value,WICBitmapEncoderNoCache));
    WicObject<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(frame.put(),nullptr));
    check(frame->Initialize(nullptr)); check(frame->SetSize(width,height));
    check(frame->SetResolution(dpi_,dpi_));
    // WIC PNG's native 8-bit alpha format is BGRA. SetPixelFormat is an in/out
    // negotiation, not a promise to accept the requested channel order.
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format));
    if (IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA)) {
        // Renderer readback is already straight RGBA: swap R/B only. Alpha
        // must remain unchanged; unpremultiplying twice corrupts translucent UI.
        for (std::size_t i=0;i<pixels.size();i+=4) std::swap(pixels[i],pixels[i+2]);
    } else if (!IsEqualGUID(format,GUID_WICPixelFormat32bppRGBA)) {
        throw std::runtime_error("PNG encoder negotiated an unsupported pixel format.");
    }
    check(frame->WritePixels(height,width*4,static_cast<UINT>(pixels.size()),pixels.data()));
    check(frame->Commit()); check(encoder->Commit());
}
bool NativeWindow::smoke_frame_ready() const {
    auto* session=active_session(); const auto frame=session?session->frame():nullptr;
    if (!frame) return false;
    for (const auto& row:frame->lines) if (row) {
        std::string line; for (const auto& cell:row->cells) line+=cell.grapheme;
        // A complete line containing only the marker proves command output,
        // not merely the command text echoed by the terminal driver.
        while (!line.empty() && line.back()==' ') line.pop_back();
        if (line==smoke_marker) return true;
    }
    return false;
}
void NativeWindow::smoke_progress(bool presented) {
    if (!options_.smoke_test || smoke_finished_) return;
    auto* session=active_session(); if (!session) { smoke_finish("Native smoke has no real session."); return; }
    const auto info=session->info();
    if (info.state==SessionState::failed) { smoke_finish(info.error.empty()?"Native session failed.":info.error); return; }
    if (!smoke_command_sent_ && info.state==SessionState::running && info.bytes_received) {
        smoke_command_sent_=true;
        terminal_text("echo " + std::string(smoke_marker) + " & exit /b 0");
        const auto* pane=workspace_.pane(workspace_.active_pane());
        auto owned=sessions_.at(pane->session);
        KeyInput enter; enter.key=Key::enter;
        submit([owned,enter] { return owned->key(enter); });
    }
    if (presented && smoke_frame_ready()) smoke_marker_presented_=true;
    if (session->closed()) {
        if (!info.exit_code || *info.exit_code!=0) { smoke_finish("Controlled smoke shell did not exit successfully."); return; }
        if (!smoke_frame_ready()) { smoke_finish("Controlled smoke shell exited without its expected output marker."); return; }
        if (smoke_marker_presented_ && renderer_->stats().presents) {
            try { capture_frame(); smoke_finish(); }
            catch (const std::exception& error) { smoke_finish(error.what()); }
        }
    }
}
void NativeWindow::smoke_finish(std::string_view error) {
    if (!options_.smoke_test || smoke_finished_) return;
    smoke_finished_=true; exit_code_=error.empty()?0:2;
    if (error.empty()) {
        const auto diagnostics=active_session()?active_session()->diagnostics():ProcessDiagnostics{};
        std::fprintf(stderr,"Native CI smoke passed: real HWND, WARP renderer, ConPTY input/output marker, presented frame, child exit 0. Provider: %s. Not physical visual/IME/Snap QA.\n",diagnostics.provider.c_str());
    } else std::fprintf(stderr,"Native CI smoke failed: %.*s\n",static_cast<int>(error.size()),error.data());
    if (hwnd_) DestroyWindow(hwnd_);
}

void NativeWindow::position_ime() {
    if (!hwnd_ || !renderer_) return;
    POINT position{}; RECT exclusion{};
    if (palette_open_ || rename_open_) {
        position={static_cast<LONG>(std::max(16.0,(width_-560)/2+16)*scale_),static_cast<LONG>(96*scale_)};
        exclusion={position.x,position.y,position.x+1,position.y+static_cast<LONG>(20*scale_)};
    } else for (const auto& pane:layout_.panes) if (pane.pane==workspace_.active_pane()) {
        const auto content=content_bounds(pane); const auto metrics=renderer_->metrics();
        auto* session=active_session(); auto frame=session?session->frame():nullptr;
        position.x=static_cast<LONG>(content.x*scale_+(frame?frame->cursor.column:0)*metrics.width);
        position.y=static_cast<LONG>(content.y*scale_+(frame?frame->cursor.row:0)*metrics.height);
        exclusion={position.x,position.y,position.x+metrics.width,position.y+metrics.height}; break;
    }
    ImmContext context(hwnd_);
    if (!context.context) return;
    COMPOSITIONFORM composition{}; composition.dwStyle=CFS_POINT; composition.ptCurrentPos=position;
    ImmSetCompositionWindow(context.context,&composition);
    CANDIDATEFORM candidate{}; candidate.dwIndex=0; candidate.dwStyle=CFS_EXCLUDE;
    candidate.ptCurrentPos={position.x,exclusion.bottom}; candidate.rcArea=exclusion;
    ImmSetCandidateWindow(context.context,&candidate);
    // IMM32 baseline only. A TSF text store and UIA TextPattern provider are
    // explicit acceptance gaps, not implied by these candidate/caret APIs.
}
void NativeWindow::composition(LPARAM flags) {
    ImmContext context(hwnd_); if (!context.context) return;
    auto read=[&](DWORD kind) {
        const LONG bytes=ImmGetCompositionStringW(context.context,kind,nullptr,0);
        if (bytes<0 || bytes>65536 || bytes%sizeof(wchar_t)) return std::string{};
        std::wstring value(static_cast<std::size_t>(bytes)/sizeof(wchar_t),L'\0');
        if (bytes && ImmGetCompositionStringW(context.context,kind,value.data(),static_cast<DWORD>(bytes))!=bytes) return std::string{};
        return narrow(value);
    };
    if (flags&GCS_RESULTSTR) { terminal_text(read(GCS_RESULTSTR)); preedit_.clear(); }
    if (flags&GCS_COMPSTR) preedit_=read(GCS_COMPSTR);
    position_ime(); changed();
}
void NativeWindow::caret_schedule() {
    auto* session=active_session(); const auto frame=session?session->frame():nullptr;
    const UINT interval=GetCaretBlinkTime();
    bool text_blinks=false;
    if (!reduced_motion_) for (const auto& pane:layout_.panes) if (auto* visible=session_for(pane.pane)) if (auto snapshot=visible->frame()) {
        for (const auto& row:snapshot->lines) if (row) for (const auto& cell:row->cells) if (cell.style.blink) { text_blinks=true; break; }
    }
    const bool cursor_blinks=frame && frame->cursor.visible && frame->cursor.blinking;
    const bool should_blink=active_ && !minimized_ && !palette_open_ && !rename_open_ && (cursor_blinks || text_blinks) && interval && interval!=INFINITE;
    if (should_blink && !cursor_timer_active_) cursor_timer_active_=SetTimer(hwnd_,cursor_timer,interval,nullptr)!=0;
    else if (!should_blink && cursor_timer_active_) { KillTimer(hwnd_,cursor_timer); cursor_timer_active_=false; cursor_on_=true; }
}
void NativeWindow::toggle_fullscreen() {
    if (!fullscreen_) {
        restore_style_=GetWindowLongPtrW(hwnd_,GWL_STYLE); restore_placement_.length=sizeof(restore_placement_); GetWindowPlacement(hwnd_,&restore_placement_);
        MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor); GetMonitorInfoW(MonitorFromWindow(hwnd_,MONITOR_DEFAULTTONEAREST),&monitor);
        fullscreen_=true;
        SetWindowLongPtrW(hwnd_,GWL_STYLE,restore_style_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        SetWindowPos(hwnd_,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_FRAMECHANGED);
    } else {
        fullscreen_=false; SetWindowLongPtrW(hwnd_,GWL_STYLE,restore_style_);
        SetWindowPlacement(hwnd_,&restore_placement_); SetWindowPos(hwnd_,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
    }
    changed(true);
}
LRESULT NativeWindow::message(UINT message, WPARAM wparam, LPARAM lparam) {
    LRESULT dwm_result{};
    const bool dwm_handled=DwmDefWindowProc(hwnd_,message,wparam,lparam,&dwm_result)!=FALSE;
    // Caption geometry is our own; DWM still observes non-client input for Snap.
    if (dwm_handled && message!=WM_NCHITTEST && message!=WM_NCCALCSIZE && message!=WM_NCPAINT && message!=WM_NCACTIVATE && message!=WM_NCMOUSEMOVE && message!=WM_NCMOUSELEAVE) return dwm_result;
    switch (message) {
    case WM_NCCALCSIZE:
        if (wparam) {
            if (IsZoomed(hwnd_) && !fullscreen_) {
                auto* parameters=reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor);
                if (GetMonitorInfoW(MonitorFromRect(&parameters->rgrc[0],MONITOR_DEFAULTTONEAREST),&monitor)) parameters->rgrc[0]=monitor.rcWork;
            }
            return 0; // One client-drawn band; never an extra standard topbar.
        }
        break;
    case WM_NCPAINT: return 0;
    case WM_NCACTIVATE: changed(); return DefWindowProcW(hwnd_,message,wparam,-1);
    case WM_NCHITTEST: {
        const auto p=point(lparam,true);
        if (zoom_return_.contains(p)) return HTCLIENT;
        const double border=(GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi_)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi_))/scale_;
        switch (chrome_hit_test(chrome_,p,{width_,height_},border,maximized_||fullscreen_)) {
        case NativeHit::caption: return HTCAPTION; case NativeHit::minimize: return HTMINBUTTON;
        case NativeHit::maximize: return HTMAXBUTTON; case NativeHit::close: return HTCLOSE;
        case NativeHit::left: return HTLEFT; case NativeHit::right: return HTRIGHT; case NativeHit::top: return HTTOP; case NativeHit::bottom: return HTBOTTOM;
        case NativeHit::top_left: return HTTOPLEFT; case NativeHit::top_right: return HTTOPRIGHT; case NativeHit::bottom_left: return HTBOTTOMLEFT; case NativeHit::bottom_right: return HTBOTTOMRIGHT;
        default: return HTCLIENT;
        }
    }
    case WM_GETMINMAXINFO: {
        auto* info=reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize={static_cast<LONG>(330*scale_),static_cast<LONG>(180*scale_)};
        MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor);
        if (!fullscreen_ && GetMonitorInfoW(MonitorFromWindow(hwnd_,MONITOR_DEFAULTTONEAREST),&monitor)) {
            info->ptMaxPosition={monitor.rcWork.left-monitor.rcMonitor.left,monitor.rcWork.top-monitor.rcMonitor.top};
            info->ptMaxSize={monitor.rcWork.right-monitor.rcWork.left,monitor.rcWork.bottom-monitor.rcWork.top};
        }
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_=HIWORD(wparam); scale_=dpi_/96.0;
        const auto* rect=reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd_,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);
        settings_changed(); changed(true); return 0;
    }
    case WM_SIZE:
        minimized_=wparam==SIZE_MINIMIZED; maximized_=wparam==SIZE_MAXIMIZED;
        if (renderer_ && !minimized_) changed(true);
        caret_schedule(); return 0;
    case WM_ENTERSIZEMOVE: in_size_move_=true; return 0;
    case WM_EXITSIZEMOVE: in_size_move_=false; changed(true); return 0;
    case WM_SETTINGCHANGE: case WM_THEMECHANGED: case WM_DWMCOMPOSITIONCHANGED:
        settings_changed(); changed(true); return 0;
    case WM_ACTIVATE:
        active_=LOWORD(wparam)!=WA_INACTIVE;
        if (active_) SetFocus(hwnd_);
        settings_changed(); caret_schedule(); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: {
        const bool focused=message==WM_SETFOCUS;
        active_=focused; text_input_.reset(); consumed_keys_.fill(false);
        if (!focused) { cancel_drag(); preedit_.clear(); composing_=false; }
        caret_schedule(); changed(); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; BeginPaint(hwnd_,&paint); EndPaint(hwnd_,&paint); dirty_=true;
        if (in_size_move_ && renderer_ && !minimized_) render();
        return 0;
    }
    case wake_message:
        wake_->posted.store(false,std::memory_order_release); update_sessions(); InvalidateRect(hwnd_,nullptr,FALSE); return 0;
    case WM_TIMER:
        if (wparam==smoke_timeout_timer) { smoke_finish("Native smoke timed out before real shell output, presentation and clean exit."); return 0; }
        if (wparam==cursor_timer) { cursor_on_=!cursor_on_; dirty_=true; InvalidateRect(hwnd_,nullptr,FALSE); } return 0;
    case WM_CHAR: case WM_SYSCHAR: {
        if (message==WM_SYSCHAR && wparam==L' ' && (lparam & (1LL<<29))) return DefWindowProcW(hwnd_,message,wparam,lparam);
        if (composing_) return 0;
        const auto unit=static_cast<char16_t>(wparam);
        if ((palette_open_ || rename_open_) && unit<0x20) return 0;
        terminal_text(text_input_.feed(unit),physical_key(text_vk_,text_flags_),text_modifiers_); return 0;
    }
    case WM_UNICHAR:
        if (wparam==UNICODE_NOCHAR) return TRUE;
        { std::string text; Utf16Input::append(text,static_cast<std::uint32_t>(wparam)); terminal_text(std::move(text)); } return 0;
    case WM_IME_SETCONTEXT:
        return DefWindowProcW(hwnd_,message,wparam,lparam & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
    case WM_IME_STARTCOMPOSITION:
        composing_=true; preedit_.clear(); text_input_.reset(); position_ime(); changed(); return 0;
    case WM_IME_COMPOSITION: composition(lparam); return 0;
    case WM_IME_ENDCOMPOSITION: composing_=false; preedit_.clear(); changed(); return 0;
    case WM_IME_CHAR: return 0; // Result committed once from GCS_RESULTSTR above.
    case WM_INPUTLANGCHANGE: text_input_.reset(); position_ime(); break;
    case WM_LBUTTONDOWN: pointer_down(point(lparam),false); return 0;
    case WM_LBUTTONDBLCLK: pointer_down(point(lparam),true); return 0;
    case WM_LBUTTONUP: pointer_up(point(lparam)); return 0;
    case WM_MOUSEMOVE: pointer_move(point(lparam)); return 0;
    case WM_MOUSELEAVE: case WM_NCMOUSELEAVE: hover_={-1,-1}; changed(); return 0;
    case WM_NCMOUSEMOVE: {
        hover_=point(lparam,true); changed();
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE|TME_NONCLIENT,hwnd_,0}; TrackMouseEvent(&tracking);
        break; // DefWindowProc keeps Windows 11 Snap hover semantics.
    }
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONUP: changed(); break;
    case WM_RBUTTONDOWN: {
        const auto p=point(lparam);
        for (const auto& placement:layout_.panes) if (placement.bounds.contains(p)) { const auto pane=placement.pane; activate(pane); break; }
        if (!(GetKeyState(VK_SHIFT)&0x8000)) terminal_mouse(p,MouseAction::press,MouseButton::right,true);
        return 0;
    }
    case WM_RBUTTONUP: {
        const auto p=point(lparam);
        if (p.y<chrome_.band.height) popup_menu(p);
        else if (mouse_capture_pane_) terminal_mouse(p,MouseAction::release,MouseButton::right,false);
        else popup_menu(p,true);
        return 0;
    }
    case WM_MBUTTONDOWN:
        for (const auto& tab:chrome_.tabs) if (tab.bounds.contains(point(lparam))) { close_tab(tab.tab); return 0; }
        terminal_mouse(point(lparam),MouseAction::press,MouseButton::middle,true); return 0;
    case WM_MBUTTONUP: terminal_mouse(point(lparam),MouseAction::release,MouseButton::middle,false); return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        auto& remainder=message==WM_MOUSEHWHEEL?horizontal_wheel_remainder_:wheel_remainder_;
        remainder+=GET_WHEEL_DELTA_WPARAM(wparam);
        const int steps=remainder/WHEEL_DELTA; remainder%=WHEEL_DELTA;
        if (!steps) return 0;
        PaneId target=workspace_.active_pane();
        const auto position=point(lparam,true);
        for (const auto& placement:layout_.panes) if (placement.bounds.contains(position)) { target=placement.pane; break; }
        auto* session=session_for(target); const auto frame=session?session->frame():nullptr;
        if (frame && frame->mouse_reporting && !(GetKeyState(VK_SHIFT)&0x8000)) {
            const auto button=message==WM_MOUSEHWHEEL?(steps>0?MouseButton::wheel_right:MouseButton::wheel_left):(steps>0?MouseButton::wheel_up:MouseButton::wheel_down);
            for (int i=0;i<std::abs(steps);++i) terminal_mouse(point(lparam,true),MouseAction::press,button,false);
        } else if (message==WM_MOUSEWHEEL) {
            UINT rows=3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&rows,0);
            if (rows==WHEEL_PAGESCROLL) rows=frame?frame->rows:24;
            if (const auto* pane=workspace_.pane(target); pane && sessions_.contains(pane->session)) {
                auto owned=sessions_.at(pane->session); const auto amount=-static_cast<std::int64_t>(steps)*rows;
                submit([owned,amount] { return owned->scroll(amount); });
            }
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lparam)==HTCLIENT) {
            POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(hwnd_,&cursor); const Point p{cursor.x/scale_,cursor.y/scale_};
            for (const auto& divider:layout_.dividers) if (divider.hit_bounds.contains(p)) { SetCursor(LoadCursorW(nullptr,divider.axis==Axis::horizontal?IDC_SIZEWE:IDC_SIZENS)); return TRUE; }
            SetCursor(LoadCursorW(nullptr,p.y<chrome_.band.height?IDC_ARROW:IDC_IBEAM)); return TRUE;
        }
        break;
    case WM_CAPTURECHANGED:
        if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
        mouse_capture_pane_={}; mouse_capture_button_=MouseButton::none;
        if (drag_!=Drag::none) cancel_drag(); return 0;
    case WM_CANCELMODE: cancel_drag(); return 0;
    case WM_CONTEXTMENU:
        if (GET_X_LPARAM(lparam)==-1 && GET_Y_LPARAM(lparam)==-1) popup_menu({width_/2,chrome_.band.height+12},true);
        else popup_menu(point(lparam,true),true);
        return 0;
    case WM_CLOSE: {
        const bool live=std::any_of(sessions_.begin(),sessions_.end(),[](const auto& pair) { return !pair.second->closed(); });
        if (!live || confirm_close("this window and all its sessions")) DestroyWindow(hwnd_);
        return 0;
    }
    case WM_DESTROY:
        wake_->window.store(nullptr,std::memory_order_release);
        KillTimer(hwnd_,cursor_timer); KillTimer(hwnd_,smoke_timeout_timer);
        for (auto& [id,session]:sessions_) { (void)id; session->request_close(); }
        PostQuitMessage(exit_code_); return 0;
    case WM_NCDESTROY: {
        const auto result=DefWindowProcW(hwnd_,message,wparam,lparam);
        SetWindowLongPtrW(hwnd_,GWLP_USERDATA,0); hwnd_=nullptr; return result;
    }
    default: break;
    }
    return DefWindowProcW(hwnd_,message,wparam,lparam);
}
} // namespace
int run_native_application(Config config, NativeRunOptions options) { NativeWindow window(std::move(config),std::move(options)); return window.run(); }
} // namespace stormtty
