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
#include <commctrl.h>
#include <shobjidl.h>
#include "stormtty/native_window.hpp"
#include "stormtty/windows_managed_shell.hpp"
#include "stormtty/windows_console_runtime.hpp"
#include "stormtty/renderer.hpp"
#include "stormtty/session.hpp"
#include "stormtty/settings_panel.hpp"
#include "stormtty/settings_services.hpp"
#include "stormtty/context_menu.hpp"
#include "app_icons.hpp"
#include "smoke_teardown_trace.hpp"
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
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace stormtty {
namespace {
constexpr UINT wake_message = WM_APP + 41;
constexpr UINT deferred_layout_message = WM_APP + 42;
constexpr UINT_PTR cursor_timer = 1;
constexpr UINT_PTR smoke_timeout_timer = 2;
constexpr UINT_PTR chrome_motion_timer = 3;
constexpr UINT_PTR material_probe_timer = 4;
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
struct SettingsCompletion {
    enum class Kind { load, save } kind{};
    SettingsSnapshotResult loaded;
    ShellDiscoveryResult shells;
    ConfigSaveResult saved;
    Config candidate;
};
struct WakeState {
    std::atomic<HWND> window{};
    std::atomic<bool> posted{};
    std::mutex mutex;
    std::optional<std::string> clipboard;
    std::optional<std::string> error;
    std::optional<SettingsCompletion> settings;
    void wake() noexcept {
        if (auto hwnd = window.load(std::memory_order_acquire)) {
            if (!posted.exchange(true, std::memory_order_acq_rel) && !PostMessageW(hwnd, wake_message, 0, 0)) posted.store(false, std::memory_order_release);
        }
    }
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
    bool smoke{};
    ~WindowClass() {
        windows::smoke_teardown_trace(smoke,"window class unregister begin");
        if (atom) UnregisterClassW(L"StormTTY.NativeWindow", instance);
        windows::smoke_teardown_trace(smoke,"window class unregister end");
    }
};

template<class T> struct ComObject {
    T* value{};
    ~ComObject() { if (value) value->Release(); }
    T** put() noexcept { return &value; }
    T* operator->() const noexcept { return value; }
};
std::string project_name(std::filesystem::path path) {
    auto name=narrow(path.filename().wstring()); if (name.empty()) name=narrow(path.wstring());
    if (name.empty()) return "Local shell";
    if (name.size()>ProjectWorkspaceCatalog::maximum_name_bytes) {
        std::size_t end=ProjectWorkspaceCatalog::maximum_name_bytes-3;
        while (end && (static_cast<unsigned char>(name[end])&0xc0)==0x80) --end;
        name.resize(end); name+="…";
    }
    return name;
}
// CreateWindowEx normalizes an overlapped (zero-valued WS_OVERLAPPED) style
// to include a caption on Windows. Remove it after creation, then force the
// documented nonclient recalculation; do not change the window to a popup.
void enforce_custom_frame_style(HWND window, bool diagnostics) {
    constexpr LONG_PTR required=WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX;
    const auto created=GetWindowLongPtrW(window,GWL_STYLE);
    const auto desired=(created|required)&~static_cast<LONG_PTR>(WS_CAPTION);
    SetLastError(ERROR_SUCCESS);
    const auto previous=SetWindowLongPtrW(window,GWL_STYLE,desired);
    if (!previous && GetLastError()!=ERROR_SUCCESS) throw std::runtime_error("Could not establish custom nonclient window styles.");
    if (!SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED))
        throw std::runtime_error("Could not recalculate the custom nonclient frame.");
    const auto actual=GetWindowLongPtrW(window,GWL_STYLE);
    const auto missing=required&~actual;
    if (diagnostics || (actual&WS_CAPTION) || missing) std::fprintf(stderr,
        "Native frame styles: requested=0x%08lx created=0x%08lx enforced=0x%08lx actual=0x%08lx caption_bits=0x%08lx missing_required=0x%08lx\n",
        static_cast<unsigned long>(WS_OVERLAPPEDWINDOW&~WS_CAPTION),static_cast<unsigned long>(created),
        static_cast<unsigned long>(desired),static_cast<unsigned long>(actual),static_cast<unsigned long>(actual&WS_CAPTION),static_cast<unsigned long>(missing));
    if ((actual&WS_CAPTION) || missing) throw std::runtime_error("Windows did not retain the required caption-free resize/system-control styles; see frame-style diagnostic.");
}

class NativeWindow final {
public:
    explicit NativeWindow(Config config, NativeRunOptions options) : config_(std::move(config)), options_(std::move(options)), wake_(std::make_shared<WakeState>()) {
        if (!options_.shell_runtime_directory.empty() && options_.shell_runtime_error.empty()) {
            const auto& root=options_.shell_runtime_directory;
            Profile managed;
            managed.id=managed_zsh_profile_id; managed.label="Zsh · managed";
            managed.executable=narrow((root/L"usr"/L"bin"/L"zsh.exe").wstring());
            managed.arguments={"-d","-i"}; // Skip optional global rc files; actual interactive Zsh.
            std::string child_path=narrow((root/L"usr"/L"bin").wstring());
            const auto count=GetEnvironmentVariableW(L"PATH",nullptr,0);
            if (count) {
                if (count>32767) throw std::runtime_error("Inherited PATH exceeds the supported child environment bound.");
                std::wstring inherited(count,L'\0');
                const auto written=GetEnvironmentVariableW(L"PATH",inherited.data(),count);
                if (written>=count) throw std::runtime_error("Inherited PATH changed while preparing the managed shell.");
                inherited.resize(written);
                if (!inherited.empty()) child_path+=';'+narrow(inherited);
            }
            managed.environment={{"ZDOTDIR",narrow((root/L"stormtty"/L"zsh").generic_wstring())},
                {"STORMTTY_SESSION_ID","managed"},{"TERM","xterm-256color"},{"COLORTERM","truecolor"},
                {"LANG","C.UTF-8"},{"MSYSTEM","MSYS"},{"PATH",std::move(child_path)}};
            managed_shell_profile_=std::move(managed);
        }
        if (!options_.shell_runtime_error.empty()) notice_="Managed Zsh unavailable. Close StormTTY and rebuild to restore runtimes/zsh. Your saved shell is unchanged.";
        std::string directory;
        for (const auto& profile:config_.profiles) if (profile.id==config_.default_profile) { directory=profile.working_directory; break; }
        if (directory.empty()) directory=narrow(std::filesystem::current_path().wstring());
        const auto path=std::filesystem::path(widen(directory));
        auto name=project_name(path);
        auto project=projects_.create({name,directory,"Local"});
        if (!project) throw std::runtime_error(project.error);
    }
    ~NativeWindow() {
        windows::smoke_teardown_trace(options_.smoke_test,"window destructor body begin");
        wake_->window.store(nullptr, std::memory_order_release);
        for (auto& [id, session] : sessions_) { (void)id; session->request_close(); }
        windows::smoke_teardown_trace(options_.smoke_test,"window destructor close requests complete");
        if (hwnd_) DestroyWindow(hwnd_);
        windows::smoke_teardown_trace(options_.smoke_test,"window destructor HWND destruction complete");
        // The window is gone: never wait for local file I/O in a WndProc.
        if (settings_worker_.joinable()) {
            if (options_.smoke_test) std::fprintf(stderr,"Native CI settings worker reap begin after window destruction\n");
            settings_worker_.join();
            if (options_.smoke_test) std::fprintf(stderr,"Native CI settings worker reap end\n");
        }
        windows::smoke_teardown_trace(options_.smoke_test,"window settings file cleanup begin");
        if (smoke_settings_file_owned_) {
            std::error_code ignored;
            std::filesystem::remove(options_.config_path,ignored);
            std::filesystem::remove(options_.config_path.parent_path(),ignored);
        }
        windows::smoke_teardown_trace(options_.smoke_test,"window destructor body end; member destruction begin");
    }
    int run();
private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM) noexcept;
    LRESULT message(UINT, WPARAM, LPARAM);
    bool key_message(MSG&);
    void create();
    void settings_changed();
    void observe_material_renderer();
    void queue_material_probe(bool invalidate = false);
    void begin_material_probe(const WindowScene&);
    void poll_material_probe();
    void publish_material_status();
    void open_settings(std::string binding = {});
    void settings_actions();
    void settings_completed();
    void preview_settings(const Config&);
    void settings_input_changed();
    bool settings_input_blocked() const { return settings_panel_.is_open(); }
    bool settings_loading() const { return settings_job_ && settings_open_requested_ && !settings_input_blocked(); }
    bool overlay_open() const { return settings_input_blocked() || settings_loading() || context_menu_.is_open(); }
    void open_menu(std::vector<ContextMenuItem>, Point, std::map<std::string,std::function<void()>>, bool keyboard = false);
    void menu_input_changed();
    void system_menu(Point, bool keyboard = false);
    void refresh_layout();
    void defer_layout();
    void update_sessions();
    void sync_focus();
    void changed(bool geometry = false);
    void render();
    bool smoke_frame_ready() const;
    void smoke_progress(bool presented = false);
    void smoke_finish(std::string_view error = {});
    void smoke_settings_stage(unsigned stage);
    void capture_frame();
    void submit(std::function<SessionSendResult()> action, std::size_t bytes = 64);
    void drain_input();
    WindowScene scene();
    void dispatch(std::string_view command);
    CommandContext command_context() const;
    Session* active_session() const;
    Session* session_for(PaneId) const;
    SessionId new_session();
    bool using_managed_shell() const { return managed_shell_profile_ && (options_.shell_runtime_override || has_managed_zsh_selection(config_)); }
    const std::string& session_profile_id() const { return using_managed_shell()?managed_shell_profile_->id:config_.default_profile; }
    void new_tab();
    void split(Direction);
    void close_pane(PaneId, bool approved = false);
    void park_pane(PaneId);
    void restore_parked(ProjectSessionReference);
    void close_parked(ProjectSessionReference, bool approved = false);
    void background_menu(Point, std::optional<SessionId> = {});
    std::string background_title(const ParkedSession&, bool status = true) const;
    BackgroundFooterLayout background_layout() const { return layout_background_footer(navigation_.footer,projects_.active()->parked_sessions.size()); }
    Rect empty_terminal_button() const { const auto r=workspace_viewport(); return {r.x+std::max(0.0,(r.width-156)/2),r.y+std::max(0.0,(r.height-36)/2),std::min(156.0,r.width),std::min(36.0,r.height)}; }
    void close_tab(TabId, bool approved = false);
    void show_modal(std::string title, std::vector<std::string> body, std::string confirm, std::function<void()> action, bool destructive = true);
    void answer_modal(ModalDecision);
    ModalLayout modal_layout() const { return layout_modal(width_,height_,chrome_.band.height); }
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
    void caret_schedule(bool restart = false);
    void motion_schedule();
    void hover_changed(Point);
    void refresh_tooltips();
    double hover_amount(std::uint64_t id) const;
    void toggle_fullscreen();
    void show_error(std::string_view);
    std::string pane_title(PaneId, bool full = false) const;
    std::string tab_title(TabId, bool full = false) const;
    PixelRect pixels(Rect r) const { return {static_cast<float>(r.x * scale_), static_cast<float>(r.y * scale_), static_cast<float>(r.width * scale_), static_cast<float>(r.height * scale_)}; }
    Point point(LPARAM lparam, bool screen = false) const {
        POINT value{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (screen) ScreenToClient(hwnd_, &value);
        return {value.x / scale_, value.y / scale_};
    }
    Rect content_bounds(const PanePlacement& p) const {
        const double header = layout_.panes.size() > 1 ? 28.0 : 0.0;
        return {p.bounds.x + 20, p.bounds.y + header + 16, std::max(0.0, p.bounds.width - 40), std::max(0.0, p.bounds.height - header - 32)};
    }
    Config config_;
    NativeRunOptions options_;
    std::optional<Profile> managed_shell_profile_; // Resolved launch profile; only a stable marker is ever saved.
    SettingsPanel settings_panel_{Platform::windows};
    std::thread settings_worker_;
    bool settings_job_{}, settings_open_requested_{};
    std::string settings_binding_;
    std::optional<ConfigFileSnapshot> settings_snapshot_;
    bool overlay_pointer_sequence_{};
    std::uint64_t settings_seen_generation_{}, menu_seen_generation_{};
    ContextMenu context_menu_;
    std::map<std::string,std::function<void()>> menu_actions_;
    std::set<SessionId> smoke_commands_sent_;
    std::set<SessionId> smoke_bundled_sessions_; // Actual provider at creation, never mutable Settings.
    std::set<SessionId> smoke_managed_sessions_; // Actual launch profile, not the mutable Settings default.
    std::map<SessionId,std::shared_ptr<Session>> smoke_sessions_; // Includes retired/disposable actors for provider assertions.
    bool smoke_marker_presented_{}, smoke_finished_{}, smoke_console_capture_pending_{};
    int exit_code_{};
    unsigned smoke_modal_stage_{}, smoke_settings_stage_{};
    ULONGLONG smoke_started_at_{};
    bool smoke_settings_file_owned_{};
    Config smoke_settings_original_;
    std::shared_ptr<Session> smoke_settings_actor_;
    bool smoke_projects_live_verified_{}, smoke_background_restored_{}, smoke_chrome_verified_{}, smoke_tab_drag_verified_{};
    SessionId smoke_parked_session_{};
    std::uint64_t smoke_parked_bytes_{};
    ProjectWorkspaceId smoke_primary_project_{}, smoke_secondary_project_{};
    std::map<SessionId,std::shared_ptr<Session>> smoke_project_actors_;
    std::shared_ptr<Session> smoke_disposable_;
    ProjectWorkspaceCatalog projects_;
    Workspace& workspace() { return *projects_.active_workspace(); }
    const Workspace& workspace() const { return *projects_.active_workspace(); }
    void choose_project_folder();
    void activate_project(ProjectWorkspaceId);
    void close_project(ProjectWorkspaceId, bool approved = false);
    void reset_project_view();
    std::string directory_context() const;
    Rect workspace_viewport() const { return navigation_.viewport; }
    bool workspace_chrome_contains(Point p) const {
        return chrome_.band.contains(p) || chrome_.tabs_band.contains(p) || navigation_.rail.contains(p) ||
            navigation_.context.contains(p) || navigation_.footer.contains(p);
    }
    bool navigation_expanded_{true}, focus_mode_{}, force_redraw_{};
    std::size_t navigation_first_{};
    ProjectNavigationLayout navigation_;
    std::string context_label_;
    std::map<SessionId, std::shared_ptr<Session>> sessions_;
    std::map<SessionId, TerminalSize> sizes_;
    std::map<SessionId, SessionState> session_states_;
    std::map<SessionId, std::string> background_labels_;
    SessionId focused_session_{};
    std::uint64_t next_session_{1};
    std::shared_ptr<WakeState> wake_;
    HWND hwnd_{}, tooltip_{};
    bool tooltip_active_{true};
    std::map<UINT_PTR, std::wstring> tooltip_texts_;
    std::map<std::uint64_t, ChromeTransition> hover_transitions_;
    std::map<TabId, ChromeTransition> tab_transitions_;
    std::map<PaneId, ChromeTransition> pane_reveals_;
    ChromeTransition palette_transition_;
    bool motion_timer_active_{};
    std::vector<ChromePrimitive> closing_palette_primitives_;
    std::vector<ChromeText> closing_palette_texts_;
    std::unique_ptr<Renderer> renderer_;
    bool renderer_call_{}, layout_pending_{}, settings_pending_{};
    double scale_{1}, width_{1100}, height_{720};
    UINT dpi_{96};
    ChromeLayout chrome_;
    Rect zoom_return_;
    Layout layout_;
    std::uint64_t chrome_generation_{1};
    bool dirty_{true}, minimized_{}, maximized_{}, active_{true}, high_contrast_{}, reduced_motion_{}, in_size_move_{}, render_busy_{};
    bool palette_open_{}, rename_open_{}, composing_{}, cursor_timer_active_{}, fullscreen_{};
    CursorBlinkClock cursor_blink_;
    WINDOWPLACEMENT restore_placement_{};
    LONG_PTR restore_style_{};
    std::string query_, preedit_, notice_;
    struct Modal { std::string title; std::vector<std::string> body; std::string confirm_label; std::function<void()> confirm; bool destructive{}; };
    std::optional<Modal> modal_;
    ModalFocus modal_focus_;
    std::optional<ModalDecision> modal_pressed_;
    WPARAM caption_pressed_{};
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
    MaterialRefreshTracker material_tracker_;
    std::string material_status_, material_probe_status_;
    bool material_probe_requested_{}, material_probe_running_{}, material_probe_succeeded_{};
    ULONGLONG material_probe_deadline_{};
    std::uint64_t material_probe_present_{};
    bool material_probe_foreground_{}, material_probe_cells_limited_{};
    std::uint32_t material_probe_default_cells_{}, material_probe_explicit_cells_{}, material_probe_other_opaque_cells_{};
    DWORD material_probe_dwm_{};
    HRESULT material_probe_dwm_result_{E_PENDING};
};

void NativeWindow::create() {
    if (options_.smoke_test) {
        smoke_started_at_=GetTickCount64();
        std::fprintf(stderr,"Native CI smoke startup: settings=%d managed_zsh=%d packaged_console=%d\n",
            options_.smoke_settings,using_managed_shell(),!options_.console_runtime_directory.empty());
    }
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, L"StormTTY.NativeWindow", L"StormTTY", WS_OVERLAPPEDWINDOW & ~WS_CAPTION,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) throw std::runtime_error("Could not create the native StormTTY window.");
    enforce_custom_frame_style(hwnd_,options_.smoke_test);
    windows::set_app_icons(hwnd_);
    wake_->window.store(hwnd_, std::memory_order_release);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    if (InitCommonControlsEx(&controls)) {
        tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (tooltip_) { SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 600); SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 600); }
    }
    dpi_ = GetDpiForWindow(hwnd_); scale_ = dpi_ / 96.0;
    RendererOptions options;
    options.force_warp = options_.smoke_test;
    options.smoke_teardown_diagnostics = options_.smoke_test;
    options.font.family = config_.font.family;
    options.font.size_dip = static_cast<float>(config_.font.size_dip);
    options.font.ligatures = config_.font.ligatures;
    renderer_ = std::make_unique<Renderer>(hwnd_, options);
    settings_changed();
    if (options_.smoke_settings) {
        // Explicit smoke only; never load or save the user's actual settings.
        std::array<wchar_t,32768> temporary{};
        const auto length=GetTempPathW(static_cast<DWORD>(temporary.size()),temporary.data());
        GUID guid{}; std::array<wchar_t,40> token{};
        if (!length || length>=temporary.size() || FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid,token.data(),static_cast<int>(token.size())))
            throw std::runtime_error("Could not create a unique settings smoke path.");
        options_.config_path=std::filesystem::path(temporary.data())/(std::wstring(L"StormTTY.SettingsSmoke.")+token.data())/L"config.stormtty";
    }
    new_tab();
    if (options_.smoke_projects) {
        smoke_primary_project_=projects_.active_id();
        std::array<wchar_t,MAX_PATH+1> directory{};
        UINT length=GetSystemDirectoryW(directory.data(),static_cast<UINT>(directory.size()));
        if (!length || length>=directory.size()) throw std::runtime_error("Could not discover the smoke workspace folder.");
        std::string path=narrow(std::wstring(directory.data(),length));
        if (path==projects_.active()->metadata.working_directory) {
            length=GetWindowsDirectoryW(directory.data(),static_cast<UINT>(directory.size()));
            if (!length || length>=directory.size()) throw std::runtime_error("Could not discover the second smoke folder.");
            path=narrow(std::wstring(directory.data(),length));
        }
        const auto second=projects_.create({project_name(std::filesystem::path(widen(path))),path,"Local"});
        if (!second) throw std::runtime_error(second.error);
        smoke_secondary_project_=*second.value; activate_project(*second.value); new_tab();
        smoke_project_actors_=sessions_; navigation_expanded_=true; activate_project(smoke_primary_project_);
    }
    if (options_.smoke_layout) {
        const auto first=workspace().active_pane();
        workspace().rename_tab(workspace().active_tab(),"Native layout smoke");
        new_tab(); workspace().rename_tab(workspace().active_tab(),"Background shell 1");
        new_tab(); workspace().rename_tab(workspace().active_tab(),"Background shell 2");
        activate(first); split(Direction::right); split(Direction::down); activate(first);
        for (const auto& divider:layout_.dividers) if (divider.axis==Axis::horizontal) {
            workspace().set_split_ratio(divider.node,0.60); break;
        }
        changed(true);
    }
    SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(1100 * scale_), static_cast<int>(720 * scale_), SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    refresh_layout();
    settings_changed();
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    if (options_.smoke_test && !SetTimer(hwnd_,smoke_timeout_timer,20000,nullptr)) throw std::runtime_error("Could not start the native smoke deadline.");
}
int NativeWindow::run() {
    WindowClass klass; klass.smoke=options_.smoke_test;
    WNDCLASSEXW definition{}; definition.cbSize=sizeof(definition);
    definition.style = CS_DBLCLKS;
    definition.lpfnWndProc = procedure;
    definition.hInstance = klass.instance;
    definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    definition.hIcon = windows::app_icon(GetDpiForSystem(),false);
    definition.hIconSm = windows::app_icon(GetDpiForSystem(),true);
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
                if (options_.smoke_test) for (const auto& [id,session]:smoke_sessions_) {
                    std::fprintf(stderr,"Native CI cleanup begin: session=%llu elapsed_ms=%llu\n",static_cast<unsigned long long>(id.value),GetTickCount64()-smoke_started_at_);
                    if (!session->wait_closed(std::chrono::seconds(5))) code=3;
                    std::fprintf(stderr,"Native CI cleanup end: session=%llu code=%d\n",static_cast<unsigned long long>(id.value),code);
                }
                if (options_.smoke_test) std::fprintf(stderr,"Native CI loop exit: code=%d settings_worker_joinable=%d\n",code,settings_worker_.joinable());
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
    if (renderer_call_) { settings_pending_=true; defer_layout(); return; }
    HIGHCONTRASTW contrast{}; contrast.cbSize=sizeof(contrast);
    high_contrast_ = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const auto background=system_color(COLOR_WINDOW), foreground=system_color(COLOR_WINDOWTEXT);
    const auto highlight=system_color(COLOR_HIGHLIGHT), highlight_text=system_color(COLOR_HIGHLIGHTTEXT), disabled=system_color(COLOR_GRAYTEXT);
    settings_panel_.set_high_contrast(high_contrast_,background,foreground,highlight,highlight_text,disabled);
    context_menu_.set_high_contrast(high_contrast_,background,foreground,highlight,highlight_text,disabled);
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    reduced_motion_ = high_contrast_ || !animations || config_.appearance.reduced_motion==ReducedMotionPreference::enabled;
    if (config_.appearance.reduced_motion==ReducedMotionPreference::disabled && animations && !high_contrast_) reduced_motion_=false;
    DWORD transparency = 1, size = sizeof(transparency);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &size);
    const BOOL dark = config_.theme.name != "daylight" && !high_contrast_;
    DwmSetWindowAttribute(hwnd_, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark));
    // Documented Windows 11 attributes. Unsupported builds return E_INVALIDARG;
    // no undocumented accent-policy calls, layered-window opacity or region hacks.
    const DWORD corners = 2; // DWMWCP_ROUND; DWM suppresses rounding when snapped/maximized.
    DwmSetWindowAttribute(hwnd_, static_cast<DWMWINDOWATTRIBUTE>(33), &corners, sizeof(corners));
    const auto render_capability = renderer_ ? renderer_->stats() : RendererStats{};
    material_tracker_.observe({render_capability.composition,render_capability.warp,render_capability.device_recoveries});
    const auto policy=window_material_policy(config_.appearance,render_capability.composition,render_capability.warp,high_contrast_,transparency!=0);
    const DWORD material=policy.system_backdrop;
    const HRESULT material_result=DwmSetWindowAttribute(hwnd_,static_cast<DWMWINDOWATTRIBUTE>(38),&material,sizeof(material));
    const bool material_requested=SUCCEEDED(material_result) && material!=1;
    MARGINS margins=material_requested?MARGINS{-1,-1,-1,-1}:MARGINS{1,1,1,1};
    const HRESULT frame_result=DwmExtendFrameIntoClientArea(hwnd_,&margins);
    backdrop_=material_requested && SUCCEEDED(frame_result);
    if (renderer_) renderer_->set_background_opacity(backdrop_?policy.opacity:1.0f);
    std::string status;
    switch(policy.fallback) {
    case MaterialFallback::solid_selected: status="Solid selected in Settings.";break;
    case MaterialFallback::no_composition: status="Opaque fallback: composition surface unavailable.";break;
    case MaterialFallback::software: status="Opaque fallback: software graphics rendering.";break;
    case MaterialFallback::high_contrast: status="Opaque: Windows high contrast is enabled.";break;
    case MaterialFallback::transparency_disabled: status="Opaque: Windows Transparency effects are off.";break;
    case MaterialFallback::none:
        if(FAILED(material_result)) status="Opaque: Windows system material is unavailable (requires Windows 11 22H2+).";
        else if(FAILED(frame_result)) status="Opaque: Windows could not extend the material into the client area.";
        else if(policy.opacity>=1) status="100% background opacity covers the material. Lower Background opacity.";
        else status=std::string(material==3?"Acrylic requested":"Mica (wallpaper) requested")+" · "+std::to_string(std::lround(policy.opacity*100))+"% tint.";
        break;
    }
    const bool policy_changed=status!=material_status_;
    if(policy_changed) {
        material_status_=std::move(status);
        char diagnostic[512]{};
        std::snprintf(diagnostic,sizeof diagnostic,"StormTTY material: %s composition=%u software=%u requested=%lu set_hr=0x%08lx frame_hr=0x%08lx\n",
            material_status_.c_str(),unsigned(render_capability.composition),unsigned(render_capability.warp),static_cast<unsigned long>(material),
            static_cast<unsigned long>(material_result),static_cast<unsigned long>(frame_result));
        OutputDebugStringA(diagnostic);
        if(options_.smoke_test) std::fputs(diagnostic,stderr);
    }
    queue_material_probe(policy_changed);
    caret_schedule(); changed();
}
void NativeWindow::publish_material_status() {
    // Put measured evidence first: this help row is deliberately single-line.
    // Keep actionable fallback errors first instead of burying them in metrics.
    const bool actionable=material_status_.starts_with("Opaque") || material_status_.starts_with("Solid") || material_status_.starts_with("100%");
    settings_panel_.set_material_status(material_probe_status_.empty()?material_status_:actionable?
        material_status_+" "+material_probe_status_:material_probe_status_+" "+material_status_);
}
void NativeWindow::queue_material_probe(bool invalidate) {
    if(hwnd_)KillTimer(hwnd_,material_probe_timer);
    if(renderer_)renderer_->cancel_alpha_probe();
    material_probe_running_=false;material_probe_requested_=true;material_probe_succeeded_=false;
    if(invalidate || material_probe_status_.empty())material_probe_status_="Canvas check after overlays close; DWM may use solid fallback.";
    publish_material_status();
}
void NativeWindow::begin_material_probe(const WindowScene& rendered) {
    if(!material_probe_requested_ || material_probe_running_ || !renderer_ || minimized_ || in_size_move_ || settings_pending_ ||
        overlay_open() || modal_ || palette_open_ || rename_open_ || palette_transition_.value(GetTickCount64())>0 || drag_!=Drag::none) return;
    std::vector<Rect> occluders;
    if(!notice_.empty())occluders.push_back({12,std::max(44.0,navigation_.footer.y-56),std::max(0.0,width_-24),44});
    std::optional<Rect> patch;SessionId sampled;
    // Prefer the active pane; if it has no uncovered padding, don't silently
    // call another pane's cell flags the active grid.
    for(const auto& pane:layout_.panes)if(pane.pane==workspace().active_pane()) {
        patch=material_probe_patch(pane.bounds,content_bounds(pane),occluders);sampled=pane.session;break;
    }
    material_probe_default_cells_=material_probe_explicit_cells_=material_probe_other_opaque_cells_=0;material_probe_cells_limited_=false;
    for(const auto& pane:rendered.panes)if(pane.id==sampled.value && pane.frame) {
        const auto& frame=*pane.frame;
        for(std::size_t r=0;r<std::min<std::size_t>(frame.rows,frame.lines.size()) && !material_probe_cells_limited_;++r) {
            if(!frame.lines[r])continue;const auto& row=*frame.lines[r];
            for(std::size_t column=0;column<std::min<std::size_t>(frame.columns,row.cells.size());++column) {
                if(material_probe_default_cells_+material_probe_explicit_cells_+material_probe_other_opaque_cells_>=65536) {material_probe_cells_limited_=true;break;}
                const auto& cell=row.cells[column];
                const bool selected=cell.selected || (row.selection && column>=row.selection->first && column<=row.selection->last);
                if(!cell.background_is_default)++material_probe_explicit_cells_;
                else if(cell.style.inverse || selected)++material_probe_other_opaque_cells_;
                else ++material_probe_default_cells_;
            }
        }
    }
    material_probe_requested_=false;
    if(!patch){material_probe_status_="Canvas check unavailable: no uncovered padding.";publish_material_status();return;}
    try {
        if(!renderer_->begin_alpha_probe(pixels(*patch))) {material_probe_status_="Canvas check unavailable: no GPU surface.";publish_material_status();return;}
        material_probe_present_=renderer_->stats().presents;
        material_probe_foreground_=GetForegroundWindow()==hwnd_;
        material_probe_dwm_=0;
        material_probe_dwm_result_=DwmGetWindowAttribute(hwnd_,static_cast<DWMWINDOWATTRIBUTE>(38),&material_probe_dwm_,sizeof(material_probe_dwm_));
        material_probe_deadline_=GetTickCount64()+2000;
        material_probe_running_=SetTimer(hwnd_,material_probe_timer,16,nullptr)!=0;
        if(!material_probe_running_) {renderer_->cancel_alpha_probe();material_probe_status_="Canvas check unavailable: timer could not start.";publish_material_status();}
    } catch(...) {renderer_->cancel_alpha_probe();material_probe_status_="Canvas check unavailable: GPU copy failed.";publish_material_status();}
}
void NativeWindow::poll_material_probe() {
    if(!material_probe_running_ || !renderer_)return;
    try {
        if(const auto sample=renderer_->poll_alpha_probe()) {
            KillTimer(hwnd_,material_probe_timer);material_probe_running_=false;material_probe_succeeded_=true;
            if(options_.smoke_test && (sample->minimum!=255 || sample->maximum!=255)) {
                smoke_finish("Alpha diagnostic sampled something other than the expected opaque WARP padding.");return;
            }
            const auto percent=[](unsigned alpha){return (alpha*100+127)/255;};
            material_probe_status_="Padding "+std::to_string(percent(sample->minimum));
            if(sample->minimum!=sample->maximum)material_probe_status_+="–"+std::to_string(percent(sample->maximum));
            const auto cells=material_probe_default_cells_+material_probe_explicit_cells_+material_probe_other_opaque_cells_;
            material_probe_status_+="% alpha (last, "+std::string(material_probe_foreground_?"active":"inactive")+")";
            if(cells)material_probe_status_+=" · grid "+std::to_string((material_probe_default_cells_*100+cells/2)/cells)+"% default"+(material_probe_cells_limited_?" (sample)":"");
            material_probe_status_+=". DWM may be solid.";
            publish_material_status();
            char diagnostic[640]{};
            std::snprintf(diagnostic,sizeof diagnostic,
                "StormTTY alpha-only diagnostic: padding_min=%u padding_max=%u samples=%u effective_opacity=%.3f composition=%u software=%u device_generation=%llu presents=%llu sampled_present=%llu sample_foreground=%u current_active_window=%u current_focused=%u dwm_type=%lu get_hr=0x%08lx exstyle=0x%08lx grid_default=%u grid_explicit=%u grid_other_opaque=%u grid_limited=%u. No RGB/text/desktop capture.\n",
                unsigned(sample->minimum),unsigned(sample->maximum),sample->samples,double(sample->effective_background_opacity),unsigned(sample->composition),unsigned(sample->software),
                static_cast<unsigned long long>(sample->device_generation),static_cast<unsigned long long>(sample->presents),static_cast<unsigned long long>(material_probe_present_),unsigned(material_probe_foreground_),
                unsigned(GetActiveWindow()==hwnd_),unsigned(GetFocus()==hwnd_),static_cast<unsigned long>(material_probe_dwm_),static_cast<unsigned long>(material_probe_dwm_result_),static_cast<unsigned long>(GetWindowLongPtrW(hwnd_,GWL_EXSTYLE)),material_probe_default_cells_,material_probe_explicit_cells_,material_probe_other_opaque_cells_,unsigned(material_probe_cells_limited_));
            OutputDebugStringA(diagnostic);if(options_.smoke_test)std::fputs(diagnostic,stderr);
            changed();return;
        }
        if(GetTickCount64()<material_probe_deadline_)return;
        material_probe_status_="Canvas check timed out; no alpha result.";
    } catch(...) {material_probe_status_="Canvas check unavailable: GPU map failed.";}
    KillTimer(hwnd_,material_probe_timer);material_probe_running_=false;renderer_->cancel_alpha_probe();publish_material_status();changed();
}
void NativeWindow::observe_material_renderer() {
    if(!renderer_) return;
    const auto capability=renderer_->stats();
    if(material_tracker_.observe({capability.composition,capability.warp,capability.device_recoveries})) {
        // Recovery can change an opaque child to DComp or vice versa. Reapply
        // after the renderer call, never reenter resource construction or poll.
        settings_pending_=true;defer_layout();
    }
}
void NativeWindow::changed(bool geometry) {
    ++chrome_generation_; dirty_ = true;
    const bool tooltip_allowed=active_ && !modal_ && !overlay_open() && !palette_open_ && !rename_open_;
    if (tooltip_ && tooltip_active_!=tooltip_allowed) {
        tooltip_active_=tooltip_allowed; SendMessageW(tooltip_,TTM_ACTIVATE,tooltip_allowed,0);
    }
    sync_focus();
    if (geometry && renderer_) refresh_layout();
    motion_schedule();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}
void NativeWindow::defer_layout() {
    if (!layout_pending_ && hwnd_) { layout_pending_=true; if (!PostMessageW(hwnd_,deferred_layout_message,0,0)) layout_pending_=false; }
}
struct RendererCallGuard {
    bool& flag;
    explicit RendererCallGuard(bool& value):flag(value){flag=true;}
    ~RendererCallGuard(){flag=false;}
};
void NativeWindow::refresh_layout() {
    // Style changes during DirectComposition construction can synchronously send
    // WM_SIZE. Never reenter renderer resource construction through that message.
    if (renderer_call_) { defer_layout(); return; }
    RECT bounds{}; GetClientRect(hwnd_, &bounds);
    width_ = (bounds.right - bounds.left) / scale_; height_ = (bounds.bottom - bounds.top) / scale_;
    maximized_ = IsZoomed(hwnd_) != FALSE;
    settings_panel_.resize({width_,height_}); context_menu_.resize({width_,height_});
    std::vector<TabId> tabs;
    for (const auto& tab : workspace().state().tabs) tabs.push_back(tab.id);
    const auto caption_width = std::max(46.0, GetSystemMetricsForDpi(SM_CXSIZE, dpi_) / scale_);
    std::vector<ProjectWorkspaceId> projects;
    for (const auto& project:projects_.entries()) projects.push_back(project.id);
    context_label_=directory_context();
    const double caption=fullscreen_?0:std::min(32.0,height_);
    navigation_=layout_project_navigation(width_,height_,caption,projects,navigation_first_,navigation_expanded_,focus_mode_||fullscreen_,true);
    chrome_=layout_chrome(width_,height_,tabs,workspace().active_tab(),caption_width,caption,navigation_.rail.width,navigation_.tabs.height);
    navigation_first_=navigation_.first;
    zoom_return_={};
    if (const auto* tab=workspace().tab(workspace().active_tab()); tab && tab->zoomed_pane && chrome_.drag.width>=260 && !fullscreen_)
        zoom_return_={chrome_.drag.x+chrome_.drag.width-128,2,120,28};
    layout_ = workspace().layout(workspace().active_tab(), workspace_viewport(), {1.0 / scale_, 7, 80, 48});
    if (renderer_) {
        {
            RendererCallGuard guard(renderer_call_);
            renderer_->resize(static_cast<std::uint32_t>(std::max(0L, bounds.right)), static_cast<std::uint32_t>(std::max(0L, bounds.bottom)), static_cast<float>(dpi_));
        }
        observe_material_renderer();
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
    refresh_tooltips();
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
    const auto* model = workspace().pane(pane);
    if (!model) return nullptr;
    auto found = sessions_.find(model->session);
    return found == sessions_.end() ? nullptr : found->second.get();
}
Session* NativeWindow::active_session() const { return session_for(workspace().active_pane()); }
SessionId NativeWindow::new_session() {
    ProcessOptions process;
    select_windows_console_provider(process,config_.windows_console_backend,options_.console_runtime_directory);
    // One factory covers tabs, splits and projects. Each Process owns its chosen
    // provider for life; only a successfully saved choice affects new sessions.
    if (has_managed_zsh_selection(config_) && !managed_shell_profile_)
        throw std::runtime_error("Managed Zsh is selected but no app-owned runtime is available. Build with STORMTTY_BUNDLE_ZSH=ON or use --shell-runtime; no fallback was launched.");
    const Profile* selected=using_managed_shell()?&*managed_shell_profile_:nullptr;
    if (!selected) for (const auto& profile:config_.profiles) if (profile.id==config_.default_profile) { selected=&profile; break; }
    if (selected) {
        process.executable=selected->executable; process.arguments=selected->arguments; process.working_directory=selected->working_directory;
        for (const auto& [name,value]:selected->environment) process.environment.push_back({name,value});
    }
    if (using_managed_shell()) {
        // Resolve only an absent HOME through wide APIs; explicit/empty HOME wins.
        for (auto& entry:managed_shell_home_environment()) process.environment.push_back(std::move(entry));
        // Child environment only: user startup hooks must not override this preset.
        for (const char* name:{"MSYS","ENV","BASH_ENV"}) process.environment.push_back({name,std::nullopt});
    }
    if (const auto* project=projects_.active(); project && !project->metadata.working_directory.empty()) process.working_directory=project->metadata.working_directory;
    // The built-in alias is resolved only against the trusted system directory,
    // never the current directory/PATH. Other configured executables must be
    // absolute and are validated by the process transport.
    std::string alias=process.executable;
    for (auto& character:alias) if (character>='A' && character<='Z') character=static_cast<char>(character-'A'+'a');
    if (!using_managed_shell() && (options_.smoke_test || alias.empty() || alias=="cmd.exe" || alias=="cmd")) {
        std::array<wchar_t, MAX_PATH + 1> directory{};
        const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        if (!length || length >= directory.size()) throw std::runtime_error("Could not discover the Windows system shell.");
        process.executable = narrow(std::wstring(directory.data(), length) + L"\\cmd.exe");
        if (options_.smoke_test) { process.arguments={"/d","/q"}; process.environment.clear(); if (!options_.smoke_projects) process.working_directory.clear(); }
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
    const bool bundled=process.provider==ProcessProvider::windows_packaged_experimental;
    auto session = Session::start(std::move(process), terminal, [weak] { if (auto state = weak.lock()) state->wake(); }, SessionRetentionPolicy::retain_document);
    if (options_.smoke_test) { smoke_sessions_.emplace(id,session); if (bundled) smoke_bundled_sessions_.insert(id); if (using_managed_shell()) smoke_managed_sessions_.insert(id); }
    sessions_.emplace(id, std::move(session));
    return id;
}
void NativeWindow::new_tab() {
    const auto id = new_session();
    auto result = workspace().create_tab(id, session_profile_id());
    if (!result) { sessions_.at(id)->request_close(); show_error(result.error); retire_unused_sessions(); return; }
    changed(true);
}
void NativeWindow::split(Direction direction) {
    const auto active = workspace().active_pane();
    if (!active) return;
    const auto id = new_session();
    auto result = workspace().split(active, direction, id, session_profile_id());
    if (!result) { sessions_.at(id)->request_close(); show_error(result.error); retire_unused_sessions(); return; }
    activate(*result.value); changed(true);
}
void NativeWindow::retire_unused_sessions() {
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
        const bool used = projects_.contains_session(iterator->first);
        if (!used) { iterator->second->request_close(); sizes_.erase(iterator->first); session_states_.erase(iterator->first); background_labels_.erase(iterator->first); iterator = sessions_.erase(iterator); }
        else ++iterator;
    }
}
void NativeWindow::show_modal(std::string title, std::vector<std::string> body, std::string confirm, std::function<void()> action, bool destructive) {
    if (modal_ || settings_input_blocked() || settings_loading()) return; // Never replace an unacknowledged decision/target.
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    cancel_drag();
    palette_open_=rename_open_=false; query_.clear(); preedit_.clear(); composing_=false; text_input_.reset();
    palette_transition_=ChromeTransition{}; closing_palette_primitives_.clear(); closing_palette_texts_.clear();
    if (ImmContext context(hwnd_); context.context) ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
    context_menu_.cancel(); menu_actions_.clear();
    modal_=Modal{std::move(title),std::move(body),std::move(confirm),std::move(action),destructive};
    modal_focus_.reset(); modal_pressed_.reset();
    if (tooltip_) SendMessageW(tooltip_,TTM_POP,0,0);
    SetFocus(hwnd_); changed(); caret_schedule();
}
void NativeWindow::answer_modal(ModalDecision decision) {
    if (!modal_) return;
    auto action=decision==ModalDecision::confirm?std::move(modal_->confirm):std::function<void()>{};
    if (GetCapture()==hwnd_) ReleaseCapture();
    modal_.reset(); modal_pressed_.reset(); text_input_.reset();
    if (action) action();
    if (hwnd_) { changed(); caret_schedule(); }
}
std::string NativeWindow::background_title(const ParkedSession& parked, bool status) const {
    std::string title=parked.title.empty()?parked.profile:parked.title;
    if (auto found=sessions_.find(parked.session); found!=sessions_.end()) {
        const auto info=found->second->info();
        if (!info.title.empty()) title=info.title;
        title.erase(std::remove_if(title.begin(),title.end(),[](unsigned char c){return c<0x20 || c==0x7f;}),title.end());
        if (title.empty()) title="Terminal";
        // Keep UTF-8 intact; DirectWrite clips bounded actor titles.
        if (!status) return title;
        if (info.exit_code) title+=" · exit "+std::to_string(*info.exit_code);
        else if (info.state==SessionState::failed) title+=" · failed";
        else if (info.state==SessionState::closing) title+=" · closing";
        else if (info.state==SessionState::starting) title+=" · starting";
        else title+=" · running";
    }
    return title;
}
void NativeWindow::park_pane(PaneId pane) {
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    cancel_drag();
    const auto result=projects_.park({projects_.active_id(),pane});
    if (!result) { show_error(result.error); return; }
    reset_project_view(); // Clear IME, selection and renderer-local placement state.
}
void NativeWindow::restore_parked(ProjectSessionReference target) {
    if (target.project!=projects_.active_id() || !sessions_.contains(target.local)) { show_error("The background target changed; nothing was restored."); return; }
    const auto result=projects_.reattach_to_new_tab(target);
    if (!result) { show_error(result.error); return; }
    notice_.clear();
    sizes_.erase(target.local); // Reattach always resizes and presents the actor's latest frame.
    reset_project_view();
}
void NativeWindow::close_parked(ProjectSessionReference target, bool approved) {
    const auto* project=projects_.find(target.project);
    if (!project || target.project!=projects_.active_id() || !sessions_.contains(target.local) ||
        std::none_of(project->parked_sessions.begin(),project->parked_sessions.end(),[&](const auto& item){return item.session==target.local;})) {
        show_error("The background target changed; nothing was closed."); return;
    }
    if (!approved) {
        show_modal("Close background terminal?",{"Its process may still be running.","Closing discards this terminal and its retained output."},"Close terminal",[this,target]{close_parked(target,true);}); return;
    }
    const auto removed=projects_.discard_parked(target);
    if (!removed) { show_error(removed.error); return; }
    retire_unused_sessions(); changed(true);
}
void NativeWindow::background_menu(Point point, std::optional<SessionId> only) {
    std::vector<ContextMenuItem> items;
    std::map<std::string,std::function<void()>> actions;
    for (const auto& parked:projects_.active()->parked_sessions) {
        if (only && parked.session!=*only) continue;
        const ProjectSessionReference target{projects_.active_id(),parked.session};
        const auto id="background."+std::to_string(parked.session.value);
        ContextMenuItem restore; restore.id=id+".restore"; restore.label="Restore terminal";
        restore.enabled=workspace().state().tabs.size()<Workspace::maximum_tabs;
        restore.disabled_reason="Tab limit reached";
        ContextMenuItem close; close.id=id+".close"; close.label="Close terminal…"; close.destructive=true;
        actions[restore.id]=[this,target]{restore_parked(target);}; actions[close.id]=[this,target]{close_parked(target);};
        if (only) { items.push_back(std::move(restore)); items.push_back(std::move(close)); }
        else {
            // One node per entry keeps all 256 supported sessions reachable
            // within the shared menu's 512-node bound.
            ContextMenuItem entry; entry.id=id; entry.label=background_title(parked); items.push_back(std::move(entry));
            actions[id]=[this,target,point] {
                if (projects_.active_id()!=target.project) { show_error("The background target changed; nothing was changed."); return; }
                background_menu(point,target.local);
            };
        }
    }
    open_menu(std::move(items),point,std::move(actions));
}
void NativeWindow::close_pane(PaneId pane, bool approved) {
    if (!workspace().pane(pane)) return;
    auto* session = session_for(pane);
    if (!approved && session && !session->process_finished()) {
        const ProjectPaneReference target{projects_.active_id(),pane};
        const auto session_id=workspace().pane(pane)->session;
        show_modal("Close this pane?",{"Programs may still be running.","Closing can discard unsaved work."},"Close pane",[this,target,session_id]{
            const auto* project=projects_.workspace(target.project); const auto* current=project?project->pane(target.local):nullptr;
            if (!current || current->session!=session_id || projects_.active_id()!=target.project) { show_error("The close target changed; nothing was closed."); return; }
            close_pane(target.local,true);
        }); return;
    }
    workspace().close_pane(pane); retire_unused_sessions();
    if (workspace().state().tabs.empty() && projects_.active()->parked_sessions.empty()) { close_project(projects_.active_id()); return; }
    changed(true);
}
void NativeWindow::close_tab(TabId tab, bool approved) {
    if (!workspace().tab(tab)) return;
    bool live = false;
    for (auto pane : workspace().pane_order(tab)) if (auto* session = session_for(pane); session && !session->process_finished()) live = true;
    if (!approved && live) {
        const ProjectTabReference target{projects_.active_id(),tab};
        std::vector<SessionId> expected;
        for (const auto pane:workspace().pane_order(tab)) expected.push_back(workspace().pane(pane)->session);
        show_modal("Close this tab?",{"Its panes may have running programs.","Closing can discard unsaved work."},"Close tab",[this,target,expected]{
            const auto* project=projects_.workspace(target.project); std::vector<SessionId> current;
            if (project) for (const auto pane:project->pane_order(target.local)) current.push_back(project->pane(pane)->session);
            if (!project || !project->tab(target.local) || current!=expected || projects_.active_id()!=target.project) { show_error("The close target changed; nothing was closed."); return; }
            close_tab(target.local,true);
        }); return;
    }
    workspace().close_tab(tab); retire_unused_sessions();
    if (workspace().state().tabs.empty() && projects_.active()->parked_sessions.empty()) { close_project(projects_.active_id()); return; }
    changed(true);
}
void NativeWindow::sync_focus() {
    SessionId desired{};
    if (active_ && !palette_open_ && !rename_open_ && !modal_ && !overlay_open()) if (const auto* pane=workspace().pane(workspace().active_pane())) desired=pane->session;
    if (desired==focused_session_) return;
    const auto previous=focused_session_; focused_session_=desired;
    if (sessions_.contains(previous)) { auto old=sessions_.at(previous); submit([old] { return old->focus(false); }); }
    if (sessions_.contains(desired)) { auto current=sessions_.at(desired); submit([current] { return current->focus(true); }); }
}
void NativeWindow::activate(PaneId pane) {
    if (workspace().active_pane() == pane) return;
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    if (!workspace().focus(pane)) return;
    text_input_.reset(); caret_schedule(true); changed(true);
}
void NativeWindow::update_sessions() {
    struct TitleUpdate { ProjectWorkspaceId project; PaneId pane; std::string title; };
    std::vector<TitleUpdate> titles;
    bool state_changed=false;
    // Hidden workspaces retain actors and model metadata. Never retire them just
    // because only the active project's panes are submitted to the renderer.
    for (const auto& project:projects_.entries()) for (const auto& [pane_id,pane]:project.workspace.state().panes) {
        auto found=sessions_.find(pane.session); if (found==sessions_.end()) continue;
        const auto info=found->second->info();
        if (!info.title.empty() && pane.title!=info.title) titles.push_back({project.id,pane_id,info.title});
        if (project.id==projects_.active_id() && !info.error.empty() && notice_!=info.error) { notice_=info.error; ++chrome_generation_; }
        if (!session_states_.contains(pane.session) || session_states_[pane.session]!=info.state) { session_states_[pane.session]=info.state; state_changed=true; ++chrome_generation_; }
    }
    for (const auto& project:projects_.entries()) for (const auto& parked:project.parked_sessions) {
        auto found=sessions_.find(parked.session); if (found==sessions_.end()) continue;
        const auto info=found->second->info();
        const auto title=background_title(parked);
        if (background_labels_[parked.session]!=title) { background_labels_[parked.session]=title; state_changed=true; ++chrome_generation_; }
        if (!session_states_.contains(parked.session) || session_states_[parked.session]!=info.state) {
            session_states_[parked.session]=info.state; state_changed=true; ++chrome_generation_;
            const std::string destination=project.id==projects_.active_id()?" Restore it from the footer to inspect output.":" Switch to this workspace and restore it from its footer to inspect output.";
            if (info.exit_code) notice_=project.metadata.name+" · Background terminal exited ("+std::to_string(*info.exit_code)+")."+destination;
            else if (info.state==SessionState::failed) notice_=project.metadata.name+" · Background terminal failed."+destination;
        }
    }
    for (auto& title:titles) { projects_.workspace(title.project)->set_pane_title(title.pane,std::move(title.title)); ++chrome_generation_; }
    std::optional<std::string> clipboard,error;
    { std::lock_guard lock(wake_->mutex); clipboard.swap(wake_->clipboard); error.swap(wake_->error); }
    if (clipboard) set_clipboard(*clipboard);
    if (error) show_error(*error);
    const auto context=directory_context();
    if (context_label_!=context) { context_label_=context; state_changed=true; ++chrome_generation_; }
    if (!titles.empty() || state_changed) refresh_tooltips();
    settings_completed();
    drain_input(); dirty_=true; caret_schedule(); position_ime();
    if (options_.smoke_test) smoke_progress();
}
std::string NativeWindow::directory_context() const {
    std::string path;
    if (auto* session=active_session()) path=session->info().working_directory;
    const bool observed=!path.empty();
    if (path.empty()) if (const auto* project=projects_.active()) path=project->metadata.working_directory;
    // OSC metadata is display-only; it is never an implicit chdir or command.
    path.erase(std::remove_if(path.begin(),path.end(),[](unsigned char c){return c<0x20 || c==0x7f;}),path.end());
    const auto* project=projects_.active();
    const std::string name=project?project->metadata.name:"Workspace";
    return path.empty()?name:name+"  /  "+(observed?"Shell directory · ":"Launch folder · ")+path;
}
void NativeWindow::reset_project_view() {
    // Local Pane/Tab/Node identifiers can collide across projects. Actor IDs are
    // host-global; renderer identity is SessionId and all local view caches reset.
    hover_transitions_.clear(); tab_transitions_.clear(); pane_reveals_.clear();
    closing_palette_primitives_.clear(); closing_palette_texts_.clear(); palette_transition_.finish();
    hover_={-1,-1}; palette_open_=rename_open_=false; query_.clear(); preedit_.clear(); composing_=false;
    text_input_.reset(); selection_pane_={}; drop_preview_.reset(); force_redraw_=true;
    if (ImmContext context(hwnd_); context.context) ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
    const auto& entries=projects_.entries();
    const auto selected=std::find_if(entries.begin(),entries.end(),[&](const auto& project){return project.id==projects_.active_id();});
    const auto index=static_cast<std::size_t>(selected-entries.begin());
    if (index<navigation_first_) navigation_first_=index;
    else if (navigation_.capacity && index>=navigation_first_+navigation_.capacity) navigation_first_=index-navigation_.capacity+1;
    changed(true); caret_schedule();
}
void NativeWindow::activate_project(ProjectWorkspaceId id) {
    if (modal_ || overlay_open() || id==projects_.active_id()) return;
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    cancel_drag();
    const auto status=projects_.activate(id); if (!status) { show_error(status.error); return; }
    reset_project_view();
}
void NativeWindow::choose_project_folder() {
    ComObject<IFileOpenDialog> dialog;
    auto check=[](HRESULT hr){if (FAILED(hr)) throw std::runtime_error("Could not open the native workspace folder picker.");};
    check(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IFileOpenDialog),reinterpret_cast<void**>(dialog.put())));
    FILEOPENDIALOGOPTIONS flags{}; check(dialog->GetOptions(&flags));
    check(dialog->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR));
    check(dialog->SetTitle(L"Choose a workspace folder")); check(dialog->SetOkButtonLabel(L"Open workspace"));
    const auto shown=dialog->Show(hwnd_); if (shown==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return; check(shown);
    if (modal_ || !hwnd_) return;
    ComObject<IShellItem> item; check(dialog->GetResult(item.put()));
    PWSTR raw=nullptr; check(item->GetDisplayName(SIGDN_FILESYSPATH,&raw));
    struct PathOwner { PWSTR value; ~PathOwner(){CoTaskMemFree(value);} } owner{raw};
    const std::filesystem::path path(raw); auto name=project_name(path);
    const auto created=projects_.create({name,narrow(path.wstring()),"Local"});
    if (!created) { show_error(created.error); return; }
    activate_project(*created.value); new_tab();
}
void NativeWindow::close_project(ProjectWorkspaceId id, bool approved) {
    if (!projects_.find(id)) return;
    bool live=false;
    for (const auto session:projects_.session_ids(id)) if (sessions_.contains(session) && !sessions_.at(session)->process_finished()) live=true;
    if (!approved && live) {
        const auto expected=projects_.session_ids(id);
        show_modal("Close this workspace?",{"Its sessions may have running programs.","Closing can discard unsaved work."},"Close workspace",[this,id,expected]{
            if (!projects_.find(id) || projects_.session_ids(id)!=expected) { show_error("The workspace changed; nothing was closed."); return; }
            close_project(id,true);
        }); return;
    }
    if (projects_.entries().size()==1) { DestroyWindow(hwnd_); return; } // Deliberate final close, never a replacement shell.
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    cancel_drag();
    const auto removed=projects_.remove(id); if (!removed) { show_error(removed.error); return; }
    retire_unused_sessions(); reset_project_view();
}
void NativeWindow::show_error(std::string_view error) { notice_ = std::string(error); changed(); }

std::string NativeWindow::pane_title(PaneId id, bool full) const {
    const auto* pane = workspace().pane(id);
    if (!pane) return {};
    std::string result = pane->title;
    const Profile* profile = managed_shell_profile_ && pane->profile==managed_shell_profile_->id?&*managed_shell_profile_:nullptr;
    if (!profile) for (const auto& candidate : config_.profiles) if (candidate.id == pane->profile) { profile = &candidate; break; }
    if (result.empty()) result = profile && !profile->label.empty() ? profile->label : "Terminal";
    // Only replace an exact executable path, never arbitrary OSC text or a user
    // rename. The untouched title remains available in the native tooltip.
    if (!full && profile && !profile->label.empty()) {
        auto canonical = [](std::string value) {
            for (auto& c : value) { if (c == '/') c = '\\'; if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); }
            return value;
        };
        const auto title = canonical(result), executable = canonical(profile->executable);
        bool exact = !executable.empty() && title == executable;
        if (executable == "cmd.exe" || executable == "cmd") {
            std::array<wchar_t, MAX_PATH + 1> directory{};
            const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
            if (length && length < directory.size()) exact = exact || title == canonical(narrow(std::wstring(directory.data(), length) + L"\\cmd.exe"));
        }
        if (exact) result = profile->label;
    }
    result.erase(std::remove_if(result.begin(), result.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }), result.end());
    // The model bounds untrusted titles. DirectWrite performs visual ellipsis;
    // do not byte-truncate UTF-8 or mutate the session title for presentation.
    if (auto* session = session_for(id); session && session->process_finished()) result += " · exited";
    return result;
}
std::string NativeWindow::tab_title(TabId id, bool full) const {
    const auto* tab = workspace().tab(id);
    if (!tab) return {};
    return tab->title.empty() ? pane_title(tab->active_pane, full) : tab->title;
}
void NativeWindow::refresh_tooltips() {
    if (!tooltip_) return;
    for (const auto& [id, text] : tooltip_texts_) {
        (void)text; TOOLINFOW info{}; info.cbSize=sizeof(info); info.hwnd=hwnd_; info.uId=id;
        SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
    tooltip_texts_.clear();
    UINT_PTR next=1;
    auto add = [&](Rect bounds, std::string text) {
        if (bounds.width <= 0 || bounds.height <= 0) return;
        const auto id=next++; auto& owned=tooltip_texts_[id]; owned=widen(text);
        TOOLINFOW info{}; info.cbSize=sizeof(info); info.hwnd=hwnd_; info.uId=id; info.uFlags=TTF_SUBCLASS;
        const auto r=pixels(bounds); info.rect={static_cast<LONG>(r.x),static_cast<LONG>(r.y),static_cast<LONG>(r.x+r.width),static_cast<LONG>(r.y+r.height)};
        info.lpszText=owned.data(); SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    };
    for (const auto& tab:chrome_.tabs) add(tab.bounds,tab_title(tab.tab,true));
    if (layout_.panes.size()>1) for (const auto& pane:layout_.panes) add({pane.bounds.x,pane.bounds.y,pane.bounds.width,28},pane_title(pane.pane,true));
    for (const auto& row:navigation_.projects) if (const auto* project=projects_.find(row.project)) add(row.bounds,project->metadata.name+"\n"+project->metadata.working_directory);
    add(navigation_.toggle,navigation_.expanded?"Collapse workspaces":width_<640?"Navigation stays compact in narrow windows":"Expand workspaces");
    add(navigation_.settings,"Settings · Ctrl+,"); add(navigation_.new_project,"New workspace… Choose a folder"); add(navigation_.context,context_label_); add(navigation_.focus,"Focus mode: hide navigation, directory context and footer");
    const auto footer=background_layout();
    for (std::size_t i=0;i<footer.chips.size();++i) add(footer.chips[i],background_title(projects_.active()->parked_sessions[i])+"\nClick to restore · Right-click for actions");
    add(footer.overflow,"All background terminals");
    add(chrome_.new_tab,"New tab · Ctrl+Shift+T"); add(chrome_.palette,"Commands · Ctrl+Shift+P"); add(chrome_.menu,"StormTTY menu");
}
double NativeWindow::hover_amount(std::uint64_t id) const {
    const auto found=hover_transitions_.find(id);
    return found==hover_transitions_.end()?0:found->second.value(GetTickCount64());
}
void NativeWindow::hover_changed(Point point) {
    hover_=point; const auto now=GetTickCount64(); bool modified=false;
    auto target=[&](std::uint64_t id, Rect bounds) {
        const double next=bounds.contains(point)?1:0;
        auto& transition=hover_transitions_[id]; const double before=transition.value(now);
        transition.target(next,now,reduced_motion_?0:80);
        modified=modified || before!=next;
    };
    target(1,chrome_.menu); target(2,chrome_.new_tab); target(3,chrome_.palette);
    target(8,navigation_.toggle); target(9,navigation_.new_project); target(10,navigation_.focus); target(11,navigation_.settings);
    for (const auto& project:navigation_.projects) target((1ull<<62)|project.project.value,project.bounds);
    target(4,chrome_.minimize); target(5,chrome_.maximize); target(6,chrome_.close); target(7,zoom_return_);
    for (const auto& tab:chrome_.tabs) { target(16+tab.tab.value*2,tab.bounds); target(17+tab.tab.value*2,tab.close); }
    for (const auto& divider:layout_.dividers) target((1ull<<63)|divider.node.value,divider.hit_bounds);
    if (modified) changed();
}
void NativeWindow::motion_schedule() {
    const auto now=GetTickCount64(); const auto duration=reduced_motion_?0:120;
    palette_transition_.target(palette_open_||rename_open_?1:0,now,reduced_motion_?0:100);
    for (const auto& tab:chrome_.tabs) tab_transitions_[tab.tab].target(tab.tab==workspace().active_tab()?1:0,now,duration);
    for (const auto& pane:layout_.panes) pane_reveals_[pane.pane].target(1,now,duration);
    std::erase_if(tab_transitions_,[&](const auto& item){return !workspace().tab(item.first);});
    std::erase_if(pane_reveals_,[&](const auto& item){return !workspace().pane(item.first);});
    std::erase_if(hover_transitions_,[&](const auto& item){
        if (item.first&(1ull<<63)) return !workspace().node(NodeId{item.first&~(1ull<<63)});
        if (item.first&(1ull<<62)) return !projects_.find(ProjectWorkspaceId{item.first&~(1ull<<62)});
        return item.first>=16 && !workspace().tab(TabId{(item.first-16)/2});
    });
    bool running=false;
    auto check=[&](ChromeTransition& transition) { if (reduced_motion_ || minimized_) transition.finish(); running=running||transition.active(now); };
    check(palette_transition_);
    for (auto& [id,t]:hover_transitions_) { (void)id; check(t); }
    for (auto& [id,t]:tab_transitions_) { (void)id; check(t); }
    for (auto& [id,t]:pane_reveals_) { (void)id; check(t); }
    if (running && !motion_timer_active_) {
        motion_timer_active_=SetTimer(hwnd_,chrome_motion_timer,16,nullptr)!=0;
        if (!motion_timer_active_) { // Resource pressure must never leave chrome half-hidden.
            palette_transition_.finish();
            for (auto& [id,t]:hover_transitions_) { (void)id; t.finish(); }
            for (auto& [id,t]:tab_transitions_) { (void)id; t.finish(); }
            for (auto& [id,t]:pane_reveals_) { (void)id; t.finish(); }
        }
    }
    else if (!running && motion_timer_active_) { KillTimer(hwnd_,chrome_motion_timer); motion_timer_active_=false; }
}
void NativeWindow::open_settings(std::string binding) {
    if (modal_ || settings_input_blocked() || settings_job_) return;
    if (options_.config_path.empty()) { show_error("No settings file path is available. Start with --config and an absolute local path."); return; }
    if (settings_worker_.joinable()) settings_worker_.join(); // Prior completed worker only.
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    cancel_drag();
    const auto path=options_.config_path;
    auto shared=wake_;
    settings_binding_=std::move(binding); settings_open_requested_=true;
    settings_job_=true;
    try {
        settings_worker_=std::thread([shared,path,configured=config_.profiles] {
            SettingsCompletion result; result.kind=SettingsCompletion::Kind::load;
            try {
                result.loaded=load_settings_snapshot(path);
                if (result.loaded) result.shells=discover_windows_shells(
                    result.loaded.snapshot->existed?result.loaded.config->profiles:configured);
            }
            catch (const std::exception& e) { result.loaded.error=e.what(); }
            catch (...) { result.loaded.error="Settings discovery failed unexpectedly."; }
            { std::lock_guard lock(shared->mutex); shared->settings=std::move(result); }
            shared->wake();
        });
    } catch (...) { settings_job_=false; settings_open_requested_=false; throw; }
    context_menu_.cancel(); menu_actions_.clear();
    palette_open_=rename_open_=false; query_.clear(); preedit_.clear(); composing_=false; text_input_.reset();
    palette_transition_=ChromeTransition{}; closing_palette_primitives_.clear(); closing_palette_texts_.clear();
    if (ImmContext context(hwnd_); context.context) ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
    notice_.clear(); caret_schedule(); changed();
}
void NativeWindow::settings_completed() {
    std::optional<SettingsCompletion> completion;
    { std::lock_guard lock(wake_->mutex); completion.swap(wake_->settings); }
    if (!completion) return;
    if (settings_worker_.joinable()) settings_worker_.join(); // I/O ended before result publication.
    settings_job_=false;
    if (completion->kind==SettingsCompletion::Kind::load) {
        if (!settings_open_requested_) return;
        settings_open_requested_=false;
        if (!completion->loaded) { show_error(completion->loaded.error); return; }
        if (modal_) { show_error("Settings are ready. Finish the current confirmation, then open Settings again."); return; }
        if (options_.smoke_settings && smoke_settings_stage_==1 && completion->loaded.snapshot->existed) {
            smoke_finish("The isolated Settings smoke path unexpectedly already exists."); return;
        }
        settings_snapshot_=std::move(completion->loaded.snapshot);
        if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
        cancel_drag(); context_menu_.cancel(); menu_actions_.clear(); palette_open_=rename_open_=false; query_.clear(); preedit_.clear(); composing_=false; text_input_.reset();
        palette_transition_=ChromeTransition{}; closing_palette_primitives_.clear(); closing_palette_texts_.clear();
        if (ImmContext context(hwnd_); context.context) ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
        if (managed_shell_profile_) completion->shells.profiles.push_back(managed_zsh_profile_reference());
        // Edit the latest valid disk value, but Cancel restores the running
        // presentation. Keep the exact token from this read for atomic Save.
        // Missing files retain running defaults (including managed shell setup).
        const auto& editor=settings_snapshot_->existed?*completion->loaded.config:config_;
        settings_panel_.open(config_,editor,completion->shells.profiles);
        if (!settings_binding_.empty()) settings_panel_.open_for_binding(config_,settings_binding_,completion->shells.profiles);
        settings_panel_.set_console_backend_status(bundled_console_runtime_present(),
            !options_.console_runtime_directory.empty()?
                "CLI override active · saved choice applies without flag · resize unqualified":
            !native_amd64_console_supported()?
                "Preview requires native AMD64 · resize unqualified":
            bundled_console_runtime_present()?
                "Preview · native AMD64 · resize unqualified · ConPTY 1.25.260930003":
                "Bundle unavailable · select System to recover · preview resize unqualified");
        settings_panel_.resize({width_,height_}); context_menu_.resize({width_,height_}); settings_binding_.clear(); notice_.clear();
        if (tooltip_) SendMessageW(tooltip_,TTM_POP,0,0);
        SetFocus(hwnd_); settings_input_changed(); caret_schedule(); changed();
    } else {
        if (completion->saved) {
            if (options_.smoke_settings) smoke_settings_file_owned_=true;
            settings_snapshot_=std::move(completion->saved.snapshot);
            settings_panel_.persistence_result(true);
            // Persistence succeeded. A later GPU/font failure must not strand
            // the editor in Saving or imply that the file write was rolled back.
            std::string apply_error;
            try { preview_settings(completion->candidate); }
            catch (const std::exception& error) { apply_error=error.what(); }
            config_=std::move(completion->candidate);
            notice_=apply_error.empty()?(!options_.console_runtime_directory.empty()?
                "Settings saved. --console-runtime overrides this launch; saved backend applies without that flag.":managed_shell_profile_ && options_.shell_runtime_override?
                "Settings saved. --shell-runtime keeps managed Zsh for this launch; the saved shell applies without that override.":
                "Settings saved. Shell and console backend apply to new tabs and panes; existing sessions are unchanged."):
                "Settings saved, but presentation could not update. Restart to apply: "+apply_error;
        } else settings_panel_.persistence_result(false,completion->saved.error);
        caret_schedule(true); changed(true);
    }
}
void NativeWindow::preview_settings(const Config& candidate) {
    const bool font_changed=config_.font!=candidate.font;
    if (renderer_ && font_changed) {
        FontOptions font; font.family=candidate.font.family; font.size_dip=static_cast<float>(candidate.font.size_dip); font.ligatures=candidate.font.ligatures;
        RendererCallGuard guard(renderer_call_); renderer_->set_font(std::move(font));
    }
    if (config_.theme!=candidate.theme) {
        const auto foreground=color_hex(candidate.theme.foreground,{238,237,236,255});
        const auto background=color_hex(candidate.theme.background,{17,21,27,255});
        // Display-only actor updates are coalesced separately from PTY input.
        // Exited sessions retain their immutable final frame; no shell restarts.
        for (const auto& [id,session]:sessions_) { (void)id; session->set_default_colors(foreground,background); }
        config_.theme=candidate.theme;
    }
    config_.font=candidate.font; config_.appearance=candidate.appearance;
    settings_changed(); caret_schedule(); changed(font_changed);
}
void NativeWindow::settings_actions() {
    while (auto action=settings_panel_.take_action()) {
        if (action->kind==SettingsActionKind::preview || action->kind==SettingsActionKind::cancel) {
            preview_settings(action->config);
            if (action->kind==SettingsActionKind::cancel) { text_input_.reset(); caret_schedule(true); }
        } else {
            if (settings_job_ || !settings_snapshot_) { settings_panel_.persistence_result(false,"A settings snapshot is not ready. Close and reopen Settings."); continue; }
            if (settings_worker_.joinable()) settings_worker_.join();
            try {
                const auto path=options_.config_path; const auto baseline=*settings_snapshot_; auto candidate=std::move(action->config); auto shared=wake_;
                settings_job_=true;
                settings_worker_=std::thread([shared,path,baseline,candidate=std::move(candidate)]() mutable {
                    SettingsCompletion result; result.kind=SettingsCompletion::Kind::save; result.candidate=std::move(candidate);
                    try { result.saved=save_settings_config(path,result.candidate,baseline); }
                    catch (const std::exception& e) { result.saved.error=e.what(); }
                    catch (...) { result.saved.error="Settings could not be saved."; }
                    { std::lock_guard lock(shared->mutex); shared->settings=std::move(result); }
                    shared->wake();
                });
            } catch (const std::exception& e) { settings_job_=false; settings_panel_.persistence_result(false,e.what()); }
        }
    }
}
void NativeWindow::settings_input_changed() {
    settings_actions();
    if (settings_seen_generation_!=settings_panel_.generation()) {
        settings_seen_generation_=settings_panel_.generation(); changed(); caret_schedule();
    }
}
CommandContext NativeWindow::command_context() const {
    CommandContext context;
    context.supports_background_sessions=true; context.parked_sessions=projects_.active()->parked_sessions.size();
    context.supports_project_workspaces=true; context.project_workspaces=projects_.entries().size(); context.supports_focus_mode=true;
    context.tabs = workspace().state().tabs.size();
    context.total_panes = workspace().state().panes.size();
    context.panes = workspace().pane_order(workspace().active_tab()).size();
    context.clipboard_has_text = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
    if (const auto* tab = workspace().tab(workspace().active_tab())) context.zoomed = tab->zoomed_pane.has_value();
    if (auto* session = active_session()) {
        context.session_running = !session->process_finished();
        if (auto frame = session->frame()) for (const auto& row : frame->lines) if (row && row->selection) { context.has_selection = true; break; }
    }
    return context;
}
CommandAvailability host_availability(std::string_view id, const CommandContext& context) {
    if (id == "terminal.find") return {false, "The retained-text search interface is not available yet"};
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
    if (modal_ || overlay_open()) return;
    if (drag_!=Drag::none || mouse_capture_pane_) cancel_drag();
    if (!host_availability(command, command_context()).enabled) { show_error(host_availability(command, command_context()).reason); return; }
    notice_.clear();
    const auto pane = workspace().active_pane();
    const auto tab = workspace().active_tab();
    const Rect viewport=workspace_viewport();
    auto report = [this](Status status) { if (!status) show_error(status.error); };
    if (command == "settings.open") { open_settings(); return; }
    if (command == "app.command_palette") { toggle_palette(); return; }
    if (command == "workspace.new") { choose_project_folder(); return; }
    if (command == "workspace.close") { close_project(projects_.active_id()); return; }
    if (command == "workspace.toggle_navigation") {
        if (width_<640 && !focus_mode_) { show_error("Widen the window to expand workspaces. The compact rail remains available."); return; }
        navigation_expanded_=!navigation_expanded_; focus_mode_=false; changed(true); return;
    }
    if (command == "window.focus_mode") { focus_mode_=!focus_mode_; changed(true); return; }
    if (command == "workspace.next" || command == "workspace.previous") {
        const auto& entries=projects_.entries();
        const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& item){return item.id==projects_.active_id();});
        const auto index=static_cast<std::size_t>(found-entries.begin());
        const auto next=(index+(command=="workspace.next"?1:entries.size()-1))%entries.size();
        activate_project(entries[next].id); return;
    }
    if (command == "tab.new") { new_tab(); return; }
    if (command == "tab.close") { close_tab(tab); return; }
    if (command == "pane.background") { park_pane(pane); return; }
    if (command == "pane.close") { close_pane(pane); return; }
    if (command == "pane.split_right") { split(Direction::right); return; }
    if (command == "pane.split_down") { split(Direction::down); return; }
    if (command == "edit.copy") { copy(); return; }
    if (command == "edit.paste") { paste(); return; }
    if (command == "window.fullscreen") { toggle_fullscreen(); return; }
    if (command == "tab.rename") {
        rename_open_ = true; palette_open_ = false; rename_tab_ = tab; query_ = workspace().tab(tab)->title; changed(); return;
    }
    if (command == "tab.next" || command == "tab.previous" || command == "tab.move_left" || command == "tab.move_right") {
        const auto& tabs = workspace().state().tabs;
        const auto found = std::find_if(tabs.begin(), tabs.end(), [tab](const Tab& item) { return item.id == tab; });
        const auto index = static_cast<std::size_t>(std::distance(tabs.begin(), found));
        const bool previous = command == "tab.previous" || command == "tab.move_left";
        const auto next = (index + tabs.size() + (previous ? tabs.size() - 1 : 1)) % tabs.size();
        if (command.starts_with("tab.move_")) report(workspace().reorder_tab(tab, next));
        else activate(tabs[next].active_pane);
    } else if (command == "pane.zoom") report(workspace().toggle_zoom());
    else if (command == "pane.new_tab") {
        auto result = workspace().move_pane_to_new_tab(pane);
        if (!result) show_error(result.error);
    } else if (command == "pane.move_to_tab" || command == "pane.swap") {
        std::vector<ContextMenuItem> items;
        std::map<std::string,std::function<void()>> actions;
        const ProjectPaneReference source{projects_.active_id(),pane};
        const auto source_session=workspace().pane(pane)->session;
        const bool swap=command=="pane.swap";
        auto add=[&](PaneId destination,std::string label) {
            const auto* target=workspace().pane(destination); if (!target) return;
            const auto id="destination:"+std::to_string(destination.value); const auto expected=target->session;
            items.push_back({id,std::move(label),{},{},true,false,{}});
            actions[id]=[this,source,source_session,destination,expected,swap] {
                auto* project=projects_.workspace(source.project);
                if (!project || projects_.active_id()!=source.project || !project->pane(source.local) || !project->pane(destination) ||
                    project->pane(source.local)->session!=source_session || project->pane(destination)->session!=expected) { show_error("The pane destination changed; nothing was moved."); return; }
                const auto result=swap?project->swap_panes(source.local,destination):project->move_pane(source.local,destination,Direction::right);
                if (!result) show_error(result.error); changed(true);
            };
        };
        if (!swap) for (const auto& candidate:workspace().state().tabs) { if (candidate.id!=tab) add(candidate.active_pane,tab_title(candidate.id)); }
        else for (const auto target:workspace().pane_order(tab)) if (target!=pane) add(target,pane_title(target));
        open_menu(std::move(items),{width_/2,80},std::move(actions),true);
        return;
    } else if (command.starts_with("pane.focus_") || command.starts_with("pane.move_")) {
        const auto direction = command.ends_with("left") ? Direction::left : command.ends_with("right") ? Direction::right : command.ends_with("up") ? Direction::up : Direction::down;
        auto status = workspace().focus_direction(direction, viewport, {1/scale_,7,80,48});
        const auto target = workspace().active_pane();
        workspace().focus(pane);
        if (!status) show_error(status.error);
        else if (command.starts_with("pane.focus_")) activate(target);
        else report(workspace().move_pane(pane, target, direction));
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
            if (command == "pane.equalize") report(workspace().equalize_split(best->node));
            else report(workspace().resize_split(best->node, best->applied_ratio + ((command.ends_with("left") || command.ends_with("up")) ? -0.05 : 0.05), viewport, {1/scale_,7,80,48}));
        } else show_error("There is no adjacent divider in that direction.");
    }
    changed(true);
}
WindowScene NativeWindow::scene() {
    WindowScene result;
    const Color background = high_contrast_ ? system_color(COLOR_WINDOW) : color_hex(config_.theme.background, {23,25,29,255});
    Color chrome = high_contrast_ ? system_color(COLOR_BTNFACE) : color_hex(config_.theme.chrome, {34,37,43,255});
    const bool daylight=config_.theme.name=="daylight";
    Color raised = high_contrast_ ? system_color(COLOR_WINDOW) : daylight?Color{225,230,235,255}:Color{37,45,58,255};
    const Color text = high_contrast_ ? system_color(COLOR_BTNTEXT) : color_hex(config_.theme.foreground, {236,238,242,255});
    const Color secondary = high_contrast_ ? text : daylight?Color{88,101,114,255}:Color{162,173,190,255};
    const Color accent = high_contrast_ ? system_color(COLOR_WINDOWTEXT) : color_hex(config_.theme.accent, {167,196,210,255});
    const Color seam = high_contrast_ ? text : Color{162,173,190,56};
    result.background = background; result.chrome_generation = chrome_generation_; result.force_redraw=force_redraw_;
    ChromeZOrder z;
    auto rectangle = [&](Rect bounds, Color fill, double radius = 0, Color stroke = {}, double stroke_width = 0, bool replace = false) {
        result.primitives.push_back({pixels(bounds), fill, stroke, static_cast<float>(radius * scale_), static_cast<float>(stroke_width * scale_), ChromePrimitiveKind::rectangle, replace, z});
    };
    auto line = [&](double x1, double y1, double x2, double y2, Color color, double width = 1) {
        result.primitives.push_back({pixels({x1,y1,x2-x1,y2-y1}), {}, color, 0, static_cast<float>(width * scale_), ChromePrimitiveKind::line, false, z});
    };
    auto label = [&](Rect bounds, std::string value, Color color, float size = 13, bool bold = false, ChromeTextAlignment alignment = ChromeTextAlignment::leading) {
        result.texts.push_back({pixels(bounds), std::move(value), color, size, bold, "Segoe UI Variable Text", alignment, z});
    };
    const auto now=GetTickCount64();
    auto opacity=[](Color color,double amount) { color.a=static_cast<std::uint8_t>(std::clamp(std::lround(color.a*amount),0L,255L)); return color; };
    auto blend=[](Color a,Color b,double t) {
        auto channel=[&](int x,int y){return static_cast<std::uint8_t>(std::lround(x+(y-x)*t));};
        return Color{channel(a.r,b.r),channel(a.g,b.g),channel(a.b,b.b),channel(a.a,b.a)};
    };
    if (navigation_.rail.width>0) {
        Color rail=high_contrast_?background:daylight?Color{244,246,248,255}:Color{19,24,31,255};
        if (backdrop_) rail.a=240;
        rectangle(navigation_.rail,rail,0,{},0,true);
        rectangle({navigation_.rail.width-1/scale_,chrome_.band.height,1/scale_,navigation_.rail.height},seam);
        if (const auto amount=hover_amount(8); amount>0) rectangle(navigation_.toggle,opacity(raised,amount),5);
        const double tx=navigation_.toggle.x+16,ty=navigation_.toggle.y+14;
        rectangle({tx-7,ty-6,14,12},{0,0,0,0},1,secondary,1);
        line(tx-2,ty-6,tx-2,ty+6,secondary);
        if (navigation_.expanded) label({44,chrome_.band.height+10,132,18},"WORKSPACES",secondary,11);
        for (const auto& row:navigation_.projects) {
            const auto* project=projects_.find(row.project); if (!project) continue;
            const bool selected=row.project==projects_.active_id();
            const double amount=hover_amount((1ull<<62)|row.project.value);
            if (selected||amount>0) rectangle(row.bounds,selected?raised:opacity(raised,amount*.65),5);
            const double x=row.bounds.x+10,y=row.bounds.y+10;
            rectangle({x,y+3,16,10},{0,0,0,0},1,selected?accent:secondary,1);
            line(x,y+3,x,y,selected?accent:secondary); line(x,y,x+6,y,selected?accent:secondary); line(x+6,y,x+9,y+3,selected?accent:secondary);
            if (navigation_.expanded) label({row.bounds.x+38,row.bounds.y+7,row.bounds.width-44,20},project->metadata.name,selected?text:secondary,13,selected);
        }
        if (const auto amount=hover_amount(9); amount>0) rectangle(navigation_.new_project,opacity(raised,amount),5);
        if (navigation_.new_project.height>0) {
            const double x=navigation_.new_project.x+18,y=navigation_.new_project.y+18;
            line(x-5,y,x+5,y,secondary); line(x,y-5,x,y+5,secondary);
            if (navigation_.expanded) label({navigation_.new_project.x+38,navigation_.new_project.y+8,navigation_.new_project.width-44,20},"New workspace",secondary,12);
        }
        if (navigation_.settings.height>0) {
            if (const auto amount=hover_amount(11); amount>0) rectangle(navigation_.settings,opacity(raised,amount),5);
            label({navigation_.settings.x+4,navigation_.settings.y+4,28,28},"⚙",secondary,17,false,ChromeTextAlignment::center);
            if (navigation_.expanded) label({navigation_.settings.x+38,navigation_.settings.y+8,navigation_.settings.width-44,20},"Settings",secondary,12);
        }
    }
    if (navigation_.context.height>0) {
        Color context=chrome; context.a=high_contrast_?255:90;
        rectangle(navigation_.context,context);
        label({navigation_.context.x+20,navigation_.context.y+7,std::max(0.0,navigation_.context.width-60),18},context_label_,secondary,12);
        rectangle({navigation_.context.x,navigation_.context.y+navigation_.context.height-1/scale_,navigation_.context.width,1/scale_},seam);
        if (const auto amount=hover_amount(10); amount>0) rectangle(navigation_.focus,opacity(raised,amount),4);
        const double x=navigation_.focus.x+12,y=navigation_.focus.y+10;
        for (const int sign:{-1,1}) { line(x+sign*7,y+sign*3,x+sign*7,y+sign*6,secondary); line(x+sign*7,y+sign*6,x+sign*3,y+sign*6,secondary); }
    }
    for (const auto& placement : layout_.panes) {
        if (auto* session = session_for(placement.pane)) {
            auto frame=session->frame();
            const bool cursor_visible=!frame || !frame->cursor.blinking || reduced_motion_ || cursor_blink_.visible();
            result.panes.push_back({placement.session.value,pixels(content_bounds(placement)),std::move(frame),active_ && placement.pane==workspace().active_pane() && !palette_open_ && !rename_open_ && !modal_ && !overlay_open(),cursor_visible,reduced_motion_ || cursor_blink_.visible()});
        }
        if (layout_.panes.size() > 1) {
            const Rect header{placement.bounds.x, placement.bounds.y, placement.bounds.width, 28};
            if (!high_contrast_) {
                Color wash=chrome; wash.a=89; rectangle(header,wash);
                rectangle({header.x,header.y+header.height-1/scale_,header.width,1/scale_},daylight?Color{130,142,155,100}:Color{43,50,61,140});
            }
            const double reveal=pane_reveals_[placement.pane].value(now);
            if (placement.pane == workspace().active_pane()) rectangle({header.x + 12,header.y + 10,2,8}, accent);
            label({header.x + 20,header.y + 6,std::max(0.0,header.width - 56),18}, pane_title(placement.pane), opacity(placement.pane == workspace().active_pane() ? text : secondary,reveal), 13);
            label({header.x + header.width - 28,header.y + 3,24,22}, "···", secondary, 16,false,ChromeTextAlignment::center);
        }
    }
    for (const auto& divider : layout_.dividers) rectangle(divider.bounds, drag_node_ == divider.node ? accent : blend(seam,accent,hover_amount((1ull<<63)|divider.node.value)));
    if (!fullscreen_) {
        if (backdrop_) chrome.a = 230;
        rectangle(chrome_.band, chrome, 0, {}, 0, true);
        rectangle(chrome_.tabs_band,chrome,0,{},0,true);
        rectangle({0,chrome_.band.height-1/scale_,width_,1/scale_},seam);
        rectangle({chrome_.tabs_band.x,chrome_.tabs_band.y+chrome_.tabs_band.height-1/scale_,chrome_.tabs_band.width,1/scale_},seam);
        Rect app_title=chrome_.drag;
        if (zoom_return_.width>0) app_title.width=std::max(0.0,zoom_return_.x-app_title.x-8);
        label(app_title,"StormTTY",secondary,12,false,ChromeTextAlignment::center);
        auto button = [&](Rect bounds, std::uint64_t id, bool close = false) {
            const auto amount=hover_amount(id);
            if (amount>0) rectangle(bounds, opacity(close ? Color{196,43,28,255} : raised,amount), close ? 0 : 5);
        };
        button(chrome_.menu,1); button(chrome_.new_tab,2); button(chrome_.palette,3);
        if (zoom_return_.width) {
            button(zoom_return_,7);
            label({zoom_return_.x+8,zoom_return_.y+5,zoom_return_.width-16,20},"Return to split",accent,12,false,ChromeTextAlignment::center);
        }
        for (const auto& tab : chrome_.tabs) {
            const bool selected = tab.tab == workspace().active_tab();
            const double settle=tab_transitions_[tab.tab].value(now), hovered=hover_amount(16+tab.tab.value*2);
            if (settle>0 || hovered>0) rectangle(tab.bounds, opacity(blend(raised,(daylight||high_contrast_)?background:Color{32,38,51,255},settle),std::max(settle,hovered*.65)),6,
                opacity(high_contrast_?accent:Color{76,86,106,180},settle),1/scale_);
            if (selected) rectangle({tab.bounds.x+3,tab.bounds.y, std::max(0.0,tab.bounds.width-6),2/scale_},accent,1/scale_);
            label({tab.bounds.x+10,tab.bounds.y+6,std::max(0.0,tab.bounds.width-40),20},tab_title(tab.tab),selected?text:secondary,13,selected);
            if (chrome_.compact) label({tab.bounds.x+std::max(0.0,tab.bounds.width-28),tab.bounds.y+6,std::min(24.0,tab.bounds.width),20},"▾",text,13,false,ChromeTextAlignment::center);
            if (!chrome_.compact && (selected || hovered>0)) {
                const double x = tab.close.x + tab.close.width / 2, y = tab.close.y + tab.close.height / 2;
                if (const auto amount=hover_amount(17+tab.tab.value*2); amount>0) rectangle(tab.close,opacity(chrome,amount),4);
                line(x - 3,y - 3,x + 3,y + 3,secondary); line(x + 3,y - 3,x - 3,y + 3,secondary);
            }
        }
        for (int row = 0; row < 3; ++row) line(chrome_.menu.x + 8,chrome_.menu.y + 10 + row * 5,chrome_.menu.x + 20,chrome_.menu.y + 10 + row * 5,secondary,1.2);
        const double nx = chrome_.new_tab.x + chrome_.new_tab.width/2, ny = chrome_.new_tab.y + chrome_.new_tab.height/2;
        if (chrome_.new_tab.width>0) { line(nx-5,ny,nx+5,ny,secondary,1.3); line(nx,ny-5,nx,ny+5,secondary,1.3); }
        const double px = chrome_.palette.x + 8, py = chrome_.palette.y + 10;
        for (int y=0;y<2;++y) for (int x=0;x<2;++x) rectangle({px+x*7,py+y*7,4,4},secondary,1);
        button(chrome_.minimize,4); button(chrome_.maximize,5); button(chrome_.close,6,true);
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
    if (navigation_.footer.height>0) {
        Color footer=chrome; if (backdrop_) footer.a=230;
        rectangle(navigation_.footer,footer,0,{},0,true);
        rectangle({0,navigation_.footer.y,width_,1/scale_},seam);
        if (!projects_.active()->parked_sessions.empty()) {
            const auto footer_layout=background_layout();
            label(footer_layout.label,"Background",secondary,11);
            for (std::size_t i=0;i<footer_layout.chips.size();++i) {
                const auto bounds=footer_layout.chips[i]; const auto& parked=projects_.active()->parked_sessions[i];
                rectangle(bounds,raised,4);
                const auto found=sessions_.find(parked.session);
                const auto info=found!=sessions_.end()?found->second->info():SessionInfo{};
                const std::string status=info.exit_code?"exit "+std::to_string(*info.exit_code):info.state==SessionState::failed?"failed":info.state==SessionState::starting?"starting":info.state==SessionState::closing?"closing":"running";
                label({bounds.x+6,bounds.y,std::max(0.0,bounds.width-78),bounds.height},background_title(parked,false),text,11);
                label({bounds.x+bounds.width-70,bounds.y,64,bounds.height},status,info.exit_code?accent:secondary,10,false,ChromeTextAlignment::trailing);
            }
            if (footer_layout.overflow.width>0) {
                rectangle(footer_layout.overflow,raised,4);
                label(footer_layout.overflow,"+"+std::to_string(projects_.active()->parked_sessions.size()-footer_layout.chips.size())+" more",secondary,11,false,ChromeTextAlignment::center);
            }
        } else {
        std::string profile="Terminal";
        if (const auto* pane=workspace().pane(workspace().active_pane())) {
            profile=pane->profile;
            if (managed_shell_profile_ && pane->profile==managed_shell_profile_->id) profile=managed_shell_profile_->label;
            else for (const auto& candidate:config_.profiles) if (candidate.id==pane->profile) { profile=candidate.label; break; }
        }
        const double count_width=width_>=640?220:104;
        label({12,navigation_.footer.y+3,std::max(0.0,width_-count_width-36),20},"Local  ·  "+profile,secondary,11);
        std::string summary=std::to_string(sessions_.size())+(sessions_.size()==1?" session":" sessions");
        if (width_>=640 && projects_.entries().size()>1) summary+="  ·  "+std::to_string(projects_.entries().size())+" workspaces";
        if (auto* session=active_session(); width_>=640 && session) {
            const auto info=session->info();
            if (info.state==SessionState::exited) summary+="  ·  exited";
            else if (info.state==SessionState::failed) summary+="  ·  failed";
            else if (info.state==SessionState::starting) summary+="  ·  starting";
            else if (info.state==SessionState::closing) summary+="  ·  closing";
        }
        label({std::max(12.0,width_-count_width-12),navigation_.footer.y+3,std::min(count_width,std::max(0.0,width_-24)),20},summary,secondary,11,false,ChromeTextAlignment::trailing);
        }
    }
    if (workspace().state().tabs.empty()) {
        const auto bounds=empty_terminal_button(); rectangle(bounds,raised,5);
        label(bounds,"+ New terminal",text,13,false,ChromeTextAlignment::center);
    }
    z={ChromeLayer::transient};
    if (drop_preview_) {
        rectangle(*drop_preview_, {167,196,210,30}, 0, accent,2);
        const auto bounds=*drop_preview_; const auto tag_width=std::min(132.0,std::max(0.0,bounds.width-16));
        const Rect tag{bounds.x+(bounds.width-tag_width)/2,bounds.y+std::max(0.0,(bounds.height-28)/2),tag_width,28};
        rectangle(tag,raised,4);
        label({tag.x+8,tag.y+5,std::max(0.0,tag.width-16),20},drop_direction_?"Move pane here":"Swap panes",text,12,false,ChromeTextAlignment::center);
    }
    if (drag_ == Drag::tab && drag_started_) {
        for (const auto& tab : chrome_.tabs) if (tab.bounds.contains(hover_)) rectangle({tab.bounds.x,tab.bounds.y,2,tab.bounds.height},accent);
    }
    if (!preedit_.empty() && composing_) {
        for (const auto& placement : layout_.panes) if (placement.pane == workspace().active_pane()) {
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
    z={ChromeLayer::toast};
    if (!notice_.empty()) {
        rectangle({12,std::max(44.0,navigation_.footer.y-56),std::max(0.0,width_-24),44},raised,6);
        label({24,std::max(49.0,navigation_.footer.y-51),std::max(0.0,width_-64),34},notice_,text,12);
    }
    z={ChromeLayer::palette}; // Cached closing primitives/text retain this same surface.
    const auto palette_primitive_start=result.primitives.size(), palette_text_start=result.texts.size();
    if (palette_open_ || rename_open_) {
        const double w = std::min(560.0,std::max(0.0,width_-32)), x = (width_-w)/2, y=std::min(72.0,std::max(0.0,height_-80));
        const auto commands = palette_commands();
        const std::size_t first = palette_index_ >= 8 ? palette_index_-7 : 0;
        const auto rows = rename_open_ ? std::size_t{0} : std::min<std::size_t>(8,commands.size()-std::min(first,commands.size()));
        rectangle({x,y,w,64+rows*44.0},raised,8,seam,1);
        label({x+16,y+12,w-32,22},query_.empty() ? (rename_open_ ? "Type a tab name; Enter saves, Escape cancels" : "Search commands…   Alt+Enter to set shortcut") : query_, query_.empty()?secondary:text,14);
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
    if (palette_open_ || rename_open_) {
        closing_palette_primitives_.assign(result.primitives.begin()+palette_primitive_start,result.primitives.end());
        closing_palette_texts_.assign(result.texts.begin()+palette_text_start,result.texts.end());
    } else if (palette_transition_.value(now)>0) {
        result.primitives.insert(result.primitives.end(),closing_palette_primitives_.begin(),closing_palette_primitives_.end());
        result.texts.insert(result.texts.end(),closing_palette_texts_.begin(),closing_palette_texts_.end());
    } else { closing_palette_primitives_.clear(); closing_palette_texts_.clear(); }
    const double reveal=palette_transition_.value(now), offset=(1-reveal)*3*scale_;
    for (auto i=palette_primitive_start;i<result.primitives.size();++i) {
        auto& item=result.primitives[i]; item.rect.y+=static_cast<float>(offset); item.fill=opacity(item.fill,reveal); item.stroke=opacity(item.stroke,reveal);
    }
    for (auto i=palette_text_start;i<result.texts.size();++i) {
        auto& item=result.texts[i]; item.rect.y+=static_cast<float>(offset); item.color=opacity(item.color,reveal);
    }
    if (modal_) {
        const auto box=modal_layout();
        z={ChromeLayer::modal};
        rectangle({0,chrome_.band.height,width_,std::max(0.0,height_-chrome_.band.height)},high_contrast_?Color{0,0,0,180}:Color{5,8,13,128});
        rectangle({box.panel.x+2,box.panel.y+5,box.panel.width,box.panel.height},{0,0,0,90},10);
        rectangle(box.panel,high_contrast_?background:Color{24,29,36,255},9,high_contrast_?accent:Color{67,78,99,255},1/scale_);
        label(box.title,modal_->title,text,18,true);
        if (box.body.height<modal_->body.size()*22 && !modal_->body.empty()) label({box.body.x,box.body.y,box.body.width,20},modal_->body.back(),secondary,13);
        else for (std::size_t i=0;i<modal_->body.size();++i) label({box.body.x,box.body.y+i*22,box.body.width,20},modal_->body[i],secondary,13);
        const auto paint_button=[&](Rect bounds,std::string title,bool confirm) {
            const bool focused=modal_focus_.confirm_focused()==confirm;
            Color fill=confirm?(modal_->destructive?Color{103,45,55,255}:Color{45,61,97,255}):raised;
            if (high_contrast_) fill=background;
            if (bounds.contains(hover_) && !high_contrast_) fill=blend(fill,Color{80,92,120,255},.25);
            rectangle(bounds,fill,5,focused?accent:Color{65,75,94,255},focused?2/scale_:1/scale_);
            label({bounds.x+12,bounds.y+7,std::max(0.0,bounds.width-24),20},std::move(title),text,13,focused,ChromeTextAlignment::center);
        };
        paint_button(box.cancel,"Cancel",false); paint_button(box.confirm,modal_->confirm_label,true);
    }
    if (settings_loading()) {
        const Rect panel{std::max(16.0,(width_-420)/2),std::max(16.0,(height_-112)/2),std::max(0.0,std::min(420.0,width_-32)),112};
        z={ChromeLayer::settings};
        rectangle({0,0,width_,height_},high_contrast_?background:Color{4,6,10,148});
        rectangle(panel,high_contrast_?background:Color{24,29,36,255},9,high_contrast_?accent:Color{67,78,99,255},1/scale_);
        label({panel.x+24,panel.y+20,panel.width-48,26},"Opening Settings",text,19,true);
        label({panel.x+24,panel.y+58,panel.width-48,32},"Reading local settings and installed shells. Escape cancels.",secondary,12);
    }
    settings_panel_.append_scene(result,static_cast<float>(scale_));
    context_menu_.append_scene(result,static_cast<float>(scale_)); // Menu depth determines stacking, not call order.
    return result;
}
void NativeWindow::render() {
    if (!renderer_ || minimized_) return;
    if (renderer_call_) { defer_layout(); return; }
    RenderResult result;const auto rendered=scene();
    { RendererCallGuard guard(renderer_call_); result=renderer_->render(rendered); }
    if (result==RenderResult::presented) force_redraw_=false;
    render_busy_ = result == RenderResult::busy;
    if (!render_busy_) dirty_ = false;
    observe_material_renderer();
    if (result==RenderResult::presented || result==RenderResult::unchanged) begin_material_probe(rendered);
    if (options_.smoke_test) smoke_progress(result==RenderResult::presented);
    // Occlusion is retried on actual exposure/activation/output, not a timer.
}

void NativeWindow::terminal_text(std::string text, Key key, std::uint16_t modifiers) {
    if (modal_ || settings_input_blocked() || settings_loading() || text.empty()) return;
    if (context_menu_.is_open()) { context_menu_.text(text,GetTickCount64()); menu_input_changed(); return; }
    if (palette_open_ || rename_open_) {
        if (query_.size() + text.size() <= 1024) query_ += text;
        palette_index_ = 0; changed(); return;
    }
    const auto* pane = workspace().pane(workspace().active_pane());
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
    caret_schedule(true);
}
bool NativeWindow::key_message(MSG& message) {
    const bool down = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
    const bool up = message.message == WM_KEYUP || message.message == WM_SYSKEYUP;
    if (!down && !up) return false;
    const auto vk = message.wParam;
    const auto modifiers = terminal_modifiers();
    const bool altgr = (modifiers & Modifier::right_alt) && (modifiers & Modifier::control);
    // Our renderer owns the system menu too; Windows still executes actual
    // move/size/minimize/maximize system commands after explicit selection.
    if ((modifiers&Modifier::alt) && !altgr && vk==VK_SPACE && !(modifiers&Modifier::control)) {
        if (down) {
            if (!modal_ && !settings_input_blocked()) system_menu({chrome_.menu.x,chrome_.band.height},true);
            if (vk<consumed_keys_.size()) consumed_keys_[vk]=true;
            consumed_keys_[VK_MENU]=true;
        }
        return true;
    }
    // Preserve other OS-reserved shortcuts before application or terminal routing.
    if ((modifiers & Modifier::super) || ((modifiers & Modifier::alt) && !altgr && (vk == VK_F4 || vk == VK_SPACE))) return false;
    if (down && vk==VK_ESCAPE && settings_job_ && settings_open_requested_ && !settings_input_blocked()) {
        settings_open_requested_=false; settings_binding_.clear(); notice_.clear(); caret_schedule(true); changed();
        if (vk<consumed_keys_.size()) consumed_keys_[vk]=true; return true;
    }
    if (up && vk < consumed_keys_.size() && consumed_keys_[vk]) { consumed_keys_[vk] = false; return true; }
    if (overlay_open()) {
        if (settings_loading()) { if (down && vk<consumed_keys_.size()) consumed_keys_[vk]=true; return true; }
        CommandModifier app=CommandModifier::none;
        if (modifiers&Modifier::control) app=app|CommandModifier::control;
        if (modifiers&Modifier::shift) app=app|CommandModifier::shift;
        if (modifiers&Modifier::alt) app=app|CommandModifier::alt;
        if (down && vk<consumed_keys_.size()) consumed_keys_[vk]=true;
        const KeyEvent event{{chord_key(vk),app},composing_,altgr,down};
        if (settings_input_blocked()) { settings_panel_.key(event); settings_input_changed(); return true; }
        context_menu_.key(event); menu_input_changed();
        // Navigation was handled, but committed layout text still takes the
        // TranslateMessage path for Unicode typeahead (never the terminal).
        if (down && context_menu_.is_open() && !nontext_key(vk) && vk!=VK_SPACE && !(modifiers&(Modifier::control|Modifier::alt|Modifier::super))) {
            text_vk_=vk; text_flags_=message.lParam; text_modifiers_=modifiers; return false;
        }
        return true;
    }
    if (modal_) {
        if (down) {
            if (vk<consumed_keys_.size()) consumed_keys_[vk]=true;
            if (vk==VK_ESCAPE) answer_modal(ModalDecision::cancel);
            else if (vk==VK_TAB || vk==VK_LEFT || vk==VK_RIGHT) { modal_focus_.next(); changed(); }
            else if (vk==VK_RETURN || vk==VK_SPACE) answer_modal(modal_focus_.activate());
        }
        return true; // All terminal/palette chords and key-up are trapped.
    }
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
            if (palette_open_ && (modifiers&Modifier::alt) && !altgr) {
                const auto commands=palette_commands();
                if (!commands.empty()) open_settings(std::string(commands[std::min(palette_index_,commands.size()-1)]->id));
                if (vk<consumed_keys_.size()) consumed_keys_[vk]=true;
                return true;
            }
            if (rename_open_) { workspace().rename_tab(rename_tab_,query_); rename_open_ = false; refresh_tooltips(); changed(); }
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
    const auto* pane = workspace().pane(workspace().active_pane());
    if (!pane || !sessions_.contains(pane->session)) return false;
    const Key key = physical_key(vk,message.lParam);
    if (key == Key::unidentified) return false;
    if (up || nontext_key(vk)) {
        auto session = sessions_.at(pane->session);
        KeyInput input;
        input.key = key; input.modifiers = modifiers;
        input.action = up ? KeyAction::release : (message.lParam & (1LL << 30) ? KeyAction::repeat : KeyAction::press);
        submit([session,input] { return session->key(input); });
        if (down) caret_schedule(true);
        return true; // Do not also manufacture WM_CHAR for Enter/Tab/Backspace.
    }
    text_vk_ = vk; text_flags_ = message.lParam; text_modifiers_ = modifiers;
    return false;
}
void NativeWindow::copy() {
    const auto* pane = workspace().pane(workspace().active_pane());
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
    const auto* pane=workspace().pane(workspace().active_pane());
    if (!pane || !sessions_.contains(pane->session)) return;
    auto session=sessions_.at(pane->session); const auto size=text.size();
    auto send=[this,session,text=std::move(text),unsafe,size] {
        if (session->process_finished()) { show_error("The paste destination has closed; nothing was sent."); return; }
        submit([session,text,unsafe] { return session->paste(text,unsafe); },size);
    };
    if (unsafe) show_modal("Review paste",{"This paste contains multiple lines","or terminal control characters.","It may execute shell commands."},"Paste anyway",std::move(send),false);
    else send();
}
void NativeWindow::terminal_mouse(Point point, MouseAction action, MouseButton button, bool pressed) {
    if (modal_ || overlay_open()) return;
    for (const auto& placement:layout_.panes) {
        const bool captured=mouse_capture_pane_ && placement.pane==mouse_capture_pane_;
        if (mouse_capture_pane_ ? !captured : !placement.bounds.contains(point)) continue;
        const auto content=content_bounds(placement);
        const auto* pane=workspace().pane(placement.pane);
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
void NativeWindow::open_menu(std::vector<ContextMenuItem> items,Point anchor,std::map<std::string,std::function<void()>> actions,bool keyboard) {
    if (modal_ || settings_input_blocked() || settings_loading()) return;
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    cancel_drag(); palette_open_=rename_open_=false; query_.clear(); preedit_.clear(); composing_=false; text_input_.reset();
    palette_transition_=ChromeTransition{}; closing_palette_primitives_.clear(); closing_palette_texts_.clear();
    if (ImmContext context(hwnd_); context.context) ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
    context_menu_.resize({width_,height_});
    if (!context_menu_.open(items,anchor,keyboard)) { menu_actions_.clear(); show_error("There are no available menu destinations."); return; }
    menu_actions_=std::move(actions);
    if (tooltip_) SendMessageW(tooltip_,TTM_POP,0,0);
    caret_schedule(); changed();
}
void NativeWindow::menu_input_changed() {
    if (auto action=context_menu_.take_action()) {
        auto callbacks=std::move(menu_actions_); menu_actions_.clear(); text_input_.reset();
        if (action->kind==ContextMenuActionKind::selected) if (const auto found=callbacks.find(action->id); found!=callbacks.end()) found->second();
        caret_schedule(true);
    }
    if (menu_seen_generation_!=context_menu_.generation()) {
        menu_seen_generation_=context_menu_.generation(); changed();
    }
}
void NativeWindow::system_menu(Point anchor,bool keyboard) {
    std::vector<ContextMenuItem> items;
    std::map<std::string,std::function<void()>> actions;
    auto item=[&](std::string id,std::string label,WPARAM command,bool enabled,bool destructive=false,std::string shortcut={}) {
        items.push_back({id,std::move(label),std::move(shortcut),enabled?"":"Unavailable in the current window state",enabled,destructive,{}});
        actions[id]=[this,command] { if (hwnd_) PostMessageW(hwnd_,WM_SYSCOMMAND,command,0); };
    };
    item("window.restore","Restore",SC_RESTORE,maximized_ && !fullscreen_);
    item("window.move","Move",SC_MOVE,!maximized_ && !fullscreen_);
    item("window.size","Resize",SC_SIZE,!maximized_ && !fullscreen_);
    items.push_back(ContextMenuItem::separator());
    item("window.minimize","Minimize",SC_MINIMIZE,true);
    item("window.maximize","Maximize",SC_MAXIMIZE,!maximized_ && !fullscreen_);
    items.push_back(ContextMenuItem::separator());
    item("window.close","Close window",SC_CLOSE,true,true,"Alt+F4");
    open_menu(std::move(items),anchor,std::move(actions),keyboard);
}
void NativeWindow::popup_menu(Point point,bool pane_actions) {
    if (modal_ || settings_input_blocked() || settings_loading()) return;
    if (navigation_.footer.contains(point) && !projects_.active()->parked_sessions.empty()) {
        const auto footer=background_layout();
        for (std::size_t i=0;i<footer.chips.size();++i) if (footer.chips[i].contains(point)) { background_menu(point,projects_.active()->parked_sessions[i].session); return; }
        background_menu(point); return;
    }
    const auto context=command_context();
    const auto project=projects_.active_id(); const auto pane=workspace().active_pane(); const auto tab=workspace().active_tab();
    const auto* model=workspace().pane(pane); const auto session=model?model->session:SessionId{};
    std::map<std::string,std::function<void()>> actions;
    auto command_item=[&](std::string_view id) {
        ContextMenuItem item; item.id=id;
        if (const auto* command=find_command(id)) item.label=command->label;
        const auto available=host_availability(id,context); item.enabled=available.enabled; item.disabled_reason=available.reason;
        item.destructive=id=="pane.close" || id=="tab.close" || id=="workspace.close";
        for (const auto& binding:config_.bindings) if (binding.command==id) { item.shortcut=format_key_chord(binding.chord,Platform::windows); break; }
        actions[item.id]=[this,id=std::string(id),project,pane,tab,session] {
            const auto* current=projects_.workspace(project); const auto* target=current?current->pane(pane):nullptr;
            if (!current || projects_.active_id()!=project || current->active_tab()!=tab || current->active_pane()!=pane || (session ? (!target || target->session!=session) : target!=nullptr)) {
                show_error("The menu target changed; nothing was changed."); return;
            }
            dispatch(id);
        };
        return item;
    };
    auto group=[&](std::string id,std::string label,std::initializer_list<std::string_view> commands) {
        ContextMenuItem item; item.id=std::move(id); item.label=std::move(label);
        for (auto command:commands) item.children.push_back(command_item(command));
        return item;
    };
    std::vector<ContextMenuItem> items;
    if (pane_actions) {
        for (auto command:{"edit.copy","edit.paste","pane.split_right","pane.split_down","pane.zoom","pane.new_tab","pane.background","pane.swap","pane.move_to_tab"}) items.push_back(command_item(command));
        items.push_back(ContextMenuItem::separator()); items.push_back(command_item("pane.close"));
    } else {
        items.push_back(command_item("tab.new"));
        items.push_back(group("group.workspace","Workspaces",{"workspace.new","workspace.next","workspace.previous","workspace.toggle_navigation","workspace.close"}));
        items.push_back(group("group.tab","Task tabs",{"tab.rename","tab.next","tab.previous","tab.move_left","tab.move_right","tab.close"}));
        items.push_back(group("group.pane","Panes",{"pane.split_right","pane.split_down","pane.zoom","pane.new_tab","pane.background","pane.swap","pane.move_to_tab","pane.close"}));
        items.push_back(ContextMenuItem::separator());
        for (auto command:{"app.command_palette","settings.open","window.focus_mode","window.fullscreen"}) items.push_back(command_item(command));
    }
    open_menu(std::move(items),point,std::move(actions));
}
void NativeWindow::pointer_down(Point point, bool double_click) {
    SetFocus(hwnd_);
    if (overlay_open()) {
        overlay_pointer_sequence_=true; SetCapture(hwnd_);
        if (settings_input_blocked()) { settings_panel_.pointer_down(point); settings_input_changed(); }
        else { context_menu_.pointer_down(point); menu_input_changed(); }
        return;
    }
    if (modal_) {
        const auto layout=modal_layout(); modal_pressed_.reset();
        if (layout.cancel.contains(point)) modal_pressed_=ModalDecision::cancel;
        else if (layout.confirm.contains(point)) modal_pressed_=ModalDecision::confirm;
        if (modal_pressed_) { modal_focus_.select(*modal_pressed_); SetCapture(hwnd_); }
        changed(); return;
    }
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
    if (chrome_.menu.contains(point)) { popup_menu({chrome_.menu.x,chrome_.band.height}); return; }
    if (chrome_.new_tab.contains(point)) { if (GetKeyState(VK_SHIFT)&0x8000) popup_menu({chrome_.new_tab.x,chrome_.new_tab.y+chrome_.new_tab.height}); else new_tab(); return; }
    if (chrome_.palette.contains(point)) { toggle_palette(); return; }
    if (navigation_.toggle.contains(point)) { dispatch("workspace.toggle_navigation"); return; }
    if (navigation_.new_project.contains(point)) { dispatch("workspace.new"); return; }
    if (navigation_.settings.contains(point)) { dispatch("settings.open"); return; }
    if (navigation_.focus.contains(point)) { dispatch("window.focus_mode"); return; }
    for (const auto& row:navigation_.projects) if (row.bounds.contains(point)) { activate_project(row.project); return; }
    if (navigation_.footer.contains(point)) {
        const auto footer=background_layout();
        for (std::size_t i=0;i<footer.chips.size();++i) if (footer.chips[i].contains(point)) {
            restore_parked({projects_.active_id(),projects_.active()->parked_sessions[i].session}); return;
        }
        if (footer.overflow.contains(point)) background_menu(point);
        return;
    }
    if (workspace().state().tabs.empty() && empty_terminal_button().contains(point)) { new_tab(); return; }
    if (navigation_.rail.contains(point) || navigation_.context.contains(point) || navigation_.footer.contains(point)) return;
    for (const auto& tab:chrome_.tabs) if (tab.bounds.contains(point)) {
        if (chrome_.compact) {
            std::vector<ContextMenuItem> items; std::map<std::string,std::function<void()>> actions;
            const auto project=projects_.active_id();
            for (const auto& item:workspace().state().tabs) {
                const auto id="tab:"+std::to_string(item.id.value); const auto destination=item.id;
                items.push_back({id,tab_title(item.id),item.id==workspace().active_tab()?"Current":"",{},true,false,{}});
                actions[id]=[this,project,destination] {
                    const auto* model=projects_.workspace(project);
                    if (!model || projects_.active_id()!=project || !model->tab(destination)) { show_error("The task tab no longer exists."); return; }
                    activate(model->tab(destination)->active_pane);
                };
            }
            open_menu(std::move(items),{tab.bounds.x,tab.bounds.y+tab.bounds.height},std::move(actions),true);
            return;
        }
        if (tab.close.contains(point)) { close_tab(tab.tab); return; }
        if (double_click) { activate(workspace().tab(tab.tab)->active_pane); dispatch("tab.rename"); return; }
        activate(workspace().tab(tab.tab)->active_pane);
        drag_=Drag::tab; drag_tab_=tab.tab; drag_origin_=point; SetCapture(hwnd_); return;
    }
    for (const auto& divider:layout_.dividers) if (divider.hit_bounds.contains(point)) {
        if (double_click) { workspace().equalize_split(divider.node); changed(true); return; }
        drag_=Drag::divider; drag_node_=divider.node; drag_divider_=divider; drag_origin_=point; SetCapture(hwnd_); return;
    }
    for (const auto& placement:layout_.panes) if (placement.bounds.contains(point)) {
        const auto pane=placement.pane;
        const auto bounds=placement.bounds;
        activate(pane);
        if (layout_.panes.size()>1 && point.y<bounds.y+28) {
            if (point.x>=bounds.x+bounds.width-32) { popup_menu(point,true); return; }
            if (double_click) { workspace().toggle_zoom(); changed(true); return; }
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
            const auto* model=workspace().pane(pane); if (model && sessions_.contains(model->session)) { auto owned=sessions_.at(model->session); submit([owned] { return owned->clear_selection(); }); }
            return;
        }
    }
}
void NativeWindow::pointer_move(Point point) {
    if (settings_loading()) return;
    if (settings_input_blocked()) { settings_panel_.pointer_move(point); settings_input_changed(); return; }
    if (context_menu_.is_open()) { context_menu_.pointer_move(point); menu_input_changed(); return; }
    if (modal_) { const auto old=hover_; hover_=point; const auto layout=modal_layout(); if (layout.cancel.contains(old)!=layout.cancel.contains(point) || layout.confirm.contains(old)!=layout.confirm.contains(point)) changed(); return; }
    hover_changed(point);
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
        if (extent>gap) workspace().resize_split(drag_node_,(position-gap/2)/(extent-gap),workspace_viewport(),{1/scale_,7,80,48});
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
            const auto* model=workspace().pane(pane.pane); if (!model || !sessions_.contains(model->session)) break;
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
    if (drag_==Drag::tab) changed();
}
void NativeWindow::pointer_up(Point point) {
    if (overlay_pointer_sequence_ || overlay_open()) {
        overlay_pointer_sequence_=false;
        const bool settings=settings_input_blocked();
        if (settings) settings_panel_.pointer_up(point);
        else if (context_menu_.is_open()) context_menu_.pointer_up(point);
        if (GetCapture()==hwnd_) ReleaseCapture();
        if (settings) settings_input_changed(); else menu_input_changed(); return;
    }
    if (caption_pressed_) {
        const auto pressed=caption_pressed_; caption_pressed_=0; if (GetCapture()==hwnd_) ReleaseCapture();
        const bool hit=pressed==HTMINBUTTON?chrome_.minimize.contains(point):pressed==HTMAXBUTTON?chrome_.maximize.contains(point):chrome_.close.contains(point);
        if (hit) PostMessageW(hwnd_,WM_SYSCOMMAND,pressed==HTMINBUTTON?SC_MINIMIZE:pressed==HTMAXBUTTON?(maximized_?SC_RESTORE:SC_MAXIMIZE):SC_CLOSE,0);
        changed(); return;
    }
    if (modal_) {
        const auto pressed=modal_pressed_; modal_pressed_.reset(); if (GetCapture()==hwnd_) ReleaseCapture(); const auto layout=modal_layout();
        if (pressed && (*pressed==ModalDecision::cancel?layout.cancel:layout.confirm).contains(point)) answer_modal(*pressed);
        return;
    }
    if (drag_==Drag::pane && drag_started_ && drop_target_) {
        const auto status=drop_direction_?workspace().move_pane(drag_pane_,drop_target_,*drop_direction_):workspace().swap_panes(drag_pane_,drop_target_);
        if (!status) show_error(status.error);
    } else if (drag_==Drag::tab && drag_started_) {
        // Cross-window tear-off is intentionally not faked. A drag outside the
        // band cancels and retains the one existing owner/session.
        if (chrome_.tabs_band.contains(point)) for (std::size_t i=0;i<chrome_.tabs.size();++i) if (chrome_.tabs[i].bounds.contains(point)) { workspace().reorder_tab(drag_tab_,i); break; }
    } else if (drag_==Drag::none) terminal_mouse(point,MouseAction::release,MouseButton::left,false);
    cancel_drag(); changed(true);
}
void NativeWindow::cancel_drag() {
    drag_=Drag::none; drag_started_=false; drag_node_={}; drag_tab_={}; drag_pane_={}; drop_target_={}; drop_preview_.reset(); drop_direction_.reset();
    if (GetCapture()==hwnd_) ReleaseCapture();
    changed();
}

void NativeWindow::capture_frame() {
    if (options_.capture_path.empty()) return;
    auto pixels=renderer_->read_pixels();
    RECT bounds{}; GetClientRect(hwnd_,&bounds);
    const auto width=static_cast<UINT>(bounds.right),height=static_cast<UINT>(bounds.bottom);
    if (!width || !height || pixels.size()!=static_cast<std::size_t>(width)*height*4) throw std::runtime_error("Native capture returned invalid framebuffer dimensions.");
    auto check=[](HRESULT result) { if (FAILED(result)) throw std::runtime_error("Could not encode the native CI framebuffer PNG."); };
    ComObject<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IWICImagingFactory),reinterpret_cast<void**>(factory.put())));
    ComObject<IWICStream> stream;
    check(factory->CreateStream(stream.put()));
    check(stream->InitializeFromFilename(options_.capture_path.c_str(),GENERIC_WRITE));
    ComObject<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()));
    check(encoder->Initialize(stream.value,WICBitmapEncoderNoCache));
    ComObject<IWICBitmapFrameEncode> frame;
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
bool frame_has_smoke_marker(const FramePtr& frame) {
    if (!frame) return false;
    for (const auto& row:frame->lines) if (row) {
        std::string line; for (const auto& cell:row->cells) line+=cell.grapheme;
        while (!line.empty() && line.back()==' ') line.pop_back();
        if (line==smoke_marker) return true;
    }
    return false;
}
bool NativeWindow::smoke_frame_ready() const {
    if (layout_.panes.empty()) return false;
    for (const auto& placement:layout_.panes) {
        auto* session=session_for(placement.pane);
        if (!session || !frame_has_smoke_marker(session->frame())) return false;
    }
    return true;
}
void NativeWindow::smoke_settings_stage(unsigned stage) {
    smoke_settings_stage_=stage;
    if (options_.smoke_settings) std::fprintf(stderr,"Native CI settings stage=%u elapsed_ms=%llu job_pending=%d panel_open=%d saving=%d\n",
        stage,GetTickCount64()-smoke_started_at_,settings_job_,settings_panel_.is_open(),settings_panel_.saving());
}
void NativeWindow::smoke_progress(bool presented) {
    if (!options_.smoke_test || smoke_finished_) return;
    if (sessions_.empty()) { smoke_finish("Native smoke has no real session."); return; }
    for (const auto& [id,session]:sessions_) { (void)id; const auto info=session->info(); if (info.state==SessionState::failed) { smoke_finish(info.error.empty()?"Native session failed.":info.error); return; } }
    if (!smoke_chrome_verified_) {
        const auto native_hit=[&](Rect bounds) {
            POINT p{static_cast<LONG>(std::lround((bounds.x+bounds.width/2)*scale_)),static_cast<LONG>(std::lround((bounds.y+bounds.height/2)*scale_))};
            ClientToScreen(hwnd_,&p);
            return SendMessageW(hwnd_,WM_NCHITTEST,0,MAKELPARAM(p.x,p.y));
        };
        if (chrome_.band.height!=32 || chrome_.tabs_band.x!=navigation_.rail.width ||
            chrome_.tabs_band.y!=chrome_.band.height || navigation_.viewport.y+navigation_.viewport.height!=navigation_.footer.y ||
            native_hit(chrome_.maximize)!=HTMAXBUTTON || native_hit(chrome_.drag)!=HTCAPTION ||
            chrome_.tabs.empty() || native_hit(chrome_.tabs.front().bounds)!=HTCLIENT || native_hit(navigation_.footer)!=HTCLIENT) {
            smoke_finish("Workspace hierarchy or native caption/task/footer hit-test invariant failed."); return;
        }
        smoke_chrome_verified_=true;
        std::fprintf(stderr,"Native hierarchy smoke: caption/tab/footer routing verified; synthetic hit tests are not desktop Snap visual verification.\n");
    }
    if (options_.smoke_settings && smoke_settings_stage_<8) {
        const bool switch_console=options_.console_runtime_directory.empty() && bundled_console_runtime_present();
        auto* active=active_session(); const auto info=active?active->info():SessionInfo{};
        const auto activate_setting=[&](std::string_view control) {
            for (unsigned i=0;i<256 && settings_panel_.focused_control()!=control;++i) settings_panel_.key({{"TAB"}});
            if (settings_panel_.focused_control()!=control) return false;
            settings_panel_.key({{"ENTER"}}); settings_input_changed(); return true;
        };
        const auto actor_has_theme=[&](const Theme& theme) {
            const auto frame=smoke_settings_actor_?smoke_settings_actor_->frame():FramePtr{};
            return frame && frame->foreground==color_hex(theme.foreground,{}) && frame->background==color_hex(theme.background,{});
        };
        if (smoke_settings_stage_==0) {
            if (info.state!=SessionState::running || !info.bytes_received) return;
            smoke_settings_original_=config_; smoke_settings_actor_=sessions_.at(workspace().pane(workspace().active_pane())->session);
            open_settings(); smoke_settings_stage(1); return;
        }
        if (smoke_settings_stage_==1) {
            if (settings_job_) return;
            if (!settings_panel_.is_open() || focused_session_ || settings_panel_.draft()!=smoke_settings_original_) { smoke_finish("Settings did not open from its owned snapshot and remove terminal focus."); return; }
            MSG key{}; key.hwnd=hwnd_; key.message=WM_KEYDOWN; key.wParam='Q';
            if (!key_message(key)) { smoke_finish("Settings printable key escaped input trapping."); return; }
            terminal_text("SETTINGS_INPUT_MUST_NOT_REACH_SHELL");
            const auto& controls=settings_panel_.layout().controls;
            const auto found=std::find_if(controls.begin(),controls.end(),[](const auto& control){return control.id=="font.increase" && control.visible && control.enabled;});
            if (found==controls.end()) { smoke_finish("Settings font preview control is not reachable."); return; }
            const Point point{found->bounds.x+found->bounds.width/2,found->bounds.y+found->bounds.height/2};
            pointer_down(point,false); pointer_up(point);
            if (config_.font.size_dip!=smoke_settings_original_.font.size_dip+1 || config_.default_profile!=smoke_settings_original_.default_profile) { smoke_finish("Settings preview did not apply only reversible presentation."); return; }
            if (!activate_setting("appearance.theme.daylight") || config_.theme!=*find_builtin_theme("daylight")) {
                smoke_finish("Named theme preview did not update the live host palette."); return;
            }
            if (switch_console) {
                if (!activate_setting("nav.shell") || !activate_setting("console.bundled") ||
                    settings_panel_.draft().windows_console_backend!=WindowsConsoleBackend::bundled_preview ||
                    config_.windows_console_backend!=WindowsConsoleBackend::system || !smoke_bundled_sessions_.empty()) {
                    smoke_finish("Console preview changed a live provider before Save."); return;
                }
                if (!activate_setting("nav.appearance")) { smoke_finish("Could not return from console settings."); return; }
            }
            smoke_settings_stage(2); return;
        }
        if (smoke_settings_stage_==2) {
            if (!presented || !actor_has_theme(*find_builtin_theme("daylight"))) return;
            if (!smoke_console_capture_pending_) {
                try { capture_frame(); } catch (const std::exception& error) { smoke_finish(error.what()); return; }
                if (switch_console && !options_.capture_path.empty()) {
                    if (!activate_setting("nav.shell")) { smoke_finish("Console Settings capture could not open the owned section."); return; }
                    smoke_console_capture_pending_=true;
                    return; // Capture only after this new scene has presented.
                }
            } else {
                const auto original_path=options_.capture_path;
                options_.capture_path=original_path.parent_path()/(original_path.stem().wstring()+L"-console"+original_path.extension().wstring());
                try { capture_frame(); }
                catch (const std::exception& error) { options_.capture_path=original_path; smoke_finish(error.what()); return; }
                options_.capture_path=original_path;
            }
            MSG key{}; key.hwnd=hwnd_; key.message=WM_KEYDOWN; key.wParam=VK_ESCAPE; key_message(key);
            if (settings_panel_.is_open() || config_!=smoke_settings_original_ || !focused_session_) { smoke_finish("Settings Escape did not roll back preview and restore terminal focus."); return; }
            system_menu({12,48},true);
            if (!context_menu_.is_open() || focused_session_) { smoke_finish("Custom system menu did not open or trap focus."); return; }
            key_message(key);
            if (context_menu_.is_open() || !focused_session_) { smoke_finish("Custom system menu Escape did not restore focus."); return; }
            popup_menu({12,48});
            const Point outside{width_-2,height_-2}; pointer_down(outside,false); pointer_up(outside);
            if (context_menu_.is_open() || drag_!=Drag::none || GetCapture()==hwnd_) { smoke_finish("Menu outside click did not dismiss without starting terminal selection."); return; }
            open_settings("pane.split_right"); smoke_settings_stage(3); return;
        }
        if (smoke_settings_stage_==3) {
            if (settings_job_) return;
            if (!settings_panel_.is_open() || !settings_panel_.capturing_shortcut()) { smoke_finish("Shortcut editor did not capture its selected palette action."); return; }
            if (!actor_has_theme(smoke_settings_original_.theme)) return; // Cancel has reached the same actor.
            settings_panel_.key({{"]",CommandModifier::alt}}); settings_input_changed();
            if (!settings_panel_.capturing_shortcut() || settings_panel_.draft().allow_terminal_shortcut_override) {
                smoke_finish("Terminal shortcut bypassed explicit confirmation."); return;
            }
            settings_panel_.key({{"TAB"}}); settings_panel_.key({{"ENTER"}}); settings_input_changed();
            if (settings_panel_.capturing_shortcut() || config_.bindings!=smoke_settings_original_.bindings || sessions_.size()!=1) { smoke_finish("Shortcut capture executed an action or applied before Save."); return; }
            if (managed_shell_profile_ && !options_.shell_runtime_override) {
                if (using_managed_shell() || !activate_setting("nav.shell") || !activate_setting("shell:stormtty-managed-zsh") ||
                    !has_managed_zsh_selection(settings_panel_.draft()) || using_managed_shell()) {
                    smoke_finish("Configured Settings did not offer managed Zsh while preserving the old shell before Save."); return;
                }
            }
            if (switch_console && (!activate_setting("nav.shell") || !activate_setting("console.bundled") ||
                config_.windows_console_backend!=WindowsConsoleBackend::system || !smoke_bundled_sessions_.empty())) {
                smoke_finish("Console selection did not remain draft-only before Save."); return;
            }
            if (!activate_setting("nav.appearance") || !activate_setting("appearance.theme.instrument")) {
                smoke_finish("Named theme could not be selected before Save."); return;
            }
            for (unsigned i=0;i<256 && settings_panel_.focused_control()!="appearance.opacity";++i) settings_panel_.key({{"TAB"}});
            settings_panel_.key({{"HOME"}});
            for (unsigned i=0;i<25;++i) settings_panel_.key({{"RIGHT"}});
            settings_input_changed();
            if (settings_panel_.draft().appearance.background_opacity!=.25) {smoke_finish("Full-range opacity did not accept 25%.");return;}
            settings_panel_.key({{"ENTER",CommandModifier::control},false,false,true}); settings_input_changed();
            if (!settings_panel_.saving() || !settings_job_) { smoke_finish("Settings Save did not enter nonblocking pending state."); return; }
            message(WM_CLOSE,0,0);
            if (!hwnd_ || !settings_panel_.saving()) { smoke_finish("Pending Save allowed stale close/cancel."); return; }
            smoke_settings_stage(4); return;
        }
        if (smoke_settings_stage_==4) {
            if (settings_job_) return;
            if (settings_panel_.is_open() || !settings_snapshot_ || !settings_snapshot_->existed || !smoke_settings_file_owned_) { smoke_finish("Isolated Settings Save did not commit successfully."); return; }
            const KeyChord expected{"]",CommandModifier::alt};
            if (std::none_of(config_.bindings.begin(),config_.bindings.end(),[&](const auto& b){return b.command=="pane.split_right" && b.chord==expected;}) ||
                sessions_.size()!=1 || sessions_.begin()->second!=smoke_settings_actor_ || workspace().state().panes.size()!=1) {
                smoke_finish("Saved shortcut or original actor identity was not retained."); return;
            }
            if (!actor_has_theme(*find_builtin_theme("instrument"))) return;
            if (config_.theme!=*find_builtin_theme("instrument") || config_.appearance.background_opacity!=.25) {
                smoke_finish("Saved theme or opacity did not remain applied."); return;
            }
            if (managed_shell_profile_ && !options_.shell_runtime_override) {
                const auto saved=parse_config(settings_snapshot_->bytes,Platform::windows);
                if (!saved || !has_managed_zsh_selection(*saved.config) || !settings_baseline_matches(*saved.config,config_,Platform::windows) ||
                    smoke_managed_sessions_.contains(sessions_.begin()->first)) {
                    smoke_finish("Managed shell reference did not persist or original Cmd actor was replaced."); return;
                }
            }
            if (switch_console) {
                const auto saved=parse_config(settings_snapshot_->bytes,Platform::windows);
                if (!saved || saved.config->windows_console_backend!=WindowsConsoleBackend::bundled_preview ||
                    config_.windows_console_backend!=WindowsConsoleBackend::bundled_preview || !smoke_bundled_sessions_.empty()) {
                    smoke_finish("Bundled backend did not persist, or replaced an existing System actor."); return;
                }
            }
            if ((managed_shell_profile_ && !options_.shell_runtime_override) || switch_console) {
                new_tab();
                const auto* pane=workspace().pane(workspace().active_pane());
                if (sessions_.size()!=2 || !pane ||
                    (managed_shell_profile_ && !options_.shell_runtime_override && !smoke_managed_sessions_.contains(pane->session)) ||
                    (switch_console && !smoke_bundled_sessions_.contains(pane->session)) ||
                    std::none_of(sessions_.begin(),sessions_.end(),[&](const auto& entry){return entry.second==smoke_settings_actor_;})) {
                    smoke_finish("Saved shell/backend did not launch a new actor while retaining the original actor."); return;
                }
                std::fprintf(stderr,"Settings smoke: saved shell/backend selected for new real actor without CLI; original System actor retained.\n");
            }
            open_settings(); smoke_settings_stage(5); return;
        }
        if (smoke_settings_stage_==5) {
            if (settings_job_) return;
            if (!settings_panel_.is_open() || !settings_baseline_matches(settings_panel_.draft(),config_,Platform::windows)) {
                smoke_finish("Settings Save-to-reopen falsely rejected its canonical saved baseline."); return;
            }
            if (switch_console && (!activate_setting("nav.shell") || !activate_setting("console.system") ||
                config_.windows_console_backend!=WindowsConsoleBackend::bundled_preview)) {
                smoke_finish("System recovery choice changed provider before Save."); return;
            }
            if (!activate_setting("nav.appearance") || !activate_setting("font.increase")) {smoke_finish("Reopened Settings could not edit font size.");return;}
            settings_panel_.key({{"ENTER",CommandModifier::control}});settings_input_changed();
            if (!settings_panel_.saving() || !settings_job_) {smoke_finish("Second Save did not start.");return;}
            smoke_settings_stage(6);return;
        }
        if (smoke_settings_stage_==6) {
            if (settings_job_) return;
            if (settings_panel_.is_open()) {smoke_finish("Second Save failed.");return;}
            if (switch_console) {
                const auto saved=parse_config(settings_snapshot_->bytes,Platform::windows);
                if (!saved || saved.config->windows_console_backend!=WindowsConsoleBackend::system ||
                    config_.windows_console_backend!=WindowsConsoleBackend::system || smoke_bundled_sessions_.size()!=1 || sessions_.size()!=2) {
                    smoke_finish("System recovery did not persist or replaced an existing bundled actor."); return;
                }
                new_tab();
                const auto* pane=workspace().pane(workspace().active_pane());
                if (!pane || sessions_.size()!=3 || smoke_bundled_sessions_.contains(pane->session)) {
                    smoke_finish("Saved System choice did not create a new System actor."); return;
                }
            }
            open_settings();smoke_settings_stage(7);return;
        }
        if (smoke_settings_stage_==7) {
            if (settings_job_) return;
            if (!settings_panel_.is_open() || !settings_baseline_matches(settings_panel_.draft(),config_,Platform::windows)) {
                smoke_finish("Second Save-to-reopen rejected its saved baseline.");return;
            }
            settings_panel_.cancel(); settings_input_changed();
            if (settings_panel_.is_open() || !focused_session_) { smoke_finish("Reopened Settings did not restore terminal focus on Cancel."); return; }
            smoke_settings_stage(8);
        }
    }
    if (options_.smoke_modal && smoke_modal_stage_<2) {
        const auto verify_activation_routing=[&] {
            const auto style=GetWindowLongPtrW(hwnd_,GWL_STYLE), extended=GetWindowLongPtrW(hwnd_,GWL_EXSTYLE);
            const auto focused=focused_session_;const bool modal_open=modal_.has_value();
            RECT client{};GetClientRect(hwnd_,&client);
            // Exercise the actual WndProc's default no-paint activation path.
            // These synthetic messages do not prove desktop blur or activation.
            const auto inactive=SendMessageW(hwnd_,WM_NCACTIVATE,FALSE,0);
            SendMessageW(hwnd_,WM_NCACTIVATE,TRUE,0);
            RECT after{};GetClientRect(hwnd_,&after);
            const auto hit=[&](Rect bounds) {
                POINT p{static_cast<LONG>(std::lround((bounds.x+bounds.width/2)*scale_)),static_cast<LONG>(std::lround((bounds.y+bounds.height/2)*scale_))};
                ClientToScreen(hwnd_,&p);return SendMessageW(hwnd_,WM_NCHITTEST,0,MAKELPARAM(p.x,p.y));
            };
            constexpr LONG_PTR required=WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX;
            const bool valid=inactive!=FALSE && !(style&WS_CAPTION) && !(required&~style) &&
                (extended&WS_EX_NOREDIRECTIONBITMAP) && style==GetWindowLongPtrW(hwnd_,GWL_STYLE) &&
                extended==GetWindowLongPtrW(hwnd_,GWL_EXSTYLE) && EqualRect(&client,&after) &&
                modal_open==modal_.has_value() && focused_session_==focused &&
                hit(chrome_.maximize)==HTMAXBUTTON && hit(chrome_.drag)==HTCAPTION &&
                !chrome_.tabs.empty() && hit(chrome_.tabs.front().bounds)==HTCLIENT;
            if(!valid)std::fprintf(stderr,"Native activation routing failed: modal=%u inactive_result=%lld style=0x%08lx exstyle=0x%08lx\n",
                unsigned(modal_open),static_cast<long long>(inactive),static_cast<unsigned long>(style),static_cast<unsigned long>(extended));
            return valid;
        };
        auto* active=active_session(); const auto info=active?active->info():SessionInfo{};
        if (smoke_modal_stage_==0 && info.state==SessionState::running && info.bytes_received) {
            const auto styles=GetWindowLongPtrW(hwnd_,GWL_STYLE);
            constexpr LONG_PTR required=WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX;
            if ((styles&WS_CAPTION) || (required&~styles)) {
                std::fprintf(stderr,"Native modal frame style=0x%08lx caption_bits=0x%08lx missing_required=0x%08lx\n",
                    static_cast<unsigned long>(styles),static_cast<unsigned long>(styles&WS_CAPTION),static_cast<unsigned long>(required&~styles));
                smoke_finish("Custom frame style invariants failed after activation; see style masks."); return;
            }
            if(!verify_activation_routing()) {smoke_finish("No-paint activation changed native frame geometry, styles, caption routing or terminal focus.");return;}
            const auto original=workspace().active_pane(); const auto original_session=workspace().pane(original)->session;
            close_pane(original);
            if (!modal_ || modal_focus_.activate()!=ModalDecision::cancel || focused_session_) { smoke_finish("Modal did not default to Cancel and remove terminal focus."); return; }
            if(!verify_activation_routing()) {smoke_finish("No-paint activation broke styled modal trapping or native caption routing.");return;}
            MSG key{}; key.hwnd=hwnd_; key.message=WM_KEYDOWN; key.wParam='X';
            if (!key_message(key)) { smoke_finish("Modal printable key leaked to terminal routing."); return; }
            key.wParam=VK_ESCAPE; key_message(key);
            if (modal_ || !workspace().pane(original) || focused_session_!=original_session) { smoke_finish("Escape did not restore terminal focus without closing."); return; }
            if(!verify_activation_routing()) {smoke_finish("No-paint activation broke focus restoration after modal Cancel.");return;}
            std::fprintf(stderr,"Native activation smoke: inactive/active no-paint dispatch preserves caption-free styles, immutable presentation style, client geometry, Snap hit target and modal focus. Synthetic routing is not desktop material verification.\n");
            close_pane(original); key.wParam=VK_RETURN; key_message(key);
            if (modal_ || !workspace().pane(original)) { smoke_finish("Default Enter did not cancel destructive close."); return; }
            new_tab(); const auto disposable=workspace().pane(workspace().active_pane())->session;
            smoke_disposable_=sessions_.at(disposable); park_pane(workspace().active_pane());
            close_parked({projects_.active_id(),disposable});
            if (!modal_) { smoke_finish("Disposable background terminal did not show styled close confirmation."); return; }
            smoke_modal_stage_=1; return;
        }
        if (smoke_modal_stage_==1 && presented) {
            // Do not cancel the disposable actor before its runtime has actually
            // started; its provider identity is asserted even after retirement.
            if (!smoke_disposable_ || !smoke_disposable_->info().bytes_received) return;
            const auto diagnostic=smoke_disposable_->diagnostics();
            if ((!options_.console_runtime_directory.empty() || config_.windows_console_backend==WindowsConsoleBackend::bundled_preview) && !diagnostic.bundled_provider) return;
            try { capture_frame(); } catch (const std::exception& error) { smoke_finish(error.what()); return; }
            modal_focus_.select(ModalDecision::confirm); answer_modal(modal_focus_.activate());
            if (modal_ || workspace().state().tabs.size()!=1 || sessions_.size()!=1) { smoke_finish("Explicit modal close did not retire only its background terminal."); return; }
            smoke_modal_stage_=2;
        } else return;
    }
    if (options_.smoke_modal && smoke_disposable_ && !smoke_disposable_->closed()) return;
    if (options_.smoke_projects && !smoke_projects_live_verified_) {
        if (projects_.entries().size()!=2 || sessions_.size()!=2 || smoke_project_actors_.size()!=2) { smoke_finish("Workspace creation did not retain two distinct actors."); return; }
        for (const auto& [id,actor]:smoke_project_actors_) {
            if (!sessions_.contains(id) || sessions_.at(id)!=actor) { smoke_finish("Workspace activation replaced an actor."); return; }
            const auto info=actor->info(); if (info.state!=SessionState::running || !info.bytes_received) return;
        }
        const auto row=std::find_if(navigation_.projects.begin(),navigation_.projects.end(),[&](const auto& item){return item.project==smoke_secondary_project_;});
        if (row==navigation_.projects.end()) { smoke_finish("Second real workspace has no visible navigation hit target."); return; }
        const Point select{row->bounds.x+row->bounds.width/2,row->bounds.y+row->bounds.height/2};
        pointer_down(select,false); pointer_up(select); retire_unused_sessions();
        if (projects_.active_id()!=smoke_secondary_project_) { smoke_finish("Workspace navigation pointer did not activate its actual project."); return; }
        dispatch("workspace.previous"); retire_unused_sessions();
        if (projects_.active_id()!=smoke_primary_project_) { smoke_finish("Keyboard/palette workspace command did not restore the previous project."); return; }
        if (sessions_.size()!=2 || !projects_.validate()) { smoke_finish("Hidden live actors were retired or workspace IDs collided."); return; }
        smoke_parked_session_=workspace().pane(workspace().active_pane())->session;
        const auto actor=sessions_.at(smoke_parked_session_); smoke_parked_bytes_=actor->info().bytes_received;
        dispatch("pane.background"); retire_unused_sessions();
        if (!workspace().state().tabs.empty() || projects_.active()->parked_sessions.size()!=1 || focused_session_ ||
            sessions_.at(smoke_parked_session_)!=actor || actor->process_finished() || !projects_.validate()) {
            smoke_finish("Parking the final pane did not retain the original live actor in an empty workspace."); return;
        }
        close_parked({smoke_primary_project_,smoke_parked_session_});
        if (!modal_ || modal_focus_.activate()!=ModalDecision::cancel) { smoke_finish("Background close did not show Cancel-default styled confirmation."); return; }
        answer_modal(ModalDecision::cancel);
        if (!sessions_.contains(smoke_parked_session_) || projects_.active()->parked_sessions.size()!=1) { smoke_finish("Canceled background close changed ownership."); return; }
        activate_project(smoke_secondary_project_);
        if (!projects_.active()->parked_sessions.empty()) { smoke_finish("Background footer leaked across projects."); return; }
        restore_parked({smoke_primary_project_,smoke_parked_session_});
        if (!projects_.find(smoke_primary_project_)->workspace.state().tabs.empty()) { smoke_finish("Cross-project restore accepted a stale target."); return; }
        notice_.clear();
        smoke_projects_live_verified_=true;
    }
    if (options_.smoke_layout && !smoke_tab_drag_verified_) {
        if (chrome_.compact || chrome_.tabs.size()<3) { smoke_finish("Layout smoke task tabs unexpectedly compact."); return; }
        const auto first=chrome_.tabs.front(), last=chrome_.tabs.back();
        const auto original=workspace().active_pane(); const auto actor_count=sessions_.size();
        const Point from{first.bounds.x+first.bounds.width/2,first.bounds.y+first.bounds.height/2};
        const Point to{last.bounds.x+last.bounds.width/2,last.bounds.y+last.bounds.height/2};
        pointer_down(from,false); pointer_move(to); pointer_up(to);
        if (workspace().state().tabs.back().id!=first.tab || sessions_.size()!=actor_count || GetCapture()==hwnd_) {
            smoke_finish("Workspace task drag did not reorder in its new band without changing actors."); return;
        }
        workspace().reorder_tab(first.tab,0); activate(original); changed(true); smoke_tab_drag_verified_=true;
        // Capture the actual background footer, not ordinary tabs named as if
        // they were parked. Preserve the accepted foreground split geometry.
        while (workspace().state().tabs.size()>1) park_pane(workspace().state().tabs.back().active_pane);
        if (projects_.active()->parked_sessions.size()!=2 || sessions_.size()!=actor_count || layout_.panes.size()!=3) {
            smoke_finish("Layout smoke did not retain its real background actors and foreground splits."); return;
        }
    }
    bool all_closed=true;
    for (const auto& [id,session]:sessions_) {
        const auto info=session->info();
        if (info.state==SessionState::failed) { smoke_finish(info.error.empty()?"Native session failed.":info.error); return; }
        if (!smoke_commands_sent_.contains(id) && info.state==SessionState::running && info.bytes_received) {
            smoke_commands_sent_.insert(id);
            const std::string command=smoke_managed_sessions_.contains(id)?
                "[[ $ZSH_VERSION == 5.9.2 && ${STORMTTY_MANAGED_ZSH-} == 1 ]] && print -r -- " + std::string(smoke_marker) + "; exit $?":
                "echo " + std::string(smoke_marker) + " & exit /b 0";
            submit([session,command] { return session->text(command); },command.size());
            KeyInput enter; enter.key=Key::enter;
            submit([session,enter] { return session->key(enter); });
        }
        if (!session->process_finished()) all_closed=false;
        else {
            // Completion can race the earlier running-state snapshot.
            const auto completed=session->info();
            if (!completed.exit_code || *completed.exit_code!=0) { smoke_finish("Controlled smoke shell did not exit successfully."); return; }
        }
    }
    if (presented && smoke_frame_ready() && !motion_timer_active_) smoke_marker_presented_=true;
    if (all_closed) {
        if (options_.smoke_projects && !smoke_background_restored_) {
            const auto actor=sessions_.at(smoke_parked_session_);
            if (actor!=smoke_project_actors_.at(smoke_parked_session_) || actor->info().bytes_received<=smoke_parked_bytes_ || !frame_has_smoke_marker(actor->frame()) || actor->closed()) {
                smoke_finish("Parked actor lost identity, live output, final frame or retained document."); return;
            }
            activate_project(smoke_primary_project_);
            restore_parked({smoke_primary_project_,smoke_parked_session_});
            if (!active_session() || active_session()!=actor.get() || !projects_.active()->parked_sessions.empty() || layout_.panes.empty()) {
                smoke_finish("Restore did not reattach the same exited actor and latest frame."); return;
            }
            notice_.clear(); smoke_background_restored_=true; smoke_marker_presented_=false; changed(true); return;
        }
        if (!smoke_frame_ready()) { smoke_finish("Controlled smoke shell exited without its expected output marker."); return; }
        if (smoke_marker_presented_ && renderer_->stats().presents) {
            if(material_probe_requested_ || material_probe_running_)return;
            if(!material_probe_succeeded_) {smoke_finish("Alpha-only normal-frame diagnostic failed: "+material_probe_status_);return;}
            if (options_.smoke_projects) {
                for (const auto& [id,actor]:smoke_project_actors_) if (!sessions_.contains(id) || sessions_.at(id)!=actor || !frame_has_smoke_marker(actor->frame())) {
                    smoke_finish("A hidden workspace lost its original actor or real shell output."); return;
                }
                try { capture_frame(); } catch (const std::exception& error) { smoke_finish(error.what()); return; }
                const auto retained=projects_.session_ids(smoke_primary_project_);
                close_project(smoke_secondary_project_,true);
                if (projects_.entries().size()!=1 || projects_.active_id()!=smoke_primary_project_ || retained.size()!=1 || sessions_.size()!=1 ||
                    !sessions_.contains(retained.front()) || sessions_.at(retained.front())!=smoke_project_actors_.at(retained.front())) {
                    smoke_finish("Scoped project close affected the surviving workspace."); return;
                }
                smoke_finish(); return;
            }
            try { if (!options_.smoke_modal && !options_.smoke_settings) capture_frame(); smoke_finish(); }
            catch (const std::exception& error) { smoke_finish(error.what()); }
        }
    }
}
void NativeWindow::smoke_finish(std::string_view error) {
    if (!options_.smoke_test || smoke_finished_) return;
    if (error.empty()) {
        for (const auto& [id,session]:smoke_sessions_) {
            const bool packaged_requested=smoke_bundled_sessions_.contains(id);
            const auto actual=session->diagnostics();
            const bool identity_matches=packaged_requested ?
                actual.bundled_provider && actual.provider.find("1.25.260930003")!=std::string::npos :
                !actual.bundled_provider && actual.provider=="Windows ConPTY (system)";
            std::fprintf(stderr,"Native CI provider session=%llu requested=%s actual=%s bundled=%d advanced_protocols_verified=%d. %s\n",
                static_cast<unsigned long long>(id.value),packaged_requested?"packaged-experimental":"system-default",
                actual.provider.c_str(),actual.bundled_provider,actual.advanced_protocols_verified,actual.detail.c_str());
            if (!identity_matches || actual.advanced_protocols_verified) {
                error="A created smoke session did not use the expected verified console runtime, or overstated protocol qualification."; break;
            }
        }
    }
    smoke_finished_=true; exit_code_=error.empty()?0:2;
    if (error.empty()) {
        const auto diagnostics=active_session()?active_session()->diagnostics():ProcessDiagnostics{};
        std::fprintf(stderr,"Native CI smoke passed: real HWND, WARP renderer, ConPTY input/output marker, presented frame, child exit 0. Provider: %s. Not physical visual/IME/Snap QA.\n",diagnostics.provider.c_str());
        if (using_managed_shell()) std::fprintf(stderr,"Managed shell CI smoke: actual upstream Zsh 5.9.2 plus managed preset sentinel, builtin output and exit 0; HOME/CWD retained and child-only environment. Not a StormTTY Zsh fork or completed redistribution qualification.\n");
        if (options_.smoke_settings) std::fputs("Settings/menu CI smoke: async isolated snapshot/discovery, real styled frame, input trapping, theme/font preview rollback, custom system menu/outside-click routing, confirmed Alt+], 25% opacity, two atomic Save/reopen cycles, original live actor retained. Not physical UI/accessibility QA.\n",stderr);
        if (options_.smoke_projects) std::fprintf(stderr,"Workspace CI smoke: two actual folder contexts; final-pane parking, canceled background close, cross-project isolation, live parked output and same retained actor restored after exit; scoped project close. Folder-picker interaction and physical UI remain manual QA.\n");
        if (options_.smoke_modal) std::fprintf(stderr,"Modal CI smoke: real styled confirmation frame, synthetic key trapping/Cancel/Escape/focus restoration and disposable actor close; not physical DWM activation/Snap proof.\n");
        if (options_.smoke_layout) std::fprintf(stderr,"Layout CI smoke: %zu real sessions, %zu tabs, %zu visible panes; settled chrome framebuffer, not desktop blur/corner proof.\n",sessions_.size(),workspace().state().tabs.size(),layout_.panes.size());
    } else std::fprintf(stderr,"Native CI smoke failed: %.*s Settings stage=%u elapsed_ms=%llu job_pending=%d saving=%d\n",
        static_cast<int>(error.size()),error.data(),smoke_settings_stage_,GetTickCount64()-smoke_started_at_,settings_job_,settings_panel_.saving());
    if (hwnd_) DestroyWindow(hwnd_);
}

void NativeWindow::position_ime() {
    if (!hwnd_ || !renderer_) return;
    POINT position{}; RECT exclusion{};
    if (palette_open_ || rename_open_) {
        position={static_cast<LONG>(std::max(16.0,(width_-560)/2+16)*scale_),static_cast<LONG>(96*scale_)};
        exclusion={position.x,position.y,position.x+1,position.y+static_cast<LONG>(20*scale_)};
    } else for (const auto& pane:layout_.panes) if (pane.pane==workspace().active_pane()) {
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
void NativeWindow::caret_schedule(bool restart) {
    if (!hwnd_) { cursor_timer_active_=false; cursor_blink_.configure(false,0,0); return; }
    auto* session=active_session(); const auto frame=session?session->frame():nullptr;
    const UINT interval=GetCaretBlinkTime();
    bool text_blinks=false;
    if (!reduced_motion_) for (const auto& pane:layout_.panes) if (auto* visible=session_for(pane.pane)) if (auto snapshot=visible->frame()) {
        for (const auto& row:snapshot->lines) if (row) for (const auto& cell:row->cells) if (cell.style.blink) { text_blinks=true; break; }
    }
    const bool cursor_blinks=!reduced_motion_ && frame && frame->cursor.visible && frame->cursor.blinking;
    const bool should_blink=active_ && !minimized_ && !palette_open_ && !rename_open_ && !modal_ && !overlay_open() && (cursor_blinks || text_blinks) && interval && interval!=INFINITE;
    const auto now=GetTickCount64();
    const bool was_visible=cursor_blink_.visible();
    const auto previous_interval=cursor_blink_.interval();
    cursor_blink_.configure(should_blink,interval,now);
    if (restart) cursor_blink_.restart(now);
    if (should_blink && (!cursor_timer_active_ || restart || previous_interval!=interval)) {
        cursor_timer_active_=SetTimer(hwnd_,cursor_timer,interval,nullptr)!=0;
        if (!cursor_timer_active_) { KillTimer(hwnd_,cursor_timer); cursor_blink_.configure(false,0,now); }
    } else if (!should_blink && cursor_timer_active_) { KillTimer(hwnd_,cursor_timer); cursor_timer_active_=false; }
    if (was_visible!=cursor_blink_.visible()) { dirty_=true; InvalidateRect(hwnd_,nullptr,FALSE); }
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
    const bool nc_input=message==WM_NCHITTEST || message==WM_NCMOUSEMOVE || message==WM_NCMOUSELEAVE;
    const bool dwm_handled=nc_input && DwmDefWindowProc(hwnd_,message,wparam,lparam,&dwm_result)!=FALSE;
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
        return 0;
    case WM_NCPAINT: return 0;
    case WM_NCACTIVATE:
        changed();
        // Preserve default activation bookkeeping without repainting a native
        // caption. The user's hardware A/B confirmed that swallowing this
        // processing left the system backdrop visually solid on their machine.
        return DefWindowProcW(hwnd_,message,wparam,-1);
    case WM_NCHITTEST: {
        const auto p=point(lparam,true);
        if (context_menu_.is_open() || settings_loading()) return HTCLIENT;
        if (settings_input_blocked() && settings_panel_.layout().panel.contains(p)) return HTCLIENT;
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
        windows::set_app_icons(hwnd_);
        const auto* rect=reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd_,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);
        settings_changed(); changed(true); return 0;
    }
    case WM_SIZE:
        minimized_=wparam==SIZE_MINIMIZED; maximized_=wparam==SIZE_MAXIMIZED;
        if (renderer_ && !minimized_) changed(true);
        caret_schedule(); motion_schedule(); return 0;
    case WM_ENTERSIZEMOVE: in_size_move_=true; return 0;
    case WM_EXITSIZEMOVE: in_size_move_=false; queue_material_probe(); changed(true); return 0;
    case WM_SETTINGCHANGE: case WM_THEMECHANGED: case WM_DWMCOMPOSITIONCHANGED:
        settings_changed(); changed(true); return 0;
    case WM_ACTIVATE:
        active_=LOWORD(wparam)!=WA_INACTIVE;
        if (active_) SetFocus(hwnd_);
        settings_changed(); caret_schedule(); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: {
        const bool focused=message==WM_SETFOCUS;
        active_=focused; text_input_.reset(); consumed_keys_.fill(false);
        if (!focused) { settings_panel_.pointer_cancel(); context_menu_.cancel(); menu_input_changed(); cancel_drag(); preedit_.clear(); composing_=false; }
        caret_schedule(focused); changed(); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; BeginPaint(hwnd_,&paint); EndPaint(hwnd_,&paint); dirty_=true;
        if (in_size_move_ && renderer_ && !minimized_) render();
        return 0;
    }
    case deferred_layout_message:
        layout_pending_=false;
        if (settings_pending_) { settings_pending_=false; settings_changed(); }
        changed(true); return 0;
    case wake_message:
        wake_->posted.store(false,std::memory_order_release); update_sessions(); InvalidateRect(hwnd_,nullptr,FALSE); return 0;
    case WM_TIMER:
        if (wparam==material_probe_timer) {poll_material_probe();return 0;}
        if (wparam==smoke_timeout_timer) { smoke_finish("Native smoke timed out before real shell output, presentation and clean exit."); return 0; }
        if (wparam==chrome_motion_timer) { motion_schedule(); ++chrome_generation_; dirty_=true; InvalidateRect(hwnd_,nullptr,FALSE); return 0; }
        if (wparam==cursor_timer && cursor_blink_.tick(GetTickCount64())) { dirty_=true; InvalidateRect(hwnd_,nullptr,FALSE); } return 0;
    case WM_CHAR: case WM_SYSCHAR: {
        if (settings_input_blocked()) return 0;
        if (message==WM_SYSCHAR && wparam==L' ' && (lparam & (1LL<<29))) return 0;
        if (composing_) return 0;
        const auto unit=static_cast<char16_t>(wparam);
        if ((palette_open_ || rename_open_ || context_menu_.is_open()) && unit<0x20) return 0;
        terminal_text(text_input_.feed(unit),physical_key(text_vk_,text_flags_),text_modifiers_); return 0;
    }
    case WM_UNICHAR:
        if (wparam==UNICODE_NOCHAR) return TRUE;
        { std::string text; Utf16Input::append(text,static_cast<std::uint32_t>(wparam)); terminal_text(std::move(text)); } return 0;
    case WM_IME_SETCONTEXT:
        if (overlay_open()) return 0;
        return DefWindowProcW(hwnd_,message,wparam,lparam & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
    case WM_IME_STARTCOMPOSITION:
        if (modal_ || overlay_open()) { if (ImmContext context(hwnd_); context.context) ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0); return 0; }
        composing_=true; preedit_.clear(); text_input_.reset(); position_ime(); changed(); return 0;
    case WM_IME_COMPOSITION: if (modal_ || overlay_open()) return 0; composition(lparam); return 0;
    case WM_IME_ENDCOMPOSITION: composing_=false; preedit_.clear(); changed(); return 0;
    case WM_IME_CHAR: return 0; // Result committed once from GCS_RESULTSTR above.
    case WM_INPUTLANGCHANGE: text_input_.reset(); position_ime(); break;
    case WM_LBUTTONDOWN: pointer_down(point(lparam),false); return 0;
    case WM_LBUTTONDBLCLK: pointer_down(point(lparam),true); return 0;
    case WM_LBUTTONUP: pointer_up(point(lparam)); return 0;
    case WM_MOUSEMOVE: pointer_move(point(lparam)); return 0;
    case WM_MOUSELEAVE: case WM_NCMOUSELEAVE: hover_changed({-1,-1}); return 0;
    case WM_NCMOUSEMOVE: {
        hover_changed(point(lparam,true));
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE|TME_NONCLIENT,hwnd_,0}; TrackMouseEvent(&tracking);
        break; // DefWindowProc keeps Windows 11 Snap hover semantics.
    }
    case WM_SYSCOMMAND:
        if ((wparam&0xfff0)==SC_KEYMENU || (wparam&0xfff0)==SC_MOUSEMENU) {
            if (!modal_ && !settings_input_blocked()) system_menu({chrome_.menu.x,chrome_.band.height},true);
            return 0;
        }
        break;
    case WM_NCRBUTTONDOWN: return 0;
    case WM_NCRBUTTONUP:
        if (wparam==HTCAPTION || wparam==HTSYSMENU) system_menu(point(lparam,true));
        return 0;
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
        if (wparam==HTMINBUTTON || wparam==HTMAXBUTTON || wparam==HTCLOSE) {
            caption_pressed_=wparam; SetCapture(hwnd_); changed(); return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (caption_pressed_) { pointer_up(point(lparam,true)); return 0; }
        if (wparam==HTMINBUTTON || wparam==HTMAXBUTTON || wparam==HTCLOSE) return 0;
        break;
    case WM_RBUTTONDOWN: {
        if (modal_ || overlay_open()) return 0;
        const auto p=point(lparam);
        for (const auto& tab:chrome_.tabs) if (tab.bounds.contains(p)) { if (const auto* model=workspace().tab(tab.tab)) activate(model->active_pane); return 0; }
        if (navigation_.rail.contains(p)) { for (const auto& row:navigation_.projects) if (row.bounds.contains(p)) { activate_project(row.project); break; } return 0; }
        if (workspace_chrome_contains(p)) return 0;
        for (const auto& placement:layout_.panes) if (placement.bounds.contains(p)) { const auto pane=placement.pane; activate(pane); break; }
        if (!(GetKeyState(VK_SHIFT)&0x8000)) terminal_mouse(p,MouseAction::press,MouseButton::right,true);
        return 0;
    }
    case WM_RBUTTONUP: {
        if (modal_ || overlay_open()) return 0;
        const auto p=point(lparam);
        // A terminal drag owns its matching release even over workspace chrome.
        if (mouse_capture_pane_) { terminal_mouse(p,MouseAction::release,MouseButton::right,false); return 0; }
        if (navigation_.rail.contains(p)) {
            for (const auto& row:navigation_.projects) if (row.bounds.contains(p)) { activate_project(row.project); break; }
            popup_menu(p);
        } else if (workspace_chrome_contains(p)) popup_menu(p);
        else popup_menu(p,true);
        return 0;
    }
    case WM_MBUTTONDOWN:
        if (modal_ || overlay_open()) return 0;
        for (const auto& tab:chrome_.tabs) if (tab.bounds.contains(point(lparam))) { close_tab(tab.tab); return 0; }
        terminal_mouse(point(lparam),MouseAction::press,MouseButton::middle,true); return 0;
    case WM_MBUTTONUP: if (overlay_open()) return 0; terminal_mouse(point(lparam),MouseAction::release,MouseButton::middle,false); return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        if (settings_loading()) return 0;
        if (settings_input_blocked()) {
            settings_panel_.wheel(point(lparam,true),static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam))/WHEEL_DELTA*3);
            settings_input_changed(); return 0;
        }
        if (context_menu_.is_open()) { context_menu_.wheel(point(lparam,true),static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam))/WHEEL_DELTA*3); menu_input_changed(); return 0; }
        if (modal_) return 0;
        auto& remainder=message==WM_MOUSEHWHEEL?horizontal_wheel_remainder_:wheel_remainder_;
        remainder+=GET_WHEEL_DELTA_WPARAM(wparam);
        const int steps=remainder/WHEEL_DELTA; remainder%=WHEEL_DELTA;
        if (!steps) return 0;
        PaneId target=workspace().active_pane();
        const auto position=point(lparam,true);
        if (navigation_.rail.contains(position)) {
            const auto next=static_cast<std::int64_t>(navigation_first_)-steps;
            navigation_first_=static_cast<std::size_t>(std::max<std::int64_t>(0,next)); changed(true); return 0;
        }
        if (workspace_chrome_contains(position)) return 0;
        for (const auto& placement:layout_.panes) if (placement.bounds.contains(position)) { target=placement.pane; break; }
        auto* session=session_for(target); const auto frame=session?session->frame():nullptr;
        if (frame && frame->mouse_reporting && !(GetKeyState(VK_SHIFT)&0x8000)) {
            const auto button=message==WM_MOUSEHWHEEL?(steps>0?MouseButton::wheel_right:MouseButton::wheel_left):(steps>0?MouseButton::wheel_up:MouseButton::wheel_down);
            for (int i=0;i<std::abs(steps);++i) terminal_mouse(point(lparam,true),MouseAction::press,button,false);
        } else if (message==WM_MOUSEWHEEL) {
            UINT rows=3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&rows,0);
            if (rows==WHEEL_PAGESCROLL) rows=frame?frame->rows:24;
            if (const auto* pane=workspace().pane(target); pane && sessions_.contains(pane->session)) {
                auto owned=sessions_.at(pane->session); const auto amount=-static_cast<std::int64_t>(steps)*rows;
                submit([owned,amount] { return owned->scroll(amount); });
            }
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lparam)==HTCLIENT) {
            if (modal_ || overlay_open()) { SetCursor(LoadCursorW(nullptr,IDC_ARROW)); return TRUE; }
            POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(hwnd_,&cursor); const Point p{cursor.x/scale_,cursor.y/scale_};
            for (const auto& divider:layout_.dividers) if (divider.hit_bounds.contains(p)) { SetCursor(LoadCursorW(nullptr,divider.axis==Axis::horizontal?IDC_SIZEWE:IDC_SIZENS)); return TRUE; }
            SetCursor(LoadCursorW(nullptr,modal_ || workspace_chrome_contains(p)?IDC_ARROW:IDC_IBEAM)); return TRUE;
        }
        break;
    case WM_CAPTURECHANGED:
        caption_pressed_=0; modal_pressed_.reset();
        settings_panel_.pointer_cancel(); context_menu_.pointer_cancel(); overlay_pointer_sequence_=false;
        if (overlay_open()) changed();
        if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
        mouse_capture_pane_={}; mouse_capture_button_=MouseButton::none;
        if (drag_!=Drag::none) cancel_drag(); return 0;
    case WM_CANCELMODE: settings_panel_.pointer_cancel(); context_menu_.cancel(); menu_input_changed(); cancel_drag(); return 0;
    case WM_CONTEXTMENU:
        if (modal_ || overlay_open()) return 0;
        if (GET_X_LPARAM(lparam)==-1 && GET_Y_LPARAM(lparam)==-1) popup_menu({width_/2,chrome_.band.height+12},true);
        else popup_menu(point(lparam,true),true);
        return 0;
    case WM_CLOSE: {
        if (settings_loading()) { settings_open_requested_=false; settings_binding_.clear(); notice_.clear(); caret_schedule(true); changed(); return 0; }
        if (context_menu_.is_open()) { context_menu_.cancel(); menu_input_changed(); }
        if (settings_input_blocked()) { settings_panel_.cancel(); settings_input_changed(); return 0; }
        const bool live=std::any_of(sessions_.begin(),sessions_.end(),[](const auto& pair) { return !pair.second->process_finished(); });
        if (modal_) { answer_modal(ModalDecision::cancel); return 0; }
        if (!live) DestroyWindow(hwnd_);
        else show_modal("Close StormTTY?",{"Its sessions may have running programs.","Closing can discard unsaved work."},"Close window",[this]{if (hwnd_) DestroyWindow(hwnd_);});
        return 0;
    }
    case WM_DESTROY:
        wake_->window.store(nullptr,std::memory_order_release);
        KillTimer(hwnd_,cursor_timer); KillTimer(hwnd_,smoke_timeout_timer); KillTimer(hwnd_,chrome_motion_timer); KillTimer(hwnd_,material_probe_timer);
        if (tooltip_ && IsWindow(tooltip_)) DestroyWindow(tooltip_);
        tooltip_=nullptr; tooltip_texts_.clear();
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
int run_native_application(Config config, NativeRunOptions options) {
    const bool smoke=options.smoke_test;
    windows::SmokeTeardownEnd completed{smoke,"native application locals destroyed"};
    NativeWindow window(std::move(config),std::move(options));
    const int result=window.run();
    windows::smoke_teardown_trace(smoke,"window run returned");
    return result;
}
} // namespace stormtty
