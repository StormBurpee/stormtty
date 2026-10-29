#include "stormtty/workspace_ribbon.hpp"
#include "stormtty/ui_colors.hpp"
#include "stormtty/chrome_paint.hpp"
#include "stormtty/pane_context.hpp"
#include "stormtty/command_watch.hpp"
#include "stormtty/previous_terminal.hpp"
#include "stormtty/smoke_commands.hpp"
#include "stormtty/settings_reload.hpp"
#include "stormtty/workspace_save_signal.hpp"
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
#include "stormtty/terminal_find.hpp"
#include "stormtty/workspace_navigator.hpp"
#include "stormtty/agent_display_text.hpp"
#include "stormtty/windows_managed_shell.hpp"
#include "stormtty/windows_shell_state.hpp"
#ifdef STORMTTY_EXPERIMENTAL_COMPLETION_SEED
#include "stormtty/windows_completion_seed.hpp"
#endif
#include "stormtty/windows_console_runtime.hpp"
#include "stormtty/renderer.hpp"
#include "stormtty/cursor_motion.hpp"
#include "stormtty/smoke_deadline.hpp"
#include "stormtty/session.hpp"
#include "stormtty/parked_attention.hpp"
#include "stormtty/settings_panel.hpp"
#include "stormtty/windows_settings_wheel.hpp"
#include <shellapi.h>
#include "stormtty/agent_panel.hpp"
#include "stormtty/agent_panel_controller.hpp"
#include "stormtty/agent_setup_service.hpp"
#include "stormtty/settings_services.hpp"
#include "stormtty/context_menu.hpp"
#include "app_icons.hpp"
#include "smoke_teardown_trace.hpp"
#include "theme_picker.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <deque>
#include <mutex>
#include <fstream>
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
std::uint64_t allocate_watch_host() {
    static std::atomic<std::uint64_t> next{1};auto value=next.load();
    while(value!=std::numeric_limits<std::uint64_t>::max())if(next.compare_exchange_weak(value,value+1))return value;
    return 0; // Saturation disables watches, never reuses an identity.
}
constexpr UINT wake_message = WM_APP + 41;
constexpr UINT deferred_layout_message = WM_APP + 42;
constexpr UINT smoke_settings_input_message = WM_APP + 43;
constexpr UINT deferred_destroy_message = WM_APP + 44;
constexpr UINT_PTR cursor_timer = 1;
constexpr UINT_PTR smoke_timeout_timer = 2;
constexpr UINT_PTR chrome_motion_timer = 3;
constexpr UINT_PTR material_probe_timer = 4;
constexpr UINT_PTR typing_motion_timer = 5;
constexpr UINT_PTR typing_smoke_timer = 6;
constexpr UINT_PTR local_clock_timer = 7;
constexpr UINT_PTR workspace_save_timer = 8;
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
    enum class Kind { load, save, reload } kind{};
    std::uint64_t generation{};
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
    std::optional<ConfigSaveResult> workspace_saved;
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
        if (workspace_worker_.joinable()) workspace_worker_.join();
        wake_->window.store(nullptr, std::memory_order_release);
        for (auto& [id, session] : sessions_) { (void)id; session->request_close(); }
        windows::smoke_teardown_trace(options_.smoke_test,"window destructor close requests complete");
        if (hwnd_) destroy_window();
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
        if (!smoke_shell_state_root_.empty()) {
            bool closed=true;
            for (const auto& [id,session]:smoke_sessions_) { (void)id; session->request_close(); }
            for (const auto& [id,session]:smoke_sessions_) { (void)id; if (!session->wait_closed(std::chrono::seconds(5))) closed=false; }
            std::error_code error;
            if (closed) std::filesystem::remove_all(smoke_shell_state_root_,error);
            std::fprintf(stderr,"Native smoke shell state cleanup: actors_closed=%d removed=%d\n",closed,closed&&!error);
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
    void refresh_clock();
    std::string local_clock_;
    void observe_material_renderer();
    void queue_material_probe(bool invalidate = false);
    void begin_material_probe(const WindowScene&);
    void poll_material_probe();
    void publish_material_status();
    void open_settings(std::string binding = {});
    void reload_settings();
    void settings_actions();
    void settings_completed();
    void preview_settings(const Config&);
    void settings_input_changed();
    void toggle_agent_panel();
    void agent_input_changed();
    std::string agent_status_label() const;
    void refresh_agent_panel();
    AgentPanelView current_agent_view() const;
    bool settings_input_blocked() const { return settings_panel_.is_open(); }
    bool settings_loading() const { return settings_job_ && settings_open_requested_ && !settings_input_blocked(); }
    bool overlay_open() const { return agent_panel_.is_open() || settings_input_blocked() || settings_loading() || context_menu_.is_open(); }
    void open_menu(std::vector<ContextMenuItem>, Point, std::map<std::string,std::function<void()>>, bool keyboard = false);
    void menu_input_changed();
    void system_menu(Point, bool keyboard = false);
    void refresh_layout();
    void defer_layout();
    void update_sessions();
    void sync_focus();
    void changed(bool geometry = false);
    void render();
    void destroy_window() noexcept;
    bool smoke_frame_ready() const;
    void smoke_progress(bool presented = false, bool frame_observed = false);
    void smoke_finish(std::string_view error = {});
    bool smoke_typing_progress();
    void smoke_typing_diagnostic(const char* event);
    void smoke_settings_stage(unsigned stage);
    bool smoke_menu_has_theme(const Theme&) const;
    void smoke_menu_theme_capture(std::wstring_view);
    bool smoke_menu_theme_running_{};
    void capture_frame();
    void capture_named_frame(std::wstring_view);
    NoticeLayout notice_layout();
    void submit(std::function<SessionSendResult()> action, std::size_t bytes = 64);
    void drain_input();
    WindowScene scene();
    void dispatch(std::string_view command);
    CommandContext command_context() const;
    Session* active_session() const;
    Session* session_for(PaneId) const;
    SessionId new_session(std::string profile_id = {});
    WatchIdentity watch_identity(SessionId) const;
    PreviousTerminal previous_terminal_;
    void commit_terminal_selection();
    struct PreviousTarget { PreviousTerminalState state{PreviousTerminalState::none}; ProjectWorkspaceId project{}; PaneId pane{}; std::string description; };
    PreviousTarget previous_terminal_target() const;
    void return_to_previous_terminal();
    bool watch_unread(SessionId) const;
    std::string watch_label(SessionId) const;
    bool observe_watch(SessionId,const SessionInfo&);
    void acknowledge_watch(SessionId);
    void open_watch(ProjectSessionReference,WatchIdentity);
    bool project_watch_unread(ProjectWorkspaceId) const;
    const std::uint64_t watch_host_{allocate_watch_host()};
    std::uint64_t next_watch_actor_{1};
    std::map<SessionId,std::uint64_t> watch_actors_;
    std::map<SessionId,CommandWatch> watches_;
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
    struct FooterPresentation {
        std::string directory, head, status;
        bool detached{}, subdued{}, failed{};
    };
    FooterPresentation footer_presentation() const;
    SessionFooterLayout footer_layout() const;
    WorkspaceBackgroundSummary background_summary() const;
    BackgroundFooterLayout background_layout() const { return footer_layout().background; }
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
    void open_navigator(bool background_only=false);
    void close_navigator();
    void refresh_navigator();
    void navigator_input_changed();
    void commit_navigator();
    std::pair<std::string,double> navigator_query_view() const;
    bool navigator_open_{},navigator_switch_pressed_{},navigator_background_only_{};
    unsigned navigator_focus_{};
    std::optional<NavigatorTarget> navigator_pressed_target_;
    WorkspaceNavigator navigator_;
    TerminalFindQuery navigator_query_;
    void open_find();
    void close_find(bool geometry=true);
    void submit_find();
    void refresh_find();
    void navigate_find(bool previous);
    void activate_find_control(unsigned);
    TerminalFindLayout find_layout() const;
    std::pair<std::string,double> find_query_view() const;
    void toggle_palette();
    void acquire_text_overlay_input();
    bool smoke_palette_evidence(SessionId);
    bool smoke_find_progress();
    bool smoke_navigator_evidence();
    bool smoke_previous_terminal_evidence();
    bool smoke_previous_terminal_done_{},smoke_previous_terminal_running_{};
    bool smoke_palette_input_progress();
    bool smoke_context_progress();
    bool smoke_watch_progress();
    unsigned smoke_watch_stage_{},smoke_watch_cycle_{};
    PaneId smoke_watch_pane_{},smoke_watch_original_{};
    SessionId smoke_watch_session_{};
    ProjectWorkspaceId smoke_watch_project_{},smoke_watch_other_{};
    std::uint64_t smoke_watch_generation_{},smoke_watch_input_{},smoke_watch_mouse_{};
    std::size_t smoke_watch_actor_count_{};
    WatchIdentity smoke_watch_identity_{};
    std::string smoke_watch_tab_title_;
    unsigned smoke_context_stage_{};
    std::vector<std::pair<PaneId,std::shared_ptr<Session>>> smoke_context_actors_;
    unsigned smoke_palette_input_stage_{};
    bool smoke_palette_disable_sent_{};
    bool smoke_navigator_done_{};
    unsigned smoke_find_stage_{}, smoke_find_trace_count_{};
    std::uint64_t smoke_find_bytes_{}, smoke_find_revision_{}, smoke_find_input_baseline_{}, smoke_terminal_input_submissions_{}, smoke_terminal_mouse_submissions_{};
    std::size_t smoke_find_selection_{};
    RECT smoke_find_window_{};
    struct PaletteEntry {
        std::string id, label, description;
        std::optional<ProjectSessionReference> session;
        int score{};
        std::optional<WatchIdentity> watch{};
    };
    using PaletteLayout = CommandPaletteLayout;
    std::vector<PaletteEntry> palette_commands() const;
    PaletteLayout palette_layout(std::span<const PaletteEntry>) const;
    CommandAvailability palette_availability(const PaletteEntry&) const;
    void activate_palette_entry(const PaletteEntry&);
    void popup_menu(Point, bool pane_actions = false);
    void pointer_down(Point, bool double_click);
    void pointer_move(Point);
    void pointer_up(Point);
    void cancel_drag();
    void position_ime();
    void composition(LPARAM);
    void caret_schedule(bool restart = false);
    void motion_schedule();
    void settings_wheel(Point, short delta, UINT lines);
    std::uint64_t chrome_now() const { return smoke_chrome_time_.value_or(GetTickCount64()); }
    void smoke_motion_capture(std::wstring_view, std::uint64_t elapsed);
    void smoke_focus_evidence(bool project);
    void smoke_workspace_frame_evidence();
    bool smoke_workspace_frame_running_{};
    void smoke_pane_focus_scene(const WindowScene&) const;
    void smoke_settings_scroll_evidence();
    std::optional<std::uint64_t> smoke_chrome_time_; // Explicit native CI presentation clock only.
    bool smoke_focus_running_{};
    std::optional<bool> smoke_pane_reduced_motion_; // Owned layout fixture: pane wash only, never caret/global policy.
    std::optional<bool> smoke_settings_scroll_reduced_motion_; // Settings-only authored motion fixture; never changes OS/config policy.
    bool smoke_settings_scroll_running_{};

    void hover_changed(Point);
    void refresh_tooltips();
    double hover_amount(std::uint64_t id) const;
    void toggle_fullscreen();
    void show_error(std::string_view);
    std::string pane_title(PaneId, bool full = false) const;
    PaneContextPresentation pane_context(PaneId) const;
    PaneContextPresentation context_for(const Pane&,std::string_view,const SessionInfo&) const;
    std::map<SessionId,SessionInfo> pane_infos_;
    std::map<SessionId, PaneContextPresentation> pane_contexts_;
    bool inspecting_context() const {return palette_open_ && inspected_context_.has_value();}
    std::optional<PaneContextPresentation> inspected_context_;
    mutable PaneContextLineCache inspected_lines_;
    TabWatchIndicator tab_watch_indicator(TabId) const;
    std::string tab_title(TabId, bool full = false) const;
    PixelRect pixels(Rect r) const { return {static_cast<float>(r.x * scale_), static_cast<float>(r.y * scale_), static_cast<float>(r.width * scale_), static_cast<float>(r.height * scale_)}; }
    Point point(LPARAM lparam, bool screen = false) const {
        POINT value{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (screen) ScreenToClient(hwnd_, &value);
        return {value.x / scale_, value.y / scale_};
    }
    Rect content_bounds(const PanePlacement& p) const {
        const double header = layout_.panes.size() > 1 ? 28.0 : 0.0;
        const double find_height=find_session_ && p.session==find_session_?layout_terminal_find(p.bounds,header).strip.height:0;
        return {p.bounds.x + 20, p.bounds.y + header + find_height + 16, std::max(0.0, p.bounds.width - 40), std::max(0.0, p.bounds.height - header - find_height - 32)};
    }
    Config config_;
    SessionId find_session_{};
    TerminalFindQuery find_query_;
    SearchSnapshotPtr find_result_;
    TerminalFindInitialSelection find_initial_selection_;
    std::uint64_t find_request_{};
    unsigned find_focus_{}; // query, previous, next, close
    std::optional<unsigned> find_pressed_;
    std::string find_error_;
    bool ai_enabled_{}; // Launch-time feature capability; never changed by reload/preview.
    NativeRunOptions options_;
    std::optional<Profile> managed_shell_profile_; // Resolved launch profile; only a stable marker is ever saved.
    SettingsPanel settings_panel_{Platform::windows};
    SettingsReload settings_reload_;
    AgentPanel agent_panel_;
    std::unique_ptr<AgentPanelController> agent_controller_;
    std::unique_ptr<AgentSetupService> agent_setup_;
    AgentProviderSettings agent_saved_settings_;
    std::string agent_setup_error_;
    bool agent_setup_reload_requested_{};
    unsigned smoke_agent_stage_{};
    std::thread settings_worker_;
    bool settings_job_{}, settings_open_requested_{}, settings_load_job_{}, smoke_hold_settings_completion_{};
    std::string settings_draft_notice_;
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
    bool smoke_modal_presented_{};
    unsigned smoke_settings_input_stage_{}; // closed scene presented, then separate input dispatch
    std::filesystem::path smoke_shell_state_root_;
    unsigned smoke_typing_stage_{};
    bool smoke_typing_handshake_sent_{}, smoke_typing_handshake_seen_{};
    int smoke_typing_input_result_{-1};
    unsigned smoke_typing_last_logged_{99};
    std::uint16_t smoke_typing_column_{}, smoke_typing_row_{};
    bool smoke_typing_reveal_seen_{}, smoke_typing_reduced_{};
    ULONGLONG smoke_started_at_{};
    SmokeDeadline smoke_deadline_;
    void smoke_phase(SmokeDeadline::Phase phase);
    bool smoke_settings_file_owned_{};
    unsigned smoke_reload_stage_{};
    Config smoke_reload_expected_;
    std::map<SessionId,std::shared_ptr<Session>> smoke_reload_actors_;
    std::uint64_t smoke_reload_received_{};
    Config smoke_settings_original_;
    std::shared_ptr<Session> smoke_settings_actor_;
    bool smoke_projects_live_verified_{}, smoke_background_restored_{}, smoke_chrome_verified_{}, smoke_tab_drag_verified_{};
    bool smoke_background_command_sent_{}, smoke_background_command_verified_{};
    unsigned smoke_background_capture_stage_{};
    SessionId smoke_parked_session_{};
    std::uint64_t smoke_parked_bytes_{};
    ProjectWorkspaceId smoke_primary_project_{}, smoke_secondary_project_{};
    std::map<SessionId,std::shared_ptr<Session>> smoke_project_actors_;
    std::shared_ptr<Session> smoke_disposable_;
    ProjectWorkspaceCatalog projects_;
    WorkspaceLibrary workspace_library_;
    std::optional<ConfigFileSnapshot> workspace_snapshot_;
    std::filesystem::path workspace_file_;
    std::thread workspace_worker_;
    bool workspace_save_busy_{}, workspace_save_pending_{}, workspace_timer_{}, restored_workspaces_{};
    std::function<Status(std::string)> edit_text_action_;
    std::string edit_text_hint_;
    void smoke_workspace_progress(bool presented);
    bool workspace_smoke_seeded_{};
    bool workspace_smoke_ready_{};
    bool workspace_smoke_menu_{};
    ULONGLONG workspace_smoke_capture_at_{};
    ULONGLONG workspace_smoke_diagnostic_at_{};
    void load_workspaces();
    WorkspaceSaveSignal workspace_save_signal_;
    void workspace_model_changed();
    void save_workspaces();
    void workspace_save_completed();
    void flush_workspaces();
    void launch_restored_project(bool retry_failed = false);
    void workspace_menu(Point, ProjectWorkspaceId target = {});
    void edit_workspace_text(std::string hint, std::string value, std::function<Status(std::string)>);
    void open_workspace_preset(std::size_t);

    Workspace& workspace() { return *projects_.active_workspace(); }
    const Workspace& workspace() const { return *projects_.active_workspace(); }
    void choose_project_folder();
    void activate_project(ProjectWorkspaceId);
    void close_project(ProjectWorkspaceId, bool approved = false);
    void reset_project_view(bool preserve_palette_motion=false);
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
    std::map<SessionId, ParkedAttention> parked_attention_;
    SessionId focused_session_{};
    std::uint64_t next_session_{1};
    std::shared_ptr<WakeState> wake_;
    HWND hwnd_{}, tooltip_{};
    bool tooltip_active_{true};
    std::map<UINT_PTR, std::wstring> tooltip_texts_;
    std::map<std::uint64_t, ChromeTransition> hover_transitions_;
    std::map<TabId, ChromeTransition> tab_transitions_;
    std::map<PaneId, ChromeTransition> pane_focus_;
    std::map<ProjectWorkspaceId, ChromeTransition> workspace_focus_;
    ChromeTransition palette_transition_, settings_transition_, menu_transition_, navigator_transition_;
    std::vector<ChromePrimitive> closing_navigator_primitives_;
    std::vector<ChromeText> closing_navigator_texts_;
    std::vector<ChromePrimitive> closing_settings_primitives_, closing_menu_primitives_;
    std::vector<ChromeText> closing_settings_texts_, closing_menu_texts_;
    bool motion_timer_active_{};
    CursorMotion typing_motion_;
    bool typing_timer_active_{};
    std::vector<ChromePrimitive> closing_palette_primitives_;
    std::vector<ChromeText> closing_palette_texts_;
    std::unique_ptr<Renderer> renderer_;
    bool renderer_call_{}, layout_pending_{}, settings_pending_{}, closing_window_{}, destroy_pending_{};
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
    std::string measured_notice_;
    Rect measured_notice_viewport_{};
    double measured_notice_scale_{};
    NoticeLayout measured_notice_layout_{};
    struct Modal { std::string title; std::vector<std::string> body; std::string confirm_label; std::function<void()> confirm; bool destructive{}; };
    std::optional<Modal> modal_;
    ModalFocus modal_focus_;
    std::optional<ModalDecision> modal_pressed_;
    WPARAM caption_pressed_{};
    std::string footer_signature_;
    std::size_t palette_index_{};
    bool palette_background_only_{};
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
    double divider_grab_offset_{};
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
    bool initial_canvas_prepared_{}, first_exposed_frame_{};
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

struct RendererCallGuard {
    bool& flag;
    explicit RendererCallGuard(bool& value):flag(value){flag=true;}
    ~RendererCallGuard(){flag=false;}
};

void NativeWindow::create() {
    ai_enabled_=config_.experimental_ai||options_.smoke_agent; // Explicit authored UI fixture, never provider calls.
    settings_panel_.set_agent_available(ai_enabled_);
    if(ai_enabled_) {
    AgentStdioOptions agent_options;agent_options.executable=narrow(options_.codex_executable.wstring());
    agent_options.account_reference=options_.codex_account_reference;agent_options.runtime_version=options_.codex_runtime_version;
    agent_controller_=std::make_unique<AgentPanelController>(std::move(agent_options),[wake=wake_]{wake->wake();});
    }
    if (options_.smoke_test) {
        smoke_started_at_=GetTickCount64();
        smoke_deadline_=SmokeDeadline(smoke_started_at_);
        std::fprintf(stderr,"Native CI smoke startup: settings=%d managed_zsh=%d packaged_console=%d\n",
            options_.smoke_settings,using_managed_shell(),!options_.console_runtime_directory.empty());
    }
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, L"StormTTY.NativeWindow", L"StormTTY", WS_OVERLAPPEDWINDOW & ~WS_CAPTION,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) throw std::runtime_error("Could not create the native StormTTY window.");
    enforce_custom_frame_style(hwnd_,options_.smoke_test);
    windows::set_app_icons(hwnd_);
    wake_->window.store(hwnd_, std::memory_order_release);
    if(ai_enabled_&&!options_.smoke_test&&!options_.agent_settings_path.empty()) {
        agent_setup_=std::make_unique<AgentSetupService>(options_.agent_settings_path,[wake=wake_]{wake->wake();});agent_setup_->load();
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    if (InitCommonControlsEx(&controls)) {
        tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (tooltip_) { SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 600); SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 600); }
    }
    dpi_ = GetDpiForWindow(hwnd_); scale_ = dpi_ / 96.0;
    // Establish final DPI-scaled client geometry before renderer/actors exist.
    // Shells no longer receive an avoidable second startup resize at high DPI.
    SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(1100 * scale_), static_cast<int>(720 * scale_), SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
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
    if (options_.smoke_test && managed_shell_profile_) {
        std::array<wchar_t,32768> temporary{};
        const auto length=GetTempPathW(static_cast<DWORD>(temporary.size()),temporary.data());
        GUID guid{}; std::array<wchar_t,40> token{};
        if (!length || length>=temporary.size() || FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid,token.data(),static_cast<int>(token.size())))
            throw std::runtime_error("Could not create unique smoke shell state.");
        const auto root=std::filesystem::path(temporary.data())/(std::wstring(L"StormTTY.ShellSmoke.")+token.data());
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("Smoke shell state directory already exists.");
        smoke_shell_state_root_=root; // Only a directory this process created is owned.
        if (!prepare_managed_shell_state_at(root,false)) throw std::runtime_error("Private smoke shell state validation failed.");
        std::fputs("Native smoke shell state: unique owned root; user history/config excluded.\n",stderr);
    }
    if (restored_workspaces_) launch_restored_project(); else new_tab();
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
    refresh_layout();
    settings_changed();
    motion_schedule();
    // Prime the real retained canvas before exposing the HWND. This is one
    // nonblocking render, never a wait for shell output, a splash, or a fake
    // ready state. Occlusion/busy keeps the normal post-show retry path intact.
    const auto initial_scene=scene();
    RenderResult initial_result;
    { RendererCallGuard guard(renderer_call_); initial_result=renderer_->render(initial_scene); }
    initial_canvas_prepared_=initial_result!=RenderResult::suspended;
    if (!initial_canvas_prepared_) throw std::runtime_error("Initial native canvas was unavailable.");
    dirty_=true; force_redraw_=true;
    if (options_.smoke_test) {
        capture_named_frame(L"startup-prepared");
        std::fprintf(stderr,"Native startup canvas: prepared_before_show=1 visible=%u panes=%zu shell_readiness_claimed=0 result=%u\n",unsigned(IsWindowVisible(hwnd_)!=FALSE),initial_scene.panes.size(),unsigned(initial_result));
        if (IsWindowVisible(hwnd_)) throw std::runtime_error("Startup canvas was prepared after window exposure.");
    }
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    if (options_.smoke_test && !options_.smoke_agent && !options_.smoke_layout && !options_.smoke_projects && !options_.smoke_settings && !options_.smoke_modal && options_.smoke_workspaces.empty() && !SetTimer(hwnd_,typing_smoke_timer,25,nullptr)) throw std::runtime_error("Could not start bounded typing smoke progress timer.");
    if (options_.smoke_test && !SetTimer(hwnd_,smoke_timeout_timer,250,nullptr)) throw std::runtime_error("Could not start the native smoke deadline.");
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
    load_workspaces();
    create();
    workspace_smoke_ready_=true;
    if(!options_.smoke_workspaces.empty()) changed(true);
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
                flush_workspaces();
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
void NativeWindow::refresh_clock() {
    if (!hwnd_) return;
    KillTimer(hwnd_,local_clock_timer);
    SYSTEMTIME time{}; GetLocalTime(&time);
    char formatted[6]{};
    std::snprintf(formatted,sizeof formatted,"%02u:%02u",static_cast<unsigned>(time.wHour),static_cast<unsigned>(time.wMinute));
    if (local_clock_!=formatted) { local_clock_=formatted; ++chrome_generation_; dirty_=true; InvalidateRect(hwnd_,nullptr,FALSE); }
    if (config_.appearance.show_clock && !config_.ribbon.hidden[ribbon_index(RibbonBinding::clock)] && !minimized_ && !fullscreen_ && !focus_mode_)
        SetTimer(hwnd_,local_clock_timer,60000-time.wSecond*1000-time.wMilliseconds,nullptr);
}
void NativeWindow::destroy_window() noexcept {
    if (!hwnd_ || closing_window_) return;
    if (renderer_call_) {
        if(!destroy_pending_) destroy_pending_=PostMessageW(hwnd_,deferred_destroy_message,0,0)!=FALSE;
        return; // Preserve the already-approved close, not a new WM_CLOSE prompt.
    }
    closing_window_=true;
    // A WARP presentation queue can stop progressing once its HWND disappears.
    // Drain and release GPU/presentation ownership while it is still valid.
    // The host dispatcher itself remains owned until after HWND/userdata teardown.
    RendererCallGuard guard(renderer_call_);
    if (renderer_) renderer_->prepare_window_destruction();
    DestroyWindow(hwnd_);
}
void NativeWindow::settings_changed() {
    refresh_clock();
    // Startup, live preview, Cancel, Save and Reload share this palette refresh.
    // Do it even when renderer-owned material work must wait for a later turn.
    context_menu_.set_theme(config_.theme);
    if (renderer_call_) { settings_pending_=true; defer_layout(); return; }
    HIGHCONTRASTW contrast{}; contrast.cbSize=sizeof(contrast);
    const bool contrast_known=SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)!=FALSE;
    high_contrast_ = contrast_known && (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const auto background=system_color(COLOR_WINDOW), foreground=system_color(COLOR_WINDOWTEXT);
    const auto highlight=system_color(COLOR_HIGHLIGHT), highlight_text=system_color(COLOR_HIGHLIGHTTEXT), disabled=system_color(COLOR_GRAYTEXT);
    settings_panel_.set_high_contrast(high_contrast_,background,foreground,highlight,highlight_text,disabled);
    context_menu_.set_high_contrast(high_contrast_,background,foreground,highlight,highlight_text,disabled);
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    reduced_motion_ = high_contrast_ || !animations || config_.appearance.reduced_motion==ReducedMotionPreference::enabled;
    if (config_.appearance.reduced_motion==ReducedMotionPreference::disabled && animations && !high_contrast_) reduced_motion_=false;
    DWORD transparency = 1, size = sizeof(transparency);
    const auto transparency_result=RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &size);
    const BOOL dark = !theme_is_light(config_.theme) && !high_contrast_;
    DwmSetWindowAttribute(hwnd_, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark));
    // Documented Windows 11 attributes. Unsupported builds return E_INVALIDARG;
    // no undocumented accent-policy calls, layered-window opacity or region hacks.
    const DWORD corners = 2; // DWMWCP_ROUND; DWM suppresses rounding when snapped/maximized.
    DwmSetWindowAttribute(hwnd_, static_cast<DWMWINDOWATTRIBUTE>(33), &corners, sizeof(corners));
    auto render_capability = renderer_ ? renderer_->stats() : RendererStats{};
    material_tracker_.observe({render_capability.composition,render_capability.warp,render_capability.device_recoveries,render_capability.host_backdrop});
    const auto policy=window_material_policy(config_.appearance,render_capability.composition,render_capability.warp,!contrast_known || high_contrast_,transparency_result==ERROR_SUCCESS && transparency!=0);
    const bool extra_requested=config_.appearance.additional_blur>0;
    const bool extra_eligible=policy.fallback==MaterialFallback::none &&
        config_.appearance.backdrop!=BackdropPreference::mica && contrast_known && transparency_result==ERROR_SUCCESS;
    if(renderer_) {
        RendererCallGuard guard(renderer_call_);
        // Never stack system acrylic under the host-effect recipe. Zero disables
        // backdrop effects entirely; opacity remains a separate canvas value.
        if(extra_requested && extra_eligible) {
            const DWORD none=1;
            DwmSetWindowAttribute(hwnd_,static_cast<DWMWINDOWATTRIBUTE>(38),&none,sizeof none);
        }
        renderer_->set_additional_blur(extra_eligible?static_cast<float>(config_.appearance.additional_blur):0);
        render_capability=renderer_->stats();
    }
    const bool extra_active=render_capability.host_backdrop;
    const bool clear_requested=policy.clear && render_capability.composition && !render_capability.warp;
    const DWORD material=extra_active?1:policy.system_backdrop;
    const HRESULT material_result=DwmSetWindowAttribute(hwnd_,static_cast<DWMWINDOWATTRIBUTE>(38),&material,sizeof(material));
    const bool clear_material=clear_requested && SUCCEEDED(material_result);
    const bool material_requested=SUCCEEDED(material_result) && material!=1;
    MARGINS margins=(extra_active || clear_material)?MARGINS{0,0,0,0}:material_requested?MARGINS{-1,-1,-1,-1}:MARGINS{1,1,1,1};
    const HRESULT frame_result=DwmExtendFrameIntoClientArea(hwnd_,&margins);
    const bool clear_active=clear_material && SUCCEEDED(frame_result);
    backdrop_=extra_active || clear_active || (material_requested && SUCCEEDED(frame_result));
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
    if(policy.clear) status=clear_active?"Clear background · no blur.":"Opaque fallback: clear composition unavailable.";
    if(extra_active) status="Additional blur "+std::to_string(std::lround(config_.appearance.additional_blur))+" · Windows base blur remains.";
    else if(extra_requested) status="Additional blur unavailable · "+status;
    if(extra_requested && config_.appearance.backdrop==BackdropPreference::mica)
        status="Additional blur needs Acrylic or Auto, not Mica.";
    if((extra_active || clear_active) && policy.opacity>=1) status="100% opacity hides the background. Lower Background opacity.";
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
NoticeLayout NativeWindow::notice_layout() {
    if (measured_notice_!=notice_ || measured_notice_viewport_!=navigation_.viewport || measured_notice_scale_!=scale_) {
        const auto available=layout_notice(navigation_.viewport,560,0);
        ChromeText label;
        label.text=notice_; label.size_dip=12; label.wrap=true;
        label.rect={0,0,static_cast<float>(available.text.width*scale_),
            static_cast<float>(std::max(1.0,navigation_.viewport.height)*scale_)};
        const auto measured=renderer_->measure_chrome_text(label);
        measured_notice_layout_=layout_notice(navigation_.viewport,measured.width/scale_,measured.height/scale_);
        measured_notice_=notice_; measured_notice_viewport_=navigation_.viewport; measured_notice_scale_=scale_;
    }
    return measured_notice_layout_;
}
void NativeWindow::begin_material_probe(const WindowScene& rendered) {
    if(!material_probe_requested_ || material_probe_running_ || !renderer_ || minimized_ || in_size_move_ || settings_pending_ ||
        overlay_open() || modal_ || palette_open_ || rename_open_ || palette_transition_.value(GetTickCount64())>0 || drag_!=Drag::none) return;
    std::vector<Rect> occluders;
    if(!notice_.empty())occluders.push_back(notice_layout().panel);
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
    if(material_tracker_.observe({capability.composition,capability.warp,capability.device_recoveries,capability.host_backdrop})) {
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
void NativeWindow::refresh_layout() {
    // Style changes during DirectComposition construction can synchronously send
    // WM_SIZE. Never reenter renderer resource construction through that message.
    if (renderer_call_) { defer_layout(); return; }
    RECT bounds{}; GetClientRect(hwnd_, &bounds);
    width_ = (bounds.right - bounds.left) / scale_; height_ = (bounds.bottom - bounds.top) / scale_;
    maximized_ = IsZoomed(hwnd_) != FALSE;
    if(navigator_open_&&(width_<320||height_<260))close_navigator();
    agent_panel_.resize({width_,height_}); settings_panel_.resize({width_,height_}); context_menu_.resize({width_,height_});
    std::vector<TabId> tabs;
    for (const auto& tab : workspace().state().tabs) tabs.push_back(tab.id);
    const auto caption_width = std::max(46.0, GetSystemMetricsForDpi(SM_CXSIZE, dpi_) / scale_);
    std::vector<ProjectWorkspaceId> projects;
    for (const auto& project:projects_.entries()) projects.push_back(project.id);
    context_label_=directory_context();
    const double caption=fullscreen_?0:std::min(32.0,height_);
    navigation_=layout_project_navigation(width_,height_,caption,projects,navigation_first_,navigation_expanded_,focus_mode_||fullscreen_,false);
    chrome_=layout_chrome(width_,height_,tabs,workspace().active_tab(),caption_width,caption,navigation_.rail.width,navigation_.tabs.height,workspace_action_lane(navigation_));
    navigation_first_=navigation_.first;
    zoom_return_={};
    if (const auto* tab=workspace().tab(workspace().active_tab()); tab && tab->zoomed_pane && chrome_.drag.width>=260 && !fullscreen_)
        zoom_return_={chrome_.drag.x+chrome_.drag.width-128,2,120,28};
    layout_ = workspace().layout(workspace().active_tab(), workspace_viewport(), {1.0 / scale_, 7, 80, 48});
    // Resizing can squeeze an already-open strip. Dismiss before actor sizing,
    // preserving the query and restoring input without a recursive layout pass.
    if(find_session_)for(const auto& place:layout_.panes)
        if(place.session==find_session_ && !terminal_find_fits(place.bounds,layout_.panes.size()>1)){close_find(false);break;}
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
SessionId NativeWindow::new_session(std::string profile_id) {
    if (profile_id.empty()) profile_id = projects_.active()->metadata.preferred_profile;
    if (profile_id == "@default") profile_id = session_profile_id();
    const bool managed = profile_id == managed_zsh_profile_id;
    ProcessOptions process;
    select_windows_console_provider(process,config_.windows_console_backend,options_.console_runtime_directory);
    // One factory covers tabs, splits and projects. Each Process owns its chosen
    // provider for life; only a successfully saved choice affects new sessions.
    if (managed && !managed_shell_profile_)
        throw std::runtime_error("Managed Zsh is selected but no app-owned runtime is available. Build with STORMTTY_BUNDLE_ZSH=ON or use --shell-runtime; no fallback was launched.");
    const Profile* selected=managed?&*managed_shell_profile_:nullptr;
    if (!selected) for (const auto& profile:config_.profiles) if (profile.id==profile_id) { selected=&profile; break; }
    if (!selected) throw std::runtime_error("Saved shell profile is unavailable. Choose a shell in the workspace menu.");
    if (selected) {
        process.executable=selected->executable; process.arguments=selected->arguments; process.working_directory=selected->working_directory;
        for (const auto& [name,value]:selected->environment) process.environment.push_back({name,value});
    }
    if (managed) {
        // Resolve only an absent HOME through wide APIs; explicit/empty HOME wins.
        process.environment.push_back(managed_shell_console_environment());
        const auto home_environment = managed_shell_home_environment();
        bool empty_home = false;
        for (const auto& entry : home_environment) {
            if (entry.name == "STORMTTY_EMPTY_HOME") empty_home = entry.value == "1";
            process.environment.push_back(entry);
        }
        // Launch-scoped native DACL validation, never an ambient trust switch.
        // State validation overrides inherited data without a subprocess, HOME
        // replacement, or changes to an existing user's files/permissions.
        const auto state = options_.smoke_test ? prepare_managed_shell_state_at(smoke_shell_state_root_,empty_home) : prepare_managed_shell_state(empty_home);
        if (options_.smoke_test && !empty_home && !state) throw std::runtime_error("Private smoke shell state failed revalidation.");
        process.environment.push_back({"STORMTTY_NATIVE_ZSH_STATE", state ?
            std::optional<std::string>(narrow(state.directory.generic_wstring())) : std::nullopt});
#ifdef STORMTTY_EXPERIMENTAL_COMPLETION_SEED
        if (state) {
            const auto runtime=std::filesystem::path(widen(process.executable)).parent_path().parent_path().parent_path();
            // Every opted-in managed launch participates, even warm/custom state.
            // Heavy integrity work, if fresh, runs on the controller before child launch.
            process.initialization=completion_seed_launch(state.directory.parent_path().parent_path(),runtime).initialization;
        }
#endif
        // Child environment only: user startup hooks must not override this preset.
        for (const char* name:{"MSYS","ENV","BASH_ENV"}) process.environment.push_back({name,std::nullopt});
    }
    if (const auto* project=projects_.active(); project && !project->metadata.working_directory.empty()) process.working_directory=project->metadata.working_directory;
    // The built-in alias is resolved only against the trusted system directory,
    // never the current directory/PATH. Other configured executables must be
    // absolute and are validated by the process transport.
    std::string alias=process.executable;
    for (auto& character:alias) if (character>='A' && character<='Z') character=static_cast<char>(character-'A'+'a');
    if (!managed && (options_.smoke_test || alias.empty() || alias=="cmd.exe" || alias=="cmd")) {
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
    terminal.ansi.emplace();
    for (std::size_t i=0;i<16;++i) (*terminal.ansi)[i]=color_hex(config_.theme.ansi[i],{0,0,0,255});
    terminal.default_background=color_hex(config_.theme.background,{23,25,29,255});
    const SessionId id{next_session_++};
    std::weak_ptr<WakeState> weak = wake_;
    const bool bundled=process.provider==ProcessProvider::windows_packaged_experimental;
    auto session = Session::start(std::move(process), terminal, [weak] { if (auto state = weak.lock()) state->wake(); }, SessionRetentionPolicy::retain_document);
    if (options_.smoke_test) { smoke_sessions_.emplace(id,session); if (bundled) smoke_bundled_sessions_.insert(id); if (managed) smoke_managed_sessions_.insert(id); }
    sessions_.emplace(id, std::move(session));
    const auto token=next_watch_actor_==std::numeric_limits<std::uint64_t>::max()?0:next_watch_actor_++;
    watch_actors_.emplace(id,token);
    return id;
}
void NativeWindow::new_tab() {
    const auto profile=projects_.active()->metadata.preferred_profile=="@default"?session_profile_id():projects_.active()->metadata.preferred_profile;
    SessionId id;
    try { id = new_session(profile); }
    catch (const std::exception& error) { show_error(error.what()); return; }
    auto result = workspace().create_tab(id, profile);
    if (!result) { sessions_.at(id)->request_close(); show_error(result.error); retire_unused_sessions(); return; }
    workspace_model_changed(); changed(true);
}
void NativeWindow::split(Direction direction) {
    const auto active = workspace().active_pane();
    if (!active) return;
    const auto profile=projects_.active()->metadata.preferred_profile=="@default"?session_profile_id():projects_.active()->metadata.preferred_profile;
    SessionId id;
    try { id = new_session(profile); }
    catch (const std::exception& error) { show_error(error.what()); return; }
    auto result = workspace().split(active, direction, id, profile);
    if (!result) { sessions_.at(id)->request_close(); show_error(result.error); retire_unused_sessions(); return; }
    workspace_model_changed(); activate(*result.value); changed(true);
}
void NativeWindow::retire_unused_sessions() {
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
        const bool used = projects_.contains_session(iterator->first);
        if (!used) { iterator->second->request_close(); sizes_.erase(iterator->first); session_states_.erase(iterator->first); background_labels_.erase(iterator->first); parked_attention_.erase(iterator->first); watches_.erase(iterator->first); watch_actors_.erase(iterator->first); iterator = sessions_.erase(iterator); }
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
WatchIdentity NativeWindow::watch_identity(SessionId id) const {
    const auto found=watch_actors_.find(id);return {watch_host_,id.value,found==watch_actors_.end()?0:found->second};
}
void NativeWindow::commit_terminal_selection() {
    const auto* pane=workspace().pane(workspace().active_pane());
    if(pane && sessions_.contains(pane->session))previous_terminal_.commit(watch_identity(pane->session));
}
NativeWindow::PreviousTarget NativeWindow::previous_terminal_target() const {
    const auto target=previous_terminal_.previous();
    if(!target)return {};
    const SessionId session{target.session};
    // Resolve current placement afresh; moving/renaming never replaces identity.
    for(const auto& project:projects_.entries()) {
        for(const auto& parked:project.parked_sessions)if(parked.session==session)
            return {PreviousTerminalState::parked,{},{},{}};
        for(const auto& [id,pane]:project.workspace.state().panes)if(pane.session==session) {
            if(!sessions_.contains(session))return {PreviousTerminalState::dormant,{},{},{}};
            if(watch_identity(session)!=target)return {PreviousTerminalState::stale,{},{},{}};
            const auto tab=project.workspace.tab_of(id);
            const auto* item=tab?project.workspace.tab(*tab):nullptr;
            const auto title=background_session_title(item?item->title:std::string{},sessions_.at(session)->info().title,"Terminal");
            return {PreviousTerminalState::available,project.id,id,
                "Return to · "+pane_context_text(agent_display_text(project.metadata.name,256))+" · "+pane_context_text(agent_display_text(title,256))};
        }
    }
    return {PreviousTerminalState::missing,{},{},{}};
}
void NativeWindow::return_to_previous_terminal() {
    const auto target=previous_terminal_target();
    const auto available=previous_terminal_availability(target.state);
    if(!available.enabled){show_error(available.reason);return;}
    auto* project=projects_.workspace(target.project);
    // No restore, launch, fallback, watch acknowledgment, or terminal input.
    const auto status=projects_.activate(target.project);
    if(!status){show_error(status.error);return;}
    if(const auto tab=project->tab_of(target.pane))project->activate_tab(*tab);
    project->focus(target.pane);
    workspace_model_changed();reset_project_view(true);
}
bool NativeWindow::watch_unread(SessionId id) const {
    const auto found=watches_.find(id);return found!=watches_.end()&&found->second.identity()==watch_identity(id)&&found->second.unread();
}
std::string NativeWindow::watch_label(SessionId id) const {
    const auto found=watches_.find(id);if(found==watches_.end()||found->second.identity()!=watch_identity(id))return {};
    const auto& watch=found->second;
    switch(watch.status()) {
    case WatchStatus::watching:return "Watching command";
    case WatchStatus::completed:return watch.exit_code()?"Watched command finished · exit "+std::to_string(*watch.exit_code()):"Watched command finished · status unknown";
    case WatchStatus::completion_unconfirmed:return "Later command finished · watched result unavailable";
    case WatchStatus::interrupted:return "Watch ended · no command result";
    case WatchStatus::unavailable:return "Watch result unavailable";
    default:return {};
    }
}
bool NativeWindow::observe_watch(SessionId id,const SessionInfo& info) {
    const auto found=watches_.find(id);if(found==watches_.end())return false;
    const auto before=found->second.status();found->second.observe(watch_identity(id),info);
    if(found->second.status()==WatchStatus::idle){watches_.erase(found);return true;}
    return before!=found->second.status();
}
void NativeWindow::acknowledge_watch(SessionId id) {
    const auto found=watches_.find(id);if(found==watches_.end()||found->second.identity()!=watch_identity(id)||!found->second.unread())return;
    if(const auto parked=parked_attention_.find(id);parked!=parked_attention_.end())parked->second.acknowledge_completion(found->second.generation());
    found->second.acknowledge();watches_.erase(found);
    if(navigator_open_)refresh_navigator();refresh_tooltips();changed();
}
bool NativeWindow::project_watch_unread(ProjectWorkspaceId id) const {
    const auto* project=projects_.workspace(id);if(!project)return false;
    for(const auto& [pane,item]:project->state().panes){(void)pane;if(watch_unread(item.session))return true;}
    return false;
}
void NativeWindow::open_watch(ProjectSessionReference target,WatchIdentity identity) {
    const auto found=watches_.find(target.local);const auto* entry=projects_.find(target.project);
    if(!entry||!sessions_.contains(target.local)||watch_identity(target.local)!=identity||found==watches_.end()||!found->second.unread()||found->second.identity()!=identity){show_error("The watched actor is no longer available.");return;}
    PaneId pane{};for(const auto& [id,item]:entry->workspace.state().panes)if(item.session==target.local){pane=id;break;}
    const bool parked=std::any_of(entry->parked_sessions.begin(),entry->parked_sessions.end(),[&](const auto& value){return value.session==target.local;});
    if(!pane&&!parked){show_error("The watched terminal is no longer placed.");return;}
    if(parked&&entry->workspace.state().tabs.size()>=Workspace::maximum_tabs){show_error("Tab limit reached; watch result retained.");return;}
    // Direct catalog activation never starts a dormant/restored shell.
    const auto activated=projects_.activate(target.project);if(!activated){show_error(activated.error);return;}
    if(parked)restore_parked(target);
    else {auto* project=projects_.workspace(target.project);if(const auto tab=project->tab_of(pane))project->activate_tab(*tab);project->focus(pane);workspace_model_changed();reset_project_view();}
    const auto* current=workspace().pane(workspace().active_pane());
    if(current&&current->session==target.local&&watch_identity(target.local)==identity)acknowledge_watch(target.local);
}
std::string NativeWindow::background_title(const ParkedSession& parked, bool status) const {
    std::string profile;
    if (managed_shell_profile_ && managed_shell_profile_->id==parked.profile) profile=managed_shell_profile_->label;
    else for (const auto& candidate:config_.profiles) if (candidate.id==parked.profile) { profile=candidate.label; break; }
    std::string title=background_session_title(parked.user_title,parked.title,profile);
    if (auto found=sessions_.find(parked.session); found!=sessions_.end()) {
        const auto info=found->second->info();
        title=background_session_title(parked.user_title,info.title.empty()?parked.title:info.title,profile);
        // Keep UTF-8 intact; DirectWrite clips bounded actor titles.
        if (!status) return title;
        if(!watch_label(parked.session).empty())title+=" · "+watch_label(parked.session);
        else if (const auto attention=parked_attention_.find(parked.session); attention!=parked_attention_.end() && attention->second.unread_completion()) title+=" · command finished";
        title+=" · "+terminal_status_label(info);
    }
    return title;
}
void NativeWindow::park_pane(PaneId pane) {
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    cancel_drag();
    const auto result=projects_.park({projects_.active_id(),pane});
    if (!result) { show_error(result.error); return; }
    // Baseline only after placement succeeds, before any reset/update can observe it.
    parked_attention_.insert_or_assign(*result.value,ParkedAttention(sessions_.at(*result.value)->info()));
    workspace_model_changed();
    reset_project_view(); // Clear IME, selection and renderer-local placement state.
}
void NativeWindow::restore_parked(ProjectSessionReference target) {
    if (target.project!=projects_.active_id() || !sessions_.contains(target.local)) { show_error("The background target changed; nothing was restored."); return; }
    const auto result=projects_.reattach_to_new_tab(target);
    if (!result) { show_error(result.error); return; }
    workspace_model_changed();
    parked_attention_.erase(target.local); // Successful restore acknowledges; failures/cancel do not.
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
        if (only) {
            items.push_back(std::move(restore));
            if(const auto watched=watches_.find(parked.session);watched!=watches_.end()) {
                const auto identity=watch_identity(parked.session);
                ContextMenuItem action;action.id=id+".watch";action.label=watched->second.unread()?"Open watched result":"Cancel watch (keep process running)";
                actions[action.id]=[this,target,identity]{if(watch_identity(target.local)!=identity)return;const auto found=watches_.find(target.local);if(found==watches_.end())return;if(found->second.unread())open_watch(target,identity);else{watches_.erase(found);refresh_tooltips();changed();}};
                items.push_back(std::move(action));
                if(watched->second.unread()){ContextMenuItem dismiss;dismiss.id=id+".dismiss";dismiss.label="Dismiss watched result";actions[dismiss.id]=[this,target,identity]{if(watch_identity(target.local)==identity)acknowledge_watch(target.local);};items.push_back(std::move(dismiss));}
            }
            items.push_back(std::move(close));
        }
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
    workspace().close_pane(pane); workspace_model_changed(); retire_unused_sessions();
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
    workspace().close_tab(tab); workspace_model_changed(); retire_unused_sessions();
    if (workspace().state().tabs.empty() && projects_.active()->parked_sessions.empty()) { close_project(projects_.active_id()); return; }
    changed(true);
}
void NativeWindow::sync_focus() {
    const auto* active_pane=workspace().pane(workspace().active_pane());
    if(find_session_ && (!active_pane || active_pane->session!=find_session_)) {
        if(auto actor=sessions_.find(find_session_);actor!=sessions_.end())(void)actor->second->cancel_search();
        if(find_pressed_ && GetCapture()==hwnd_)ReleaseCapture();
        find_session_={};find_result_.reset();find_request_=0;find_initial_selection_.cancel();find_pressed_.reset();
        preedit_.clear();composing_=false;text_input_.reset();
        if(ImmContext context(hwnd_);context.context)ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
    }
    SessionId desired{};
    if (active_ && !navigator_open_ && !find_session_ && !palette_open_ && !rename_open_ && !modal_ && !overlay_open()) if (const auto* pane=workspace().pane(workspace().active_pane())) desired=pane->session;
    if (desired==focused_session_) return;
    const auto previous=focused_session_; focused_session_=desired;
    if (sessions_.contains(previous)) { auto old=sessions_.at(previous); submit([old] { return old->focus(false); }); }
    if (sessions_.contains(desired)) { auto current=sessions_.at(desired); submit([current] { return current->focus(true); }); }
}
void NativeWindow::activate(PaneId pane) {
    if (workspace().active_pane() == pane) return;
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    if (!workspace().focus(pane)) return;
    workspace_model_changed();
    text_input_.reset(); caret_schedule(true); changed(true);
}
void NativeWindow::update_sessions() {
    struct TitleUpdate { ProjectWorkspaceId project; PaneId pane; std::string title; };
    std::vector<TitleUpdate> titles;
    bool state_changed=false;
    std::erase_if(pane_infos_,[this](const auto& value){return !sessions_.contains(value.first);});
    std::erase_if(pane_contexts_,[this](const auto& value){return !sessions_.contains(value.first);});
    ParkedNoticeBatch notice_batch;
    std::string batch_notice;
    // Hidden workspaces retain actors and model metadata. Never retire them just
    // because only the active project's panes are submitted to the renderer.
    for (const auto& project:projects_.entries()) for (const auto& [pane_id,pane]:project.workspace.state().panes) {
        auto found=sessions_.find(pane.session); if (found==sessions_.end()) continue;
        const auto info=found->second->info();
        pane_infos_[pane.session]=info;
        if(observe_watch(pane.session,info)){state_changed=true;++chrome_generation_;}
        auto presentation=context_for(pane,project.metadata.working_directory,info);
        if(!pane_contexts_.contains(pane.session) || pane_contexts_.at(pane.session)!=presentation){
            pane_contexts_[pane.session]=std::move(presentation);state_changed=true;++chrome_generation_;
        }
        if (!info.title.empty() && pane.title!=info.title) titles.push_back({project.id,pane_id,info.title});
        if (project.id==projects_.active_id() && !info.error.empty() && notice_!=info.error) {
            if (notice_batch.consider(ParkedNotice::process_failed)) batch_notice=info.error;
            ++chrome_generation_;
        }
        if (!session_states_.contains(pane.session) || session_states_[pane.session]!=info.state) { session_states_[pane.session]=info.state; state_changed=true; ++chrome_generation_; }
    }
    for (const auto& project:projects_.entries()) for (const auto& parked:project.parked_sessions) {
        auto found=sessions_.find(parked.session); if (found==sessions_.end()) continue;
        const auto info=found->second->info();
        if(observe_watch(parked.session,info)){state_changed=true;++chrome_generation_;}
        pane_infos_[parked.session]=info; // Restore uses the latest owned snapshot, even if the actor is quiet.

        // Normally inserted by park_pane; fallback baselines without inventing an alert.
        auto attention=parked_attention_.try_emplace(parked.session,info).first;
        const auto event=attention->second.observe(info);
        const auto title=background_title(parked);
        if (background_labels_[parked.session]!=title) { background_labels_[parked.session]=title; state_changed=true; ++chrome_generation_; }
        if (!session_states_.contains(parked.session) || session_states_[parked.session]!=info.state) {
            session_states_[parked.session]=info.state; state_changed=true; ++chrome_generation_;
        }
        if (notice_batch.consider(event)) {
            const std::string destination=project.id==projects_.active_id()?" Restore it from the footer to inspect output.":" Switch to this workspace and restore it from its footer to inspect output.";
            if (event==ParkedNotice::process_failed) batch_notice=project.metadata.name+" · Background terminal failed."+destination;
            else if (event==ParkedNotice::process_exited) batch_notice=project.metadata.name+" · Background terminal exited"+(info.exit_code?" ("+std::to_string(*info.exit_code)+")":"")+"."+destination;
            else batch_notice=parked_command_finished_notice(project.metadata.name,info.command.last_exit_code);
            state_changed=true; ++chrome_generation_;
        }
    }
    if (notice_batch.selected()!=ParkedNotice::none) notice_=std::move(batch_notice);
    for (auto& title:titles) { projects_.workspace(title.project)->set_pane_title(title.pane,std::move(title.title)); ++chrome_generation_; }
    std::optional<std::string> clipboard,error;
    { std::lock_guard lock(wake_->mutex); clipboard.swap(wake_->clipboard); error.swap(wake_->error); }
    if (clipboard) set_clipboard(*clipboard);
    if (error) show_error(*error);
    const auto context=directory_context();
    if (context_label_!=context) { context_label_=context; state_changed=true; ++chrome_generation_; }
    const auto footer=footer_presentation();
    const auto footer_signature=footer.directory+"\n"+footer.head+"\n"+footer.status+(footer.subdued?"1":"0")+(footer.detached?"1":"0");
    if(footer_signature_!=footer_signature) {footer_signature_=footer_signature;state_changed=true;++chrome_generation_;}
    if (!titles.empty() || state_changed) {refresh_tooltips();if(navigator_open_)refresh_navigator();}
    refresh_find();
    settings_completed();
    refresh_agent_panel();
    workspace_save_completed();
    drain_input(); dirty_=true; caret_schedule(); position_ime();
    if (options_.smoke_test) smoke_progress();
}
NativeWindow::FooterPresentation NativeWindow::footer_presentation() const {
    FooterPresentation result;
    const auto* actor=active_session();
    if (!actor) return result;
    const auto info=actor->info();
    const auto* active_pane=workspace().pane(workspace().active_pane());
    const bool managed=active_pane && managed_shell_profile_ && active_pane->profile==managed_shell_profile_->id;
    if (info.shell_context) {
        result.directory=info.shell_context->display_directory;
        result.head=info.shell_context->head;
        result.detached=info.shell_context->git==GitHeadKind::detached;
    } else if (!managed) {
        // Unintegrated shells still expose their observed directory. It remains
        // display data, never a local filesystem lookup or implicit command.
        result.directory=info.working_directory;
        if(result.directory.empty()) if(const auto* project=projects_.active()) result.directory=project->metadata.working_directory;
        std::erase_if(result.directory,[](unsigned char c){return c<0x20 || c==0x7f;});
    }
    // A managed context clear can accompany an invalid/oversized new PWD.
    // Never revive its previous raw directory; show the neutral profile instead.
    if(result.directory.empty()) {
        if(const auto* pane=workspace().pane(workspace().active_pane())) {
            result.directory=pane->profile;
            if(managed_shell_profile_ && managed_shell_profile_->id==pane->profile) result.directory=managed_shell_profile_->label;
            else for(const auto& profile:config_.profiles) if(profile.id==pane->profile) {result.directory=profile.label;break;}
        }
    }
    result.subdued=info.command.phase==CommandPhase::running;
    if(info.state==SessionState::failed) {result.status="failed";result.failed=true;}
    else if(info.state==SessionState::exited) result.status="exited";
    else if(info.state==SessionState::closing) result.status="closing";
    else if(info.state==SessionState::starting) result.status="starting";
    else if(info.command.phase==CommandPhase::completed && info.command.last_exit_code && *info.command.last_exit_code!=0) {
        result.status="exit "+std::to_string(*info.command.last_exit_code); result.failed=true;
    }
    return result;
}
WorkspaceBackgroundSummary NativeWindow::background_summary() const {
    WorkspaceBackgroundSummary result;
    if(const auto* project=projects_.active())for(const auto& parked:project->parked_sessions){const auto found=parked_attention_.find(parked.session);result.add(watch_unread(parked.session)||(found!=parked_attention_.end()&&found->second.unread_completion()));}
    return result;
}
SessionFooterLayout NativeWindow::footer_layout() const {
    const auto context=footer_presentation();
    return layout_session_footer(navigation_.footer,projects_.active()->parked_sessions.size(),
        active_session()!=nullptr,!context.head.empty(),!context.status.empty(),config_.appearance.show_clock,config_.ribbon,{ai_enabled_,agent_controller_&&agent_controller_->attention(),context.failed,true,background_summary().unread()!=0});
}
std::string NativeWindow::directory_context() const {
    std::string path;
    bool managed_without_context=false;
    if (auto* session=active_session()) {
        const auto info=session->info();
        const auto* pane=workspace().pane(workspace().active_pane());
        managed_without_context=!info.shell_context && pane && managed_shell_profile_ && pane->profile==managed_shell_profile_->id;
        if(!managed_without_context) path=info.shell_context?info.shell_context->display_directory:info.working_directory;
    }
    if (path.empty() && !managed_without_context) if (const auto* project=projects_.active()) path=project->metadata.working_directory;
    // OSC metadata is display-only; it is never an implicit chdir or command.
    path.erase(std::remove_if(path.begin(),path.end(),[](unsigned char c){return c<0x20 || c==0x7f;}),path.end());
    const auto* project=projects_.active();
    const std::string name=project?project->metadata.name:"Workspace";
    return path.empty()?name:name+"  /  "+path;
}
void NativeWindow::load_workspaces() {
    if ((options_.smoke_test && options_.smoke_workspaces.empty()) || options_.config_path.empty()) return;
    workspace_file_=options_.config_path.parent_path()/L"workspaces.stormtty";
    WorkspaceLibrarySnapshotResult loaded;
    // Before the window exists; no filesystem work runs in a window callback.
    std::thread reader([&]{ loaded=load_workspace_library_snapshot(workspace_file_); }); reader.join();
    if (!loaded) { notice_="Workspaces were not restored: "+loaded.error+" The saved file was left unchanged."; return; }
    workspace_snapshot_=std::move(loaded.snapshot);
    workspace_library_=std::move(*loaded.library);
    if (!workspace_library_.restore_on_launch || workspace_library_.last_session.projects.empty()) return;
    const auto status=restore_workspace_snapshot(projects_,workspace_library_.last_session);
    if (!status) { notice_="Workspaces were not restored: "+status.error; workspace_snapshot_.reset(); }
    else restored_workspaces_=true;
}
void NativeWindow::workspace_model_changed() {
    // Model commits only: navigation previews and input owners never call this
    // with a different selection. Metadata/geometry changes deduplicate.
    commit_terminal_selection();
    if(navigator_open_)refresh_navigator();
    if (options_.smoke_test || !workspace_snapshot_) return;
    workspace_save_signal_.mark_changed();
    if (hwnd_ && !workspace_timer_)
        workspace_timer_=SetTimer(hwnd_,workspace_save_timer,1500,nullptr)!=0;
}
void NativeWindow::save_workspaces() {
    if ((options_.smoke_test && options_.smoke_workspaces.empty()) || !workspace_snapshot_) return;
    if (workspace_save_busy_) { workspace_save_pending_=true; return; }
    auto captured=capture_workspace_snapshot(projects_);
    if (!captured) { show_error(captured.error); return; }
    workspace_library_.last_session=std::move(*captured.value);
    auto bytes=serialize_workspace_library(workspace_library_);
    if (!bytes) { show_error(bytes.error); return; }
    if (workspace_snapshot_->existed && *bytes.value==workspace_snapshot_->bytes) return;
    if (workspace_worker_.joinable()) workspace_worker_.join();
    const auto path=workspace_file_; const auto baseline=*workspace_snapshot_; const auto library=workspace_library_; const auto shared=wake_;
    workspace_save_busy_=true;
    workspace_worker_=std::thread([path,baseline,library,shared] {
        auto result=save_workspace_library(path,library,baseline);
        { std::lock_guard lock(shared->mutex); shared->workspace_saved=std::move(result); }
        shared->wake();
    });
}
void NativeWindow::workspace_save_completed() {
    std::optional<ConfigSaveResult> completed;
    { std::lock_guard lock(wake_->mutex); completed.swap(wake_->workspace_saved); }
    if (!completed) return;
    if (workspace_worker_.joinable()) workspace_worker_.join();
    workspace_save_busy_=false;
    if (*completed) workspace_snapshot_=std::move(completed->snapshot);
    else {
        notice_="Workspaces were not saved: "+completed->error;
        // Preserve the failed/conflicting file; never silently refresh its token.
        workspace_snapshot_.reset();
        std::fprintf(stderr,"Workspace save failed: %s\n",completed->error.c_str());
    }
    const bool pending=std::exchange(workspace_save_pending_,false);
    if (pending) save_workspaces();
}
void NativeWindow::flush_workspaces() {
    if (options_.smoke_test && options_.smoke_workspaces.empty()) return;
    if (workspace_worker_.joinable()) workspace_worker_.join();
    workspace_save_completed();
    save_workspaces();
    if (workspace_worker_.joinable()) workspace_worker_.join();
    workspace_save_completed();
}
void NativeWindow::launch_restored_project(bool retry_failed) {
    if (options_.smoke_test && options_.smoke_workspaces.empty()) return;
    // Hidden projects remain metadata only until selected. Never launch every
    // saved workspace at boot, and never replay prior commands or parked jobs.
    const auto* project=projects_.active();
    if (!project) return;
    if (!project->metadata.working_directory.empty()) {
        const auto path=std::filesystem::path(widen(project->metadata.working_directory));
        const auto native=path.native();
        if (!path.is_absolute() || native.starts_with(L"\\\\")) {
            show_error("Workspace folder is unavailable. Set its launch folder in Workspace settings; no replacement shell was started."); return;
        }
    }
    if (workspace().state().panes.size()>64) { show_error("Workspace has more than 64 panes; automatic shell launch was skipped."); return; }
    std::vector<PaneId> unbound;
    for (const auto& [id,pane]:workspace().state().panes) {
        if (!pane.session) unbound.push_back(id);
        else if (retry_failed && sessions_.contains(pane.session) && sessions_.at(pane.session)->info().state==SessionState::failed) {
            // Explicit repair only: never restart exited or running commands.
            unbound.push_back(id);
        }
    }
    for (const auto id:unbound) if (workspace().pane(id)->session) workspace().bind_session(id,{});
    retire_unused_sessions();
    for (const auto id:unbound) {
        try {
            const auto actor=new_session(workspace().pane(id)->profile);
            workspace().bind_session(id,actor);
        } catch (const std::exception& error) { show_error(error.what()); break; }
    }
    commit_terminal_selection();
}
void NativeWindow::edit_workspace_text(std::string hint,std::string value,std::function<Status(std::string)> action) {
    acquire_text_overlay_input();
    edit_text_hint_=std::move(hint); edit_text_action_=std::move(action);
    query_=std::move(value); rename_open_=true; palette_open_=false; preedit_.clear(); text_input_.reset(); changed();
}
void NativeWindow::open_workspace_preset(std::size_t index) {
    if (index>=workspace_library_.presets.size()) return;
    const auto& snapshot=workspace_library_.presets[index].snapshot;
    if (snapshot.projects.empty()) { show_error("This saved set contains no workspaces."); return; }
    if (projects_.entries().size()+snapshot.projects.size()>ProjectWorkspaceCatalog::maximum_projects) { show_error("Workspace limit reached."); return; }
    // Opening a preset is additive: current actors/jobs are never terminated.
    // Clones receive new persistent IDs so future agent bindings cannot alias.
    std::vector<ProjectWorkspaceId> added;
    for (const auto& project:snapshot.projects) {
        auto metadata=project.metadata; metadata.persistent_id.clear();
        auto created=projects_.create(std::move(metadata),project.layout);
        if (!created) {
            for (const auto id:added) { auto removed=projects_.remove(id); (void)removed; }
            show_error(created.error); return;
        }
        added.push_back(*created.value);
    }
    activate_project(added[snapshot.active_index]); workspace_model_changed();
}
void NativeWindow::workspace_menu(Point point, ProjectWorkspaceId target) {
    const auto id=target?target:projects_.active_id();
    const auto* project=projects_.find(id); if (!project) return;
    auto metadata=project->metadata;
    std::vector<ContextMenuItem> items;
    std::map<std::string,std::function<void()>> actions;
    auto edit=[this,id](ProjectWorkspaceMetadata value) {
        const auto status=projects_.update_metadata(id,std::move(value));
        if (!status) { show_error(status.error); return; }
        refresh_tooltips(); changed(); workspace_model_changed();
    };
    items.push_back({"settings.reload","Reload settings",{},"Global config.toml · existing terminals stay running",true,false,{}});
    actions["settings.reload"]=[this] { reload_settings(); };
    items.push_back(ContextMenuItem::separator());
    items.push_back({"workspace.name","Rename workspace…",{},{},true,false,{}});
    actions["workspace.name"]=[this,id,metadata] {
        edit_workspace_text("Workspace name",metadata.name,[this,id](std::string value) {return projects_.rename(id,std::move(value));});
    };
    items.push_back({"workspace.close","Close workspace…",{},{},true,true,{}});
    actions["workspace.close"]=[this,id] {
        if (!projects_.find(id)) { show_error("Workspace no longer exists"); return; }
        close_project(id);
    };
    items.push_back(ContextMenuItem::separator());
    items.push_back({"workspace.folder","Launch folder…",{},{},true,false,{}});
    actions["workspace.folder"]=[this,id,metadata] {
        edit_workspace_text("Absolute launch folder",metadata.working_directory,[this,id](std::string value) {
            const auto path=std::filesystem::path(widen(value));
            if (!path.is_absolute() || path.native().starts_with(L"\\\\"))
                return Status::failure("Choose an absolute local folder");
            const auto* target=projects_.find(id); if (!target) return Status::failure("Workspace no longer exists");
            auto updated=target->metadata; updated.working_directory=std::move(value);
            return projects_.update_metadata(id,std::move(updated));
        });
    };
    ContextMenuItem icons; icons.id="workspace.icons"; icons.label="Icon";
    for (const auto* key:{"terminal","code","folder","globe","server","bolt","flask","book","cloud","git"}) {
        const std::string action="workspace.icon."+std::string(key);
        icons.children.push_back({action,std::string(key)+(metadata.icon==key?"  ✓":""),{},{},true,false,{}});
        actions[action]=[edit,metadata,key] () mutable { metadata.icon=key; edit(std::move(metadata)); };
    }
    items.push_back(std::move(icons));
    ContextMenuItem colours; colours.id="workspace.colours"; colours.label="Colour";
    for (const auto& [name,hex]:std::vector<std::pair<std::string,std::string>>{{"Theme",""},{"Blue","#74A7FF"},{"Violet","#B49AFF"},{"Rose","#ED91AF"},{"Amber","#E7BA73"},{"Mint","#79C9AC"},{"Slate","#A6B3C7"}}) {
        const auto action="workspace.colour."+name;
        colours.children.push_back({action,name+(metadata.accent==hex?"  ✓":""),{},{},true,false,{}});
        actions[action]=[edit,metadata,hex] () mutable {metadata.accent=hex;edit(std::move(metadata));};
    }
    colours.children.push_back({"workspace.colour.custom","Custom colour…",{},{},true,false,{}});
    actions["workspace.colour.custom"]=[this,id,metadata] {
        edit_workspace_text("Accent #RRGGBB",metadata.accent,[this,id](std::string value) {
            const auto* target=projects_.find(id); if (!target) return Status::failure("Workspace no longer exists");
            auto updated=target->metadata;updated.accent=std::move(value);return projects_.update_metadata(id,std::move(updated));
        });
    };
    items.push_back(std::move(colours));
    ContextMenuItem shells; shells.id="workspace.shells"; shells.label="Shell for new / restored panes";
    auto shell=[&](const std::string& key,const std::string& label) {
        const auto action="workspace.shell."+key;
        shells.children.push_back({action,label+(metadata.preferred_profile==key?"  ✓":""),{},{},true,false,{}});
        actions[action]=[this,id,edit,metadata,key]() mutable {
            metadata.preferred_profile=key; edit(std::move(metadata));
            if (auto* target=projects_.workspace(id)) {
                auto state=target->state();
                for (auto& [pane_id,pane]:state.panes) { (void)pane_id; if (!pane.session || (sessions_.contains(pane.session) && sessions_.at(pane.session)->info().state==SessionState::failed)) pane.profile=key; }
                auto changed_layout=Workspace::from_state(std::move(state));
                if (changed_layout) *target=std::move(*changed_layout.value);
            }
            workspace_model_changed();
        };
    };
    shell("@default","App default");
    for (const auto& profile:config_.profiles) shell(profile.id,profile.label.empty()?profile.id:profile.label);
    if (managed_shell_profile_ && std::none_of(config_.profiles.begin(),config_.profiles.end(),[](const auto& p){return p.id==managed_zsh_profile_id;})) shell(managed_shell_profile_->id,managed_shell_profile_->label);
    items.push_back(std::move(shells));
    items.push_back({"workspace.retry","Start restored panes",{},{},true,false,{}});
    actions["workspace.retry"]=[this,id]{
        if (!projects_.find(id)) {show_error("Workspace no longer exists");return;}
        if (projects_.active_id()!=id) activate_project(id);
        launch_restored_project(true);changed(true);
    };
    items.push_back(ContextMenuItem::separator());
    items.push_back({"workspace.save_preset","Save all workspaces as set…",{},{},true,false,{}});
    actions["workspace.save_preset"]=[this] {
        edit_workspace_text("Workspace set name","",[this](std::string name) {
            if (!workspace_snapshot_) return Status::failure("Workspace file is unavailable; saved data has not been changed");
            auto captured=capture_workspace_snapshot(projects_); if (!captured) return Status::failure(captured.error);
            return save_workspace_preset(workspace_library_,std::move(name),std::move(*captured.value));
        });
    };
    ContextMenuItem presets; presets.id="workspace.presets";presets.label="Add workspace set";
    ContextMenuItem remove; remove.id="workspace.remove_presets";remove.label="Delete workspace set";
    for (std::size_t i=0;i<workspace_library_.presets.size();++i) {
        const auto action="workspace.preset."+std::to_string(i);const auto name=workspace_library_.presets[i].name;
        presets.children.push_back({action,name,{},{},true,false,{}});actions[action]=[this,i]{open_workspace_preset(i);};
        const auto deletion="workspace.delete_preset."+std::to_string(i);
        remove.children.push_back({deletion,name,{},{},true,false,{}});actions[deletion]=[this,name]{
            show_modal("Delete workspace set?",{name},"Delete workspace set",[this,name]{remove_workspace_preset(workspace_library_,name);workspace_model_changed();});
        };
    }
    if (!presets.children.empty()) {items.push_back(std::move(presets));items.push_back(std::move(remove));}
    items.push_back({"workspace.restore",std::string("Restore workspaces on launch")+(workspace_library_.restore_on_launch?"  ✓":""),{},{},true,false,{}});
    actions["workspace.restore"]=[this]{workspace_library_.restore_on_launch=!workspace_library_.restore_on_launch;workspace_model_changed();changed();};
    open_menu(std::move(items),point,std::move(actions),true);
}
void NativeWindow::reset_project_view(bool preserve_palette_motion) {
    // Local Pane/Tab/Node identifiers can collide across projects. Actor IDs are
    // host-global; renderer identity is SessionId and all local view caches reset.
    hover_transitions_.clear(); tab_transitions_.clear(); pane_focus_.clear();
    if(!preserve_palette_motion) {closing_palette_primitives_.clear(); closing_palette_texts_.clear(); palette_transition_.finish();}
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
    workspace_model_changed();
    launch_restored_project();
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
    if(agent_controller_ && agent_controller_->active(projects_.find(id)->metadata.persistent_id)) {show_error("Interrupt the workspace agent before closing this workspace.");return;}
    bool live=false;
    for (const auto session:projects_.session_ids(id)) if (sessions_.contains(session) && !sessions_.at(session)->process_finished()) live=true;
    if (!approved && live) {
        const auto expected=projects_.session_ids(id);
        show_modal("Close this workspace?",{"Its sessions may have running programs.","Closing can discard unsaved work."},"Close workspace",[this,id,expected]{
            if (!projects_.find(id) || projects_.session_ids(id)!=expected) { show_error("The workspace changed; nothing was closed."); return; }
            close_project(id,true);
        }); return;
    }
    if (projects_.entries().size()==1) { destroy_window(); return; } // Deliberate final close, never a replacement shell.
    // Consent is complete. Transfer the inactive provider to the bounded
    // controller reaper before dropping the workspace; never join on this UI thread.
    if(agent_controller_&&!agent_controller_->retire(projects_.find(id)->metadata.persistent_id)) {
        show_error("Codex is still finishing in this workspace. Try closing it again in a moment.");return;
    }
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    cancel_drag();
    const bool was_active=projects_.active_id()==id;
    const auto removed=projects_.remove(id); if (!removed) { show_error(removed.error); return; }
    workspace_model_changed();
    retire_unused_sessions();
    if (was_active) launch_restored_project();
    reset_project_view();
}
void NativeWindow::show_error(std::string_view error) { notice_ = std::string(error); changed(); }

std::string NativeWindow::pane_title(PaneId id, bool full) const {
    const auto* pane = workspace().pane(id);
    if (!pane) return {};
    std::string result = pane->title;
    const Profile* profile = managed_shell_profile_ && pane->profile==managed_shell_profile_->id?&*managed_shell_profile_:nullptr;
    if (!profile) for (const auto& candidate : config_.profiles) if (candidate.id == pane->profile) { profile = &candidate; break; }
    if (result.empty()) result = profile && !profile->label.empty() ? profile->label : "Terminal";
    (void)full;
    result=pane_context_text(result);
    // The model bounds untrusted titles. DirectWrite performs visual ellipsis;
    // do not byte-truncate UTF-8 or mutate the session title for presentation.
    if (auto* session = session_for(id); session && session->process_finished()) result += " · exited";
    return result;
}
PaneContextPresentation NativeWindow::context_for(const Pane& pane,std::string_view start,const SessionInfo& info) const {
    const Profile* profile=managed_shell_profile_ && pane.profile==managed_shell_profile_->id?&*managed_shell_profile_:nullptr;
    if(!profile)for(const auto& candidate:config_.profiles)if(candidate.id==pane.profile){profile=&candidate;break;}
    // Preserve the existing model-resolved title and its validation bound; raw
    // actor titles rejected by the workspace must not bypass that UI contract.
    auto title=pane.title;
    if(title.empty())title=profile&&!profile->label.empty()?profile->label:"Terminal";
    const bool exited=info.state==SessionState::exited || info.state==SessionState::failed;
    if(exited)title+=" · exited";
    const auto configured=start.empty() && profile?std::string_view(profile->working_directory):start;
    return make_pane_context({title,profile?profile->label:pane.profile,info.working_directory,configured,
        info.shell_context?&*info.shell_context:nullptr,managed_shell_profile_ && pane.profile==managed_shell_profile_->id,exited});
}
PaneContextPresentation NativeWindow::pane_context(PaneId id) const {
    const auto* pane=workspace().pane(id);
    if(!pane)return make_pane_context({});
    // Session metadata is copied once at the existing actor wake. Current
    // workspace/profile labels are resolved here, so edits need no shell wake.
    const auto found=pane_infos_.find(pane->session);
    const SessionInfo unavailable;
    return context_for(*pane,projects_.active()->metadata.working_directory,found==pane_infos_.end()?unavailable:found->second);
}
TabWatchIndicator NativeWindow::tab_watch_indicator(TabId id) const {
    bool waiting=false;
    for(const auto pane:workspace().pane_order(id))if(const auto* item=workspace().pane(pane)) {
        if(watch_unread(item->session))return TabWatchIndicator::unread;
        const auto found=watches_.find(item->session);
        waiting=waiting||(found!=watches_.end()&&found->second.identity()==watch_identity(item->session)&&found->second.status()==WatchStatus::watching);
    }
    return waiting?TabWatchIndicator::watching:TabWatchIndicator::none;
}
std::string NativeWindow::tab_title(TabId id, bool full) const {
    const auto* tab = workspace().tab(id);
    if (!tab) return {};
    auto title=tab->title.empty()?pane_title(tab->active_pane,full):tab->title;
    const auto state=tab_watch_indicator(id);
    return title+(state==TabWatchIndicator::unread?" · result":state==TabWatchIndicator::watching?" · watching":"");
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
    for (const auto& tab:chrome_.tabs) {
        auto detail="Task: "+pane_context_text(tab_title(tab.tab,true));
        if(const auto* model=workspace().tab(tab.tab))detail+="\n"+pane_context(model->active_pane).detail;
        add(tab.bounds,detail+"\nCommands → Inspect pane context");
    }
    if (layout_.panes.size()>1) for (const auto& pane:layout_.panes) add({pane.bounds.x,pane.bounds.y,pane.bounds.width,28},pane_context(pane.pane).detail);
    for (const auto& row:navigation_.projects) if (const auto* project=projects_.find(row.project)) add(row.bounds,project->metadata.name+"\n"+project->metadata.working_directory);
    add(navigation_.toggle,navigation_.expanded?"Collapse workspaces":width_<640?"Navigation stays compact in narrow windows":"Expand workspaces");
    if(ai_enabled_)add(footer_layout().agent,agent_status_label()+"\nOpen agent · Ctrl+Shift+J");
    add(navigation_.settings,"Settings · Ctrl+,"); add(navigation_.new_project,"New workspace… Choose a folder");
    add(navigation_.focus,focus_mode_?"Exit focus mode · Restore workspaces and footer":"Focus mode · Hide workspaces and footer");
    const auto context=footer_presentation(); const auto placement=footer_layout();
    add(placement.directory,pane_context(workspace().active_pane()).detail+"\nSwitch workspace or terminal · Ctrl+Shift+O\nCommands → Inspect pane context"); add(placement.git,context.head); add(placement.status,context.status);
    const auto footer=placement.background;
    for (std::size_t i=0;i<footer.chips.size();++i) add(footer.chips[i],background_title(projects_.active()->parked_sessions[i])+"\nClick to restore · Right-click for actions");
    add(footer.overflow,background_summary().accessible_label()+"\nOpen background terminals · Ctrl+Shift+O opens all terminals");
    if(find_session_){const auto f=find_layout();add(f.previous,"Previous match · Shift+Enter");add(f.next,"Next match · Enter");add(f.close,"Close Find · Escape");add(f.query,"Literal search in this terminal; ASCII case-insensitive");}
    add(chrome_.new_tab,"New tab · Ctrl+Shift+T"); add(chrome_.palette,"Commands · Ctrl+Shift+P"); add(chrome_.menu,"StormTTY menu");
}
double NativeWindow::hover_amount(std::uint64_t id) const {
    const auto found=hover_transitions_.find(id);
    return found==hover_transitions_.end()?0:found->second.value(chrome_now());
}
void NativeWindow::hover_changed(Point point) {
    hover_=point; const auto now=chrome_now(); bool modified=false;
    auto target=[&](std::uint64_t id, Rect bounds) {
        const double next=bounds.contains(point)?1:0;
        auto& transition=hover_transitions_[id]; const double before=transition.value(now);
        transition.target(next,now,reduced_motion_?0:80);
        modified=modified || before!=next;
    };
    target(1,chrome_.menu); target(2,chrome_.new_tab); target(3,chrome_.palette);
    target(12,footer_layout().background.overflow);
    target(8,navigation_.toggle); target(9,navigation_.new_project); target(10,navigation_.focus); target(11,navigation_.settings);
    for (const auto& project:navigation_.projects) target((1ull<<62)|project.project.value,project.bounds);
    target(4,chrome_.minimize); target(5,chrome_.maximize); target(6,chrome_.close); target(7,zoom_return_);
    for (const auto& tab:chrome_.tabs) { target(16+tab.tab.value*2,tab.bounds); target(17+tab.tab.value*2,tab.close); }
    for (const auto& divider:layout_.dividers) target((1ull<<63)|divider.node.value,divider.hit_bounds);
    if (modified) changed();
}
void NativeWindow::motion_schedule() {
    const auto now=chrome_now(); const auto duration=(reduced_motion_ || !IsWindowVisible(hwnd_))?0:120;
    // Only presentation geometry changes on ticks. No Settings actions, live
    // previews, persistence, terminal input or terminal-scroll work is emitted.
    if (smoke_settings_scroll_reduced_motion_.value_or(reduced_motion_) || minimized_ || !IsWindowVisible(hwnd_)) settings_panel_.finish_scroll();
    else settings_panel_.tick_scroll(now);
    const bool pane_reduced=options_.smoke_test && options_.smoke_layout && smoke_focus_running_ && smoke_chrome_time_?
        smoke_pane_reduced_motion_.value_or(reduced_motion_):reduced_motion_;
    const auto pane_duration=pane_decoration_duration(pane_reduced,high_contrast_,IsWindowVisible(hwnd_)!=FALSE,minimized_);
    palette_transition_.target(palette_open_||rename_open_?1:0,now,reduced_motion_?0:100);
    navigator_transition_.target(navigator_open_?1:0,now,(reduced_motion_||high_contrast_)?0:100);
    settings_transition_.target(settings_loading()||settings_input_blocked()?1:0,now,reduced_motion_?0:100);
    menu_transition_.target(context_menu_.is_open()?1:0,now,reduced_motion_?0:100);
    for (const auto& project:projects_.entries()) target_chrome_selection(workspace_focus_,project.id,project.id==projects_.active_id()?1:0,now,duration);
    for (const auto& pane:layout_.panes) target_chrome_selection(pane_focus_,pane.pane,pane.pane==workspace().active_pane()?1:0,now,pane_duration);
    std::erase_if(workspace_focus_,[&](const auto& item){return !projects_.find(item.first);});
    std::erase_if(pane_focus_,[&](const auto& item){return !workspace().pane(item.first);});
    for (const auto& tab:chrome_.tabs) target_chrome_selection(tab_transitions_,tab.tab,tab.tab==workspace().active_tab()?1:0,now,duration);
    std::erase_if(tab_transitions_,[&](const auto& item){return !workspace().tab(item.first);});
    std::erase_if(hover_transitions_,[&](const auto& item){
        if (item.first&(1ull<<63)) return !workspace().node(NodeId{item.first&~(1ull<<63)});
        if (item.first&(1ull<<62)) return !projects_.find(ProjectWorkspaceId{item.first&~(1ull<<62)});
        return item.first>=16 && !workspace().tab(TabId{(item.first-16)/2});
    });
    bool running=settings_panel_.scroll_active();
    auto check=[&](ChromeTransition& transition) { if (reduced_motion_ || minimized_) transition.finish(); running=running||transition.active(now); };
    check(palette_transition_); check(settings_transition_); check(menu_transition_); check(navigator_transition_);
    for (auto& [id,t]:workspace_focus_) { (void)id; check(t); }
    for (auto& [id,t]:pane_focus_) { (void)id; if(!pane_duration)t.finish(); running=running||t.active(now); }
    for (auto& [id,t]:hover_transitions_) { (void)id; check(t); }
    for (auto& [id,t]:tab_transitions_) { (void)id; check(t); }
    if (running && !motion_timer_active_) {
        motion_timer_active_=SetTimer(hwnd_,chrome_motion_timer,16,nullptr)!=0;
        if (!motion_timer_active_) { // Resource pressure must never leave chrome half-hidden.
            settings_panel_.finish_scroll();
            palette_transition_.finish(); settings_transition_.finish(); menu_transition_.finish(); navigator_transition_.finish();
            for (auto& [id,t]:workspace_focus_) { (void)id; t.finish(); }
            for (auto& [id,t]:pane_focus_) { (void)id; t.finish(); }
            for (auto& [id,t]:hover_transitions_) { (void)id; t.finish(); }
            for (auto& [id,t]:tab_transitions_) { (void)id; t.finish(); }
        }
    }
    else if (!running && motion_timer_active_) { KillTimer(hwnd_,chrome_motion_timer); motion_timer_active_=false; }
}
void NativeWindow::settings_wheel(Point position, short delta, UINT lines) {
    const bool discrete=settings_wheel_packet_is_discrete(delta);
    const bool animate=discrete && !smoke_settings_scroll_reduced_motion_.value_or(reduced_motion_) && !minimized_;
    settings_panel_.wheel_pixels(position,settings_wheel_distance(delta,lines,lines==WHEEL_PAGESCROLL,settings_panel_.layout().content.height),chrome_now(),animate);
    settings_input_changed();
}
void NativeWindow::reload_settings() {
    if (modal_) { show_error("Finish the current confirmation before reloading settings."); return; }
    if (options_.config_path.empty()) { show_error("No settings file path is available."); return; }
    const auto allowed=settings_reload_.begin(settings_panel_.is_open(),settings_job_);
    if (!allowed) { show_error(allowed.error); if(settings_panel_.is_open())settings_draft_notice_=notice_; return; }
    if (settings_worker_.joinable()) settings_worker_.join(); // Completed jobs only.
    settings_job_=true;
    try {
        settings_worker_=std::thread([shared=wake_,path=options_.config_path,generation=settings_reload_.generation()] {
            SettingsCompletion result; result.kind=SettingsCompletion::Kind::reload; result.generation=generation;
            try { result.loaded=load_settings_snapshot(path); }
            catch (const std::exception& e) { result.loaded.error=e.what(); }
            catch (...) { result.loaded.error="Settings could not be reloaded."; }
            { std::lock_guard lock(shared->mutex); shared->settings=std::move(result); }
            shared->wake();
        });
    } catch (...) { settings_job_=false; settings_reload_.cancel(); show_error("Could not start settings reload; current settings were kept."); return; }
    notice_="Reloading settings…"; changed();
}
void NativeWindow::open_settings(std::string binding) {
    if(navigator_open_)close_navigator();
    if(find_session_)close_find();
    if (modal_ || settings_input_blocked() || (settings_job_&&!settings_load_job_)) return;
    if (options_.config_path.empty()) { show_error("No settings file path is available. Start with --config and an absolute local path."); return; }
    if (!settings_job_&&settings_worker_.joinable()) settings_worker_.join(); // Prior completed worker only.
    if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    cancel_drag();
    const auto path=options_.config_path;
    auto shared=wake_;
    settings_binding_=std::move(binding); settings_open_requested_=true;
    if(!settings_job_) {
    settings_job_=settings_load_job_=true;
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
    } catch (...) { settings_job_=settings_load_job_=false; settings_open_requested_=false; throw; }
    }
    context_menu_.cancel(); menu_actions_.clear();
    palette_open_=rename_open_=false; query_.clear(); preedit_.clear(); composing_=false; text_input_.reset();
    palette_transition_=ChromeTransition{}; closing_palette_primitives_.clear(); closing_palette_texts_.clear();
    if (ImmContext context(hwnd_); context.context) ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
    notice_.clear(); caret_schedule(); changed();
}
void NativeWindow::settings_completed() {
    if(options_.smoke_settings&&smoke_hold_settings_completion_)return;
    std::optional<SettingsCompletion> completion;
    { std::lock_guard lock(wake_->mutex); completion.swap(wake_->settings); }
    if (!completion) return;
    if (settings_worker_.joinable()) settings_worker_.join(); // I/O ended before result publication.
    settings_job_=settings_load_job_=false;
    if (completion->kind==SettingsCompletion::Kind::reload) {
        auto result=settings_reload_.complete(completion->generation,std::move(completion->loaded));
        if (result.ignored) return;
        if (!result.config) { show_error(result.error); return; }
        // No editor can open/save during this serialized read. Keep the old
        // value on a presentation failure; no session lifecycle calls here.
        const auto previous=config_;
        try { preview_settings(*result.config); }
        catch (const std::exception& e) {
            // A renderer may have installed its font before measurement failed.
            // Force the rollback path to restore it even if Config was not yet updated.
            config_.font=result.config->font;
            try { preview_settings(previous); } catch (...) {}
            config_=previous; show_error(std::string("Reload failed; previous settings retained. Presentation may need a restart: ")+e.what()); return;
        }
        config_=std::move(*result.config);
        settings_snapshot_.reset(); // The next editor obtains its own exact baseline.
        notice_="Settings reloaded.";
        if(config_.experimental_ai!=ai_enabled_)notice_+=" Restart StormTTY to apply the feature change.";
        caret_schedule(true); changed(true);
        return;
    }
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
        agent_panel_.resize({width_,height_}); settings_panel_.resize({width_,height_}); context_menu_.resize({width_,height_}); settings_binding_.clear(); notice_.clear();
        if (tooltip_) SendMessageW(tooltip_,TTM_POP,0,0);
        SetFocus(hwnd_); settings_input_changed(); caret_schedule(); changed();
    } else {
        if (completion->saved) {
            if (options_.smoke_settings) smoke_settings_file_owned_=true;
            settings_snapshot_=std::move(completion->saved.snapshot);
            settings_panel_.persistence_result(true);
            settings_draft_notice_.clear();
            // Persistence succeeded. A later GPU/font failure must not strand
            // the editor in Saving or imply that the file write was rolled back.
            std::string apply_error;
            try { preview_settings(completion->candidate); }
            catch (const std::exception& error) { apply_error=error.what(); }
            config_=std::move(completion->candidate);
            notice_=apply_error.empty()?(!options_.console_runtime_directory.empty()?
                "Settings saved. Console launch override is still active.":managed_shell_profile_ && options_.shell_runtime_override?
                "Settings saved. Shell launch override is still active.":
                "Settings saved."):
                "Settings saved, but presentation could not update. Restart to apply: "+apply_error;
            if(config_.experimental_ai!=ai_enabled_)notice_+=" Restart StormTTY to apply the feature change.";
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
        std::array<Color,16> ansi;
        for (std::size_t i=0;i<16;++i) ansi[i]=color_hex(candidate.theme.ansi[i],{0,0,0,255});
        for (const auto& [id,session]:sessions_) { (void)id; session->set_default_colors(foreground,background,ansi); }
        config_.theme=candidate.theme;
    }
    config_.font=candidate.font; config_.appearance=candidate.appearance; config_.ribbon=candidate.ribbon;
    settings_changed(); caret_schedule(); refresh_tooltips(); changed(font_changed);
}
void NativeWindow::settings_actions() {
    while (auto action=settings_panel_.take_action()) {
        if (action->kind==SettingsActionKind::import_theme || action->kind==SettingsActionKind::export_theme) {
            const auto generation=settings_panel_.generation();
            const auto picked=pick_theme(hwnd_,action->kind==SettingsActionKind::import_theme,action->config.theme);
            // Native dialogs pump messages. Never apply to a closed/reopened or
            // otherwise changed Settings draft after returning from that loop.
            if (!hwnd_ || !settings_panel_.is_open() || settings_panel_.generation()!=generation) continue;
            if (!picked.error.empty()) settings_panel_.theme_exchange_error(picked.error);
            else {
                if (picked.imported) settings_panel_.import_palette(*picked.imported);
                if (!picked.notice.empty()) settings_panel_.theme_exchange_notice(picked.notice);
            }
        } else if (action->kind==SettingsActionKind::preview || action->kind==SettingsActionKind::cancel) {
            preview_settings(action->config);
            if (action->kind==SettingsActionKind::cancel) {
                if(notice_==settings_draft_notice_)notice_.clear();
                settings_draft_notice_.clear();text_input_.reset(); caret_schedule(true);
            }
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
    context.supports_previous_terminal=true;
    const auto previous=previous_terminal_availability(previous_terminal_target().state);
    context.previous_terminal_available=previous.enabled;context.previous_terminal_reason=previous.reason;
    context.supports_settings_reload=true; context.supports_pane_context=true;context.supports_command_watch=true;
    context.supports_agent_panel=ai_enabled_;
    context.supports_background_sessions=true; context.parked_sessions=projects_.active()->parked_sessions.size();
    context.supports_workspace_navigator=true;context.supports_project_workspaces=true; context.project_workspaces=projects_.entries().size(); context.supports_focus_mode=true;
    context.tabs = workspace().state().tabs.size();
    context.total_panes = workspace().state().panes.size();
    context.panes = workspace().pane_order(workspace().active_tab()).size();
    context.clipboard_has_text = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
    if (const auto* tab = workspace().tab(workspace().active_tab())) context.zoomed = tab->zoomed_pane.has_value();
    if (auto* session = active_session()) {
        context.session_running = !session->process_finished();
        const auto id=workspace().pane(workspace().active_pane())->session;const auto found=watches_.find(id);
        const auto info=session->info();context.watch_can_arm=found==watches_.end()&&CommandWatch::eligible(watch_identity(id),info,true);
        context.watch_waiting=found!=watches_.end()&&found->second.status()==WatchStatus::watching;
        context.watch_unread=found!=watches_.end()&&found->second.unread();
        if (auto frame = session->frame()) for (const auto& row : frame->lines) if (row && row->selection) { context.has_selection = true; break; }
    }
    return context;
}
CommandAvailability host_availability(std::string_view id, const CommandContext& context) {
    return command_availability(id, context);
}
TerminalFindLayout NativeWindow::find_layout() const {
    for(const auto& pane:layout_.panes)if(pane.session==find_session_)return layout_terminal_find(pane.bounds,layout_.panes.size()>1?28:0);
    return {};
}
std::pair<std::string,double> NativeWindow::find_query_view() const {
    const auto bounds=find_layout().query;
    return terminal_find_visible(find_query_,std::max(0.0,bounds.width-34),[this](std::string_view text){
        ChromeText measure;measure.text=std::string(text);measure.size_dip=13;measure.rect={0,0,100000,28*static_cast<float>(scale_)};
        return renderer_->measure_chrome_text(measure).width/scale_;
    });
}
void NativeWindow::refresh_navigator() {
    if(!navigator_open_)return;
    const auto selected=navigator_.target();
    std::vector<NavigatorWorkspace> entries;
    const auto text=[](std::string_view value){return agent_display_text(value,512);};
    for(const auto& project:projects_.entries()) {
        NavigatorWorkspace item{project.id,text(project.metadata.name),text(project.metadata.working_directory),{}};
        auto append=[&](SessionId id,std::string_view user_title,std::string_view profile,bool parked) {
            if(navigator_background_only_&&!parked)return;
            const auto actor=sessions_.find(id);if(actor==sessions_.end())return;
            const auto info=actor->second->info();
            std::string profile_label(profile);for(const auto& p:config_.profiles)if(p.id==profile)profile_label=p.label;
            if(managed_shell_profile_&&managed_shell_profile_->id==profile)profile_label=managed_shell_profile_->label;
            auto title=background_session_title(user_title,info.title,profile_label);if(title.empty())title="Terminal";
            std::string state=terminal_status_label(info);
            if(!watch_label(id).empty())state+=" · "+watch_label(id);
            if(parked)state="Background · "+state;
            const bool managed=managed_shell_profile_&&managed_shell_profile_->id==profile;
            const auto directory=info.shell_context?info.shell_context->display_directory:managed?std::string{}:info.working_directory;
            item.terminals.push_back({id,text(title),text(directory),state,parked,active_session()==actor->second.get()});
        };
        for(const auto& tab:project.workspace.state().tabs)for(const auto pane_id:project.workspace.pane_order(tab.id))if(const auto* pane=project.workspace.pane(pane_id))append(pane->session,tab.title,pane->profile,false);
        for(const auto& parked:project.parked_sessions)append(parked.session,parked.user_title,parked.profile,true);
        entries.push_back(std::move(item));
    }
    SessionId active{};if(const auto* pane=workspace().pane(workspace().active_pane()))active=pane->session;
    navigator_.open(std::move(entries),selected?selected->project:projects_.active_id(),selected?selected->session:active);
    navigator_.search(navigator_query_.text());
}
void NativeWindow::open_navigator(bool background_only) {
    if(modal_||overlay_open())return;
    if(width_<320||height_<260){show_error("Enlarge the window to use the workspace navigator.");return;}
    if(navigator_open_){close_navigator();return;}
    if(find_session_)close_find();
    cancel_drag();palette_open_=rename_open_=false;query_.clear();
    navigator_=WorkspaceNavigator{};navigator_query_=TerminalFindQuery{};navigator_focus_=0;navigator_switch_pressed_=false;
    closing_palette_primitives_.clear();closing_palette_texts_.clear();palette_transition_=ChromeTransition{};
    navigator_background_only_=background_only;navigator_open_=true;refresh_navigator();preedit_.clear();composing_=false;text_input_.reset();
    if(ImmContext context(hwnd_);context.context)ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
    changed();caret_schedule();position_ime();
}
void NativeWindow::close_navigator() {
    if(!navigator_open_)return;
    navigator_open_=false;navigator_switch_pressed_=false;
    if(GetCapture()==hwnd_)ReleaseCapture();
    preedit_.clear();composing_=false;text_input_.reset();
    if(ImmContext context(hwnd_);context.context)ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
    changed();caret_schedule(true);
}
void NativeWindow::navigator_input_changed() {navigator_.search(navigator_query_.text());changed();position_ime();}
void NativeWindow::commit_navigator() {
    const auto target=navigator_.target();
    if(!target||!target->session)return;
    auto* project=projects_.workspace(target->project);
    if(!project||!sessions_.contains(target->session)){refresh_navigator();changed();return;}
    PaneId pane{};for(const auto& [id,item]:project->state().panes)if(item.session==target->session){pane=id;break;}
    const auto* entry=projects_.find(target->project);
    const bool parked=std::any_of(entry->parked_sessions.begin(),entry->parked_sessions.end(),[&](const auto& item){return item.session==target->session;});
    if((target->parked&&!parked)||(!target->parked&&!pane)){refresh_navigator();changed();return;}
    if(parked && project->state().tabs.size()>=Workspace::maximum_tabs){show_error("Tab limit reached; close a tab before restoring this terminal.");return;}
    close_navigator();
    // Direct catalog activation deliberately does not launch dormant restored
    // shells. This command only focuses or reattaches the validated live actor.
    const auto activated=projects_.activate(target->project);if(!activated){show_error(activated.error);return;}
    if(parked){restore_parked({target->project,target->session});return;}
    const auto tab=project->tab_of(pane);if(tab)project->activate_tab(*tab);
    project->focus(pane);workspace_model_changed();reset_project_view();
}
std::pair<std::string,double> NativeWindow::navigator_query_view() const {
    const auto layout=layout_workspace_navigator(width_,height_,navigator_,navigator_focus_);
    return terminal_find_visible(navigator_query_,std::max(0.0,layout.query.width-28),[this](std::string_view text){ChromeText measure;measure.text=std::string(text);measure.size_dip=16;measure.rect={0,0,100000,32*static_cast<float>(scale_)};return renderer_->measure_chrome_text(measure).width/scale_;});
}

void NativeWindow::open_find() {
    if(navigator_open_)close_navigator();
    if(modal_||overlay_open())return;
    const auto* pane=workspace().pane(workspace().active_pane());
    if(!pane||!sessions_.contains(pane->session))return;
    for(const auto& place:layout_.panes)if(place.pane==pane->id && !terminal_find_fits(place.bounds,layout_.panes.size()>1)) {show_error("Enlarge this pane to use Find.");return;}
    if(find_session_ && find_session_!=pane->session)close_find();
    find_session_=pane->session; find_focus_=0; find_pressed_.reset();find_query_.select_all();
    preedit_.clear();composing_=false;
    if(ImmContext context(hwnd_);context.context)ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
    palette_open_=rename_open_=false;query_.clear();typing_motion_.reset();text_input_.reset();
    changed(true);caret_schedule();submit_find();
}
void NativeWindow::close_find(bool geometry) {
    if(!find_session_)return;
    if(find_pressed_ && GetCapture()==hwnd_)ReleaseCapture();
    if(auto actor=sessions_.find(find_session_);actor!=sessions_.end())(void)actor->second->cancel_search();
    find_session_={};find_request_=0;find_initial_selection_.cancel();find_result_.reset();find_pressed_.reset();find_error_.clear();
    preedit_.clear();composing_=false;text_input_.reset();
    if(ImmContext context(hwnd_);context.context)ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
    changed(geometry);caret_schedule(true);
}
void NativeWindow::submit_find() {
    if(!find_session_||!sessions_.contains(find_session_))return;
    find_error_.clear();find_result_.reset();
    auto actor=sessions_.at(find_session_);
    (void)actor->clear_selection(); // A replaced/no-match query must not leave an old match highlighted.
    const auto request=find_query_.text().empty()?actor->cancel_search():actor->search({find_query_.text()});
    find_request_=request.request_id;find_initial_selection_.begin(find_query_.text().empty()?0:request.request_id);
    if(request.result!=SessionSendResult::accepted)find_error_="Search unavailable";
    changed();position_ime();
}
void NativeWindow::refresh_find() {
    if(!find_session_)return;
    const auto* pane=workspace().pane(workspace().active_pane());
    if(!pane||pane->session!=find_session_||!sessions_.contains(find_session_)){close_find();return;}
    auto actor=sessions_.at(find_session_);auto result=actor->search_snapshot();
    if(!result||!current_find_result(*result,find_request_,find_query_.text()))return;
    if(find_result_!=result){
        find_result_=std::move(result);++chrome_generation_;
        if(options_.smoke_projects && smoke_find_trace_count_++<64)
            std::fprintf(stderr,"Native Find trace: stage=%u request=%llu revision=%llu generation=%llu status=%d matches=%zu selected=%lld apply=%d auto=%llu attempted_revision=%llu attempted_generation=%llu\n",smoke_find_stage_,static_cast<unsigned long long>(find_result_->token.request_id),static_cast<unsigned long long>(find_result_->token.revision),static_cast<unsigned long long>(find_result_->token.generation),int(find_result_->status),find_result_->matches.size(),find_result_->selected_index?static_cast<long long>(*find_result_->selected_index):-1,find_result_->last_apply_result?int(*find_result_->last_apply_result):-1,static_cast<unsigned long long>(find_initial_selection_.request()),static_cast<unsigned long long>(find_initial_selection_.attempted()?find_initial_selection_.attempted()->revision:0),static_cast<unsigned long long>(find_initial_selection_.attempted()?find_initial_selection_.attempted()->generation:0));
    }
    if(const auto token=find_initial_selection_.take(*find_result_)) {
        const auto accepted=actor->select_search_match(*token,0);
        if(options_.smoke_projects && smoke_find_trace_count_<64)std::fprintf(stderr,"Native Find auto-select: revision=%llu generation=%llu accepted=%d\n",static_cast<unsigned long long>(token->revision),static_cast<unsigned long long>(token->generation),int(accepted));
    }
}
void NativeWindow::navigate_find(bool previous) {
    find_initial_selection_.cancel(); // Explicit navigation owns selection from here.
    if(!find_session_||!sessions_.contains(find_session_))return;
    // Read a fresh owned snapshot at dispatch; the core still verifies all three
    // token generations after queued output/resize, so stale cells never apply.
    auto actor=sessions_.at(find_session_);const auto result=actor->search_snapshot();
    if(result && current_find_result(*result,find_request_,find_query_.text()) && result->status==SearchStatus::complete && !result->matches.empty())
        (void)actor->navigate_search(result->token,previous?SearchDirection::backward:SearchDirection::forward,true);
}
void NativeWindow::activate_find_control(unsigned index) {
    if(index==3)close_find();else if(index==1||index==2)navigate_find(index==1);
}
void NativeWindow::acquire_text_overlay_input() {
    // Deliver the final captured release while the old terminal owner is still
    // eligible. Only then transfer pointer/IME ownership to the text overlay.
    if(mouse_capture_pane_)terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
    cancel_drag();text_input_.reset();composing_=false;preedit_.clear();
    if(ImmContext context(hwnd_);context.context)ImmNotifyIME(context.context,NI_COMPOSITIONSTR,CPS_CANCEL,0);
}
void NativeWindow::toggle_palette() {
    if(navigator_open_)close_navigator();
    if(find_session_)close_find();
    palette_background_only_=false; inspected_context_.reset();
    if(!palette_open_)acquire_text_overlay_input();
    palette_open_ = !palette_open_; rename_open_ = false; query_.clear(); palette_index_ = 0;
    composing_ = false; preedit_.clear();
    if (ImmContext context(hwnd_); context.context) ImmNotifyIME(context.context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    caret_schedule(); changed();
}
std::vector<NativeWindow::PaletteEntry> NativeWindow::palette_commands() const {
    std::vector<PaletteEntry> result;
    if(inspecting_context()) {
        // Bounded viewport and keyboard paging reuse the existing Commands input
        // owner. This frozen owned snapshot has no action IDs or PTY operations.
        const auto width=std::max(40.0,std::min(640.0,width_-32)-72);
        const auto& lines=inspected_lines_.lines(inspected_context_->detail,width,scale_,[this](std::string_view text,double wrap_width,double scale){
            ChromeText item;item.text=std::string(text);item.size_dip=14;item.semibold=true;
            item.rect={0,0,static_cast<float>(wrap_width*scale),28*static_cast<float>(scale)};
            return renderer_->wrap_chrome_text(item); // One native shaping/layout, not one per scalar.
        });
        for(const auto& line:lines)
            if(command_search_score(query_,line,"Pane context"))result.push_back({{},line,"Read-only snapshot · Escape returns to terminal",{},0});
        return result;
    }

    if (!palette_background_only_) for (const auto& command:command_registry()) {
        std::string description(command.description);
        if(command.id=="terminal.previous") {
            const auto target=previous_terminal_target();
            if(target.state==PreviousTerminalState::available)description=target.description;
        }
        const auto search_description=command.id=="terminal.previous"?std::string(command.description)+" "+description:description;
        if ((ai_enabled_||command.id!="agent.toggle")) if (const auto score=command_search_score(query_,command.label,search_description))
            result.push_back({std::string(command.id),std::string(command.label),description,{},*score});
    }
    // Only the current workspace: restoration must never silently switch projects.
    // Titles are presentation/search data, never interpreted as commands or IDs.
    for (const auto& parked:projects_.active()->parked_sessions) {
        const auto title=(palette_background_only_?"":"Restore · ")+background_title(parked,false);
        auto context=projects_.active()->metadata.name;
        if (const auto actor=sessions_.find(parked.session); actor!=sessions_.end()) {
            const auto info=actor->second->info();
            const bool managed=managed_shell_profile_ && parked.profile==managed_shell_profile_->id;
            // Match the footer: typed context is display data; a managed clear
            // must not resurrect the previous raw OSC 7 URI in search results.
            auto cwd=info.shell_context?info.shell_context->display_directory:
                managed?managed_shell_profile_->label:info.working_directory;
            std::erase_if(cwd,[](unsigned char c){return c<0x20 || c==0x7f;});
            if (!cwd.empty() && cwd!=context) context+=(context.empty()?"":" · ")+cwd;
        }
        if (const auto score=command_search_score(query_,title,context))
            result.push_back({{},title,context,ProjectSessionReference{projects_.active_id(),parked.session},*score});
    }
    if(!palette_background_only_)for(const auto& project:projects_.entries())for(const auto& [id,watch]:watches_) {
        if(!watch.unread()||watch.identity()!=watch_identity(id))continue;
        bool belongs=false;std::string terminal;
        for(const auto& [pane,item]:project.workspace.state().panes)if(item.session==id){(void)pane;belongs=true;terminal=item.title;}
        for(const auto& parked:project.parked_sessions)if(parked.session==id){belongs=true;terminal=background_title(parked,false);}
        if(!belongs)continue;
        const auto title="Open result · "+pane_context_text(project.metadata.name)+" · "+agent_display_text(terminal,512);
        const auto description=watch_label(id);
        if(const auto score=command_search_score(query_,title,description))result.push_back({"watch.result",title,description,ProjectSessionReference{project.id,id},*score,watch.identity()});
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.score>b.score;});
    return result;
}
NativeWindow::PaletteLayout NativeWindow::palette_layout(std::span<const PaletteEntry> entries) const {
    std::vector<double> heights;
    heights.reserve(entries.size());
    for (const auto& entry:entries) heights.push_back(entry.session || !palette_availability(entry).enabled?48.0:36.0);
    return layout_command_palette(width_,height_,heights,palette_index_,rename_open_);
}
CommandAvailability NativeWindow::palette_availability(const PaletteEntry& entry) const {
    if (inspecting_context()) return {true,{}};
    if(entry.watch) {
        if(!entry.session||watch_identity(entry.session->local)!=*entry.watch||!watch_unread(entry.session->local))return {false,"This watched actor is no longer available"};
        return {true,{}};
    }
    if (!entry.session) return host_availability(entry.id,command_context());
    const auto target=*entry.session;
    const auto* project=projects_.find(target.project);
    if (!project || target.project!=projects_.active_id() || !sessions_.contains(target.local) ||
        std::none_of(project->parked_sessions.begin(),project->parked_sessions.end(),[&](const auto& item){return item.session==target.local;}))
        return {false,"This background terminal is no longer available"};
    if (project->workspace.state().tabs.size()>=Workspace::maximum_tabs) return {false,"Tab limit reached"};
    return {true,{}};
}
void NativeWindow::activate_palette_entry(const PaletteEntry& entry) {
    if(inspecting_context())return;
    const auto available=palette_availability(entry);
    if (!available.enabled) { show_error(available.reason); return; }
    // Inspection inherits the Commands input owner; do not pulse terminal focus
    // between two views of the same read-only overlay.
    if(!entry.session && entry.id=="pane.inspect") { dispatch(entry.id); return; }
    palette_open_=false; query_.clear();
    if(entry.watch&&entry.session)open_watch(*entry.session,*entry.watch);
    else if (entry.session) restore_parked(*entry.session);
    else dispatch(entry.id);
}
void NativeWindow::dispatch(std::string_view command) {
    if (modal_ || overlay_open()) return;
    if (drag_!=Drag::none || mouse_capture_pane_) cancel_drag();
    if (!host_availability(command, command_context()).enabled) { show_error(host_availability(command, command_context()).reason); return; }
    notice_.clear();
    const auto pane = workspace().active_pane();
    const auto tab = workspace().active_tab();
    const Rect viewport=workspace_viewport();
    auto report = [this](Status status) { if (!status) show_error(status.error); else workspace_model_changed(); };
    if (command == "workspace.navigator") {open_navigator();return;}
    if(navigator_open_)close_navigator();
    if (command == "terminal.find") { open_find(); return; }
    if (find_session_) close_find();
    if (command == "terminal.previous") { return_to_previous_terminal(); return; }
    if (command == "settings.reload") { reload_settings(); return; }
    if (command == "settings.open") { open_settings(); return; }
    if (command == "app.command_palette") { toggle_palette(); return; }
    if (command == "workspace.settings") { workspace_menu({navigation_.rail.x+16,chrome_.band.height+44}); return; }
    if (command == "workspace.new") { choose_project_folder(); return; }
    if (command == "agent.toggle") {toggle_agent_panel();return;}
    if (command == "workspace.close") { close_project(projects_.active_id()); return; }
    if (command == "workspace.toggle_navigation") {
        if (width_<640 && !focus_mode_) { show_error("Widen the window to expand workspaces. The compact rail remains available."); return; }
        navigation_expanded_=!navigation_expanded_; focus_mode_=false; refresh_clock(); changed(true); return;
    }
    if (command == "window.focus_mode") { focus_mode_=!focus_mode_; refresh_clock(); changed(true); return; }
    if (command == "workspace.next" || command == "workspace.previous") {
        const auto& entries=projects_.entries();
        const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& item){return item.id==projects_.active_id();});
        const auto index=static_cast<std::size_t>(found-entries.begin());
        const auto next=(index+(command=="workspace.next"?1:entries.size()-1))%entries.size();
        activate_project(entries[next].id); return;
    }
    if (command == "tab.new") { new_tab(); return; }
    if (command == "tab.close") { close_tab(tab); return; }
    if (command == "background.restore") {
        if (!palette_open_) toggle_palette();
        palette_background_only_=true; query_.clear(); palette_index_=0; changed(); return;
    }
    if(command.starts_with("watch.")) {
        const auto* item=workspace().pane(pane);if(!item||!sessions_.contains(item->session))return;
        const auto id=item->session;
        if(command=="watch.arm") {CommandWatch watch;if(!watches_.contains(id)&&watch.arm(watch_identity(id),sessions_.at(id)->info(),true))watches_.emplace(id,std::move(watch));else show_error("Available while shell integration reports a running command; dismiss any previous watch first.");}
        else if(command=="watch.cancel") {const auto found=watches_.find(id);if(found!=watches_.end()&&found->second.status()==WatchStatus::watching)watches_.erase(found);}
        else if(command=="watch.dismiss")acknowledge_watch(id);
        refresh_tooltips();changed();return;
    }
    if (command == "pane.inspect") {
        acquire_text_overlay_input(); inspected_context_=pane_context(pane);
        palette_open_=true; rename_open_=false; palette_background_only_=false; query_.clear(); palette_index_=0; changed(); return;
    }
    if (command == "pane.background") { park_pane(pane); return; }
    if (command == "pane.close") { close_pane(pane); return; }
    if (command == "pane.split_right") { split(Direction::right); return; }
    if (command == "pane.split_down") { split(Direction::down); return; }
    if (command == "edit.copy") { copy(); return; }
    if (command == "edit.paste") { paste(); return; }
    if (command == "window.fullscreen") { toggle_fullscreen(); return; }
    if (command == "tab.rename") {
        acquire_text_overlay_input();
        edit_text_action_={}; rename_open_ = true; palette_open_ = false; rename_tab_ = tab; query_ = workspace().tab(tab)->title; changed(); return;
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
        if (!result) show_error(result.error); else workspace_model_changed();
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
                if (!result) show_error(result.error); else workspace_model_changed(); changed(true);
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
    const auto ui=resolve_ui_colors(config_.theme);
    const Color background = high_contrast_ ? system_color(COLOR_WINDOW) : ui.canvas;
    Color chrome = high_contrast_ ? system_color(COLOR_BTNFACE) : ui.surface;
    const bool daylight=theme_is_light(config_.theme);
    Color raised = high_contrast_ ? system_color(COLOR_WINDOW) : ui.raised;
    const Color text = high_contrast_ ? system_color(COLOR_BTNTEXT) : ui.ink;
    const Color secondary = high_contrast_ ? text : ui.secondary;
    const Color accent = high_contrast_ ? system_color(COLOR_WINDOWTEXT) : ui.accent;
    const Color seam = high_contrast_ ? text : ui.divider;
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
    const auto now=chrome_now();
    auto opacity=[](Color color,double amount) { color.a=static_cast<std::uint8_t>(std::clamp(std::lround(color.a*amount),0L,255L)); return color; };
    auto blend=[](Color a,Color b,double t) {
        auto channel=[&](int x,int y){return static_cast<std::uint8_t>(std::lround(x+(y-x)*t));};
        return Color{channel(a.r,b.r),channel(a.g,b.g),channel(a.b,b.b),channel(a.a,b.a)};
    };
    if (navigation_.rail.width>0) {
        Color rail=high_contrast_?background:ui.navigation;
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
            const double focus=workspace_focus_[row.project].value(now);
            if (focus>0||amount>0) rectangle(row.bounds,opacity(high_contrast_?raised:blend(raised,ui.selected,focus*.25),std::max(focus,amount*.65)),4);
            const auto icon=color_hex(project->metadata.accent,blend(secondary,accent,focus));
            const double x=row.bounds.x+10,y=row.bounds.y+10;
            if (project->metadata.icon=="folder") {
                rectangle({x,y+3,16,10},{0,0,0,0},1,icon,1);
                line(x,y+3,x,y,icon); line(x,y,x+6,y,icon); line(x+6,y,x+9,y+3,icon);
            } else if (project->metadata.icon=="code") {
                line(x+5,y+2,x,y+7,icon); line(x,y+7,x+5,y+12,icon);
                line(x+11,y+2,x+16,y+7,icon); line(x+16,y+7,x+11,y+12,icon);
            } else if (project->metadata.icon=="terminal") {
                rectangle({x,y,16,14},{0,0,0,0},2,icon,1);
                line(x+3,y+4,x+6,y+7,icon); line(x+6,y+7,x+3,y+10,icon); line(x+9,y+10,x+13,y+10,icon);
            } else if (project->metadata.icon=="bolt") {
                line(x+10,y,x+3,y+8,icon);line(x+3,y+8,x+9,y+8,icon);
                line(x+9,y+8,x+6,y+15,icon);line(x+6,y+15,x+14,y+5,icon);line(x+14,y+5,x+8,y+5,icon);
            } else if (project->metadata.icon=="globe") {
                rectangle({x,y,16,16},{0,0,0,0},8,icon,1);
                rectangle({x+5,y,6,16},{0,0,0,0},3,icon,1);line(x,y+8,x+16,y+8,icon);
            } else if (project->metadata.icon=="git") {
                line(x+4,y+3,x+4,y+14,icon);line(x+4,y+10,x+12,y+6,icon);line(x+12,y+6,x+12,y+3,icon);
                rectangle({x+2,y,4,4},{0,0,0,0},2,icon,1);rectangle({x+10,y,4,4},{0,0,0,0},2,icon,1);
                rectangle({x+2,y+12,4,4},{0,0,0,0},2,icon,1);
            } else if (project->metadata.icon=="flask") {
                line(x+5,y,x+11,y,icon);line(x+6,y,x+6,y+5,icon);line(x+10,y,x+10,y+5,icon);
                line(x+6,y+5,x+1,y+14,icon);line(x+10,y+5,x+15,y+14,icon);line(x+1,y+14,x+15,y+14,icon);
                line(x+4,y+9,x+12,y+9,icon);
            } else if (project->metadata.icon=="cloud") {
                rectangle({x,y+6,16,9},{0,0,0,0},4,icon,1);rectangle({x+4,y+1,9,11},{0,0,0,0},5,icon,1);
            } else {
                rectangle({x,y,16,15},{0,0,0,0},2,icon,1);
                if (project->metadata.icon=="book") {line(x+8,y,x+8,y+15,icon);line(x+3,y+4,x+6,y+4,icon);line(x+10,y+4,x+13,y+4,icon);}
                else {line(x,y+5,x+16,y+5,icon);line(x,y+10,x+16,y+10,icon);}
            }
            if (selected) rectangle({row.bounds.x+1,row.bounds.y+7,2,row.bounds.height-14},icon,1);
            if(!selected&&!navigation_.expanded&&project_watch_unread(project->id))rectangle({row.bounds.x+row.bounds.width-7,row.bounds.y+6,4,4},accent,2);
            if (navigation_.expanded) label({row.bounds.x+38,row.bounds.y+7,row.bounds.width-44,20},project->metadata.name+(!selected&&project_watch_unread(project->id)?" · result":""),blend(secondary,text,focus),13,selected);
        }
        if (const auto amount=hover_amount(9); amount>0) rectangle(navigation_.new_project,opacity(raised,amount),5);
        if (navigation_.new_project.height>0) {
            const double x=navigation_.new_project.x+18,y=navigation_.new_project.y+18;
            line(x-5,y,x+5,y,secondary); line(x,y-5,x,y+5,secondary);
            if (navigation_.expanded) label({navigation_.new_project.x+38,navigation_.new_project.y+8,navigation_.new_project.width-44,20},"New workspace",secondary,12);
        }
        if (navigation_.settings.height>0) {
            if (const auto amount=hover_amount(11); amount>0) rectangle(navigation_.settings,opacity(raised,amount),5);
            append_settings_icon(result,{navigation_.settings.x+18,navigation_.settings.y+18},8,secondary,static_cast<float>(scale_),1.2,z);
            if (navigation_.expanded) label({navigation_.settings.x+38,navigation_.settings.y+8,navigation_.settings.width-44,20},"Settings",secondary,12);
        }
    }
    for (const auto& placement : layout_.panes) {
        if (auto* session = session_for(placement.pane)) {
            auto frame=session->frame();
            const bool cursor_visible=!frame || !frame->cursor.blinking || reduced_motion_ || cursor_blink_.visible();
            result.panes.push_back({placement.session.value,pixels(content_bounds(placement)),std::move(frame),active_ && !navigator_open_ && !find_session_ && placement.pane==workspace().active_pane() && !palette_open_ && !rename_open_ && !modal_ && !overlay_open(),cursor_visible,reduced_motion_ || cursor_blink_.visible()});
            if (placement.pane==workspace().active_pane()) {
                auto& rendered=result.panes.back();
                rendered.cursor_reveal=typing_motion_.observe(rendered,GetTickCount64(),!reduced_motion_ && !composing_ && !minimized_ && !in_size_move_ && drag_!=Drag::selection);
                if (options_.smoke_test && (smoke_typing_stage_==1 || smoke_typing_stage_==4) && rendered.cursor_reveal<1) smoke_typing_reveal_seen_=true;
            }
        }
        if (layout_.panes.size() > 1) {
            const Rect header{placement.bounds.x, placement.bounds.y, placement.bounds.width, 28};
            const auto focus=pane_focus_colors(ui,placement.pane==workspace().active_pane(),
                pane_focus_[placement.pane].value(now),high_contrast_,text,accent);
            if (!high_contrast_) {
                rectangle(header,focus.wash);
                rectangle({header.x,header.y+header.height-1/scale_,header.width,1/scale_},daylight?Color{130,142,155,100}:Color{43,50,61,140});
            }
            if (focus.show_marker) rectangle({header.x + 12,header.y + 10,2,8},focus.marker);
            const auto context=pane_context(placement.pane);
            const double available=std::max(0.0,header.width-56);
            const auto measure=[&](const std::string& value,double size){ChromeText item;item.text=value;item.size_dip=static_cast<float>(size);item.rect={0,0,100000,28*static_cast<float>(scale_)};return renderer_->measure_chrome_text(item).width/scale_;};
            const double primary_width=measure(context.primary,13);
            auto detail=pane_context_secondary(context);
            if(primary_width+16+measure(detail,11)>available)detail=pane_context_secondary(context,PaneContextDensity::directory);
            if(primary_width+16+48>available || detail.size()>32768)detail.clear();
            const double title_width=detail.empty()?available:std::min(primary_width,available);
            label({header.x+20,header.y+6,title_width,18},context.primary,focus.label,13);
            if(!detail.empty())label({header.x+20+title_width+16,header.y+7,std::max(0.0,available-title_width-16),17},detail,secondary,11);
            label({header.x + header.width - 28,header.y + 3,24,22}, "···", secondary, 16,false,ChromeTextAlignment::center);
        }
    }
    if(find_session_) {
        const auto find=find_layout();
        rectangle(find.strip,chrome);
        line(find.strip.x,find.strip.y+find.strip.height,find.strip.x+find.strip.width,find.strip.y+find.strip.height,seam);
        rectangle(find.query,background,4,find_focus_==0?accent:seam,find_focus_==0?1.5:1);
        if(find_query_.selected())rectangle({find.query.x+3,find.query.y+3,std::max(0.0,find.query.width-6),std::max(0.0,find.query.height-6)},blend(background,accent,.25),2);
        const auto visible=find_query_view();
        label({find.query.x+8,find.query.y+5,std::max(0.0,find.query.width-16),18},find_query_.text().empty()?"Find in terminal…":visible.first,find_query_.text().empty()?secondary:text,13);
        if(find_focus_==0 && !find_query_.selected()) {
            const double caret=std::min(find.query.x+find.query.width-8,find.query.x+8+visible.second);
            line(caret,find.query.y+6,caret,find.query.y+22,accent,1.2);
        }
        if(find_focus_==0 && composing_ && !preedit_.empty()) {
            const double left=std::min(find.query.x+find.query.width-8,find.query.x+8+visible.second),width=std::max(0.0,find.query.x+find.query.width-8-left);
            rectangle({left,find.query.y+3,width,find.query.height-6},background);
            label({left,find.query.y+5,width,18},preedit_,text,13);
            line(left,find.query.y+24,left+width,find.query.y+24,accent);
        }
        label({find.status.x,find.status.y+6,find.status.width,18},find_error_.empty()?terminal_find_status(find_result_.get(),find_request_,find_query_.text()):find_error_,secondary,11);
        const bool ready=find_result_ && current_find_result(*find_result_,find_request_,find_query_.text()) && find_result_->status==SearchStatus::complete && !find_result_->matches.empty();
        const std::array<Rect,3> buttons{find.previous,find.next,find.close};
        for(unsigned i=0;i<buttons.size();++i) {
            const auto b=buttons[i];const unsigned index=i+1;const bool focused=find_focus_==index;
            const bool hover=b.contains(hover_), pressed=find_pressed_==index;
            rectangle(b,pressed?blend(chrome,accent,.22):hover?raised:chrome,4,focused?accent:Color{0,0,0,0},focused?1.5:0);
            const auto ink=i==2||ready?text:secondary;const double cx=b.x+b.width/2,cy=b.y+b.height/2;
            if(i==2){line(cx-4,cy-4,cx+4,cy+4,ink);line(cx+4,cy-4,cx-4,cy+4,ink);}
            else {const double sign=i==0?-1:1;line(cx-4,cy-sign*2,cx,cy+sign*2,ink);line(cx,cy+sign*2,cx+4,cy-sign*2,ink);}
        }
    }
    for (const auto& divider : layout_.dividers) rectangle(divider.bounds, drag_node_ == divider.node ? accent : blend(seam,accent,hover_amount((1ull<<63)|divider.node.value)));
    if (!fullscreen_) {
        if (backdrop_) chrome.a = 230;
        rectangle(chrome_.band, chrome, 0, {}, 0, true);
        rectangle(chrome_.tabs_band,chrome,0,{},0,true);
        rectangle({0,chrome_.band.height-1/scale_,width_,1/scale_},seam);
        rectangle({chrome_.tabs_band.x,chrome_.tabs_band.y+chrome_.tabs_band.height-1/scale_,chrome_.tabs_band.width,1/scale_},seam);
        const double title_right=zoom_return_.width>0?zoom_return_.x-8:chrome_.drag.x+chrome_.drag.width;
        const Rect app_title=layout_window_title(chrome_,title_right);
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
        const auto tab_colors=tab_selection_colors(ui,high_contrast_,background,accent);
        for (const auto& tab : chrome_.tabs) {
            const bool selected = tab.tab == workspace().active_tab();
            const double settle=tab_transitions_[tab.tab].value(now), hovered=hover_amount(16+tab.tab.value*2);
            if (settle>0 || hovered>0) rectangle(tab.bounds, opacity(blend(raised,tab_colors.surface,settle),std::max(settle,hovered*.65)),4,
                high_contrast_?opacity(tab_colors.border,settle):Color{0,0,0,0},high_contrast_?1/scale_:0);
            if (selected) rectangle({tab.bounds.x+3,tab.bounds.y+tab.bounds.height-2/scale_,std::max(0.0,tab.bounds.width-6),2/scale_},accent,1/scale_);
            const auto watch=tab_watch_indicator(tab.tab);
            const auto caption=layout_tab_label(tab.bounds,watch);
            if(caption.indicator.width>0) {
                // Hollow = watching, filled = unread. Shape carries state even
                // in high contrast; neither implies command success or animates.
                const auto ink=selected?text:secondary;
                rectangle(caption.indicator,watch==TabWatchIndicator::unread?ink:Color{0,0,0,0},3,ink,1);
            }
            label(caption.text,tab_title(tab.tab),selected?text:secondary,13,selected);
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
        if(navigation_.focus.width>0 && navigation_.focus.height>0) {
            button(navigation_.focus,10);
            const auto ink=focus_mode_?accent:secondary;
            const double x=navigation_.focus.x+navigation_.focus.width/2,y=navigation_.focus.y+navigation_.focus.height/2;
            // Outward corners enter focus mode; inward corners restore navigation.
            for(const int sx:{-1,1})for(const int sy:{-1,1}) {
                const double corner=focus_mode_?3:6,end=focus_mode_?6:3;
                line(x+sx*corner,y+sy*end,x+sx*corner,y+sy*corner,ink);
                line(x+sx*corner,y+sy*corner,x+sx*end,y+sy*corner,ink);
            }
        }
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
        rectangle({navigation_.footer.x,navigation_.footer.y,navigation_.footer.width,1/scale_},seam);
        const auto placement=footer_layout();
        const auto context=footer_presentation();
        if(placement.agent.width>0) {rectangle(placement.agent,raised,4);label(placement.agent,agent_status_label(),agent_controller_&&agent_controller_->attention()?accent:secondary,11);}
        if (placement.clock.width>0) label(placement.clock,local_clock_,secondary,11,false,ChromeTextAlignment::trailing);
        if(placement.directory.contains(hover_))rectangle(placement.directory,raised,3);
        label(placement.directory,context.directory,context.subdued?secondary:text,12);
        if(placement.git.width>0 && !context.head.empty()) {
            const auto r=placement.git;
            // Owned vector branch mark, independent of shell font/icon plugins.
            const double x=r.x+3,y=r.y+r.height/2;
            line(x,y-5,x,y+5,secondary,1);
            line(x,y,x+6,y-4,secondary,1);
            rectangle({x-1.5,y-6.5,3,3},secondary,1.5);
            rectangle({x-1.5,y+3.5,3,3},secondary,1.5);
            rectangle({x+4.5,y-5.5,3,3},secondary,1.5);
            label({r.x+16,r.y,std::max(0.0,r.width-16),r.height},context.detached?"@"+context.head:context.head,
                context.subdued?secondary:accent,12);
        }
        label(placement.status,context.status,context.failed?accent:secondary,12,false,ChromeTextAlignment::trailing);
        if(placement.divider.height>0) rectangle(placement.divider,seam);
        if (!projects_.active()->parked_sessions.empty()) {
            const auto bounds=placement.background.overflow;
            if(bounds.width>0) {
                const auto summary=background_summary();
                rectangle(bounds,high_contrast_?raised:blend(raised,ui.hover,hover_amount(12)),4);
                if(summary.unread())rectangle({bounds.x+6,bounds.y+bounds.height/2-2,4,4},accent,2);
                auto title=summary.label();
                if(summary.unread()&&bounds.width>=180)title+=" · "+summary.completion_label();
                const double inset=summary.unread()?16:10;
                label({bounds.x+inset,bounds.y,std::max(0.0,bounds.width-inset-8),bounds.height},title,summary.unread()?accent:text,12);
            }
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
    if (!navigator_open_ && !find_session_ && !preedit_.empty() && composing_) {
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
        const auto notice=notice_layout();
        rectangle(notice.panel,raised,6);
        label(notice.text,notice_,text,12);
        result.texts.back().wrap=true;
    }
    z={ChromeLayer::palette}; // Cached closing primitives/text retain this same surface.
    // Cache unmodified owned surfaces: dismissal fades the last real frame, never
    // a regenerated placeholder. Reopening reverses from the current opacity.
    auto reveal_surface=[&](bool open, ChromeTransition& transition, std::size_t primitives, std::size_t texts,
                            std::vector<ChromePrimitive>& cached_primitives, std::vector<ChromeText>& cached_texts) {
        const auto amount=transition.value(now);
        if (open) {
            cached_primitives.assign(result.primitives.begin()+primitives,result.primitives.end());
            cached_texts.assign(result.texts.begin()+texts,result.texts.end());
        } else if (amount>0) {
            result.primitives.insert(result.primitives.end(),cached_primitives.begin(),cached_primitives.end());
            result.texts.insert(result.texts.end(),cached_texts.begin(),cached_texts.end());
        } else { cached_primitives.clear(); cached_texts.clear(); }
        for (auto i=primitives;i<result.primitives.size();++i) {
            auto& item=result.primitives[i]; item.fill=opacity(item.fill,amount); item.stroke=opacity(item.stroke,amount);
            // COPY is only valid for the settled opaque material surface. During
            // reveal/dismiss it would erase lower content (or force alpha=255 in
            // opaque fallback) rather than blend with the already painted scene.
            if (amount<1) item.background_replace=false;
        }
        for (auto i=texts;i<result.texts.size();++i) result.texts[i].color=opacity(result.texts[i].color,amount);
    };
    const auto navigator_primitive_start=result.primitives.size(),navigator_text_start=result.texts.size();
    if(navigator_open_) {
        const auto nav=layout_workspace_navigator(width_,height_,navigator_,navigator_focus_);const auto p=nav.panel;
        rectangle({p.x,p.y+5,p.width,p.height},{0,0,0,56},10);rectangle(p,raised,10,seam,1);
        rectangle(nav.query,background,5,navigator_focus_==0?accent:seam,navigator_focus_==0?1.5:1);
        const auto visible=navigator_query_view();
        if(navigator_query_.selected())rectangle({nav.query.x+4,nav.query.y+4,nav.query.width-8,nav.query.height-8},blend(background,accent,.22),3);
        label({nav.query.x+9,nav.query.y+6,nav.query.width-18,22},navigator_query_.text().empty()?(navigator_background_only_?"Find a background terminal…":"Switch workspace or terminal…"):visible.first,navigator_query_.text().empty()?secondary:text,16);
        if(navigator_focus_==0&&!navigator_query_.selected()){const auto x=nav.query.x+9+visible.second;line(x,nav.query.y+7,x,nav.query.y+25,accent,1.2);}
        if(composing_&&!preedit_.empty()){const auto x=nav.query.x+9+visible.second;label({x,nav.query.y+6,std::max(0.0,nav.query.x+nav.query.width-8-x),22},preedit_,text,16);}
        if(nav.close.contains(hover_))rectangle(nav.close,blend(raised,accent,.1),4);
        const auto cx=nav.close.x+nav.close.width/2,cy=nav.close.y+nav.close.height/2;line(cx-4,cy-4,cx+4,cy+4,secondary);line(cx+4,cy-4,cx-4,cy+4,secondary);
        const auto* selected_workspace=navigator_.workspace();
        if(!nav.compact){
            label({nav.workspace_body.x+8,nav.heading.y,nav.workspace_body.width-16,24},"Workspaces",secondary,11,true);
            label({nav.terminal_body.x+8,nav.heading.y,nav.terminal_body.width-16,24},selected_workspace?selected_workspace->name:"Terminals",text,13,true);
            line(nav.terminal_body.x-8,nav.heading.y,nav.terminal_body.x-8,nav.footer.y,seam);
        }else label(nav.heading,navigator_focus_==1?"Workspaces":selected_workspace?"‹  "+selected_workspace->name+"   ·   "+std::to_string(navigator_.terminals().size())+" terminals":"Terminals",secondary,12,true);
        for(std::size_t i=0;i<nav.workspace_rows.size();++i){
            const auto index=nav.workspace_first+i;const auto r=nav.workspace_rows[i];const auto& entry=navigator_.entries()[navigator_.workspaces()[index]];const bool selected=index==navigator_.workspace_index();
            if(selected||r.contains(hover_))rectangle({r.x+2,r.y+2,r.width-4,r.height-4},selected?blend(raised,accent,.13):blend(raised,text,.04),5,selected&&navigator_focus_==1?accent:Color{0,0,0,0},1);
            if(entry.project==projects_.active_id())rectangle({r.x+8,r.y+18,3,6},accent,1);
            label({r.x+18,r.y+10,std::max(0.0,r.width-54),22},entry.name,text,13,selected);
            label({r.x+r.width-34,r.y+11,24,20},std::to_string(entry.terminals.size()),secondary,11,false,ChromeTextAlignment::trailing);
        }
        for(std::size_t i=0;i<nav.terminal_rows.size();++i){
            const auto index=nav.terminal_first+i;const auto r=nav.terminal_rows[i];const auto& entry=selected_workspace->terminals[navigator_.terminals()[index]];const bool selected=index==navigator_.terminal_index();
            if(selected||r.contains(hover_))rectangle({r.x+2,r.y+2,r.width-4,r.height-4},selected?blend(raised,accent,.14):blend(raised,text,.04),5,selected&&navigator_focus_==2?accent:Color{0,0,0,0},1);
            const double x=r.x+10,y=r.y+12;rectangle({x,y,15,12},{0,0,0,0},2,entry.active?accent:secondary,1);line(x+3,y+3,x+6,y+6,secondary);line(x+6,y+6,x+3,y+9,secondary);line(x+8,y+9,x+12,y+9,secondary);
            label({r.x+36,r.y+6,std::max(0.0,r.width-48),21},entry.title,text,13,selected);
            label({r.x+36,r.y+28,std::max(0.0,r.width-48),18},entry.state+(entry.directory.empty()?"":"  ·  "+entry.directory),secondary,11);
        }
        if(navigator_.workspaces().empty()){label({p.x+24,nav.heading.y+58,p.width-48,28},"No matching workspaces or terminals",text,16,true);label({p.x+24,nav.heading.y+92,p.width-48,22},"Try a workspace name, terminal title or directory.",secondary,12);}
        else if(navigator_.terminals().empty()&&(!nav.compact||navigator_focus_!=1))label({nav.terminal_body.x+8,nav.terminal_body.y+18,nav.terminal_body.width-16,36},"No existing terminals in this workspace",secondary,12);
        line(nav.footer.x,nav.footer.y,nav.footer.x+nav.footer.width,nav.footer.y,seam);
        const auto* terminal=navigator_.terminal();const Rect action{nav.footer.x+nav.footer.width-104,nav.footer.y+12,88,30};
        if(terminal){rectangle(action,navigator_switch_pressed_?blend(raised,accent,.3):blend(raised,accent,.17),5,seam,1);label(action,terminal->parked?"Restore":"Switch",text,12,true,ChromeTextAlignment::center);}
        const double detail_width=std::max(0.0,nav.footer.width-136);
        label({nav.footer.x+16,nav.footer.y+9,detail_width,18},terminal&&!terminal->directory.empty()?terminal->directory:selected_workspace?"Workspace · "+selected_workspace->directory:"",secondary,11);
        label({nav.footer.x+16,nav.footer.y+30,detail_width,17},nav.compact?"Tab: lists  ·  Enter: switch  ·  Esc: cancel":"Tab: query / workspaces / terminals   ·   Enter: switch   ·   Esc: cancel",secondary,10);
    }
    reveal_surface(navigator_open_,navigator_transition_,navigator_primitive_start,navigator_text_start,closing_navigator_primitives_,closing_navigator_texts_);
    const auto palette_primitive_start=result.primitives.size(), palette_text_start=result.texts.size();
    if (palette_open_ || rename_open_) {
        const auto commands=palette_commands();
        const auto layout=palette_layout(commands);
        const auto [x,y,w,h]=layout.panel;
        rectangle({x,y+4,w,h},{0,0,0,48},10);
        rectangle(layout.panel,raised,10,seam,1);
        // A quiet search mark, drawn with native vectors rather than a font glyph.
        if (!rename_open_) {
            rectangle({x+20,y+19,10,10},{0,0,0,0},5,secondary,1.25);
            line(x+29,y+28,x+34,y+33,secondary,1.25);
        }
        rectangle({x+w-49,y+18,30,19},background,4,seam,1);
        label({x+w-44,y+20,24,15},"Esc",secondary,10);
        const double search_left=x+(rename_open_?20:46);
        label({search_left,y+16,std::max(0.0,x+w-66-search_left),24},query_.empty()?(rename_open_?(edit_text_action_?edit_text_hint_:"Tab name"):inspecting_context()?"Inspect pane context · filter fields":palette_background_only_?"Search background terminals…":"Type a command…"):query_,query_.empty()?secondary:text,16);
        // A stationary caret remains visible with reduced motion. Measure the
        // actual chrome font; byte counts are not glyph widths (IME/Unicode).
        ChromeText query_measure; query_measure.text=query_; query_measure.size_dip=16;
        query_measure.rect={0,0,static_cast<float>(std::max(1.0,w)*scale_),24*static_cast<float>(scale_)};
        const double caret_x=std::min(x+w-66,search_left+renderer_->measure_chrome_text(query_measure).width/scale_);
        if (caret_x>=search_left) line(caret_x,y+18,caret_x,y+36,accent,1.25);
        line(x+1,y+50,x+w-1,y+50,seam);
        for (std::size_t row=0;row<layout.rows;++row) {
            const auto index=layout.first+row; const auto& command=commands[index];
            const auto available=palette_availability(command);
            const auto bounds=layout.row_bounds[row];
            const auto detail=!available.enabled?available.reason:command.session?std::string_view(command.description):std::string_view{};
            const double top=bounds.y+(detail.empty()?7:3);
            const bool selected=index==std::min(palette_index_,commands.size()-1);
            if (selected) {
                rectangle({bounds.x,bounds.y,bounds.width,bounds.height-4},high_contrast_?background:blend(raised,accent,.13),5,high_contrast_?accent:Color{0,0,0,0},high_contrast_?1.0:0.0);
                rectangle({bounds.x,bounds.y+7,2,bounds.height-18},accent,1);
            }
            // Action families are semantic, not decorative per-command badges.
            // The same 16-DIP native line language as workspace navigation.
            const Color mark=selected?accent:secondary;
            const double ix=x+20, iy=top+3;
            if (inspecting_context()) {
                // Metadata rows are not actions: no command/launch icon.
            } else if (command.id.starts_with("workspace.")) {
                rectangle({ix,iy+4,16,10},{0,0,0,0},1,mark,1);
                line(ix,iy+4,ix,iy+1,mark); line(ix,iy+1,ix+6,iy+1,mark); line(ix+6,iy+1,ix+9,iy+4,mark);
            } else if (command.id.starts_with("pane.")) {
                rectangle({ix,iy,16,14},{0,0,0,0},2,mark,1);
                if (command.id=="pane.split_down" || command.id=="pane.focus_down" || command.id=="pane.focus_up") line(ix,iy+7,ix+16,iy+7,mark);
                else line(ix+8,iy,ix+8,iy+14,mark);
            } else if (command.id.starts_with("tab.") || command.id.starts_with("window.")) {
                rectangle({ix,iy,16,14},{0,0,0,0},2,mark,1); line(ix,iy+4,ix+16,iy+4,mark);
            } else if (command.id.starts_with("settings.")) {
                for (int n=0;n<3;++n) { const double yy=iy+2+n*5, knob=ix+(n==1?10:5); line(ix,yy,ix+16,yy,mark); rectangle({knob,yy-2,3,4},raised,1,mark,1); }
            } else {
                line(ix+1,iy+2,ix+6,iy+7,mark); line(ix+6,iy+7,ix+1,iy+12,mark); line(ix+9,iy+12,ix+15,iy+12,mark);
            }
            std::string shortcut;
            for (const auto& binding:config_.bindings) if (binding.command==command.id) { shortcut=format_key_chord(binding.chord,Platform::windows); break; }
            ChromeText key_measure; key_measure.text=shortcut; key_measure.size_dip=10;
            key_measure.rect={0,0,static_cast<float>(w*scale_),21*static_cast<float>(scale_)};
            const double key_width=shortcut.empty()?0:std::min(w*.4,renderer_->measure_chrome_text(key_measure).width/scale_+16);
            label({x+(inspecting_context()?20:48),top,std::max(0.0,w-(inspecting_context()?40:68)-key_width-(key_width?12:0)),21},command.label,available.enabled?text:secondary,14,selected);
            if (!detail.empty()) label({x+48,top+21,std::max(0.0,w-68),17},std::string(detail),secondary,11);
            if (key_width) {
                rectangle({x+w-20-key_width,top,key_width,21},raised,4,seam,1);
                label({x+w-12-key_width,top+3,key_width-16,15},shortcut,secondary,10);
            }
        }
        if (!rename_open_ && commands.empty()) {
            label({x+20,y+60,w-40,21},inspecting_context()?"No matching fields":palette_background_only_ && query_.empty()?"No background terminals":"No matching commands",text,14);
            label({x+20,y+83,w-40,18},query_.empty()?"Move a terminal to background to keep it running here.":inspecting_context()?"Try fewer letters or clear the filter.":"Try fewer letters or a different action name.",secondary,11);
        }
        const auto hint=inspecting_context()?"Read only     ↑↓ / PgUp PgDn  Browse     Esc  Close":rename_open_?"Enter  Save":palette_background_only_?"↑ ↓  Navigate     Enter  Restore":"↑↓  Move     Enter  Run     Alt+Enter  Shortcut";
        line(x+1,layout.detail.y,x+w-1,layout.detail.y,seam);
        if (!rename_open_ && !commands.empty()) {
            const auto& selected=commands[std::min(palette_index_,commands.size()-1)];
            // Row context already explains availability/session identity. The
            // shelf describes the operation, never repeats that same context.
            const std::string_view description=selected.watch?"Open this existing actor and acknowledge its watched result":selected.session?"Restore this terminal in a new tab":std::string_view(selected.description);
            label({layout.detail.x,layout.detail.y+9,layout.detail.width,20},std::string(description),secondary,12);
        }
        const double count_width=!rename_open_ && w>=420?66:0;
        label({layout.hints.x,layout.hints.y+6,std::max(0.0,layout.hints.width-count_width),16},hint,secondary,10);
        if (count_width && !commands.empty()) {
            label({x+w-80,layout.hints.y+6,60,16},std::to_string(std::min(palette_index_,commands.size()-1)+1)+" / "+std::to_string(commands.size()),secondary,10);
            result.texts.back().alignment=ChromeTextAlignment::trailing;
        }
    }
    if (palette_open_ || rename_open_) {
        closing_palette_primitives_.assign(result.primitives.begin()+palette_primitive_start,result.primitives.end());
        closing_palette_texts_.assign(result.texts.begin()+palette_text_start,result.texts.end());
    } else if (palette_transition_.value(now)>0) {
        result.primitives.insert(result.primitives.end(),closing_palette_primitives_.begin(),closing_palette_primitives_.end());
        result.texts.insert(result.texts.end(),closing_palette_texts_.begin(),closing_palette_texts_.end());
    } else { closing_palette_primitives_.clear(); closing_palette_texts_.clear(); }
    const double reveal=palette_transition_.value(now); // Stationary surfaces keep visual and input geometry identical.
    for (auto i=palette_primitive_start;i<result.primitives.size();++i) {
        auto& item=result.primitives[i]; item.fill=opacity(item.fill,reveal); item.stroke=opacity(item.stroke,reveal);
    }
    for (auto i=palette_text_start;i<result.texts.size();++i) {
        auto& item=result.texts[i]; item.color=opacity(item.color,reveal);
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
    agent_panel_.append_scene(result,config_.theme,static_cast<float>(scale_),[this](const ChromeText& text){return renderer_->wrap_chrome_text(text);});
    const auto settings_primitive_start=result.primitives.size(), settings_text_start=result.texts.size();
    if (settings_loading()) {
        const Rect panel{std::max(16.0,(width_-420)/2),std::max(16.0,(height_-112)/2),std::max(0.0,std::min(420.0,width_-32)),112};
        z={ChromeLayer::settings};
        rectangle({0,0,width_,height_},high_contrast_?background:Color{4,6,10,148});
        rectangle(panel,high_contrast_?background:Color{24,29,36,255},9,high_contrast_?accent:Color{67,78,99,255},1/scale_);
        label({panel.x+24,panel.y+20,panel.width-48,26},"Opening Settings",text,19,true);
        label({panel.x+24,panel.y+58,panel.width-48,32},"Reading local settings and installed shells. Escape cancels.",secondary,12);
    }
    settings_panel_.append_scene(result,static_cast<float>(scale_));
    reveal_surface(settings_loading()||settings_input_blocked(),settings_transition_,settings_primitive_start,settings_text_start,
                   closing_settings_primitives_,closing_settings_texts_);
    const auto menu_primitive_start=result.primitives.size(), menu_text_start=result.texts.size();
    context_menu_.append_scene(result,static_cast<float>(scale_)); // Menu depth determines stacking, not call order.
    reveal_surface(context_menu_.is_open(),menu_transition_,menu_primitive_start,menu_text_start,
                   closing_menu_primitives_,closing_menu_texts_);
    const bool typing=typing_motion_.active(GetTickCount64());
    if (typing && !typing_timer_active_) {
        typing_timer_active_=SetTimer(hwnd_,typing_motion_timer,16,nullptr)!=0;
        if (!typing_timer_active_) { typing_motion_.reset(); for (auto& pane:result.panes) pane.cursor_reveal=1; }
    } else if (!typing && typing_timer_active_) { KillTimer(hwnd_,typing_motion_timer); typing_timer_active_=false; }
    return result;
}
void NativeWindow::render() {
    if (!hwnd_ || closing_window_ || !renderer_ || minimized_) return;
    if (renderer_call_) { defer_layout(); return; }
    RenderResult result;const auto rendered=scene();
    { RendererCallGuard guard(renderer_call_); result=renderer_->render(rendered); }
    if (result==RenderResult::presented) force_redraw_=false;
    render_busy_ = result == RenderResult::busy;
    if (!render_busy_) dirty_ = false;
    observe_material_renderer();
    if (result==RenderResult::presented || result==RenderResult::unchanged) begin_material_probe(rendered);
    if (!first_exposed_frame_ && result==RenderResult::presented && IsWindowVisible(hwnd_)) {
        first_exposed_frame_=true;
        if (options_.smoke_test) {
            if (!initial_canvas_prepared_) throw std::runtime_error("First exposed frame had no prepared canvas.");
            capture_named_frame(L"startup-exposed");
            std::fprintf(stderr,"Native startup canvas: first_exposed=1 prepared_before_show=1 shell_readiness_claimed=0\n");
        }
    }
    if (options_.smoke_test) smoke_progress(result==RenderResult::presented,true);
    // Occlusion is retried on actual exposure/activation/output, not a timer.
}

void NativeWindow::toggle_agent_panel() {
    if(navigator_open_)close_navigator();
    if(!ai_enabled_||!agent_controller_)return;
    if (agent_panel_.is_open()) agent_panel_.close();
    else if (!modal_ && !overlay_open() && !palette_open_ && !rename_open_) {
        const auto attention=agent_controller_->attention_workspace();
        if(!attention.empty()) for(const auto& entry:projects_.entries()) if(entry.metadata.persistent_id==attention && entry.id!=projects_.active_id()) {activate_project(entry.id);break;}
        const auto* project=projects_.active(); if (!project) return;
        if(!options_.smoke_test&&!options_.smoke_agent) agent_controller_->opened(project->metadata.persistent_id,project->metadata.working_directory);
        auto view=current_agent_view();
        agent_panel_.open(std::move(view)); agent_panel_.resize({width_,height_});
        text_input_.reset(); typing_motion_.reset();
    }
    agent_input_changed();
}
std::string NativeWindow::agent_status_label() const {
    if(options_.smoke_agent&&agent_panel_.is_open())return "Codex · "+agent_panel_.status_label();
    const auto* project=projects_.active();if(!project||!agent_controller_)return "Codex · Unavailable";
    const auto attention=agent_controller_->attention_workspace();
    if(!attention.empty()&&attention!=project->metadata.persistent_id) for(const auto& entry:projects_.entries()) if(entry.metadata.persistent_id==attention) return "Codex · Approval: "+entry.metadata.name;
    const auto view=agent_controller_->view(project->metadata.persistent_id,project->metadata.name,project->metadata.working_directory);
    AgentPanel status;status.update(view);
    return "Codex · "+status.status_label()+(agent_controller_->attention()?" · !":"");
}
AgentPanelView NativeWindow::current_agent_view() const {
    if(!ai_enabled_||!agent_controller_)return {};
    const auto* project=projects_.active();if(!project)return {};
    auto view=agent_controller_->view(project->metadata.persistent_id,project->metadata.name,project->metadata.working_directory);
    const bool overridden=!options_.codex_executable.empty()||!options_.codex_account_reference.empty();
    view.provider_settings=agent_saved_settings_;view.setup_busy=agent_setup_&&agent_setup_->busy();
    view.setup_reload_available=agent_setup_&&!overridden&&agent_controller_->can_reconfigure();
    view.setup_available=view.setup_reload_available&&agent_setup_->ready();
    if(overridden)view.setup_status="Provider set by launch arguments. Remove those overrides to use Setup.";
    else if(!agent_setup_)view.setup_status="Machine-local settings location is unavailable.";
    else if(view.setup_busy)view.setup_status="Reading or saving local provider settings…";
    else if(!agent_setup_error_.empty())view.setup_status=agent_setup_error_;
    else if(!agent_controller_->can_reconfigure())view.setup_status="Disconnect the provider before changing Setup.";
    return view;
}
void NativeWindow::refresh_agent_panel() {
    if(!agent_controller_||options_.smoke_agent)return;
    if(agent_setup_) if(auto completion=agent_setup_->take_completion()) {
        agent_setup_error_=completion->error;
        if(completion->error.empty()) {
            agent_saved_settings_=completion->settings;
            if(options_.codex_executable.empty()&&options_.codex_account_reference.empty()&&!agent_controller_->reconfigure(agent_saved_settings_))
                agent_setup_error_="Provider state changed; disconnect before applying local setup.";
        }
        if(completion->saved)agent_panel_.settings_result(agent_setup_error_.empty(),agent_setup_error_);
        else if(agent_setup_reload_requested_){agent_setup_reload_requested_=false;agent_panel_.settings_reloaded(agent_saved_settings_,agent_setup_error_);}
    }
    if(agent_panel_.is_open()) {
        const auto* project=projects_.active();if(!project)return;
        agent_panel_.update(current_agent_view());
    }
    changed();
}
void NativeWindow::agent_input_changed() {
    if(!ai_enabled_||!agent_controller_){agent_panel_.close();(void)agent_panel_.take_action();return;}
    if(auto action=agent_panel_.take_action()) {
        if(action->kind==AgentPanelActionKind::open_sign_in||action->kind==AgentPanelActionKind::install_codex) {
            const auto current=current_agent_view();
            const bool matching=current.workspace_id==action->workspace_id&&current.provider_generation==action->provider_generation&&current.settings_generation==action->settings_generation;
            if(!options_.smoke_test&&!options_.smoke_agent&&matching&&!agent_panel_.view().fixture) {
                std::string url;
                if(action->kind==AgentPanelActionKind::install_codex)url="https://developers.openai.com/codex/cli";
                else if(current.auth==AgentAuth::signing_in&&current.login_id==action->login_id&&valid_agent_auth_url(current.auth_url))url=current.auth_url;
                if(!url.empty()&&reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_,L"open",widen(url).c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)notice_="Could not open your browser. Try Continue in browser again.";
            }
            sync_focus();changed();return;
        }
        if(action->kind==AgentPanelActionKind::reload_settings) {
            if(options_.smoke_test||!agent_setup_||agent_setup_->busy()||!agent_controller_->can_reconfigure()||!options_.codex_executable.empty()||!options_.codex_account_reference.empty())
                agent_panel_.settings_reloaded(agent_saved_settings_,"Disconnect the provider before reloading local setup.");
            else {agent_setup_reload_requested_=true;agent_setup_->load();}
            refresh_agent_panel();sync_focus();caret_schedule(true);changed();return;
        }
        if(action->kind==AgentPanelActionKind::save_settings) {
            if(options_.smoke_test||!agent_setup_||!agent_controller_->can_reconfigure()||!options_.codex_executable.empty()||!options_.codex_account_reference.empty()||!agent_setup_->save(action->provider_settings))
                agent_panel_.settings_result(false,"Settings are unavailable or the provider is still connected.");
            refresh_agent_panel();sync_focus();caret_schedule(true);changed();return;
        }
        const auto* project=projects_.active();
        if(project && project->metadata.persistent_id==action->workspace_id && !options_.smoke_agent)
            agent_controller_->dispatch(*action,project->metadata.working_directory);
        refresh_agent_panel();
    }
    sync_focus(); caret_schedule(true); changed();
}
void NativeWindow::terminal_text(std::string text, Key key, std::uint16_t modifiers) {
    if(navigator_open_&&!modal_&&!overlay_open()){navigator_focus_=0;if(navigator_query_.insert(text))navigator_input_changed();return;}
    if(find_session_ && !modal_ && !overlay_open()) {if(find_focus_==0 && find_query_.insert(text))submit_find();return;}
    if (agent_panel_.is_open()) { agent_panel_.text(text); agent_input_changed(); return; }
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
    if(options_.smoke_test)++smoke_terminal_input_submissions_;
    const bool animate=!composing_ && !reduced_motion_ && text.size()<=4 && static_cast<unsigned char>(text[0])>=0x20 && text[0]!=0x7f;
    if (!animate) typing_motion_.reset();
    if (key == Key::unidentified && !modifiers) submit([this,session,animate,text = std::move(text)] {
        const auto result=session->text(text);
        if (options_.smoke_test && smoke_typing_stage_>0 && smoke_typing_stage_<6) smoke_typing_input_result_=static_cast<int>(result);
        if (animate && result==SessionSendResult::accepted && active_ && !composing_ && !reduced_motion_ && !minimized_ && !in_size_move_ && drag_!=Drag::selection && session_for(workspace().active_pane())==session.get()) typing_motion_.input(GetTickCount64());
        return result;
    },bytes);
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
        submit([this,session,animate,input = std::move(input)] {
            const auto result=session->key(input);
        if (options_.smoke_test && smoke_typing_stage_>0 && smoke_typing_stage_<6) smoke_typing_input_result_=static_cast<int>(result);
            if (animate && result==SessionSendResult::accepted && active_ && !composing_ && !reduced_motion_ && !minimized_ && !in_size_move_ && drag_!=Drag::selection && session_for(workspace().active_pane())==session.get()) typing_motion_.input(GetTickCount64());
            return result;
        },bytes+64);
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
    CommandModifier agent_modifiers=CommandModifier::none;
    if(modifiers&Modifier::control) agent_modifiers=agent_modifiers|CommandModifier::control;
    if(modifiers&Modifier::shift) agent_modifiers=agent_modifiers|CommandModifier::shift;
    if(modifiers&Modifier::alt) agent_modifiers=agent_modifiers|CommandModifier::alt;
    const auto agent_command=resolve_command({{chord_key(vk),agent_modifiers},composing_,altgr,down},config_.bindings,command_context(),Platform::windows);
    if (down && agent_command && *agent_command=="agent.toggle") {
        if (!(message.lParam & (1LL<<30))) toggle_agent_panel();
        consumed_keys_[vk]=true; return true;
    }
    if (agent_panel_.is_open()) {
        if(down && vk=='V' && (modifiers&Modifier::control) && !altgr) {paste();return true;}
        CommandModifier app=CommandModifier::none;
        if(modifiers&Modifier::control) app=app|CommandModifier::control;
        if(modifiers&Modifier::shift) app=app|CommandModifier::shift;
        if(modifiers&Modifier::alt) app=app|CommandModifier::alt;
        if (down && vk==VK_RETURN && (message.lParam & (1LL<<30))) return true;
        agent_panel_.key({{chord_key(vk),app},composing_,altgr,down}); agent_input_changed();
        if (down && !nontext_key(vk) && (altgr || !(modifiers&(Modifier::control|Modifier::alt|Modifier::super)))) {
            text_vk_=vk; text_flags_=message.lParam; text_modifiers_=modifiers; return false;
        }
        if(down && vk<consumed_keys_.size()) consumed_keys_[vk]=true;
        return true;
    }
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
    if(navigator_open_ && !modal_ && !overlay_open()) {
        if(composing_||vk==VK_PROCESSKEY)return false;
        if(up)return true;
        if(vk<consumed_keys_.size())consumed_keys_[vk]=true;
        if(vk==VK_ESCAPE){close_navigator();return true;}
        if(vk==VK_TAB){navigator_focus_=(navigator_focus_+((modifiers&Modifier::shift)?2:1))%3;changed();position_ime();return true;}
        if(vk==VK_RETURN){if(!(message.lParam&(1LL<<30))){if(navigator_focus_==1){navigator_focus_=2;changed();}else commit_navigator();}return true;}
        if(vk==VK_UP||vk==VK_DOWN){navigator_.move(navigator_focus_==1,vk==VK_UP?-1:1);changed();return true;}
        if(navigator_focus_ && (vk==VK_LEFT||vk==VK_RIGHT)){navigator_focus_=vk==VK_LEFT?1:2;changed();return true;}
        if((modifiers&Modifier::control)&&!altgr){
            if(vk=='A'){navigator_focus_=0;navigator_query_.select_all();changed();return true;}
            if(vk=='V'){paste();return true;}
            if((vk=='C'||vk=='X')&&navigator_query_.selected()){set_clipboard(navigator_query_.text());if(vk=='X'&&navigator_query_.erase(true))navigator_input_changed();return true;}
            return true;
        }
        if(vk==VK_BACK||vk==VK_DELETE){navigator_focus_=0;if(navigator_query_.erase(vk==VK_BACK))navigator_input_changed();return true;}
        if(vk==VK_LEFT)navigator_query_.left();else if(vk==VK_RIGHT)navigator_query_.right();else if(vk==VK_HOME)navigator_query_.home();else if(vk==VK_END)navigator_query_.end();
        else if(nontext_key(vk)||(!altgr&&(modifiers&(Modifier::alt|Modifier::super))))return true;
        else {text_vk_=vk;text_flags_=message.lParam;text_modifiers_=modifiers;return false;}
        changed();position_ime();return true;
    }
    if (find_session_ && !modal_ && !overlay_open()) {
        if(composing_ || vk==VK_PROCESSKEY)return false;
        if(up)return true;
        if(vk<consumed_keys_.size())consumed_keys_[vk]=true;
        if(vk==VK_ESCAPE){close_find();return true;}
        if(vk==VK_TAB){find_focus_=(find_focus_+((modifiers&Modifier::shift)?3:1))%4;changed();return true;}
        if(vk==VK_RETURN){if(!(message.lParam&(1LL<<30))){if(find_focus_==0)navigate_find((modifiers&Modifier::shift)!=0);else activate_find_control(find_focus_);}return true;}
        if(vk==VK_SPACE && find_focus_){activate_find_control(find_focus_);return true;}
        if(find_focus_)return true;
        if((modifiers&Modifier::control)&&!altgr) {
            if(vk=='A'){find_query_.select_all();changed();return true;}
            if(vk=='V'){paste();return true;}
            if((vk=='C'||vk=='X')&&find_query_.selected()){set_clipboard(find_query_.text());if(vk=='X'&&find_query_.erase(true))submit_find();return true;}
            if(vk=='F'){find_query_.select_all();changed();return true;}
            return true;
        }
        if(vk==VK_BACK||vk==VK_DELETE){if(find_query_.erase(vk==VK_BACK))submit_find();return true;}
        if(vk==VK_LEFT)find_query_.left();else if(vk==VK_RIGHT)find_query_.right();else if(vk==VK_HOME)find_query_.home();else if(vk==VK_END)find_query_.end();
        else if(nontext_key(vk)||(!altgr && (modifiers&(Modifier::alt|Modifier::super))))return true;
        else {text_vk_=vk;text_flags_=message.lParam;text_modifiers_=modifiers;return false;}
        changed();position_ime();return true;
    }
    if (composing_ || vk == VK_PROCESSKEY) return false;
    if (down && vk == VK_ESCAPE && drag_ != Drag::none) { cancel_drag(); consumed_keys_[vk] = true; return true; }
    if (palette_open_ || rename_open_) {
        if (up) return true;
        if(vk<consumed_keys_.size())consumed_keys_[vk]=true;
        if (vk == VK_ESCAPE) { palette_open_ = rename_open_ = false; query_.clear(); changed(); caret_schedule(); return true; }
        if (vk == VK_BACK) {
            if (!query_.empty()) { auto start=query_.size()-1; while (start>0 && (static_cast<unsigned char>(query_[start]) & 0xc0)==0x80) --start; query_.resize(start); }
            palette_index_ = 0; changed(); return true;
        }
        if (vk == VK_RETURN) {
            if (palette_open_ && !inspecting_context() && (modifiers&Modifier::alt) && !altgr) {
                const auto commands=palette_commands();
                if (!commands.empty() && !commands[std::min(palette_index_,commands.size()-1)].session) open_settings(commands[std::min(palette_index_,commands.size()-1)].id);
                if (vk<consumed_keys_.size()) consumed_keys_[vk]=true;
                return true;
            }
            if (rename_open_) {
                auto status=edit_text_action_?edit_text_action_(query_):workspace().rename_tab(rename_tab_,query_);
                if (!status) { show_error(status.error); return true; }
                edit_text_action_={}; rename_open_=false; refresh_tooltips(); changed(); workspace_model_changed();
            }
            else {
                const auto commands = palette_commands();
                if (!commands.empty()) {
                    activate_palette_entry(commands[std::min(palette_index_,commands.size()-1)]);
                }
            }
            caret_schedule(); return true;
        }
        if (!rename_open_ && (vk==VK_HOME || vk==VK_END || vk==VK_PRIOR || vk==VK_NEXT)) {
            const auto commands=palette_commands();
            if (!commands.empty()) {
                const auto last=commands.size()-1, step=palette_layout(commands).rows;
                palette_index_=std::min(palette_index_,last);
                if (vk==VK_HOME) palette_index_=0;
                else if (vk==VK_END) palette_index_=last;
                else if (vk==VK_PRIOR) palette_index_=palette_index_>step?palette_index_-step:0;
                else palette_index_=std::min(last,palette_index_+step);
            }
            changed(); return true;
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
        if(options_.smoke_test)++smoke_terminal_input_submissions_;
        submit([this,session,input] { const auto result=session->key(input);
            if (options_.smoke_test && smoke_typing_stage_>0 && smoke_typing_stage_<6) smoke_typing_input_result_=static_cast<int>(result);
            return result; });
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
    if(navigator_open_){navigator_focus_=0;if(navigator_query_.insert(text))navigator_input_changed();return;}
    if(find_session_){if(find_focus_==0 && find_query_.insert(text))submit_find();return;}
    if(agent_panel_.is_open()) {agent_panel_.text(text);agent_input_changed();return;}
    const bool unsafe=!TerminalCore::paste_is_safe(text);
    const auto* pane=workspace().pane(workspace().active_pane());
    if (!pane || !sessions_.contains(pane->session)) return;
    auto session=sessions_.at(pane->session); const auto size=text.size();
    auto send=[this,session,text=std::move(text),unsafe,size] {
        if (session->process_finished()) { show_error("The paste destination has closed; nothing was sent."); return; }
        if(options_.smoke_test)++smoke_terminal_input_submissions_;
        submit([session,text,unsafe] { return session->paste(text,unsafe); },size);
    };
    if (unsafe) show_modal("Review paste",{"This paste contains multiple lines","or terminal control characters.","It may execute shell commands."},"Paste anyway",std::move(send),false);
    else send();
}
void NativeWindow::terminal_mouse(Point point, MouseAction action, MouseButton button, bool pressed) {
    if(navigator_open_||palette_open_||rename_open_)return;
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
            mouse_capture_pane_=placement.pane; mouse_capture_button_=button;
            // Reacquiring the same HWND sends WM_CAPTURECHANGED synchronously
            // on Windows, which would release this just-established owner.
            if(GetCapture()!=hwnd_)SetCapture(hwnd_);
        }
        MouseInput input{action,button,terminal_modifiers(),(point.x-content.x)*scale_,(point.y-content.y)*scale_,pressed};
        if(options_.smoke_test)++smoke_terminal_mouse_submissions_;
        submit([session,input] { return session->mouse(input); });
        if (action==MouseAction::release && physical_button) {
            mouse_capture_pane_={}; mouse_capture_button_=MouseButton::none;
            if (GetCapture()==hwnd_) ReleaseCapture();
        }
        return;
    }
}
void NativeWindow::open_menu(std::vector<ContextMenuItem> items,Point anchor,std::map<std::string,std::function<void()>> actions,bool keyboard) {
    if(navigator_open_)close_navigator();
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
    if (navigation_.rail.contains(point)) {
        for (const auto& row:navigation_.projects) if (row.bounds.contains(point)) {workspace_menu(point,row.project);return;}
        workspace_menu(point); return;
    }
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
        for (auto command:{"edit.copy","edit.paste","pane.inspect","pane.split_right","pane.split_down","pane.zoom","pane.new_tab","pane.background","pane.swap","pane.move_to_tab"}) items.push_back(command_item(command));
        items.push_back(ContextMenuItem::separator()); items.push_back(command_item("pane.close"));
    } else {
        items.push_back(command_item("tab.new"));
        items.push_back({"workspace.customize","Workspace settings…",{},{},true,false,{}});
        actions["workspace.customize"]=[this,point]{workspace_menu(point);};
        items.push_back(group("group.workspace","Workspaces",{"workspace.new","workspace.next","workspace.previous","workspace.toggle_navigation","workspace.close"}));
        items.push_back(group("group.tab","Task tabs",{"tab.rename","tab.next","tab.previous","tab.move_left","tab.move_right","tab.close"}));
        items.push_back(group("group.pane","Panes",{"pane.inspect","pane.split_right","pane.split_down","pane.zoom","pane.new_tab","pane.background","pane.swap","pane.move_to_tab","pane.close"}));
        items.push_back(ContextMenuItem::separator());
        for (auto command:{"app.command_palette","settings.open","window.focus_mode","window.fullscreen"}) items.push_back(command_item(command));
    }
    open_menu(std::move(items),point,std::move(actions));
}
void NativeWindow::pointer_down(Point point, bool double_click) {
    SetFocus(hwnd_);
    if (agent_panel_.is_open()) { SetCapture(hwnd_); agent_panel_.pointer_down(point); agent_input_changed(); return; }
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
    if(navigator_open_) {
        const auto nav=layout_workspace_navigator(width_,height_,navigator_,navigator_focus_);
        if(!nav.panel.contains(point)||nav.close.contains(point)){close_navigator();return;}
        if(nav.query.contains(point)){navigator_focus_=0;navigator_query_.end();changed();position_ime();return;}
        if(nav.compact&&nav.heading.contains(point)){navigator_focus_=navigator_focus_==1?2:1;changed();return;}
        for(std::size_t i=0;i<nav.workspace_rows.size();++i)if(nav.workspace_rows[i].contains(point)){navigator_.select_workspace(nav.workspace_first+i);navigator_focus_=1;changed();return;}
        for(std::size_t i=0;i<nav.terminal_rows.size();++i)if(nav.terminal_rows[i].contains(point)){navigator_.select_terminal(nav.terminal_first+i);navigator_focus_=2;if(double_click)commit_navigator();else changed();return;}
        const Rect action{nav.footer.x+nav.footer.width-104,nav.footer.y+12,88,30};
        if(action.contains(point)&&navigator_.terminal()){navigator_switch_pressed_=true;navigator_pressed_target_=navigator_.target();SetCapture(hwnd_);changed();}return;
    }
    if(find_session_) {
        const auto find=find_layout();
        if(find.strip.contains(point)) {
            const std::array<Rect,4> controls{find.query,find.previous,find.next,find.close};
            for(unsigned i=0;i<controls.size();++i)if(controls[i].contains(point)){find_focus_=i;if(i){find_pressed_=i;SetCapture(hwnd_);}else find_query_.end();changed();position_ime();break;}
            return;
        }
        close_find();
    }
    notice_.clear();
    if (palette_open_ || rename_open_) {
        const auto commands=palette_commands(); const auto layout=palette_layout(commands);
        const auto& panel=layout.panel;
        if (!rename_open_) for (std::size_t row=0;row<layout.row_bounds.size();++row) {
            if (layout.row_bounds[row].contains(point)) {
                palette_index_=layout.first+row; activate_palette_entry(commands[palette_index_]);
                changed(); return;
            }
        }
        if (!panel.contains(point)) { palette_open_=rename_open_=false; query_.clear(); }
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
        if(footer_layout().directory.contains(point)){open_navigator();return;}
        if(footer_layout().agent.contains(point)) {toggle_agent_panel();return;}
        const auto footer=background_layout();
        for (std::size_t i=0;i<footer.chips.size();++i) if (footer.chips[i].contains(point)) {
            restore_parked({projects_.active_id(),projects_.active()->parked_sessions[i].session}); return;
        }
        if (footer.overflow.contains(point)) open_navigator(true);
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
        if (double_click) { if (workspace().equalize_split(divider.node)) workspace_model_changed(); changed(true); return; }
        drag_=Drag::divider; drag_node_=divider.node; drag_divider_=divider; drag_origin_=point;
        divider_grab_offset_=divider.axis==Axis::horizontal?point.x-divider.bounds.x-divider.bounds.width/2:
                                                           point.y-divider.bounds.y-divider.bounds.height/2;
        SetCapture(hwnd_); changed(); return;
    }
    for (const auto& placement:layout_.panes) if (placement.bounds.contains(point)) {
        const auto pane=placement.pane;
        const auto bounds=placement.bounds;
        activate(pane);
        if (layout_.panes.size()>1 && point.y<bounds.y+28) {
            if (point.x>=bounds.x+bounds.width-32) { popup_menu(point,true); return; }
            if (double_click) { if (workspace().toggle_zoom()) workspace_model_changed(); changed(true); return; }
            drag_=Drag::pane; drag_pane_=pane; drag_origin_=point; SetCapture(hwnd_); return;
        }
        auto* session=active_session(); auto frame=session?session->frame():nullptr;
        if (frame && frame->mouse_reporting && !(GetKeyState(VK_SHIFT)&0x8000)) {
            terminal_mouse(point,MouseAction::press,MouseButton::left,true); return;
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
    if(palette_open_||rename_open_){hover_=point;return;}
    if(navigator_open_){
        const auto nav=layout_workspace_navigator(width_,height_,navigator_,navigator_focus_);
        auto hit=[&](Point p){if(nav.close.contains(p))return std::size_t{1};for(std::size_t i=0;i<nav.workspace_rows.size();++i)if(nav.workspace_rows[i].contains(p))return i+2;for(std::size_t i=0;i<nav.terminal_rows.size();++i)if(nav.terminal_rows[i].contains(p))return i+2+nav.workspace_rows.size();return std::size_t{0};};
        const bool repaint=hit(point)!=hit(hover_);hover_=point;if(repaint)changed();return;
    }
    if(find_session_ && (find_layout().strip.contains(point)||find_layout().strip.contains(hover_))) {hover_=point;changed();}
    if (agent_panel_.is_open()) {if(agent_panel_.pointer_move(point))changed();return;}
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
        const double position=(drag_divider_.axis==Axis::horizontal?point.x-split.x:point.y-split.y)-divider_grab_offset_;
        const double gap=1/scale_;
        if (extent>gap && workspace().resize_split(drag_node_,(position-gap/2)/(extent-gap),workspace_viewport(),{1/scale_,7,80,48})) workspace_model_changed();
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
    if(navigator_open_){const bool pressed=navigator_switch_pressed_&&navigator_pressed_target_==navigator_.target();navigator_switch_pressed_=false;navigator_pressed_target_.reset();if(GetCapture()==hwnd_)ReleaseCapture();const auto nav=layout_workspace_navigator(width_,height_,navigator_,navigator_focus_);if(pressed&&Rect{nav.footer.x+nav.footer.width-104,nav.footer.y+12,88,30}.contains(point))commit_navigator();else changed();return;}

    if(find_pressed_) {
        const auto pressed=*find_pressed_;find_pressed_.reset();if(GetCapture()==hwnd_)ReleaseCapture();
        const auto find=find_layout();const std::array<Rect,4> controls{find.query,find.previous,find.next,find.close};
        if(find_session_ && controls[pressed].contains(point))activate_find_control(pressed);
        changed();return;
    }
    if (agent_panel_.is_open()) { agent_panel_.pointer_up(point); if(GetCapture()==hwnd_) ReleaseCapture(); agent_input_changed(); return; }
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
    if(palette_open_||rename_open_)return;
    if (drag_==Drag::pane && drag_started_ && drop_target_) {
        const auto status=drop_direction_?workspace().move_pane(drag_pane_,drop_target_,*drop_direction_):workspace().swap_panes(drag_pane_,drop_target_);
        if (!status) show_error(status.error); else workspace_model_changed();
    } else if (drag_==Drag::tab && drag_started_) {
        // Cross-window tear-off is intentionally not faked. A drag outside the
        // band cancels and retains the one existing owner/session.
        if (chrome_.tabs_band.contains(point)) for (std::size_t i=0;i<chrome_.tabs.size();++i) if (chrome_.tabs[i].bounds.contains(point)) { if (workspace().reorder_tab(drag_tab_,i)) workspace_model_changed(); break; }
    } else if (drag_==Drag::none) terminal_mouse(point,MouseAction::release,MouseButton::left,false);
    cancel_drag(); changed(true);
}
void NativeWindow::cancel_drag() {
    drag_=Drag::none; drag_started_=false; drag_node_={}; drag_tab_={}; drag_pane_={}; drop_target_={}; drop_preview_.reset(); drop_direction_.reset();
    if (GetCapture()==hwnd_) ReleaseCapture();
    changed();
}

void NativeWindow::smoke_phase(SmokeDeadline::Phase phase) {
    const auto now=GetTickCount64();
    const auto previous=smoke_deadline_.phase(); const auto elapsed=smoke_deadline_.phase_elapsed(now);
    if (smoke_deadline_.advance(phase,now))
        std::fprintf(stderr,"Native smoke phase=%u previous=%u previous_elapsed_ms=%llu total_elapsed_ms=%llu budget_ms=%llu hard_budget_ms=50000\n",
            unsigned(phase),unsigned(previous),static_cast<unsigned long long>(elapsed),now-smoke_started_at_,static_cast<unsigned long long>(smoke_deadline_.phase_budget()));
}

void NativeWindow::smoke_typing_diagnostic(const char* event) {
    if (!options_.smoke_test) return;
    auto* actor=active_session(); const auto frame=actor?actor->frame():FramePtr{};
    const auto info=actor?actor->info():SessionInfo{};
    std::fprintf(stderr,"Typing smoke event=%s phase=%u elapsed_ms=%llu active=%d reduced=%d visible=%d alternate=%d cursor=%u,%u expected=%u,%u input_result=%d pending=%zu frame_generation=%llu rx_bytes=%llu process_state=%d finished=%d handshake=%d/%d reveal_seen=%d motion=%d timer=%d\n",
        event,smoke_typing_stage_,GetTickCount64()-smoke_started_at_,active_,reduced_motion_,frame && frame->cursor.visible,frame && frame->alternate_screen,
        frame?unsigned(frame->cursor.column):0,frame?unsigned(frame->cursor.row):0,unsigned(smoke_typing_column_+((smoke_typing_stage_==1 || smoke_typing_stage_==4)?1:0)),unsigned(smoke_typing_row_),smoke_typing_input_result_,pending_input_.size(),
        static_cast<unsigned long long>(frame?frame->generation:0),static_cast<unsigned long long>(info.bytes_received),static_cast<int>(info.state),actor && actor->process_finished(),
        smoke_typing_handshake_sent_,smoke_typing_handshake_seen_,smoke_typing_reveal_seen_,typing_motion_.active(GetTickCount64()),typing_timer_active_);
}

// Exercise the real HWND input bridge and actual PTY echo, not a synthetic
// cursor amount. The existing overall smoke deadline bounds a missing echo.
bool NativeWindow::smoke_typing_progress() {
    if (smoke_typing_stage_==6) { KillTimer(hwnd_,typing_smoke_timer); return true; }
    if (smoke_typing_last_logged_!=smoke_typing_stage_) { smoke_typing_last_logged_=smoke_typing_stage_; smoke_typing_diagnostic("phase-enter"); }
    auto* session=active_session(); auto frame=session?session->frame():FramePtr{};
    if (!session || !frame || !active_ || frame->alternate_screen || layout_.panes.empty()) return false;
    const auto send_character=[&](WPARAM character,WPARAM virtual_key) {
        smoke_typing_input_result_=-1;
        text_vk_=virtual_key; text_flags_=0; text_modifiers_=0;
        if (virtual_key==VK_BACK) {
            // Real Backspace is consumed by key_message before TranslateMessage.
            MSG key{}; key.hwnd=hwnd_; key.message=WM_KEYDOWN; key.wParam=VK_BACK;
            if (!key_message(key)) { smoke_finish("Typing smoke Backspace was not handled by the normal key path."); return; }
        } else SendMessageW(hwnd_,WM_CHAR,character,0);
        smoke_typing_diagnostic("input-dispatched");
        text_vk_=0;
    };
    const bool at_origin=frame->cursor.row==smoke_typing_row_ && frame->cursor.column==smoke_typing_column_;
    const bool echoed=frame->cursor.row==smoke_typing_row_ && frame->cursor.column==smoke_typing_column_+1 &&
        frame->cursor.row<frame->lines.size() && frame->lines[frame->cursor.row] &&
        smoke_typing_column_<frame->lines[frame->cursor.row]->cells.size() &&
        frame->lines[frame->cursor.row]->cells[smoke_typing_column_].grapheme=="x";
    if (smoke_typing_stage_==0) {
        if (session->info().state!=SessionState::running || !session->info().bytes_received || frame->cursor.column+2>=frame->columns) return false;
        // Startup bytes are not a ready prompt. First prove the interactive
        // child has executed a controlled command and published its exact
        // controlled prompt; never type into a half-built startup frame.
        if (!smoke_typing_handshake_sent_) {
            const auto* pane=workspace().pane(workspace().active_pane());
            auto actor=sessions_.at(pane->session);
            // Controlled prompt belongs only to this disposable smoke child.
            // Other modes still exercise the configured managed prompt/hooks.
            const std::string command=using_managed_shell()?
                "precmd_functions=(); PROMPT='__STORMTTY_TYPING_PROMPT__>'; print -r -- __STORMTTY_TYPING_READY__":
                "prompt __STORMTTY_TYPING_PROMPT__$G&echo __STORMTTY_TYPING_READY__";
            // This is authored fixture setup, not a simulated typing workload.
            // Use the same negotiated bracketed-paste path as a real bulk paste;
            // click/WM_CHAR/backspace assertions below remain real key input.
            submit([actor,command]{return actor->paste(command);},command.size());
            KeyInput enter; enter.key=Key::enter;
            submit([actor,enter]{return actor->key(enter);});
            smoke_typing_handshake_sent_=true; smoke_typing_diagnostic("handshake-sent"); return false;
        }
        if (!smoke_typing_handshake_seen_) {
            for (const auto& row:frame->lines) if (row) {
                std::string line; for (const auto& cell:row->cells) if(cell.span) line+=cell.grapheme.empty()?" ":cell.grapheme;
                while (!line.empty() && line.back()==' ') line.pop_back();
                if (line=="__STORMTTY_TYPING_READY__") { smoke_typing_handshake_seen_=true; smoke_typing_diagnostic("handshake-seen"); break; }
            }
            if (!smoke_typing_handshake_seen_) return false;
        }
        constexpr std::string_view ready_prompt="__STORMTTY_TYPING_PROMPT__>";
        if (frame->cursor.row>=frame->lines.size() || !frame->lines[frame->cursor.row] || frame->cursor.column!=ready_prompt.size()) return false;
        std::string prompt;
        for (const auto& cell:frame->lines[frame->cursor.row]->cells) if (cell.span) prompt+=cell.grapheme.empty()?" ":cell.grapheme;
        while (!prompt.empty() && prompt.back()==' ') prompt.pop_back();
        if (prompt!=ready_prompt || (!reduced_motion_ && !frame->cursor.visible)) return false;
        smoke_typing_diagnostic("prompt-ready");
        smoke_typing_column_=frame->cursor.column; smoke_typing_row_=frame->cursor.row;
        smoke_typing_reduced_=reduced_motion_;
        const auto bounds=content_bounds(layout_.panes.front());
        const Point point{bounds.x+4,bounds.y+4};
        pointer_down(point,false); pointer_up(point);
        if (drag_!=Drag::none || GetCapture()==hwnd_) { smoke_finish("Typing smoke click retained capture/selection drag."); return false; }
        (void)scene(); // Normal host observation establishes the post-click baseline.
        smoke_typing_stage_=1; send_character(L'x',L'X'); return false;
    }
    if (smoke_typing_stage_==1) {
        if (!echoed) return false;
        if (smoke_typing_input_result_!=static_cast<int>(SessionSendResult::accepted)) { smoke_finish("Typing echo was observed without accepted host input."); return false; }
        if (!smoke_typing_reduced_ && !smoke_typing_reveal_seen_) { smoke_finish("Accepted WM_CHAR echo after terminal click did not start cursor settle."); return false; }
        if (smoke_typing_reduced_ && (typing_motion_.active(GetTickCount64()) || typing_timer_active_)) { smoke_finish("OS reduced motion allowed typing animation."); return false; }
        smoke_typing_stage_=2;
    }
    if (smoke_typing_stage_==2) {
        if (typing_motion_.active(GetTickCount64()) || typing_timer_active_) return false;
        smoke_typing_stage_=3; send_character(L'\b',VK_BACK); return false;
    }
    if (smoke_typing_stage_==3) {
        if (!at_origin) return false;
        if (smoke_typing_input_result_!=static_cast<int>(SessionSendResult::accepted)) { smoke_finish("Typing smoke erase did not use accepted input."); return false; }
        if (smoke_typing_reduced_) {
            std::fprintf(stderr,"Typing host smoke: click/WM_CHAR/PTY echo and reduced-motion idle verified; active settle unavailable under OS reduced motion.\n");
            smoke_typing_stage_=6; return true;
        }
        smoke_typing_reveal_seen_=false; smoke_typing_stage_=4; send_character(L'x',L'X'); return false;
    }
    if (smoke_typing_stage_==4) {
        if (!echoed) return false;
        if (smoke_typing_input_result_!=static_cast<int>(SessionSendResult::accepted)) { smoke_finish("Typing echo was observed without accepted host input."); return false; }
        if (!smoke_typing_reveal_seen_) { smoke_finish("Repeated host typing did not start cursor settle."); return false; }
        reduced_motion_=true;
        const auto reduced_scene=scene();
        const bool canceled=!typing_motion_.active(GetTickCount64()) && !typing_timer_active_ &&
            std::all_of(reduced_scene.panes.begin(),reduced_scene.panes.end(),[](const auto& pane){return pane.cursor_reveal==1;});
        reduced_motion_=smoke_typing_reduced_;
        if (!canceled) { smoke_finish("Reduced motion did not cancel the host typing timer/presentation."); return false; }
        smoke_typing_stage_=5; send_character(L'\b',VK_BACK); return false;
    }
    if (smoke_typing_stage_==5 && at_origin) {
        if (smoke_typing_input_result_!=static_cast<int>(SessionSendResult::accepted)) { smoke_finish("Typing smoke final erase did not use accepted input."); return false; }
        std::fprintf(stderr,"Typing host smoke: real click/release, WM_CHAR acceptance and PTY echo, active settle, finite idle and reduced-motion cancellation verified.\n");
        smoke_typing_stage_=6; return true;
    }
    return false;
}

void NativeWindow::capture_named_frame(std::wstring_view suffix) {
    if (!options_.smoke_test || options_.capture_path.empty()) return;
    const auto original=options_.capture_path;
    options_.capture_path=original.parent_path()/(original.stem().wstring()+L"-"+std::wstring(suffix)+original.extension().wstring());
    try { capture_frame(); } catch (...) { options_.capture_path=original; throw; }
    options_.capture_path=original;
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
// Sample real native scenes at a deterministic presentation time. This does not
// inject terminal content or claim wall-clock frame pacing / physical desktop QA.
void NativeWindow::smoke_motion_capture(std::wstring_view suffix, std::uint64_t elapsed) {
    if (!options_.smoke_test || !smoke_chrome_time_) throw std::runtime_error("Motion capture requires explicit native smoke clock.");
    const auto original=options_.capture_path;
    ++chrome_generation_; force_redraw_=true;
    const auto actual=scene();
    if(smoke_focus_running_ && options_.smoke_layout && smoke_pane_reduced_motion_.has_value()) {
        smoke_pane_focus_scene(actual);
        std::fprintf(stderr,"Native pane decoration sample=%s policy=%s host_reduced_motion=%d controlled_clock=1 physical_frame_pacing_verified=0\n",
            narrow(std::wstring(suffix)).c_str(),smoke_pane_reduced_motion_.value_or(reduced_motion_)?"synthetic_reduced_motion":"synthetic_normal_motion",reduced_motion_);
    }
    if(options_.smoke_settings && suffix.starts_with(L"appearance-menu-")) {
        // Reuse these existing normal/narrow captures: no extra Settings
        // stages, input, scene evaluations or resize cycles for chrome evidence.
        const auto ui=resolve_ui_colors(config_.theme);
        const auto colors=tab_selection_colors(ui,high_contrast_,system_color(COLOR_WINDOW),system_color(COLOR_WINDOWTEXT));
        const auto selected=std::find_if(chrome_.tabs.begin(),chrome_.tabs.end(),[&](const auto& tab){return tab.tab==workspace().active_tab();});
        if(selected==chrome_.tabs.end() || std::none_of(actual.primitives.begin(),actual.primitives.end(),[&](const auto& p){return p.rect==pixels(selected->bounds)&&p.fill==colors.surface&&p.stroke==colors.border;}))
            throw std::runtime_error("Native selected tab lost semantic surface/border colors.");
        WindowScene icon;append_settings_icon(icon,{navigation_.settings.x+18,navigation_.settings.y+18},8,
            high_contrast_?system_color(COLOR_BTNTEXT):ui.secondary,static_cast<float>(scale_),1.2);
        for(const auto& stroke:icon.primitives)if(std::find(actual.primitives.begin(),actual.primitives.end(),stroke)==actual.primitives.end())
            throw std::runtime_error("Native sidebar Settings lost its owned monochrome cog.");
        if(icon.primitives.size()!=56 || std::any_of(actual.texts.begin(),actual.texts.end(),[](const auto& text){return text.text=="⚙";}))
            throw std::runtime_error("Native sidebar Settings fell back to a font/emoji icon.");
        std::fprintf(stderr,"Native chrome consistency sample=%s selected_tab=semantic settings_icon=owned_monochrome shared_paint_hit_geometry=1\n",narrow(std::wstring(suffix)).c_str());
    }
    RenderResult result=RenderResult::busy;
    const auto deadline=GetTickCount64()+2000;
    do {
        { RendererCallGuard guard(renderer_call_); result=renderer_->render(actual); }
        if (result!=RenderResult::busy) break;
        Sleep(1);
    } while (GetTickCount64()<deadline);
    if (result!=RenderResult::presented) throw std::runtime_error("Native motion scene did not present before readback.");
    if (!original.empty()) {
        options_.capture_path=original.parent_path()/(original.stem().wstring()+L"-"+std::wstring(suffix)+original.extension().wstring());
        try { capture_frame(); } catch (...) { options_.capture_path=original; throw; }
        options_.capture_path=original;
    }
    std::fprintf(stderr,"Native motion sample=%s presentation_elapsed_ms=%llu controlled_clock=1 reduced_motion=%d native_present=1 physical_frame_pacing_verified=0\n",
        narrow(std::wstring(suffix)).c_str(),static_cast<unsigned long long>(elapsed),reduced_motion_);
    std::fprintf(stderr,"Native motion values settings=%.6f menu=%.6f selected_workspace=%.6f active_pane=%.6f chrome_timer=%d\n",
        settings_transition_.value(chrome_now()),menu_transition_.value(chrome_now()),
        workspace_focus_[projects_.active_id()].value(chrome_now()),pane_focus_[workspace().active_pane()].value(chrome_now()),motion_timer_active_);
    if(smoke_settings_scroll_running_)
        std::fprintf(stderr,"Native Settings scroll sample=%s displayed_dip=%.6f target_dip=%.6f active=%d synthetic_reduced_motion=%d controlled_clock=1 physical_frame_pacing_verified=0\n",
            narrow(std::wstring(suffix)).c_str(),settings_panel_.scroll_offset(),settings_panel_.scroll_target(),settings_panel_.scroll_active(),
            smoke_settings_scroll_reduced_motion_.value_or(reduced_motion_));
}
bool NativeWindow::smoke_watch_progress() {
    if(smoke_watch_stage_==4)return true;
    const auto fail=[&](std::string_view message){smoke_finish(message);return false;};
    const auto key=[&](WPARAM vk){MSG message{};message.hwnd=hwnd_;message.message=WM_KEYDOWN;message.wParam=vk;key_message(message);message.message=WM_KEYUP;key_message(message);};
    const auto capture=[&](std::wstring_view name){smoke_chrome_time_=GetTickCount64()+1000;smoke_motion_capture(name,0);smoke_chrome_time_.reset();};
    if(!smoke_watch_stage_) {
        if(workspace().state().tabs.size()<3)return fail("Watch fixture requires existing background tabs.");
        smoke_watch_original_=workspace().active_pane();smoke_watch_project_=projects_.active_id();
        smoke_watch_pane_=workspace().state().tabs.back().active_pane;activate(smoke_watch_pane_);
        smoke_watch_session_=workspace().pane(smoke_watch_pane_)->session;smoke_watch_identity_=watch_identity(smoke_watch_session_);
        smoke_watch_tab_title_=workspace().tab(workspace().active_tab())->title;
        if(!workspace().rename_tab(workspace().active_tab(),"Long watched build title · 日本語 · deliberately beyond the visible tab width"))return fail("Watch long-title fixture rejected.");
        smoke_watch_actor_count_=sessions_.size();smoke_watch_input_=smoke_terminal_input_submissions_;smoke_watch_mouse_=smoke_terminal_mouse_submissions_;
        const auto other=projects_.create({"Watch review",projects_.active()->metadata.working_directory,"Local"});
        if(!other)return fail("Watch fixture could not create its empty owned workspace.");smoke_watch_other_=*other.value;
        std::wstring module(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
        if(!length||length>=module.size())return fail("Watch fixture child path unavailable.");module.resize(length);
        const auto child=std::filesystem::path(module).parent_path()/L"terminal_search_tests.exe";
        const auto command="\""+narrow(child.generic_wstring())+"\" --native-watch-child";
        auto actor=sessions_.at(smoke_watch_session_);
        if(!std::filesystem::is_regular_file(child)||!TerminalCore::paste_is_safe(command)||actor->paste(command)!=SessionSendResult::accepted)return fail("Watch native child launch input rejected.");
        KeyInput enter;enter.key=Key::enter;if(actor->key(enter)!=SessionSendResult::accepted)return fail("Watch native child Enter rejected.");
        smoke_watch_stage_=1;return false;
    }
    const auto found=sessions_.find(smoke_watch_session_);if(found==sessions_.end()||watch_identity(smoke_watch_session_)!=smoke_watch_identity_)return fail("Watch fixture lost its original actor incarnation.");
    const auto actor=found->second;
    const auto has_marker=[&](std::string_view marker){const auto frame=actor->frame();if(!frame)return false;for(const auto& row:frame->lines){std::string text;for(const auto& cell:row->cells)text+=cell.grapheme;while(!text.empty()&&text.back()==' ')text.pop_back();if(text==marker)return true;}return false;};
    if(smoke_watch_stage_==1) {
        const auto info=actor->info();
        if(!has_marker("WATCH_READY")||info.command.phase!=CommandPhase::running||info.command.command_generation<=smoke_watch_generation_)return false;
        smoke_watch_generation_=info.command.command_generation;
        if(projects_.active_id()!=smoke_watch_project_){projects_.activate(smoke_watch_project_);reset_project_view();}
        activate(smoke_watch_pane_);const auto before=layout_.panes;
        toggle_palette();query_="Watch this command";const auto commands=palette_commands();
        if(commands.empty()||commands.front().id!="watch.arm")return fail("Watch action missing from Commands.");key(VK_RETURN);
        if(palette_open_||!watches_.contains(smoke_watch_session_)||watches_.at(smoke_watch_session_).status()!=WatchStatus::watching||layout_.panes.size()!=before.size())return fail("Watch arming changed placement or failed eligibility.");
        if(smoke_watch_cycle_==0) {
            if(tab_watch_indicator(workspace().active_tab())!=TabWatchIndicator::watching)return fail("Watching tab indicator absent.");
            capture(L"watch-armed");
        }
        if(smoke_watch_cycle_==2){park_pane(smoke_watch_pane_);if(projects_.active()->parked_sessions.size()!=1)return fail("Watch parking lost its single parked representation.");}
        if(smoke_watch_cycle_==6){dispatch("watch.cancel");if(watches_.contains(smoke_watch_session_)||actor->process_finished())return fail("Cancel watch stopped the actor or retained the watch.");}
        if(smoke_watch_cycle_==1){projects_.activate(smoke_watch_other_);reset_project_view();}
        else activate(smoke_watch_original_);
        const std::array<char,7> controls{'0','7','u','c','l','r','0'};
        if(actor->text(std::string(1,controls[smoke_watch_cycle_]))!=SessionSendResult::accepted)return fail("Watch fixture result control rejected.");
        smoke_watch_stage_=2;return false;
    }
    if(smoke_watch_stage_==2) {
        const std::array<std::string_view,7> markers{"WATCH_RESULT_0","WATCH_RESULT_7","WATCH_RESULT_UNKNOWN","WATCH_COALESCED_EXACT","WATCH_COALESCED_LATER","WATCH_RESET_RESULT","WATCH_RESULT_0"};
        if(!has_marker(markers[smoke_watch_cycle_]))return false;
        if(smoke_watch_cycle_==6) {
            if(watches_.contains(smoke_watch_session_))return fail("Canceled watch automatically armed or replayed a result.");
        } else {
            const auto watched=watches_.find(smoke_watch_session_);if(watched==watches_.end()||!watched->second.unread())return false;
            const auto& watch=watched->second;
            if(smoke_watch_cycle_<4 && (watch.status()!=WatchStatus::completed || watch.exit_code()!=(smoke_watch_cycle_==2?std::optional<std::int32_t>{}:std::optional<std::int32_t>{smoke_watch_cycle_==1?7:0})))return fail("Watch attributed the wrong completed generation/status.");
            if(smoke_watch_cycle_==4 && !((watch.status()==WatchStatus::completed&&watch.exit_code()==0)||watch.status()==WatchStatus::completion_unconfirmed))return fail("Watch borrowed a later command result.");
            if(smoke_watch_cycle_==5 && (watch.status()==WatchStatus::completed||watch.exit_code()))return fail("Watch claimed command success after reset.");
            if((smoke_watch_cycle_==1 && projects_.active_id()!=smoke_watch_other_) || (smoke_watch_cycle_!=1 && workspace().active_pane()!=smoke_watch_original_))return fail("Watch completion navigated or moved focus automatically.");
            if(sessions_.size()!=smoke_watch_actor_count_||actor->process_finished())return fail("Watch completion replaced or stopped its live shell actor.");
            capture(L"watch-result-"+std::to_wstring(smoke_watch_cycle_));
            if(smoke_watch_cycle_==0) {
                activate(smoke_watch_pane_);
                if(tab_watch_indicator(workspace().active_tab())!=TabWatchIndicator::unread)return fail("Unread tab indicator absent.");
                RECT original{};GetWindowRect(hwnd_,&original);
                SetWindowPos(hwnd_,nullptr,0,0,static_cast<int>(480*scale_),static_cast<int>(480*scale_),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                capture(L"watch-indicator-narrow");
                const bool contrast=high_contrast_;high_contrast_=true;capture(L"watch-indicator-high-contrast-fixture");high_contrast_=contrast;
                SetWindowPos(hwnd_,nullptr,0,0,original.right-original.left,original.bottom-original.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                activate(smoke_watch_original_);
                if(!watch_unread(smoke_watch_session_))return fail("Watch indicator capture acknowledged result.");
                dispatch("terminal.previous");
                if(active_session()!=actor.get()||!watch_unread(smoke_watch_session_))return fail("Return acknowledged a real unread watched result or lost its actor.");
                dispatch("terminal.previous");
                if(workspace().active_pane()!=smoke_watch_original_||!watch_unread(smoke_watch_session_))return fail("Return from watched result failed to preserve work and unread state.");
            }
            if(smoke_watch_cycle_==1){projects_.activate(smoke_watch_project_);reset_project_view();}
            if(smoke_watch_cycle_==2) {
                if(background_summary().unread()!=1)return fail("Watch and parked attention double-counted one completion.");
                close_parked({smoke_watch_project_,smoke_watch_session_});if(!modal_)return fail("Watch parked close lacked confirmation.");answer_modal(ModalDecision::cancel);
                if(!watch_unread(smoke_watch_session_))return fail("Canceled close acknowledged watch.");
                auto stale=smoke_watch_identity_;++stale.actor;open_watch({smoke_watch_project_,smoke_watch_session_},stale);
                if(!watch_unread(smoke_watch_session_))return fail("Failed result navigation acknowledged watch.");notice_.clear();
                restore_parked({smoke_watch_project_,smoke_watch_session_});smoke_watch_pane_=workspace().active_pane();
                if(active_session()!=actor.get()||!watch_unread(smoke_watch_session_))return fail("Ordinary restore acknowledged watch or changed actor.");
                park_pane(smoke_watch_pane_);activate(smoke_watch_original_);
                if(background_summary().unread()!=1)return fail("Complete/restore/repark multiplied watch indicators.");
            } else {activate(smoke_watch_pane_);activate(smoke_watch_original_);if(!watch_unread(smoke_watch_session_))return fail("Ordinary tab visibility acknowledged watch.");}
            toggle_palette();query_="Open result";palette_index_=0;const auto entries=palette_commands();
            if(entries.empty()||!entries.front().watch||entries.front().watch!=smoke_watch_identity_)return fail("Qualified watch result absent from Commands.");
            capture(L"watch-open-commands");
            if(smoke_watch_cycle_==1){const auto layout=palette_layout(entries);if(layout.row_bounds.empty())return fail("Watch pointer row unavailable.");const auto row=layout.row_bounds.front();pointer_down({row.x+20,row.y+row.height/2},false);}
            else key(VK_RETURN);
            if(active_session()!=actor.get()||watches_.contains(smoke_watch_session_)||sessions_.size()!=smoke_watch_actor_count_)return fail("Open result did not acknowledge exactly the existing actor.");
            smoke_watch_pane_=workspace().active_pane();
        }
        if(smoke_terminal_input_submissions_!=smoke_watch_input_||smoke_terminal_mouse_submissions_!=smoke_watch_mouse_)return fail("Watch UI submitted terminal input.");
        if(++smoke_watch_cycle_<7){smoke_watch_generation_=actor->info().command.command_generation;if(actor->text("a")!=SessionSendResult::accepted)return fail("Watch next fixture command rejected.");smoke_watch_stage_=1;return false;}
        if(actor->text("q")!=SessionSendResult::accepted)return fail("Watch child stop rejected.");smoke_watch_stage_=3;return false;
    }
    if(!has_marker("WATCH_DONE"))return false;
    if(projects_.active_id()!=smoke_watch_project_){projects_.activate(smoke_watch_project_);reset_project_view();}
    if(const auto tab=workspace().tab_of(smoke_watch_pane_))workspace().rename_tab(*tab,smoke_watch_tab_title_);
    activate(smoke_watch_original_);const auto removed=projects_.remove(smoke_watch_other_);if(!removed)return fail("Watch empty fixture workspace could not be removed.");
    if(layout_.panes.size()!=3||sessions_.size()!=smoke_watch_actor_count_||!watches_.empty())return fail("Watch fixture changed original layout ownership or left an unread result.");
    smoke_watch_stage_=4;changed(true);
    std::fputs("Native command watch: explicit arm, hidden-tab/workspace completion, zero/nonzero/unknown status, coalesced generations/reset without borrowed success, cancel without stopping, same-actor keyboard/pointer Open, ordinary visibility/restore retains unread, park/restore/repark dedup, canceled close and stale identity rejection; zero frontend UI input. Native fixture OSC observations, not external-process or physical performance proof.\n",stderr);
    return true;
}
bool NativeWindow::smoke_context_progress() {
    if(smoke_context_stage_==3)return true;
    const auto fail=[&](std::string_view message){smoke_finish(message);return false;};
    if(!smoke_context_stage_) {
        std::wstring module(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
        if(!length||length>=module.size())return fail("Context fixture could not resolve its native child.");
        module.resize(length);const auto child=std::filesystem::path(module).parent_path()/L"terminal_search_tests.exe";
        if(!std::filesystem::is_regular_file(child))return fail("Context fixture child is missing.");
        unsigned index=0;
        for(const auto& placement:layout_.panes) {
            auto actor=sessions_.at(placement.session);smoke_context_actors_.push_back({placement.pane,actor});
            const auto command="\""+narrow(child.generic_wstring())+"\" --native-context-child "+std::to_string(index++);
            if(!TerminalCore::paste_is_safe(command)||actor->paste(command)!=SessionSendResult::accepted)return fail("Context fixture safe input was not accepted.");
            KeyInput enter;enter.key=Key::enter;if(actor->key(enter)!=SessionSendResult::accepted)return fail("Context fixture Enter was not accepted.");
        }
        if(index!=3)return fail("Context fixture requires three existing distinct pane actors.");
        smoke_context_stage_=1;return false;
    }
    if(smoke_context_stage_==1) {
        unsigned ready_index=0;
        for(const auto& [pane,actor]:smoke_context_actors_) {
            const auto info=actor->info();if(!info.shell_context)return false;
            if(!info.shell_context->directory_key.starts_with("file:///C:/context-fixture-"))return false;
            const auto context=pane_context(pane);
            if(context.directory!=info.shell_context->display_directory || context.primary!=pane_context_text(info.title))return false;
            const auto expected_title=ready_index==0?"cmd.exe":ready_index==1?"Review · café":"Build · 日本語";
            if(info.title!=expected_title)return false;
            const auto frame=actor->frame();if(!frame || actor->diagnostics().queued_input_bytes)return false;
            bool ready=false;const auto marker="STORMTTY_CONTEXT_READY_"+std::to_string(ready_index++);
            for(const auto& row:frame->lines) {
                std::string text;for(const auto& cell:row->cells)text+=cell.grapheme;
                while(!text.empty()&&text.back()==' ')text.pop_back();ready=ready||text==marker;
            }
            // Context metadata may arrive before the rest of this native child's
            // startup packet. A final visible marker is the output/ready gate,
            // never the echoed command or an arbitrary settling delay.
            if(!ready)return false;
        }
        const auto original=workspace().active_pane();const auto count=sessions_.size();
        const auto baseline=smoke_terminal_input_submissions_;const auto mouse_baseline=smoke_terminal_mouse_submissions_;
        const auto key=[&](WPARAM vk){MSG m{};m.hwnd=hwnd_;m.message=WM_KEYDOWN;m.wParam=vk;key_message(m);m.message=WM_KEYUP;key_message(m);};
        const auto capture=[&](std::wstring_view name){smoke_chrome_time_=GetTickCount64()+1000;smoke_motion_capture(name,0);smoke_chrome_time_.reset();};
        if(pane_context(smoke_context_actors_[0].first).primary!=pane_context_text(smoke_context_actors_[0].second->info().title) || !pane_context(smoke_context_actors_[1].first).detached)return fail("Context title provenance or detached HEAD changed.");
        capture(L"context-headers");
        for(const auto& [pane,actor]:smoke_context_actors_) {
            activate(pane);const auto before_info=actor->info();const auto before=before_info.bytes_sent;const auto geometry=layout_.panes;const auto frame=actor->frame();
            toggle_palette();query_="Inspect pane context";const auto commands=palette_commands();
            if(commands.empty()||commands.front().id!="pane.inspect")return fail("Context command is not discoverable.");
            key(VK_RETURN);
            if(!palette_open_||!inspected_context_||focused_session_||inspected_context_->directory!=actor->info().shell_context->display_directory)return fail("Context inspection lost its owned snapshot or input ownership.");
            key(VK_END);key(VK_HOME);terminal_text("Repository");key(VK_RETURN);query_.clear();
            const auto after_info=actor->info();
            std::fprintf(stderr,"Native context isolation: pane=%llu bytes_before=%llu bytes_after=%llu frontend_delta=%llu mouse_delta=%llu actor_same=%u actors_before=%zu actors_after=%zu placements_before=%zu placements_after=%zu queued_input=%zu\n",
                static_cast<unsigned long long>(pane.value),static_cast<unsigned long long>(before),static_cast<unsigned long long>(after_info.bytes_sent),
                static_cast<unsigned long long>(smoke_terminal_input_submissions_-baseline),static_cast<unsigned long long>(smoke_terminal_mouse_submissions_-mouse_baseline),
                unsigned(active_session()==actor.get()),count,sessions_.size(),geometry.size(),layout_.panes.size(),actor->diagnostics().queued_input_bytes);
            const auto frontend=after_info.sent.frontend()-before_info.sent.frontend();
            const auto focus=after_info.sent.focus_bytes-before_info.sent.focus_bytes;
            const auto protocol=after_info.sent.protocol_bytes-before_info.sent.protocol_bytes;
            std::fprintf(stderr,"Native context write origins: frontend=%llu focus=%llu protocol=%llu total_before=%llu total_after=%llu\n",static_cast<unsigned long long>(frontend),static_cast<unsigned long long>(focus),static_cast<unsigned long long>(protocol),static_cast<unsigned long long>(before_info.sent.total()),static_cast<unsigned long long>(after_info.sent.total()));
            if(before_info.sent.total()!=before || after_info.sent.total()!=after_info.bytes_sent || after_info.bytes_sent-before!=frontend+focus+protocol)return fail("Context inspection has unattributed transport bytes.");
            if(frontend || smoke_terminal_input_submissions_!=baseline || smoke_terminal_mouse_submissions_!=mouse_baseline)return fail("Read-only context inspection submitted frontend input.");
            if(layout_.panes.size()!=geometry.size()||active_session()!=actor.get()||sessions_.size()!=count)return fail("Read-only context inspection changed pane or actor identity.");
            const auto after=actor->frame();if(!frame||!after||frame->columns!=after->columns||frame->rows!=after->rows)return fail("Context inspection changed the terminal grid.");
            capture(L"context-inspect");key(VK_ESCAPE);
        }
        activate(smoke_context_actors_[2].first);dispatch("pane.inspect");
        RECT original_window{};GetWindowRect(hwnd_,&original_window);
        SetWindowPos(hwnd_,nullptr,0,0,static_cast<int>(480*scale_),static_cast<int>(480*scale_),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        capture(L"context-narrow");const bool contrast=high_contrast_;high_contrast_=true;capture(L"context-high-contrast-fixture");high_contrast_=contrast;
        SetWindowPos(hwnd_,nullptr,0,0,original_window.right-original_window.left,original_window.bottom-original_window.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        key(VK_ESCAPE);activate(original);
        smoke_workspace_frame_evidence();
        if(smoke_terminal_input_submissions_!=baseline||smoke_terminal_mouse_submissions_!=mouse_baseline)return fail("Context keyboard inspection leaked frontend input.");
        for(const auto& [pane,actor]:smoke_context_actors_){(void)pane;if(actor->text("q")!=SessionSendResult::accepted)return fail("Context fixture stop was rejected.");}
        smoke_context_stage_=2;return false;
    }
    for(const auto& [pane,actor]:smoke_context_actors_) {
        if(session_for(pane)!=actor.get())return fail("Context fixture changed pane actor ownership.");
        const auto info=actor->info();if(info.shell_context&&info.shell_context->directory_key.starts_with("file:///C:/context-fixture-"))return false;
    }
    smoke_context_stage_=3;
    std::fputs("Native pane context: three distinct owned actors, exact OSC titles preserved, observed directory/branch/detached HEAD, Unicode long path, keyboard Commands inspection, zero frontend PTY input with all focus/protocol writes attributed, unchanged grid and identity, narrow/high-contrast captures; context from explicit test child, not live Git discovery.\n",stderr);
    return true;
}
bool NativeWindow::smoke_palette_input_progress() {
    if(smoke_palette_input_stage_==2)return true;
    auto* actor=active_session();if(!actor){smoke_finish("Palette ownership smoke lost its actor.");return false;}
    auto owned=sessions_.at(workspace().pane(workspace().active_pane())->session);
    auto mode=[&](bool enable){
        if(!enable) {
            if(owned->text("q")!=SessionSendResult::accepted){smoke_finish("Palette fixture stop sentinel was not accepted.");return false;}
            return true;
        }
        std::wstring module(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
        if(!length||length>=module.size()){smoke_finish("Palette fixture could not resolve its native child.");return false;}
        module.resize(length);
        const auto child=std::filesystem::path(module).parent_path()/L"terminal_search_tests.exe";
        const std::string command=native_mouse_child_command(narrow(child.generic_wstring()));
        if(!std::filesystem::is_regular_file(child)){smoke_finish("Palette fixture native child is missing.");return false;}
        if(command.empty()||!TerminalCore::paste_is_safe(command)||owned->paste(command)!=SessionSendResult::accepted){smoke_finish("Palette fixture safe paste was not accepted.");return false;}
        KeyInput enter;enter.key=Key::enter;
        if(owned->key(enter)!=SessionSendResult::accepted){smoke_finish("Palette fixture Enter was not accepted.");return false;}
        return true;
    };
    if(!smoke_palette_input_stage_){if(mode(true))smoke_palette_input_stage_=1;return false;}
    const auto frame=actor->frame();if(!frame)return false;
    if(smoke_palette_disable_sent_){if(frame->mouse_reporting)return false;
        bool done=false;for(const auto& row:frame->lines){std::string text;for(const auto& cell:row->cells)text+=cell.grapheme;while(!text.empty()&&text.back()==' ')text.pop_back();done=done||text=="STORMTTY_MOUSE_DONE";}if(!done)return false;const auto sent=actor->info();if(!sent.sent.mouse_bytes||sent.sent.total()!=sent.bytes_sent){smoke_finish("Palette mouse transport origin accounting failed.");return false;}smoke_palette_input_stage_=2;std::fputs("Native palette ownership smoke: acknowledged1003/1006 mouse mode, captured-button release before transfer, zero hidden mouse/key ingress, Escape/Enter release consumption, real divider ratio and capture cancellation verified.\n",stderr);return true;}
    if(!frame->mouse_reporting)return false;
    const auto pane=std::find_if(layout_.panes.begin(),layout_.panes.end(),[&](const auto& p){return p.pane==workspace().active_pane();});
    if(pane==layout_.panes.end()||layout_.dividers.empty()){smoke_finish("Palette ownership smoke requires its real split layout.");return false;}
    const auto content=content_bounds(*pane);const Point inside{content.x+20,content.y+20};
    const auto mouse=[&](UINT message,Point p,WPARAM flags=0){SendMessageW(hwnd_,message,flags,MAKELPARAM(static_cast<int>(p.x*scale_),static_cast<int>(p.y*scale_)));};
    const auto key=[&](WPARAM vk){MSG message{};message.hwnd=hwnd_;message.message=WM_KEYDOWN;message.wParam=vk;key_message(message);message.message=WM_KEYUP;key_message(message);};
    std::fprintf(stderr,"Native palette press before: point=%.1f,%.1f content=%.1f,%.1f,%.1f,%.1f pane=%llu active=%llu mouse=%d shift=%d overlay=%d drag=%d capture=%d\n",inside.x,inside.y,content.x,content.y,content.width,content.height,static_cast<unsigned long long>(pane->pane.value),static_cast<unsigned long long>(workspace().active_pane().value),frame->mouse_reporting,bool(GetKeyState(VK_SHIFT)&0x8000),overlay_open(),int(drag_),GetCapture()==hwnd_);
    mouse(WM_LBUTTONDOWN,inside,MK_LBUTTON);
    std::fprintf(stderr,"Native palette press after: active=%llu mouse_capture=%llu capture=%d drag=%d submissions=%llu overlay=%d\n",static_cast<unsigned long long>(workspace().active_pane().value),static_cast<unsigned long long>(mouse_capture_pane_.value),GetCapture()==hwnd_,int(drag_),static_cast<unsigned long long>(smoke_terminal_mouse_submissions_),overlay_open());
    if(!mouse_capture_pane_||GetCapture()!=hwnd_){smoke_finish("Palette fixture did not acquire terminal mouse capture.");return false;}
    const auto before_release=smoke_terminal_mouse_submissions_;toggle_palette();
    if(mouse_capture_pane_||GetCapture()==hwnd_||focused_session_||smoke_terminal_mouse_submissions_!=before_release+1){smoke_finish("Palette failed to release old terminal pointer ownership before transfer.");return false;}
    const auto mouse_baseline=smoke_terminal_mouse_submissions_,key_baseline=smoke_terminal_input_submissions_;
    mouse(WM_MOUSEMOVE,{inside.x+12,inside.y+12});mouse(WM_LBUTTONUP,inside);key(VK_ESCAPE);
    if(palette_open_||!focused_session_||smoke_terminal_mouse_submissions_!=mouse_baseline||smoke_terminal_input_submissions_!=key_baseline){smoke_finish("Palette motion or Escape release leaked to the terminal.");return false;}
    const auto divider=layout_.dividers.front();const auto ratio=workspace().node(divider.node)->ratio;const Point handle{divider.hit_bounds.x+divider.hit_bounds.width/2,divider.hit_bounds.y+divider.hit_bounds.height/2};
    mouse(WM_LBUTTONDOWN,handle,MK_LBUTTON);
    if(drag_!=Drag::divider||GetCapture()!=hwnd_){smoke_finish("Palette fixture did not acquire real divider drag.");return false;}
    toggle_palette();mouse(WM_MOUSEMOVE,{handle.x+80,handle.y+80},MK_LBUTTON);mouse(WM_LBUTTONUP,{handle.x+80,handle.y+80});
    if(drag_!=Drag::none||GetCapture()==hwnd_||workspace().node(divider.node)->ratio!=ratio){smoke_finish("Palette allowed an underlying divider mutation after ownership transfer.");return false;}
    key(VK_ESCAPE);dispatch("tab.rename");const auto rename_baseline=smoke_terminal_input_submissions_;key(VK_RETURN);
    if(rename_open_||smoke_terminal_input_submissions_!=rename_baseline){smoke_finish("Rename Enter release leaked to the terminal.");return false;}
    if(mode(false))smoke_palette_disable_sent_=true;return false;
}

bool NativeWindow::smoke_navigator_evidence() {
    if(smoke_navigator_done_)return true;
    const auto original_project=projects_.active_id();auto* original_actor=active_session();const auto actors=sessions_.size();
    const auto baseline=smoke_terminal_input_submissions_;
    const auto fail=[&](std::string_view error){smoke_finish(error);return false;};
    const auto capture=[&](std::wstring_view name){smoke_chrome_time_=GetTickCount64()+1000;smoke_motion_capture(name,0);smoke_chrome_time_.reset();};
    const auto key=[&](WPARAM vk){MSG message{};message.hwnd=hwnd_;message.message=WM_KEYDOWN;message.wParam=vk;key_message(message);message.message=WM_KEYUP;key_message(message);};
    open_navigator();if(!navigator_open_||focused_session_||!navigator_.terminal())return fail("Navigator failed to own input or enumerate existing terminal.");
    if(navigator_.workspace()->project!=original_project||navigator_.workspace()->name!=agent_display_text(projects_.active()->metadata.name,512)||navigator_.terminal()->title.empty()||navigator_.terminal()->state.empty())return fail("Navigator selected metadata does not match the live catalog.");
    capture(L"navigator-normal");
    navigator_focus_=1;key(VK_DOWN);
    if(projects_.active_id()!=original_project||active_session()!=original_actor)return fail("Navigator preview switched the live workspace.");
    capture(L"navigator-preview");
    RECT bounds{};GetWindowRect(hwnd_,&bounds);SetWindowPos(hwnd_,nullptr,0,0,static_cast<int>(480*scale_),static_cast<int>(480*scale_),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    capture(L"navigator-narrow-workspaces");navigator_focus_=2;changed();capture(L"navigator-narrow-terminals");
    SetWindowPos(hwnd_,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    navigator_query_.select_all();terminal_text("STORMTTY_NAVIGATOR_NO_MATCH_713");
    if(navigator_.target())return fail("Navigator no-match query retained an actionable target.");
    capture(L"navigator-empty");key(VK_ESCAPE);
    if(navigator_open_||!focused_session_||active_session()!=original_actor||smoke_terminal_input_submissions_!=baseline)return fail("Navigator dismissal leaked input or changed terminal focus.");
    open_navigator();navigator_focus_=1;
    for(std::size_t i=0;i<navigator_.workspaces().size();++i)if(navigator_.entries()[navigator_.workspaces()[i]].project==smoke_secondary_project_)navigator_.select_workspace(i);
    const auto target=navigator_.target();if(!target||!target->session)return fail("Navigator cross-workspace target unavailable.");
    navigator_focus_=2;key(VK_RETURN);
    if(navigator_open_||projects_.active_id()!=target->project||active_session()!=sessions_.at(target->session).get()||sessions_.size()!=actors)return fail("Navigator commit did not preserve the target actor.");
    activate_project(original_project);
    open_navigator();const auto stale=navigator_.target();if(!stale||!stale->session)return fail("Navigator stale-target fixture unavailable.");
    // Remove the exact actor association after taking the view, while retaining
    // ownership locally so the fixture never kills or replaces the process.
    auto retained=sessions_.at(stale->session);sessions_.erase(stale->session);commit_navigator();
    const bool rejected=navigator_open_&&projects_.active_id()==original_project;
    sessions_.emplace(stale->session,retained);
    if(!rejected||active_session()!=original_actor||sessions_.size()!=actors)return fail("Navigator accepted a removed session identity.");
    close_navigator();smoke_navigator_done_=true;
    std::fputs("Native navigator smoke: metadata-only filtering, preview without switching, narrow two-stage lists, empty result, Escape focus, zero frontend input, stable cross-workspace actor commit and stale identity rejection verified.\n",stderr);return true;
}
bool NativeWindow::smoke_previous_terminal_evidence() {
    if(smoke_previous_terminal_done_)return true;
    // update_sessions deliberately exercises output invariance below, and ends
    // by calling smoke_progress. Block nested fixture execution, not that update.
    if(smoke_previous_terminal_running_)return false;
    smoke_previous_terminal_running_=true;
    struct Reset {bool& running;~Reset(){running=false;}} reset{smoke_previous_terminal_running_};
    const auto original_project=projects_.active_id();
    const auto saved_history=previous_terminal_;
    const auto original_pane=workspace().active_pane();
    const auto a=workspace().pane(original_pane)->session;
    const auto other_project=original_project==smoke_secondary_project_?smoke_primary_project_:smoke_secondary_project_;
    auto* other=projects_.workspace(other_project);
    const auto other_pane=other->active_pane();const auto b=other->pane(other_pane)->session;
    const auto actor_a=sessions_.at(a),actor_b=sessions_.at(b);
    const auto identity_a=watch_identity(a),identity_b=watch_identity(b);
    const auto actors=sessions_.size(),keys=smoke_terminal_input_submissions_,mouse=smoke_terminal_mouse_submissions_;
    const auto before_a=actor_a->info(),before_b=actor_b->info();
    const auto fail=[&](std::string_view error){smoke_finish(error);return false;};
    const auto capture=[&](std::wstring_view name){smoke_chrome_time_=GetTickCount64()+1000;smoke_motion_capture(name,0);smoke_chrome_time_.reset();};
    const auto key=[&](WPARAM vk){MSG message{};message.hwnd=hwnd_;message.message=WM_KEYDOWN;message.wParam=vk;key_message(message);message.message=WM_KEYUP;key_message(message);};
    previous_terminal_=PreviousTerminal{};commit_terminal_selection();
    activate_project(other_project);
    if(previous_terminal_.previous()!=identity_a||previous_terminal_.current()!=identity_b)return fail("Return history missed committed cross-workspace selection.");
    // Overlays, preview/Escape, output refresh, and activation do not navigate.
    open_navigator();navigator_focus_=1;key(VK_DOWN);key(VK_ESCAPE);
    toggle_palette();key(VK_ESCAPE);open_find();close_find();
    const bool was_active=active_;active_=false;changed();active_=was_active;changed();update_sessions();
    if(previous_terminal_.previous()!=identity_a||previous_terminal_.current()!=identity_b)return fail("Transient focus or preview polluted return history.");
    toggle_palette();query_="return";palette_index_=0;changed();
    const auto entries=palette_commands();
    const auto entry=std::find_if(entries.begin(),entries.end(),[](const auto& item){return item.id=="terminal.previous";});
    if(entry==entries.end()||!palette_availability(*entry).enabled||entry->description!=previous_terminal_target().description)return fail("Return Commands destination is absent or stale.");
    palette_index_=static_cast<std::size_t>(entry-entries.begin());capture(L"previous-terminal-normal");
    RECT bounds{};GetWindowRect(hwnd_,&bounds);SetWindowPos(hwnd_,nullptr,0,0,static_cast<int>(480*scale_),static_cast<int>(480*scale_),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    capture(L"previous-terminal-narrow");
    SetWindowPos(hwnd_,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    key(VK_RETURN);
    if(palette_open_||active_session()!=actor_a.get()||focused_session_!=a||previous_terminal_.previous()!=identity_b)return fail("Return did not immediately focus original actor without overlay input.");
    dispatch("terminal.previous");
    if(active_session()!=actor_b.get()||focused_session_!=b||previous_terminal_.previous()!=identity_a)return fail("Repeated Return did not toggle to exact second actor.");
    // Narrow model-only fixtures preserve all actors and restore placement below.
    // They never invoke the destructive host close/park/retirement paths.
    auto* original=projects_.workspace(original_project);
    const auto original_model=*original,other_model=*other;
    const auto parked=projects_.park({original_project,original_pane});
    if(!parked||previous_terminal_target().state!=PreviousTerminalState::parked)return fail("Return failed to reject parked actor.");
    return_to_previous_terminal();if(active_session()!=actor_b.get())return fail("Return restored a parked actor.");
    if(!projects_.reattach_to_new_tab({original_project,a}))return fail("Return fixture could not restore its original model.");
    *original=original_model;
    original->close_pane(original_pane);
    if(previous_terminal_target().state!=PreviousTerminalState::missing)return fail("Return failed to reject closed placement.");
    return_to_previous_terminal();if(active_session()!=actor_b.get())return fail("Return fell back from a closed target.");
    *original=original_model;
    sessions_.erase(a);const auto dormant=previous_terminal_target().state;sessions_.emplace(a,actor_a);
    if(dormant!=PreviousTerminalState::dormant)return fail("Return accepted a dormant target without retained actor.");
    const auto token=watch_actors_.at(a);watch_actors_.at(a)=token+1;
    const auto stale=previous_terminal_target().state;return_to_previous_terminal();watch_actors_.at(a)=token;
    if(stale!=PreviousTerminalState::stale||active_session()!=actor_b.get())return fail("Return accepted a recycled actor incarnation.");
    notice_.clear();
    // Move B into A's split with no spawn, then into a new tab. Identity, not
    // local pane/tab position or renamed metadata, must define the destination.
    other->close_pane(other_pane);
    const auto split=original->split(original_pane,Direction::right,b,"default");
    if(!split)return fail("Return split fixture could not attach retained actor.");
    projects_.activate(original_project);original->focus(*split.value);workspace_model_changed();reset_project_view();
    dispatch("terminal.previous");if(active_session()!=actor_a.get())return fail("Return failed across split panes.");
    dispatch("terminal.previous");if(active_session()!=actor_b.get())return fail("Return failed repeated split toggle.");
    const auto tab=original->move_pane_to_new_tab(*split.value,"Return destination renamed");
    if(!tab)return fail("Return tab fixture could not move retained actor.");
    workspace_model_changed();changed(true);
    if(previous_terminal_.previous()!=identity_a)return fail("Pane move or rename rewrote history.");
    dispatch("terminal.previous");dispatch("terminal.previous");
    if(active_session()!=actor_b.get()||workspace().active_tab()!=*tab.value)return fail("Return failed across moved tab identity.");
    *original=original_model;*other=other_model;
    projects_.activate(original_project);previous_terminal_=saved_history;reset_project_view();
    if(sessions_.size()!=actors||sessions_.at(a)!=actor_a||sessions_.at(b)!=actor_b||watch_identity(a)!=identity_a||watch_identity(b)!=identity_b||
       smoke_terminal_input_submissions_!=keys||smoke_terminal_mouse_submissions_!=mouse||focused_session_!=a)
        return fail("Return changed actor ownership, leaked frontend input, or failed immediate focus.");
    const auto after_a=actor_a->info(),after_b=actor_b->info();
    for(const auto& [before,after]:{std::pair{before_a,after_a},std::pair{before_b,after_b}}) {
        const auto frontend=after.sent.frontend()-before.sent.frontend();
        const auto focus=after.sent.focus_bytes-before.sent.focus_bytes;
        const auto protocol=after.sent.protocol_bytes-before.sent.protocol_bytes;
        if(frontend||before.sent.total()!=before.bytes_sent||after.sent.total()!=after.bytes_sent||after.bytes_sent-before.bytes_sent!=frontend+focus+protocol)
            return fail("Return transport contains frontend or unattributed bytes.");
        std::fprintf(stderr,"Native previous-terminal write origins: frontend=%llu focus=%llu protocol=%llu\n",
            static_cast<unsigned long long>(frontend),static_cast<unsigned long long>(focus),static_cast<unsigned long long>(protocol));
    }
    std::fputs("Native previous-terminal smoke: committed cross-workspace/split/tab toggles, rename/move identity, preview/Escape focus invariants, parked/closed/dormant/stale rejection; zero frontend key/mouse input; automatic focus/protocol writes separately attributed.\n",stderr);
    smoke_previous_terminal_done_=true;return true;
}
bool NativeWindow::smoke_find_progress() {
    if(smoke_find_stage_==7)return true;
    auto* actor=active_session();if(!actor){smoke_finish("Find smoke lost its active terminal.");return false;}
    const auto capture=[&](std::wstring_view name){smoke_chrome_time_=GetTickCount64()+1000;smoke_motion_capture(name,0);smoke_chrome_time_.reset();};
    const auto key=[&](WPARAM vk){MSG message{};message.hwnd=hwnd_;message.message=WM_KEYDOWN;message.wParam=vk;key_message(message);message.message=WM_KEYUP;key_message(message);};
    if(smoke_find_stage_==0) {
        const std::string command(native_find_fixture_command(smoke_managed_sessions_.contains(workspace().pane(workspace().active_pane())->session)));
        if(!TerminalCore::paste_is_safe(command)){smoke_finish("Find fixture violates ordinary paste safety.");return false;}
        auto owned=sessions_.at(workspace().pane(workspace().active_pane())->session);
        if(owned->paste(command)!=SessionSendResult::accepted){smoke_finish("Find fixture paste was not accepted.");return false;}
        KeyInput enter;enter.key=Key::enter;
        if(owned->key(enter)!=SessionSendResult::accepted){smoke_finish("Find fixture Enter was not accepted.");return false;}
        smoke_find_stage_=1;return false;
    }
    if(smoke_find_stage_==1) {
        const auto frame=actor->frame();if(!frame)return false;
        bool marker=false;for(const auto& row:frame->lines){std::string text;for(const auto& cell:row->cells)text+=cell.grapheme;marker=marker||native_find_fixture_output(text);}
        if(!marker)return false;
        dispatch("terminal.find");if(!find_session_||focused_session_){smoke_finish("Find did not take exclusive query focus.");return false;}
        smoke_find_input_baseline_=smoke_terminal_input_submissions_;smoke_find_bytes_=actor->info().bytes_sent;
        find_query_.select_all();terminal_text("STORMTTY_FIND_NEEDLE");smoke_find_stage_=2;return false;
    }
    refresh_find();
    const auto result=find_result_;
    if(!result||!current_find_result(*result,find_request_,find_query_.text())||result->status!=SearchStatus::complete)return false;
    if(smoke_find_stage_==2) {
        if(result->matches.size()<2||!result->selected_index)return false;
        const auto frame=actor->frame();if(!frame)return false;
        bool selected=false;for(const auto& row:frame->lines)for(const auto& cell:row->cells)selected=selected||cell.selected;
        if(!selected)return false; // The search mailbox can precede its rendered selection frame.
        capture(L"find-results");smoke_find_selection_=*result->selected_index;key(VK_RETURN);smoke_find_stage_=3;return false;
    }
    if(smoke_find_stage_==3) {
        if(!result->selected_index||*result->selected_index==smoke_find_selection_)return false;
        capture(L"find-next");smoke_find_revision_=result->token.revision;GetWindowRect(hwnd_,&smoke_find_window_);
        SetWindowPos(hwnd_,nullptr,0,0,static_cast<int>(480*scale_),static_cast<int>(480*scale_),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        smoke_find_stage_=4;return false;
    }
    if(smoke_find_stage_==4) {
        const auto frame=actor->frame();if(!frame||result->token.revision==smoke_find_revision_)return false;
        capture(L"find-narrow");
        SetWindowPos(hwnd_,nullptr,0,0,smoke_find_window_.right-smoke_find_window_.left,smoke_find_window_.bottom-smoke_find_window_.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        find_query_.select_all();terminal_text("STORMTTY_FIND_ABSENT_713");smoke_find_stage_=5;return false;
    }
    if(smoke_find_stage_==5) {
        if(!result->matches.empty()){smoke_finish("Find no-match query retained old result positions.");return false;}
        const auto frame=actor->frame();if(!frame)return false;
        for(const auto& row:frame->lines)for(const auto& cell:row->cells)if(cell.selected)return false;
        capture(L"find-empty");key(VK_ESCAPE);
        std::fprintf(stderr,"Native Find isolation: open=%u focused=%llu frontend_input_delta=%llu transport_bytes_delta=%llu (transport includes automatic protocol replies during resize).\n",unsigned(bool(find_session_)),static_cast<unsigned long long>(focused_session_.value),static_cast<unsigned long long>(smoke_terminal_input_submissions_-smoke_find_input_baseline_),static_cast<unsigned long long>(actor->info().bytes_sent-smoke_find_bytes_));
        if(find_session_||!focused_session_||smoke_terminal_input_submissions_!=smoke_find_input_baseline_){smoke_finish("Find dismissal lost focus or leaked query/navigation to PTY.");return false;}
        dispatch("terminal.find");find_query_.select_all();terminal_text("STORMTTY_FIND_NEEDLE");
        find_query_.select_all();terminal_text("transient-absent");find_query_.select_all();terminal_text("STORMTTY_FIND_NEEDLE");smoke_find_stage_=6;return false;
    }
    if(smoke_find_stage_==6) {
        if(result->matches.empty())return false;
        const auto target=find_session_;activate_project(smoke_secondary_project_);
        if(find_session_||active_session()==actor){smoke_finish("Find crossed workspace/session ownership.");return false;}
        activate_project(smoke_primary_project_);
        if(active_session()!=actor||!sessions_.contains(target)){smoke_finish("Find changed the terminal actor.");return false;}
        smoke_find_stage_=7;std::fputs("Native Find smoke: real retained output, asynchronous match selection/navigation, narrow strip, no-match replacement, rapid query coalescing, Escape focus, zero frontend query/key/paste submissions and workspace isolation verified.\n",stderr);return true;
    }
    return false;
}
bool NativeWindow::smoke_palette_evidence(SessionId parked) {
    const auto actor=sessions_.at(parked);
    const auto actor_count=sessions_.size();
    const auto parked_context=actor->info().shell_context;
    const auto fail=[&](std::string_view message){smoke_finish(message); return false;};
    smoke_chrome_time_=GetTickCount64()+1000;
    toggle_palette(); palette_transition_.finish();
    smoke_motion_capture(L"palette-commands",0);
    MSG navigation{}; navigation.hwnd=hwnd_; navigation.message=WM_KEYDOWN; navigation.wParam=VK_END;
    const auto transmitted=actor->info().bytes_sent;
    if (!key_message(navigation)) return fail("Palette End was not consumed.");
    auto all=palette_commands(); auto last_layout=palette_layout(all);
    if (all.empty() || palette_index_!=all.size()-1 || last_layout.first+last_layout.rows!=all.size())
        return fail("Palette End did not reveal the final result.");
    smoke_motion_capture(L"palette-end",0);
    navigation.wParam=VK_HOME; key_message(navigation);
    if (palette_index_!=0) return fail("Palette Home did not restore the first result.");
    RECT original{}; GetWindowRect(hwnd_,&original);
    SetWindowPos(hwnd_,nullptr,0,0,static_cast<int>(480*scale_),static_cast<int>(480*scale_),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    smoke_motion_capture(L"palette-narrow",0);
    SetWindowPos(hwnd_,nullptr,0,0,original.right-original.left,original.bottom-original.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    navigation.wParam=VK_ESCAPE; key_message(navigation);
    if (palette_open_ || actor->info().bytes_sent!=transmitted) return fail("Palette navigation leaked terminal input.");
    toggle_palette(); palette_transition_.finish();
    if (!query_.empty() || palette_index_!=0) return fail("Repeated palette opening retained stale navigation.");
    query_="splr";
    auto entries=palette_commands();
    if (entries.empty() || entries.front().id!="pane.split_right") return fail("Palette fuzzy ranking did not resolve split-right abbreviation.");
    smoke_motion_capture(L"palette-fuzzy",0);
    query_="zzzzzzzzzzzzzzzz";
    if (!palette_commands().empty()) return fail("Palette no-result query unexpectedly matched.");
    smoke_motion_capture(L"palette-no-results",0);
    toggle_palette(); dispatch("background.restore"); palette_transition_.finish();
    entries=palette_commands();
    if (entries.size()!=1 || !entries.front().session || entries.front().session->local!=parked || focused_session_)
        return fail("Background picker lost the qualified parked identity or leaked terminal focus.");
    const auto stale=entries.front();
    smoke_motion_capture(L"palette-background",0);
    MSG key{}; key.hwnd=hwnd_; key.message=WM_KEYDOWN; key.wParam=VK_RETURN;
    if (!key_message(key)) return fail("Background picker did not consume Enter.");
    key.message=WM_KEYUP; key_message(key);
    if (palette_open_ || active_session()!=actor.get() || sessions_.size()!=actor_count ||
        !projects_.active()->parked_sessions.empty() || palette_availability(stale).enabled)
        return fail("Keyboard background restore restarted/replaced an actor or left a stale entry enabled.");
    // Repeated stale activation is rejected before touching placement or ownership.
    activate_palette_entry(stale); notice_.clear();
    if (active_session()!=actor.get() || sessions_.size()!=actor_count) return fail("Stale palette activation changed session ownership.");
    if (parked_context && actor->info().shell_context!=parked_context) return fail("Restoring a background actor changed its owned shell context.");
    const auto footer=footer_layout();
    if (footer.directory.width<=0 || footer_presentation().directory.empty()) return fail("Restored terminal has no active footer context.");
    smoke_motion_capture(L"footer-restored",0);
    park_pane(workspace().active_pane());
    if (parked_context && actor->info().shell_context!=parked_context) return fail("Parking an actor transplanted its shell context.");
    if (projects_.active()->parked_sessions.size()!=1 || projects_.active()->parked_sessions.front().session!=parked)
        return fail("Palette smoke could not restore the original background fixture.");
    smoke_chrome_time_.reset();
    std::fputs("Native palette smoke: fuzzy rank, empty results, keyboard background restore, stale identity rejection and same session actor verified; real framebuffer captures emitted.\n",stderr);
    return true;
}
void NativeWindow::smoke_pane_focus_scene(const WindowScene& actual) const {
    const auto ui=resolve_ui_colors(config_.theme);
    const auto* active=workspace().pane(workspace().active_pane());
    if(!active || focused_session_!=active->session || actual.panes.size()!=layout_.panes.size() ||
       std::count_if(actual.panes.begin(),actual.panes.end(),[](const auto& pane){return pane.focused;})!=1)
        throw std::runtime_error("Pane focus paint delayed the actual input owner.");
    for(const auto& placement:layout_.panes) {
        const bool selected=placement.pane==workspace().active_pane();
        const auto paint=pane_focus_colors(ui,selected,pane_focus_.at(placement.pane).value(chrome_now()),
            high_contrast_,system_color(COLOR_BTNTEXT),system_color(COLOR_WINDOWTEXT));
        const auto marker=pixels({placement.bounds.x+12,placement.bounds.y+10,2,8});
        const auto markers=std::count_if(actual.primitives.begin(),actual.primitives.end(),[&](const auto& p){return p.rect==marker;});
        if(markers!=(selected?1:0) || (selected && std::none_of(actual.primitives.begin(),actual.primitives.end(),[&](const auto& p){return p.rect==marker&&p.fill==paint.marker;})))
            throw std::runtime_error("Pane marker did not follow selected ownership immediately and opaquely.");
        const auto label=pixels({placement.bounds.x+20,placement.bounds.y+6,0,18});
        const auto primary=pane_context(placement.pane).primary;
        if(std::none_of(actual.texts.begin(),actual.texts.end(),[&](const auto& t){return t.rect.x==label.x&&t.rect.y==label.y&&t.rect.height==label.height&&t.text==primary&&t.color==paint.label&&t.size_dip==13&&!t.semibold;}))
            throw std::runtime_error("Pane primary label changed weight/geometry or interpolated its active ink.");
        const auto header=pixels({placement.bounds.x,placement.bounds.y,placement.bounds.width,28});
        if(!high_contrast_ && std::none_of(actual.primitives.begin(),actual.primitives.end(),[&](const auto& p){return p.rect==header&&p.fill==paint.wash;}))
            throw std::runtime_error("Pane decoration lost the semantic low-alpha wash.");
        if(std::none_of(actual.panes.begin(),actual.panes.end(),[&](const auto& p){return p.id==placement.session.value&&p.content==pixels(content_bounds(placement))&&p.focused==selected;}))
            throw std::runtime_error("Pane focus changed terminal bounds or renderer focus ownership.");
    }
}
// Geometry changes here deliberately resize existing actors; unlike the pane
// decoration fixture, this tests layout/input ownership, not invariant grids.
void NativeWindow::smoke_workspace_frame_evidence() {
    if(!options_.smoke_test || !options_.smoke_layout || layout_.panes.size()!=3 || focus_mode_ || fullscreen_ || smoke_workspace_frame_running_)
        throw std::runtime_error("Workspace frame evidence requires the owned three-pane fixture.");
    const auto actors=sessions_;const auto original_config=config_;
    const auto project=projects_.active_id();const auto tab=workspace().active_tab();const auto pane=workspace().active_pane();
    const auto original_sizes=sizes_;const auto expanded=navigation_expanded_;const auto contrast=high_contrast_;
    const auto first=navigation_first_;const auto zoom=workspace().tab(tab)->zoomed_pane;
    const auto input=smoke_terminal_input_submissions_,mouse=smoke_terminal_mouse_submissions_;
    const auto clock=smoke_chrome_time_;
    RECT bounds{};
    if(!GetWindowRect(hwnd_,&bounds))throw std::runtime_error("Workspace frame fixture could not read its original window bounds.");
    const auto rollback=[&] {
        // Independent best-effort steps: a failed native/renderer operation must
        // not strand another fixture policy or replace the original exception.
        smoke_chrome_time_=clock;high_contrast_=contrast;focus_mode_=false;
        navigation_expanded_=expanded;navigation_first_=first;config_.ribbon=original_config.ribbon;
        try {cancel_drag();} catch(...) {}
        palette_open_=rename_open_=palette_background_only_=false;inspected_context_.reset();query_.clear();
        palette_transition_=ChromeTransition{};closing_palette_primitives_.clear();closing_palette_texts_.clear();
        text_input_.reset();composing_=false;preedit_.clear();
        try {
            if(auto* model=projects_.workspace(project);model && model->tab(tab) && model->pane(pane)) {
                projects_.activate(project);model->activate_tab(tab);model->focus(pane);
                if(model->tab(tab)->zoomed_pane!=zoom)model->toggle_zoom();
            }
        } catch(...) {}
        if(!SetWindowPos(hwnd_,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))
            std::fputs("Workspace frame rollback could not restore native window bounds; original fixture failure retained.\n",stderr);
        try {refresh_clock();changed(true);} catch(...) {}
        try {refresh_tooltips();} catch(...) {}
    };
    struct Restore {
        bool& running;const std::function<void()> rollback;bool restored{};
        ~Restore(){if(!restored)try{rollback();}catch(...){}running=false;}
    } restore{smoke_workspace_frame_running_,rollback};
    smoke_workspace_frame_running_=true;
    const auto resize=[&](int width,int height) {
        if(!SetWindowPos(hwnd_,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))
            throw std::runtime_error("Workspace frame fixture could not set native window bounds.");
        refresh_layout();
    };
    const auto key=[&](WPARAM vk){MSG m{};m.hwnd=hwnd_;m.message=WM_KEYDOWN;m.wParam=vk;key_message(m);m.message=WM_KEYUP;key_message(m);};
    const auto invariant=[&] {
        if(sessions_!=actors || projects_.active_id()!=project || workspace().active_tab()!=tab || workspace().active_pane()!=pane ||
           smoke_terminal_input_submissions_!=input || smoke_terminal_mouse_submissions_!=mouse ||
           navigation_.context.height!=0 || navigation_.viewport.y!=chrome_.tabs_band.y+chrome_.tabs_band.height ||
           navigation_.footer.x!=navigation_.viewport.x || chrome_.new_tab.x+chrome_.new_tab.width>navigation_.focus.x ||
           navigation_.focus.width<=0 || navigation_.focus.height<=0)
            throw std::runtime_error("Workspace frame changed actor/input ownership or overlapped its action lane.");
        if(!inspecting_context() && focused_session_!=workspace().pane(pane)->session)
            throw std::runtime_error("Workspace frame changed real keyboard focus ownership.");
    };
    const auto capture=[&](std::wstring_view name){invariant();smoke_chrome_time_=GetTickCount64()+1000;smoke_motion_capture(name,0);smoke_chrome_time_=clock;invariant();};
    const auto click_focus=[&]{const auto r=navigation_.focus;const Point p{r.x+r.width/2,r.y+r.height/2};pointer_down(p,false);pointer_up(p);};
    const auto tooltip_has_context=[&]{const auto detail=widen(pane_context(pane).detail);return std::any_of(tooltip_texts_.begin(),tooltip_texts_.end(),[&](const auto& item){return item.second.find(detail)!=std::wstring::npos;});};
    capture(L"workspace-frame-normal");
    // Both transitions use the real pointer route and the action's new bounds.
    click_focus();
    if(!focus_mode_ || navigation_.rail.width!=0 || navigation_.footer.height!=0)throw std::runtime_error("Focus action did not hide navigation/footer.");
    capture(L"workspace-frame-focus");click_focus();
    if(focus_mode_ || navigation_expanded_!=expanded)throw std::runtime_error("Focus action could not restore the prior workspace frame.");
    dispatch("workspace.toggle_navigation");
    if(navigation_.expanded || !tooltip_has_context())throw std::runtime_error("Collapsed workspace lost complete task context tooltip.");
    capture(L"workspace-frame-collapsed");
    dispatch("pane.zoom");
    if(layout_.panes.size()!=1 || !tooltip_has_context())throw std::runtime_error("Single pane lost full context without its split header.");
    // Even hidden directory/footer bindings cannot remove the task tooltip or
    // keyboard Inspector. Restore the exact declarative preferences afterward.
    config_.ribbon.hidden.fill(true);refresh_tooltips();
    if(!tooltip_has_context())throw std::runtime_error("Hidden ribbon bindings removed single-pane context access.");
    toggle_palette();query_="Inspect pane context";const auto commands=palette_commands();
    if(commands.empty() || commands.front().id!="pane.inspect")throw std::runtime_error("Single-pane context is not discoverable in Commands.");
    key(VK_RETURN);
    if(!inspecting_context() || inspected_context_->detail!=pane_context(pane).detail || focused_session_)
        throw std::runtime_error("Single-pane Inspector lost complete context or exclusive input ownership.");
    capture(L"workspace-frame-single-inspect");key(VK_ESCAPE);
    config_.ribbon=original_config.ribbon;refresh_tooltips();
    capture(L"workspace-frame-single");dispatch("pane.zoom");dispatch("workspace.toggle_navigation");
    resize(static_cast<int>(480*scale_),static_cast<int>(360*scale_));capture(L"workspace-frame-narrow");
    high_contrast_=true;capture(L"workspace-frame-high-contrast-fixture");high_contrast_=contrast;
    click_focus();click_focus();invariant();
    resize(static_cast<int>(330*scale_),static_cast<int>(180*scale_));capture(L"workspace-frame-minimum");click_focus();click_focus();invariant();
    navigation_first_=first;resize(bounds.right-bounds.left,bounds.bottom-bounds.top);invariant();
    RECT restored_bounds{};
    if(!GetWindowRect(hwnd_,&restored_bounds) || restored_bounds.right-restored_bounds.left!=bounds.right-bounds.left ||
       restored_bounds.bottom-restored_bounds.top!=bounds.bottom-bounds.top)
        throw std::runtime_error("Workspace frame fixture could not verify restored native window bounds.");
    if(config_!=original_config || navigation_expanded_!=expanded || high_contrast_!=contrast || focus_mode_ ||
       navigation_first_!=first || workspace().tab(tab)->zoomed_pane!=zoom || smoke_chrome_time_!=clock ||
       inspecting_context() || palette_open_ || layout_.panes.size()!=3 || sizes_!=original_sizes || GetCapture()==hwnd_)
        throw std::runtime_error("Workspace frame fixture failed to restore its original configuration, geometry or pointer owner.");
    restore.restored=true;
    std::fputs("Native workspace frame: context band removed, reserved client Focus action enters/exits, workspace-local footer, collapsed/single-pane full context tooltip and keyboard Inspector with hidden ribbon bindings, narrow/high-contrast captures; same actors and zero frontend input. Not physical Snap, IME, blur or accessibility verification.\n",stderr);
}
void NativeWindow::smoke_focus_evidence(bool project) {
    if(!options_.smoke_test || (!project&&!options_.smoke_layout) || smoke_focus_running_ ||
       (!project && (layout_.panes.size()<2 || !active_ || !focused_session_ || minimized_ || !IsWindowVisible(hwnd_) || GetCapture()==hwnd_)))
        throw std::runtime_error("Focus evidence requires its owned, focused native smoke fixture.");
    if(!project && high_contrast_)
        throw std::runtime_error("Synthetic normal-motion pane qualification unavailable under actual OS high contrast; OS policy retained.");
    const auto old_clock=smoke_chrome_time_;const auto old_policy=smoke_pane_reduced_motion_;
    const auto original_project=projects_.active_id();
    const auto original_pane=workspace().active_pane();
    const auto original_tab=workspace().active_tab();
    struct Restore {
        NativeWindow& window;PaneId original_pane;bool pane_fixture,restored{};
        std::optional<std::uint64_t>& clock;std::optional<std::uint64_t> old_clock;
        std::optional<bool>& policy;std::optional<bool> old_policy;bool& running;
        void restore(){clock=old_clock;policy=old_policy;running=false;restored=true;}
        ~Restore(){
            if(restored)return;
            // Failure cleanup must not mask the capture error or escape a callback.
            try {if(pane_fixture)window.cancel_drag();}catch(...){}
            try {if(pane_fixture)window.activate(original_pane);}catch(...){}
            if(pane_fixture)for(auto& [id,t]:window.pane_focus_){(void)id;t.finish();}
            restore();try{window.motion_schedule();}catch(...){}
        }
    } restore{*this,original_pane,!project,false,smoke_chrome_time_,old_clock,smoke_pane_reduced_motion_,old_policy,smoke_focus_running_};
    smoke_focus_running_=true;
    smoke_pane_reduced_motion_=project?std::nullopt:std::optional<bool>{false}; // Pane wash only, never global/caret/OS policy.
    const auto actors=sessions_;const auto sizes=sizes_;const auto original_layout=layout_;
    const auto input=smoke_terminal_input_submissions_,mouse=smoke_terminal_mouse_submissions_;
    const auto original_config=config_;const bool original_reduced=reduced_motion_,original_contrast=high_contrast_;
    smoke_chrome_time_=GetTickCount64()+1000; motion_schedule();
    const auto start=*smoke_chrome_time_;
    const auto target_pane=project?PaneId{}:layout_.panes.back().pane;
    if(!project && target_pane==original_pane)throw std::runtime_error("Pane focus fixture requires distinct existing panes.");
    const auto select_pane=[&](PaneId pane) {
        activate(pane);
        const auto* selected=workspace().pane(pane);
        if(!selected || workspace().active_pane()!=pane || focused_session_!=selected->session)
            throw std::runtime_error("Pane switch did not commit the requested keyboard owner immediately.");
    };
    const auto pane_motion_ended=[&] {
        const auto now=chrome_now();
        const auto active=[&](const auto& transitions){return std::any_of(transitions.begin(),transitions.end(),[&](const auto& item){return item.second.active(now);});};
        const bool other=palette_transition_.active(now)||settings_transition_.active(now)||menu_transition_.active(now)||navigator_transition_.active(now)||
            active(workspace_focus_)||active(hover_transitions_)||active(tab_transitions_);
        // The timer is shared: unrelated legitimate chrome motion may retain it.
        return !active(pane_focus_) && (other || !motion_timer_active_);
    };
    const auto invariant=[&] {
        if(project)return;
        if(sessions_!=actors || sizes_!=sizes || projects_.active_id()!=original_project || workspace().active_tab()!=original_tab ||
           config_!=original_config || reduced_motion_!=original_reduced || high_contrast_!=original_contrast ||
           smoke_terminal_input_submissions_!=input || smoke_terminal_mouse_submissions_!=mouse ||
           layout_.panes.size()!=original_layout.panes.size() || layout_.dividers.size()!=original_layout.dividers.size())
            throw std::runtime_error("Pane decoration changed actors, grids, policy or frontend input.");
        for(std::size_t i=0;i<layout_.panes.size();++i) {
            const auto& a=layout_.panes[i];const auto& b=original_layout.panes[i];
            if(a.pane!=b.pane || a.session!=b.session || a.bounds!=b.bounds)
                throw std::runtime_error("Pane decoration changed pane identity or bounds.");
        }
        for(std::size_t i=0;i<layout_.dividers.size();++i) {
            const auto& a=layout_.dividers[i];const auto& b=original_layout.dividers[i];
            if(a.node!=b.node || a.axis!=b.axis || a.bounds!=b.bounds || a.hit_bounds!=b.hit_bounds || a.split_bounds!=b.split_bounds || a.applied_ratio!=b.applied_ratio)
                throw std::runtime_error("Pane decoration changed split geometry.");
        }
        const auto* selected=workspace().pane(workspace().active_pane());
        if(!selected || focused_session_!=selected->session)
            throw std::runtime_error("Pane switch delayed the real keyboard input target.");
    };
    const auto capture=[&](std::wstring_view name,std::uint64_t elapsed){invariant();smoke_motion_capture(name,elapsed);invariant();};
    if (project) activate_project(smoke_secondary_project_); else select_pane(target_pane);
    if (project) {
        if (focused_session_!=workspace().pane(workspace().active_pane())->session)
            throw std::runtime_error("Workspace switch delayed the real keyboard input target.");
        for (const auto& tab:chrome_.tabs) if (tab.tab==workspace().active_tab() && tab_transitions_.at(tab.tab).value(start)!=1)
            throw std::runtime_error("Workspace switch faded its new active tab from zero.");
    }
    capture(project?L"workspace-focus-000ms":L"pane-focus-000ms",0);
    *smoke_chrome_time_=start+40;
    auto& incoming=project?workspace_focus_[smoke_secondary_project_]:pane_focus_[target_pane];
    const auto midway=incoming.value(chrome_now());
    if ((!project || !reduced_motion_) && !(midway>0 && midway<1)) throw std::runtime_error("Focus handoff skipped its intermediate value.");
    capture(project?L"workspace-focus-040ms":L"pane-focus-040ms",40);
    if (project) activate_project(original_project); else select_pane(original_pane);
    if ((!project || !reduced_motion_) && std::abs(incoming.value(chrome_now())-midway)>1e-9) throw std::runtime_error("Reversed focus handoff jumped.");
    capture(project?L"workspace-focus-reversed-040ms":L"pane-focus-reversed-040ms",40);
    *smoke_chrome_time_=start+160; motion_schedule();
    if (incoming.value(chrome_now())!=0 || (project?motion_timer_active_:!pane_motion_ended())) throw std::runtime_error("Focus handoff did not settle and release its timer work.");
    capture(project?L"workspace-focus-settled-160ms":L"pane-focus-settled-160ms",160);
    if (!project) {
        *smoke_chrome_time_=start+200;select_pane(target_pane);
        *smoke_chrome_time_=start+320;motion_schedule();
        if(incoming.value(chrome_now())!=1 || !pane_motion_ended())throw std::runtime_error("Pane wash did not settle at 120 ms.");
        capture(L"pane-focus-settled-120ms",120);
        *smoke_chrome_time_=start+400;select_pane(original_pane);
        *smoke_chrome_time_=start+440;
        if(!(incoming.value(chrome_now())>0 && incoming.value(chrome_now())<1))throw std::runtime_error("Pane reduced-motion probe did not begin mid-transition.");
        smoke_pane_reduced_motion_=true;motion_schedule();
        if(incoming.value(chrome_now())!=0 || pane_focus_[original_pane].value(chrome_now())!=1 || !pane_motion_ended())
            throw std::runtime_error("Pane reduced-motion policy did not cancel only its wash immediately.");
        capture(L"pane-focus-reduced-040ms",40);
        std::fprintf(stderr,"Native pane focus: identity_immediate=1 synthetic_normal_motion_interpolation=1 samples_ms=0,40,120 rapid_reversal=1 reduced_motion_cancellation=1 actors_bounds_grids_unchanged=1 frontend_input_unchanged=1 focus_protocol_writes_allowed=1 pane_motion_ended=1 shared_timer_accounted=1 host_reduced_motion=%d global_policy_unchanged=1 physical_frame_pacing_verified=0\n",reduced_motion_);
        const auto divider=layout_.dividers.front();
        const auto before=divider.applied_ratio;
        const Point edge=divider.axis==Axis::horizontal?Point{divider.hit_bounds.x+.25,divider.bounds.y+divider.bounds.height/2}:
            Point{divider.bounds.x+divider.bounds.width/2,divider.hit_bounds.y+.25};
        pointer_down(edge,false); pointer_move(edge);
        const auto found=std::find_if(layout_.dividers.begin(),layout_.dividers.end(),[&](const auto& d){return d.node==divider.node;});
        if (found==layout_.dividers.end() || std::abs(found->applied_ratio-before)>1e-8) throw std::runtime_error("Divider edge grab jumped before pointer travel.");
        pointer_up(edge);
        std::fputs("Native motion divider edge-grab: zero pointer travel preserves applied split ratio; capture released.\n",stderr);
    }
    restore.restore();changed();
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
// Dedicated qualification uses the actual startup/load and shutdown/flush paths.
// No normal user path is reachable: wWinMain requires an isolated fixture marker.
void NativeWindow::smoke_workspace_progress(bool presented) {
    if (!workspace_smoke_ready_ || !presented || smoke_finished_) return;
    const auto finish=[this](std::string error) {
    if(options_.smoke_projects && !error.empty()) {
        const auto found=find_result_;std::fprintf(stderr,"Native Find failure: stage=%u open=%u focused=%llu request=%llu status=%d matches=%zu selected=%lld apply=%d frontend_delta=%llu transport_delta=%llu\n",smoke_find_stage_,unsigned(bool(find_session_)),static_cast<unsigned long long>(focused_session_.value),static_cast<unsigned long long>(find_request_),found?int(found->status):-1,found?found->matches.size():0,found&&found->selected_index?static_cast<long long>(*found->selected_index):-1,found&&found->last_apply_result?int(*found->last_apply_result):-1,static_cast<unsigned long long>(smoke_terminal_input_submissions_-smoke_find_input_baseline_),static_cast<unsigned long long>(active_session()?active_session()->info().bytes_sent-smoke_find_bytes_:0));
    }
    smoke_finished_=true; exit_code_=error.empty()?0:2;
        std::fprintf(stderr,"Workspace persistence smoke %s: %s%s%s\n",
            error.empty()?"passed":"failed",options_.smoke_workspaces.c_str(),error.empty()?"":": ",error.c_str());
        if(hwnd_) destroy_window();
    };
    if(workspace_smoke_menu_) {
        for(const auto& [id,actor]:sessions_) {
            (void)id; const auto info=actor->info();
            if(info.state==SessionState::failed) { finish(info.error); return; }
            if(info.state!=SessionState::running || !info.bytes_received || !actor->frame()) return;
        }
        // Capture only a subsequent settled presented frame, never the frame
        // whose callback opened the owned menu. No synchronous render recursion.
        if(GetTickCount64()-workspace_smoke_capture_at_<180) { changed(); return; }
        if(!context_menu_.is_open()) { finish("Workspace customization menu did not open."); return; }
        const auto original=options_.capture_path;
        options_.capture_path=original.parent_path()/(original.stem().wstring()+L"-menu"+original.extension().wstring());
        try { capture_frame(); } catch(const std::exception& error) { options_.capture_path=original; finish(error.what()); return; }
        options_.capture_path=original;
        finish({}); return;
    }
    const auto capture_then_menu=[this,&finish] {
        if(options_.capture_path.empty()) { finish({}); return; }
        try { capture_frame(); } catch(const std::exception& error) { finish(error.what()); return; }
        workspace_smoke_menu_=true; workspace_smoke_capture_at_=GetTickCount64();
        workspace_menu({navigation_.rail.x+16,chrome_.band.height+44});
        changed();
    };
    for(const auto& [id,actor]:sessions_) {
        (void)id; const auto info=actor->info();
        if(info.state==SessionState::failed) { finish(info.error); return; }
        if(info.state!=SessionState::running || !info.bytes_received || !actor->frame()) return;
    }
    if(options_.smoke_workspaces=="corrupt") {
        if(workspace_snapshot_ || restored_workspaces_ || notice_.find("left unchanged")==std::string::npos) {
            finish("Corrupt file did not disable restore and autosave."); return;
        }
        finish({}); return; // Orchestrator compares exact bytes AFTER normal shutdown flush.
    }
    if(!workspace_snapshot_) { finish("Fixture library could not be loaded."); return; }
    if(options_.smoke_workspaces=="seed") {
        if(!workspace_smoke_seeded_) {
            workspace_smoke_seeded_=true;
            auto metadata=projects_.active()->metadata;
            metadata.name="Fixture Alpha"; metadata.icon="code"; metadata.accent="#59A6FF";
            auto status=projects_.update_metadata(projects_.active_id(),metadata);
            if(!status) { finish(status.error); return; }
            workspace().rename_tab(workspace().active_tab(),"Saved split"); split(Direction::right);
            for(const auto& divider:layout_.dividers) { workspace().set_split_ratio(divider.node,0.37); break; }
            auto created=projects_.create({"Fixture Beta","","Local","flask","#D7AA53"});
            if(!created) { finish(created.error); return; }
            activate_project(*created.value); new_tab();
            workspace().rename_tab(workspace().active_tab(),"Saved active tab");
            auto captured=capture_workspace_snapshot(projects_);
            if(!captured) { finish(captured.error); return; }
            status=save_workspace_preset(workspace_library_,"Fixture preset",std::move(*captured.value));
            if(!status) { finish(status.error); return; }
            workspace_smoke_capture_at_=GetTickCount64();
            changed(true); return;
        }
        if(!options_.capture_path.empty() && GetTickCount64()-workspace_smoke_capture_at_<180) { changed(); return; }
        capture_then_menu(); return; // Actual run() shutdown calls flush_workspaces().
    }
    if(!restored_workspaces_ || projects_.entries().size()!=2 || workspace_library_.presets.size()!=1) {
        finish("Second app did not restore saved workspaces/preset."); return;
    }
    auto captured=capture_workspace_snapshot(projects_);
    if(!captured) { finish(captured.error); return; }
    auto expected=serialize_workspace_library(workspace_library_);
    auto actual_library=workspace_library_; actual_library.last_session=*captured.value;
    auto actual=serialize_workspace_library(actual_library);
    if(!expected || !actual || *expected.value!=*actual.value ||
        projects_.active()->metadata.name!="Fixture Beta" ||
        projects_.entries()[0].metadata.icon!="code" || projects_.entries()[1].metadata.accent!="#D7AA53") {
        finish("Boot restore changed metadata, durable identity, active selection or layout."); return;
    }
    // Hidden project panes must remain unbound: restoring does not eagerly replay sessions.
    if(projects_.session_ids(projects_.entries()[0].id).size()!=0 || sessions_.size()!=1) {
        finish("Hidden restored panes launched eagerly."); return;
    }
    if(!options_.capture_path.empty()) {
        if(!workspace_smoke_capture_at_) { workspace_smoke_capture_at_=GetTickCount64(); changed(); return; }
        if(GetTickCount64()-workspace_smoke_capture_at_<180) { changed(); return; }
        try { capture_frame(); } catch(const std::exception& error) { finish(error.what()); return; }
    }
    const auto actors=sessions_; const auto previous=projects_.active_id();
    const auto previous_ids=projects_.session_ids(previous);
    open_workspace_preset(0);
    if(projects_.entries().size()!=4 || projects_.session_ids(previous)!=previous_ids) {
        finish("Preset replaced the existing workspace or session ownership."); return;
    }
    for(const auto& [id,actor]:actors) {
        if(!sessions_.contains(id) || sessions_.at(id)!=actor || actor->info().state!=SessionState::running) {
            finish("Preset discarded or restarted a live actor."); return;
        }
    }
    for(std::size_t i=0;i<2;++i) if(projects_.entries()[i].metadata.persistent_id==projects_.entries()[i+2].metadata.persistent_id) {
        finish("Additive preset aliased a durable workspace identity."); return;
    }
    if(options_.capture_path.empty()) { finish({}); return; }
    workspace_smoke_menu_=true; workspace_smoke_capture_at_=GetTickCount64();
    workspace_menu({navigation_.rail.x+16,chrome_.band.height+44}); changed();
}

void NativeWindow::smoke_settings_stage(unsigned stage) {
    smoke_settings_stage_=stage;
    if (options_.smoke_settings) std::fprintf(stderr,"Native CI settings stage=%u elapsed_ms=%llu job_pending=%d panel_open=%d saving=%d\n",
        stage,GetTickCount64()-smoke_started_at_,settings_job_,settings_panel_.is_open(),settings_panel_.saving());
}
void NativeWindow::smoke_settings_scroll_evidence() {
    if (!options_.smoke_settings || !settings_panel_.is_open() || settings_panel_.saving() || smoke_settings_scroll_running_)
        throw std::runtime_error("Settings scroll evidence requires its owned editable smoke panel.");
    const auto original_panel=settings_panel_; const auto original_config=config_; const auto actors=sessions_;
    const auto input=smoke_terminal_input_submissions_,mouse=smoke_terminal_mouse_submissions_;
    const auto old_clock=smoke_chrome_time_; const auto old_policy=smoke_settings_scroll_reduced_motion_;
    const auto close_value=[](double a,double b){return std::abs(a-b)<.001;};
    const auto require=[](bool okay,const char* error){if(!okay)throw std::runtime_error(error);};
    smoke_settings_scroll_running_=true;
    smoke_chrome_time_=GetTickCount64()+2000;
    smoke_settings_scroll_reduced_motion_=false;
    const auto reset=[&] {
        settings_panel_=original_panel; settings_seen_generation_=settings_panel_.generation();
        *smoke_chrome_time_+=1000; settings_transition_.finish(); motion_schedule();
    };
    const auto restore=[&] {
        if(GetCapture()==hwnd_)ReleaseCapture();
        settings_panel_=original_panel; settings_seen_generation_=settings_panel_.generation();
        smoke_settings_scroll_reduced_motion_=old_policy; smoke_chrome_time_=old_clock;
        smoke_settings_scroll_running_=false; changed();
    };
    try {
        reset();
        const auto viewport=settings_panel_.layout().content;
        const Point inside{viewport.x+viewport.width/2,viewport.y+viewport.height/2};
        require(close_value(settings_wheel_distance(-120,0,false,viewport.height),0) &&
                close_value(settings_wheel_distance(-15,3,false,viewport.height),13.5) &&
                close_value(settings_wheel_distance(-120,0,true,viewport.height),std::max(48.0,viewport.height-48)),
                "Settings wheel policy lost disabled, fractional or page-scroll behavior.");
        const auto start=*smoke_chrome_time_;
        settings_wheel(inside,-120,3);
        const auto first=settings_panel_.scroll_offset();
        require(first>0 && first<108 && close_value(settings_panel_.scroll_target(),108) && settings_panel_.scroll_active(),
                "Settings discrete wheel did not respond immediately with a bounded animated remainder.");
        smoke_motion_capture(L"settings-scroll-first-000ms",0);
        *smoke_chrome_time_=start+16;motion_schedule();
        require(settings_panel_.scroll_offset()>first && settings_panel_.scroll_offset()<108,
                "Settings scroll skipped its first intermediate presentation.");
        smoke_motion_capture(L"settings-scroll-intermediate-016ms",16);
        *smoke_chrome_time_=start+32;settings_wheel(inside,-120,3);
        require(close_value(settings_panel_.scroll_target(),216),"Settings wheel burst lost accumulated distance.");
        smoke_motion_capture(L"settings-scroll-burst-032ms",32);
        *smoke_chrome_time_=start+48;motion_schedule();
        const auto before_reverse=settings_panel_.scroll_offset();
        settings_wheel(inside,120,3);
        require(settings_panel_.scroll_target()<before_reverse && settings_panel_.scroll_offset()<=before_reverse &&
                settings_panel_.scroll_offset()>=settings_panel_.scroll_target(),
                "Settings reverse wheel retained old-direction backlog or overshot.");
        smoke_motion_capture(L"settings-scroll-reversed-048ms",48);
        *smoke_chrome_time_=start+138;motion_schedule();
        require(!settings_panel_.scroll_active() && close_value(settings_panel_.scroll_offset(),settings_panel_.scroll_target()) && !motion_timer_active_,
                "Settings wheel did not settle and stop its timer within 90 ms of final input.");
        smoke_motion_capture(L"settings-scroll-settled-138ms",138);

        reset();settings_wheel(inside,-120,0);
        require(close_value(settings_panel_.scroll_offset(),0) && !settings_panel_.scroll_active(),"Windows zero wheel-lines policy still moved Settings.");
        settings_wheel(inside,-15,3);
        require(close_value(settings_panel_.scroll_offset(),13.5) && !settings_panel_.scroll_active(),"Fractional Settings wheel packet was quantized or artificially delayed.");
        POINT screen{static_cast<LONG>(inside.x*scale_),static_cast<LONG>(inside.y*scale_)};
        require(ClientToScreen(hwnd_,&screen)!=FALSE,"Settings wheel smoke could not resolve its owned pointer.");
        message(WM_MOUSEHWHEEL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(screen.x,screen.y));
        require(close_value(settings_panel_.scroll_offset(),13.5),"Horizontal wheel was converted into vertical Settings movement.");
        smoke_motion_capture(L"settings-scroll-fractional-direct",0);

        reset();settings_wheel(inside,-120,3);*smoke_chrome_time_+=16;motion_schedule();
        const auto displayed=settings_panel_.scroll_offset();
        const auto& controls=settings_panel_.layout().controls;
        const auto partial=std::find_if(controls.begin(),controls.end(),[&](const auto& control){
            return control.content && control.enabled && control.visible && control.bounds.y<viewport.y && control.hit_bounds.height>0;
        });
        require(partial!=controls.end(),"Settings scroll did not retain a partially exposed control.");
        const auto id=partial->id;
        const Point exposed{partial->hit_bounds.x+partial->hit_bounds.width/2,partial->hit_bounds.y+partial->hit_bounds.height/2};
        pointer_down(exposed,false);
        require(!settings_panel_.scroll_active() && close_value(settings_panel_.scroll_offset(),displayed) && settings_panel_.focused_control()==id,
                "Pointer hit geometry followed the scroll target or moved the displayed control during press.");
        pointer_up({0,0});
        require(settings_panel_.draft()==original_panel.draft() && GetCapture()!=hwnd_,"Canceled Settings press mutated its draft or retained capture.");

        reset();settings_wheel(inside,-120,3);smoke_settings_scroll_reduced_motion_=true;motion_schedule();
        require(!settings_panel_.scroll_active() && close_value(settings_panel_.scroll_offset(),108) && !motion_timer_active_,
                "Reduced motion did not settle active Settings scrolling and remove timer work.");
        settings_wheel(inside,-120,3);
        require(!settings_panel_.scroll_active() && close_value(settings_panel_.scroll_offset(),216),"Reduced motion delayed a new Settings wheel event.");
        smoke_motion_capture(L"settings-scroll-reduced-motion-direct",0);
        require(config_==original_config && sessions_==actors && !focused_session_ &&
                smoke_terminal_input_submissions_==input && smoke_terminal_mouse_submissions_==mouse,
                "Settings scroll evidence changed config, terminal ownership or terminal input.");
    } catch(...) {restore();throw;}
    restore();
    std::fputs("Native Settings scroll: raw wheel conversion, immediate response, finite burst/reversal, partial-control pointer hit, fractional direct input, horizontal consumption, zero/page policy and reduced-motion timer shutdown verified; synthetic_motion_policy=1 controlled_clock=1 native_present=1 physical_frame_pacing_verified=0.\n",stderr);
}
bool NativeWindow::smoke_menu_has_theme(const Theme& theme) const {
    // Probe the palette already supplied by the host without opening an overlay
    // over Settings or weakening its exclusive input gate. OS contrast itself
    // is covered separately; only this owned copy disables its paint override.
    auto probe=context_menu_;
    probe.set_high_contrast(false);probe.resize({1024,768});
    const std::vector<ContextMenuItem> items{
        {"normal","Normal",{},{},true,false,{}},
        {"disabled","Disabled",{},"Reason",false,false,{}},
        {"danger","Danger",{},{},true,true,{}}};
    if(!probe.open(items,{20,20},true))return false;
    WindowScene scene;probe.append_scene(scene);
    const auto ui=resolve_ui_colors(theme);
    const auto text=[&](std::string_view label,Color color){return std::any_of(scene.texts.begin(),scene.texts.end(),[&](const auto& t){return t.text==label&&t.color==color;});};
    return text("Normal",ui.ink)&&text("Disabled",ui.subdued)&&text("Danger",ui.danger)&&
        std::any_of(scene.primitives.begin(),scene.primitives.end(),[&](const auto& p){return p.background_replace&&p.fill==ui.surface&&p.stroke==ui.divider;})&&
        std::any_of(scene.primitives.begin(),scene.primitives.end(),[&](const auto& p){return p.fill==ui.selected&&p.stroke==ui.selected_border;});
}
void NativeWindow::smoke_menu_theme_capture(std::wstring_view suffix) {
    if(!options_.smoke_settings || settings_input_blocked() || settings_loading() || context_menu_.is_open() || !focused_session_ || smoke_menu_theme_running_)
        throw std::runtime_error("Menu theme capture requires the owned Settings smoke window with terminal focus restored.");
    smoke_menu_theme_running_=true;
    struct Reset {bool& running;~Reset(){running=false;}} reset{smoke_menu_theme_running_};
    const auto original_config=config_;const auto actors=sessions_;const auto focused=focused_session_;
    const auto input=smoke_terminal_input_submissions_,mouse=smoke_terminal_mouse_submissions_;
    const auto old_clock=smoke_chrome_time_;
    struct RestoreClock {std::optional<std::uint64_t>& clock;std::optional<std::uint64_t> old;~RestoreClock(){clock=old;}} restore_clock{smoke_chrome_time_,old_clock};
    RECT original{};if(!GetWindowRect(hwnd_,&original))throw std::runtime_error("Menu theme capture could not read owned window geometry.");
    smoke_chrome_time_=GetTickCount64();
    const auto restore=[&] {
        context_menu_.cancel();menu_input_changed();
        const auto resized=SetWindowPos(hwnd_,nullptr,0,0,original.right-original.left,original.bottom-original.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        *smoke_chrome_time_+=1000;menu_transition_.finish();settings_transition_.finish();motion_schedule();smoke_chrome_time_=old_clock;
        return resized!=FALSE;
    };
    try {
        for(const bool narrow:{false,true}) {
            if(narrow && !SetWindowPos(hwnd_,nullptr,0,0,static_cast<int>(480*scale_),static_cast<int>(720*scale_),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))
                throw std::runtime_error("Menu theme capture could not resize owned window.");
            system_menu({12,48},true);
            if(!context_menu_.is_open() || focused_session_ || !smoke_menu_has_theme(config_.theme))
                throw std::runtime_error("Menu theme capture lost host palette or exclusive menu focus.");
            // The narrow sample also demonstrates selected destructive ink.
            if(narrow) {context_menu_.key({{"END"}});menu_input_changed();}
            *smoke_chrome_time_+=1000;motion_schedule();
            smoke_motion_capture(std::wstring(suffix)+(narrow?L"-narrow":L"-normal"),0);
            context_menu_.cancel();menu_input_changed();
            *smoke_chrome_time_+=1000;motion_schedule();
        }
    } catch(...) {restore();throw;}
    if(!restore())throw std::runtime_error("Menu theme capture could not restore owned window geometry.");
    RECT restored{};
    if(!GetWindowRect(hwnd_,&restored) || restored.right-restored.left!=original.right-original.left || restored.bottom-restored.top!=original.bottom-original.top)
        throw std::runtime_error("Menu theme capture did not restore owned window dimensions.");
    if(config_!=original_config || sessions_!=actors || focused_session_!=focused || context_menu_.is_open() ||
       smoke_terminal_input_submissions_!=input || smoke_terminal_mouse_submissions_!=mouse || GetCapture()==hwnd_)
        throw std::runtime_error("Menu theme capture changed config, actor or input ownership.");
    std::fprintf(stderr,"Native menu theme capture=%s normal=1 narrow=1 palette=1 frontend_input_unchanged=1 physical_display_verified=0\n",narrow(std::wstring(suffix)).c_str());
}
void NativeWindow::smoke_progress(bool presented, bool frame_observed) {
    if (!options_.smoke_test || smoke_finished_ || smoke_menu_theme_running_ || smoke_focus_running_ || smoke_settings_scroll_running_ || smoke_workspace_frame_running_) return;
    if(!options_.smoke_agent&&projects_.active()) {
        if(ai_enabled_||agent_controller_||agent_setup_||agent_panel_.is_open()||footer_layout().agent.width>0||command_context().supports_agent_panel) {smoke_finish("Disabled AI feature exposed UI or provider ownership.");return;}
        for(const auto& entry:palette_commands())if(entry.id=="agent.toggle"){smoke_finish("Disabled AI feature leaked into Commands.");return;}
    }
    if (smoke_deadline_.expired(GetTickCount64())) { smoke_finish("Native smoke phase deadline expired."); return; }
    // Wake messages can arrive before the first normal render. Do not let
    // scenario actions/direct capture renders precede first-exposure evidence.
    if (!first_exposed_frame_) return;
    if (!options_.smoke_workspaces.empty()) { smoke_workspace_progress(presented); return; }
    if (sessions_.empty()) { smoke_finish("Native smoke has no real session."); return; }
    for (const auto& [id,session]:sessions_) { (void)id; const auto info=session->info(); if (info.state==SessionState::failed) { smoke_finish(info.error.empty()?"Native session failed.":info.error); return; } }
    if (smoke_deadline_.phase()==SmokeDeadline::Phase::startup) {
        // Managed context proves the preset reached precmd; negotiated paste
        // mode proves ZLE is accepting bulk input. First output bytes alone do
        // not prove either, and can arrive during completion/module startup.
        for (const auto& [id,session]:sessions_) {
            const auto info=session->info(); const auto frame=session->frame();
            if (info.state!=SessionState::running || !info.bytes_received) return;
            if (smoke_managed_sessions_.contains(id) && (!info.shell_context || !frame || !frame->bracketed_paste)) return;
        }
        smoke_phase(SmokeDeadline::Phase::interaction);
    }
    if (options_.smoke_layout && !smoke_palette_input_progress()) return;
    if (options_.smoke_layout && !smoke_context_progress()) return;
    if (options_.smoke_layout && !smoke_watch_progress()) return;
    if (options_.smoke_projects && !smoke_find_progress()) return;
    if (options_.smoke_projects && !smoke_navigator_evidence()) return;
    if (options_.smoke_projects && !smoke_previous_terminal_evidence()) return;
    if (options_.smoke_agent) {
        if (!smoke_agent_stage_) {
            toggle_agent_panel();
            auto initial=agent_panel_.view();initial.fixture=true;initial.state=AgentPanelState::ready;initial.auth=AgentAuth::signed_in;initial.task_available=true;initial.setup_status.clear();
            agent_panel_.open(initial);agent_panel_.text("Review the terminal input path");
            initial.state=AgentPanelState::unavailable;initial.auth=AgentAuth::unknown;agent_panel_.update(initial);
            if (!agent_panel_.is_open() || focused_session_) {smoke_finish("Agent panel did not isolate terminal focus.");return;}
            ++smoke_agent_stage_; changed(); return;
        }
        if (!presented) return;
        const auto path=options_.capture_path;
        options_.capture_path=path.parent_path()/(path.stem().wstring()+L"-agent-"+std::to_wstring(smoke_agent_stage_)+path.extension().wstring());
        try {capture_frame();} catch(const std::exception& error) {options_.capture_path=path;smoke_finish(error.what());return;}
        options_.capture_path=path;
        if (smoke_agent_stage_==9) {
            agent_panel_.key({{"ESCAPE"}}); // Cancel Setup without any save/connect action.
            agent_panel_.key({{"ESCAPE"}});agent_input_changed();
            if(agent_panel_.is_open()) {smoke_finish("Agent Escape failed.");return;}
            toggle_agent_panel();
            if(agent_panel_.draft()!="Review the terminal input path" || agent_panel_.take_action()) {smoke_finish("Agent reopen lost draft or replayed an action.");return;}
            std::fprintf(stderr,"Native agent UI fixture: missing/detecting/sign-in/browser/ready/running/approval/error/advanced captures; no Codex server, authentication or model call.\n");
            smoke_finish();return;
        }
        auto view=agent_panel_.view();view.fixture=true;view.setup_status.clear();
        if(smoke_agent_stage_==1) {view.state=AgentPanelState::connecting;}
        if(smoke_agent_stage_==2) {view.state=AgentPanelState::ready;view.auth=AgentAuth::signed_out;}
        if(smoke_agent_stage_==3) {view.auth=AgentAuth::signing_in;view.login_id="fixture-login";view.auth_url="https://auth.openai.com/fixture";}
        if(smoke_agent_stage_==4) {view.auth=AgentAuth::signed_in;view.account_label="fixture@example.com";view.auth_url.clear();view.login_id.clear();}
        if(smoke_agent_stage_==5) {view.state=AgentPanelState::running;view.transcript={{"user","Review the terminal input path"},{"assistant","Authored fixture: inspecting keyboard routing and checking that composed text reaches the active terminal only once."}};}
        if(smoke_agent_stage_==6) {view.approvals={{{1,"fixture-thread","fixture-turn","fixture-item","1"},"Read source files","Read-only fixture scope: src/platform/windows/window.cpp\n\nThis request can inspect source files. It cannot write files or access the network.",true}};}
        if(smoke_agent_stage_==7) {view.approvals.clear();view.state=AgentPanelState::failed;view.error="Authored fixture: provider disconnected. No task was retried.";}
        if(smoke_agent_stage_==8) {view.state=AgentPanelState::unavailable;view.error.clear();view.setup_available=true;view.provider_settings={"C:\\Fixture\\codex.exe",""};}
        view.task_available=view.auth==AgentAuth::signed_in&&view.state==AgentPanelState::ready;
        agent_panel_.update(std::move(view));
        if(smoke_agent_stage_==8) for(const auto& control:agent_panel_.controls()) if(control.id=="settings.open") {const Point point{control.bounds.x+1,control.bounds.y+1};agent_panel_.pointer_down(point);agent_panel_.pointer_up(point);break;}
        ++smoke_agent_stage_;changed();return;
    }
    if (!smoke_chrome_verified_) {
        const auto native_hit=[&](Rect bounds) {
            POINT p{static_cast<LONG>(std::lround((bounds.x+bounds.width/2)*scale_)),static_cast<LONG>(std::lround((bounds.y+bounds.height/2)*scale_))};
            ClientToScreen(hwnd_,&p);
            return SendMessageW(hwnd_,WM_NCHITTEST,0,MAKELPARAM(p.x,p.y));
        };
        if (chrome_.band.height!=32 || chrome_.tabs_band.x!=navigation_.rail.width || navigation_.context.height!=0 ||
            chrome_.tabs_band.y!=chrome_.band.height || navigation_.viewport.y+navigation_.viewport.height!=navigation_.footer.y ||
            navigation_.viewport.y!=chrome_.tabs_band.y+chrome_.tabs_band.height || navigation_.footer.x!=navigation_.viewport.x ||
            navigation_.focus.width<=0 || native_hit(navigation_.focus)!=HTCLIENT ||
            chrome_.new_tab.x+chrome_.new_tab.width>navigation_.focus.x ||
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
        // Only the owned --smoke-settings window and isolated config participate.
        // Use the actual native scene renderer; never load user captures or frames.
        const auto capture_settings=[&](std::wstring_view suffix) {
            const auto old_clock=smoke_chrome_time_;
            smoke_chrome_time_=GetTickCount64()+1000;
            settings_transition_.finish(); motion_schedule();
            try { smoke_motion_capture(suffix,0); }
            catch (...) {smoke_chrome_time_=old_clock;throw;}
            smoke_chrome_time_=old_clock;
        };
        const auto set_blur=[&](unsigned sigma) {
            for(unsigned i=0;i<256 && settings_panel_.focused_control()!="appearance.blur";++i) settings_panel_.key({{"TAB"}});
            if(settings_panel_.focused_control()!="appearance.blur") return false;
            settings_panel_.key({{"HOME"}});
            for(unsigned i=0;i<sigma;++i) settings_panel_.key({{"RIGHT"}});
            settings_input_changed();
            return config_.appearance.additional_blur==sigma && settings_panel_.draft().appearance.additional_blur==sigma &&
                renderer_->stats().warp && !renderer_->stats().host_backdrop;
        };
        const auto edit_ansi_red=[&](std::string_view digits,bool capture_editor) {
            // Every newly opened Appearance editor starts at Background; ANSI1
            // is the sixth role. Send hex through the native key trap, not a
            // direct Config assignment, and assert it never reaches the shell.
            for (unsigned i=0;i<5;++i) if (!activate_setting("appearance.color.field")) return false;
            if (!activate_setting("appearance.color.value")) return false;
            for (const auto c:digits) {
                MSG key{};key.hwnd=hwnd_;key.message=WM_KEYDOWN;key.wParam=static_cast<WPARAM>(c);
                if (!key_message(key)) return false;
            }
            if (capture_editor) capture_settings(L"appearance-custom-edit");
            MSG enter{};enter.hwnd=hwnd_;enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
            if (!key_message(enter)) return false;
            return settings_panel_.draft().theme.name=="custom" && settings_panel_.draft().theme.ansi[1]=="#"+std::string(digits);
        };
        const auto saved_custom=[] {
            auto theme=*find_builtin_theme("evergreen");theme.name="custom";theme.ansi[1]="#C07090";return theme;
        };
        const auto actor_has_theme=[&](const Theme& theme) {
            const auto frame=smoke_settings_actor_?smoke_settings_actor_->frame():FramePtr{};
            return frame && frame->foreground==color_hex(theme.foreground,{}) && frame->background==color_hex(theme.background,{});
        };
        if (smoke_settings_stage_==0) {
            if (info.state!=SessionState::running || !info.bytes_received) return;
            smoke_settings_original_=config_; smoke_settings_actor_=sessions_.at(workspace().pane(workspace().active_pane())->session);
            smoke_hold_settings_completion_=true;
            open_settings();const auto worker=settings_worker_.get_id();
            MSG escape{};escape.hwnd=hwnd_;escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;key_message(escape);escape.message=WM_KEYUP;key_message(escape);
            if(settings_open_requested_||!settings_job_||!settings_load_job_){smoke_finish("Settings loading cancel lost its owned worker.");return;}
            open_settings("terminal.find");open_settings();
            if(!settings_open_requested_||!settings_binding_.empty()||settings_worker_.get_id()!=worker){smoke_finish("Settings reopen did not reattach latest intent to same load.");return;}
            smoke_settings_stage(1);smoke_hold_settings_completion_=false;settings_completed();
            std::fputs("Native Settings loading intent: held completion, Escape, immediate reopen and latest binding reuse same owned worker.\n",stderr);return;
        }
        if (smoke_settings_stage_==1) {
            if (settings_job_) return;
            if (!settings_panel_.is_open() || focused_session_ || settings_panel_.draft()!=smoke_settings_original_) { smoke_finish("Settings did not open from its owned snapshot and remove terminal focus."); return; }
            smoke_chrome_time_=GetTickCount64()+1000;
            settings_transition_=ChromeTransition{}; motion_schedule();
            const auto start=*smoke_chrome_time_;
            *smoke_chrome_time_=start+40; smoke_motion_capture(L"settings-open-040ms",40);
            *smoke_chrome_time_=start+100; motion_schedule(); smoke_motion_capture(L"settings-open-settled-100ms",100);
            smoke_chrome_time_.reset();
            const auto gallery_ok=[&](unsigned columns) {
                std::vector<Rect> cards;
                for(const auto& control:settings_panel_.layout().controls)if(control.id.starts_with("appearance.theme."))cards.push_back(control.bounds);
                if(cards.size()!=9)return false;
                unsigned first_row{};for(const auto& card:cards)if(std::abs(card.y-cards.front().y)<.1)++first_row;
                return first_row==columns;
            };
            if(!gallery_ok(3)){smoke_finish("Normal theme gallery did not expose nine cards in three columns.");return;}
            capture_settings(L"theme-gallery-normal");
            RECT gallery_original{};GetWindowRect(hwnd_,&gallery_original);
            SetWindowPos(hwnd_,nullptr,0,0,static_cast<int>(480*scale_),static_cast<int>(720*scale_),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            if(!gallery_ok(2)){smoke_finish("Narrow theme gallery did not retain nine cards in two columns.");return;}
            capture_settings(L"theme-gallery-narrow");
            SetWindowPos(hwnd_,nullptr,0,0,gallery_original.right-gallery_original.left,gallery_original.bottom-gallery_original.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            settings_panel_.set_high_contrast(true,{0,0,0,255},{255,255,255,255},{255,255,0,255},{0,0,0,255},{192,192,192,255});
            capture_settings(L"theme-gallery-high-contrast-fixture");
            settings_changed();
            if(settings_panel_.draft()!=smoke_settings_original_||config_!=smoke_settings_original_||focused_session_){smoke_finish("Theme gallery presentation changed draft or terminal input ownership.");return;}
            std::fputs("Native theme gallery: nine preset cards, normal3/narrow2 columns, authored high-contrast panel palette, unchanged draft and exclusive Settings focus; not OS accessibility qualification.\n",stderr);
            smoke_settings_scroll_evidence();
            MSG key{}; key.hwnd=hwnd_; key.message=WM_KEYDOWN; key.wParam='Q';
            if (!key_message(key)) { smoke_finish("Settings printable key escaped input trapping."); return; }
            terminal_text("SETTINGS_INPUT_MUST_NOT_REACH_SHELL");
            for(unsigned i=0;i<256&&settings_panel_.focused_control()!="font.increase";++i)settings_panel_.key({{"TAB"}});
            const auto& controls=settings_panel_.layout().controls;
            const auto found=std::find_if(controls.begin(),controls.end(),[](const auto& control){return control.id=="font.increase" && control.visible && control.enabled;});
            if (found==controls.end()) { smoke_finish("Settings font preview control is not reachable."); return; }
            const Point point{found->bounds.x+found->bounds.width/2,found->bounds.y+found->bounds.height/2};
            pointer_down(point,false); pointer_up(point);
            if (config_.font.size_dip!=smoke_settings_original_.font.size_dip+1 || config_.default_profile!=smoke_settings_original_.default_profile) { smoke_finish("Settings preview did not apply only reversible presentation."); return; }
            if (!activate_setting("appearance.theme.daylight") || !smoke_menu_has_theme(config_.theme)) {
                smoke_finish("Light theme preview did not refresh the host menu palette.");return;
            }
            if (!activate_setting("appearance.theme.midnight") || config_.theme!=*find_builtin_theme("midnight") || !smoke_menu_has_theme(config_.theme)) {
                smoke_finish("Named theme preview did not update the live host palette."); return;
            }
            // Incomplete native hex input must not publish a partial palette.
            if (!activate_setting("appearance.color.value")) {smoke_finish("Custom color editor was unreachable.");return;}
            MSG hex{};hex.hwnd=hwnd_;hex.message=WM_KEYDOWN;hex.wParam='F';key_message(hex);
            hex.wParam=VK_ESCAPE;key_message(hex);
            if (!settings_panel_.is_open() || config_.theme!=*find_builtin_theme("midnight")) {smoke_finish("Partial hex Escape changed the theme or closed Settings.");return;}
            if (!edit_ansi_red("D080A0",false) || config_.theme.ansi[1]!="#D080A0" || !smoke_menu_has_theme(config_.theme)) {smoke_finish("Native ANSI custom preview failed.");return;}
            capture_settings(L"appearance-custom-preview");
            for (const auto* id:{"appearance.import","appearance.export"}) {
                for(unsigned i=0;i<256&&settings_panel_.focused_control()!=id;++i)settings_panel_.key({{"TAB"}});
                if(settings_panel_.focused_control()!=id){smoke_finish("Theme import/export controls are not keyboard reachable.");return;}
                const auto& palette_controls=settings_panel_.layout().controls;
                if(std::none_of(palette_controls.begin(),palette_controls.end(),[&](const auto& control){return control.id==id && control.visible && control.enabled;})) {
                    smoke_finish("Theme import/export controls are not visible and reachable.");return;
                }
            }
            capture_settings(L"appearance-theme-exchange");
            std::fputs("Theme exchange buttons: visible=1 enabled=1 picker_selection_automated=0\n",stderr);
            if(smoke_settings_original_.appearance.additional_blur!=0 || !set_blur(24)) {
                smoke_finish("Additional blur default/preview or WARP fallback failed.");return;
            }
            capture_settings(L"appearance-blur-preview");
            if(!set_blur(0)) {smoke_finish("Blur zero did not preview clear preference.");return;}
            for(unsigned i=0;i<256 && settings_panel_.focused_control()!="appearance.opacity";++i) settings_panel_.key({{"TAB"}});
            settings_panel_.key({{"HOME"}});settings_input_changed();
            if(config_.appearance.background_opacity!=0 || renderer_->stats().host_backdrop) {smoke_finish("Clear-zero preference altered blur or tint semantics.");return;}
            if(!set_blur(0)) {smoke_finish("Clear-zero control unreachable.");return;}
            capture_settings(L"appearance-clear-zero");
            if (!activate_setting("nav.ribbon") || !activate_setting("ribbon.toggle:session.git") ||
                config_.ribbon.hidden[ribbon_index(RibbonBinding::git)]==smoke_settings_original_.ribbon.hidden[ribbon_index(RibbonBinding::git)]) {
                smoke_finish("Ribbon toggle did not live preview before Cancel.");return;
            }
            capture_settings(L"ribbon-preview");
            if (!activate_setting("nav.appearance")) {smoke_finish("Appearance return was unreachable.");return;}
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
            if (!presented || !actor_has_theme(*find_builtin_theme("midnight"))) return;
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
            smoke_chrome_time_=GetTickCount64()+1000; motion_schedule();
            const auto motion_start=*smoke_chrome_time_;
            MSG key{}; key.hwnd=hwnd_; key.message=WM_KEYDOWN; key.wParam=VK_ESCAPE; key_message(key);
            *smoke_chrome_time_=motion_start+40; smoke_motion_capture(L"settings-close-040ms",40);
            *smoke_chrome_time_=motion_start+100; motion_schedule(); smoke_motion_capture(L"settings-close-settled-100ms",100);
            if (settings_panel_.is_open() || config_!=smoke_settings_original_ || !focused_session_ || !smoke_menu_has_theme(config_.theme)) { smoke_finish("Settings Escape did not roll back preview and restore terminal focus."); return; }
            system_menu({12,48},true);
            if (!context_menu_.is_open() || focused_session_) { smoke_finish("Custom system menu did not open or trap focus."); return; }
            const auto menu_start=*smoke_chrome_time_;
            *smoke_chrome_time_=menu_start+40; smoke_motion_capture(L"menu-open-040ms",40);
            const auto menu_midway=menu_transition_.value(chrome_now());
            key_message(key);
            if (!reduced_motion_ && std::abs(menu_transition_.value(chrome_now())-menu_midway)>1e-9) throw std::runtime_error("Menu dismissal jumped during reveal.");
            *smoke_chrome_time_=menu_start+70; smoke_motion_capture(L"menu-close-reversed-070ms",70);
            *smoke_chrome_time_=menu_start+140; motion_schedule(); smoke_motion_capture(L"menu-close-settled-140ms",140);
            if (menu_transition_.value(chrome_now())!=0 || motion_timer_active_) throw std::runtime_error("Menu dismissal left idle motion work.");
            system_menu({12,48},true);
            *smoke_chrome_time_=menu_start+240; motion_schedule(); smoke_motion_capture(L"menu-open-settled-100ms",100);
            key_message(key);
            *smoke_chrome_time_=menu_start+340; motion_schedule();
            smoke_chrome_time_.reset();
            if (context_menu_.is_open() || !focused_session_) { smoke_finish("Custom system menu Escape did not restore focus."); return; }
            popup_menu({12,48});
            const Point outside{width_-2,height_-2}; pointer_down(outside,false); pointer_up(outside);
            if (context_menu_.is_open() || drag_!=Drag::none || GetCapture()==hwnd_) { smoke_finish("Menu outside click did not dismiss without starting terminal selection."); return; }
            smoke_menu_theme_capture(L"appearance-menu-cancelled");
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
            if (!activate_setting("nav.appearance") || !activate_setting("appearance.theme.evergreen")) {
                smoke_finish("Named theme could not be selected before Save."); return;
            }
            if (!edit_ansi_red("C07090",true) || config_.theme!=saved_custom()) {smoke_finish("Custom ANSI save draft did not reach the host.");return;}
            if (!activate_setting("nav.ribbon") || !activate_setting("ribbon.up:session.status") ||
                !activate_setting("ribbon.toggle:session.git")) {smoke_finish("Ribbon order/visibility draft was unreachable.");return;}
            capture_settings(L"ribbon-before-save");
            if (!activate_setting("nav.appearance")) {smoke_finish("Appearance return before Save failed.");return;}
            for (unsigned i=0;i<256 && settings_panel_.focused_control()!="appearance.opacity";++i) settings_panel_.key({{"TAB"}});
            settings_panel_.key({{"HOME"}});
            for (unsigned i=0;i<25;++i) settings_panel_.key({{"RIGHT"}});
            settings_input_changed();
            if (settings_panel_.draft().appearance.background_opacity!=.25) {smoke_finish("Full-range opacity did not accept 25%.");return;}
            if(!set_blur(24) || config_.appearance.background_opacity!=.25) {smoke_finish("Blur preview changed independent opacity.");return;}
            capture_settings(L"appearance-blur-before-save");
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
            if (!actor_has_theme(saved_custom())) return;
            if (config_.theme!=saved_custom() || config_.appearance.background_opacity!=.25 || config_.appearance.additional_blur!=24 || !smoke_menu_has_theme(config_.theme)) {
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
            smoke_menu_theme_capture(L"appearance-menu-saved-custom");
            open_settings(); smoke_settings_stage(5); return;
        }
        if (smoke_settings_stage_==5) {
            if (settings_job_) return;
            if (!settings_panel_.is_open() || !settings_baseline_matches(settings_panel_.draft(),config_,Platform::windows)) {
                smoke_finish("Settings Save-to-reopen falsely rejected its canonical saved baseline."); return;
            }
            const auto saved=parse_config(settings_snapshot_->bytes,Platform::windows);
            if (!saved || saved.config->theme!=saved_custom() || settings_panel_.draft().theme!=saved_custom() ||
                config_.ribbon.order[1]!=RibbonBinding::status ||
                config_.ribbon.hidden[ribbon_index(RibbonBinding::git)]==smoke_settings_original_.ribbon.hidden[ribbon_index(RibbonBinding::git)]) {
                smoke_finish("Custom ANSI or Ribbon order/visibility did not survive Save/reopen.");return;
            }
            capture_settings(L"appearance-saved-reopened");
            if(saved.config->appearance.additional_blur!=24 || !set_blur(24)) {smoke_finish("Additional blur did not persist/reopen.");return;}
            capture_settings(L"appearance-blur-saved-reopened");
            std::fputs("Additional blur settings: persisted=24 software=1 host_graph=0 physical_blur_verified=0\n",stderr);
            if (!activate_setting("nav.ribbon")) {smoke_finish("Saved Ribbon section was unreachable.");return;}
            capture_settings(L"ribbon-saved-reopened");
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
            const auto saved_baseline=config_;
            if (!activate_setting("appearance.theme.ember") || !activate_setting("appearance.theme.custom") ||
                settings_panel_.draft().theme!=saved_custom()) {smoke_finish("Saved Custom was lost when switching presets after reopen.");return;}
            if (!activate_setting("appearance.theme.aubergine") || !edit_ansi_red("B090E0",false)) {
                smoke_finish("Second-cycle custom preview was unreachable.");return;
            }
            const auto unsaved=settings_panel_.draft(); const auto preview=config_;
            reload_settings();
            if (settings_job_ || settings_panel_.draft()!=unsaved || config_!=preview || notice_.empty()) {
                smoke_finish("Reload did not preserve/refuse the unsaved Settings draft."); return;
            }
            settings_panel_.cancel(); settings_input_changed();
            if (config_!=saved_baseline || !smoke_menu_has_theme(config_.theme)) {smoke_finish("Second-cycle Cancel did not restore saved ANSI/Ribbon configuration.");return;}
            if(!notice_.empty()||!settings_draft_notice_.empty()){smoke_finish("Closed Settings retained its draft-only reload notice.");return;}
            if (settings_panel_.is_open() || !focused_session_) { smoke_finish("Reopened Settings did not restore terminal focus on Cancel."); return; }
            smoke_settings_stage(8);
            force_redraw_=true; dirty_=true; // Capture the closed scene even if open/cancel coalesced.
            return; // Cancel cannot also impersonate a subsequent native input event.
        }
    }
    if (options_.smoke_settings && smoke_settings_stage_==8 && smoke_settings_input_stage_<2) {
        if (!smoke_settings_input_stage_ && presented && !settings_panel_.is_open() && !motion_timer_active_) {
            const auto restored=smoke_settings_actor_?smoke_settings_actor_->frame():FramePtr{};
            if(!restored||restored->foreground!=color_hex(config_.theme.foreground,{})||restored->background!=color_hex(config_.theme.background,{}))return;
            capture_named_frame(L"appearance-shell-same-theme");
            std::fputs("Semantic UI capture: saved custom theme, settings closed, presented=1 motion_settled=1\n",stderr);
            smoke_settings_input_stage_=1;
            if (!PostMessageW(hwnd_,smoke_settings_input_message,0,0)) smoke_finish("Could not post the settings-close paste input event.");
        }
        return;
    }
    if (options_.smoke_settings && smoke_reload_stage_<4) {
        const auto write_fixture=[&](std::string_view bytes) {
            // Only the smoke-owned, isolated config created by the earlier Save.
            if (!smoke_settings_file_owned_) throw std::runtime_error("Reload smoke has no owned settings fixture.");
            std::ofstream output(options_.config_path,std::ios::binary|std::ios::trunc);
            output.write(bytes.data(),static_cast<std::streamsize>(bytes.size())); output.close();
            if (!output) throw std::runtime_error("Could not write reload smoke fixture.");
        };
        if (!smoke_reload_stage_) {
            smoke_reload_actors_=sessions_;
            smoke_reload_expected_=config_;
            smoke_reload_expected_.experimental_ai=true; // Reload must not enable the launch-time capability.
            smoke_reload_expected_.theme=*find_builtin_theme("ember");
            smoke_reload_expected_.font.size_dip=17;
            smoke_reload_expected_.ribbon.hidden[ribbon_index(RibbonBinding::git)]=!config_.ribbon.hidden[ribbon_index(RibbonBinding::git)];
            if (smoke_reload_expected_.bindings.size()>1)
                std::swap(smoke_reload_expected_.bindings[0].chord,smoke_reload_expected_.bindings[1].chord);
            ++smoke_reload_expected_.scrollback_lines;
            smoke_reload_expected_.profiles.front().label="Reloaded profile";
            smoke_reload_expected_.appearance.additional_blur=36;
            smoke_reload_expected_.appearance.show_clock=!config_.appearance.show_clock;
            smoke_reload_received_=smoke_settings_actor_->info().bytes_received;
            const auto bytes=serialize_config(smoke_reload_expected_,Platform::windows);
            const auto canonical=parse_config(bytes,Platform::windows);
            if (!canonical) { smoke_finish("Reload smoke fixture failed validation."); return; }
            smoke_reload_expected_=*canonical.config;
            write_fixture(bytes);
            dispatch("settings.reload");
            const auto generation=settings_reload_.generation();
            reload_settings(); // Repeated activation must not enqueue or supersede.
            if (!settings_job_ || generation!=settings_reload_.generation()) { smoke_finish("Repeated reload was not serialized."); return; }
            smoke_reload_stage_=1; return;
        }
        if (settings_job_) return;
        if (sessions_!=smoke_reload_actors_ ||
            smoke_settings_actor_->info().bytes_received<smoke_reload_received_ || !smoke_settings_actor_->frame()) {
            smoke_finish("Reload replaced the live actor or lost retained output."); return;
        }
        if (config_!=smoke_reload_expected_ || !smoke_menu_has_theme(config_.theme)) { smoke_finish("Reload did not apply valid settings or replaced settings on failure."); return; }
        if (smoke_reload_stage_==1) {
            smoke_menu_theme_capture(L"appearance-menu-reloaded");
            write_fixture("[not valid TOML"); reload_settings(); smoke_reload_stage_=2; return;
        }
        if (smoke_reload_stage_==2) {
            if (notice_.empty()) { smoke_finish("Invalid reload had no native error notice."); return; }
            std::filesystem::remove(options_.config_path); reload_settings(); smoke_reload_stage_=3; return;
        }
        if (notice_.empty()) { smoke_finish("Missing reload had no native error notice."); return; }
        write_fixture(serialize_config(smoke_reload_expected_,Platform::windows));
        smoke_reload_stage_=4;
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
        if (smoke_modal_stage_==1 && presented) smoke_modal_presented_=true;
        if (smoke_modal_stage_==1 && smoke_modal_presented_) {
            // Presentation and background launch readiness are independent events.
            // Parked output may leave the visible modal scene unchanged.
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
            if (smoke_managed_sessions_.contains(id) && !info.shell_context) return;
        }
        smoke_focus_evidence(true);
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
        if (!smoke_palette_evidence(smoke_parked_session_)) return;
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
        if(const auto* pane=workspace().pane(workspace().active_pane()); pane && smoke_managed_sessions_.contains(pane->session)) {
            const auto* actor=active_session(); if(!actor || !actor->info().shell_context) return;
        }
        if (chrome_.compact || chrome_.tabs.size()<3) { smoke_finish("Layout smoke task tabs unexpectedly compact."); return; }
        smoke_focus_evidence(false);
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
        const auto footer=footer_layout();
        if(footer.directory.width<=0 || (footer.background.chips.empty() && footer.background.overflow.width<=0)) {
            smoke_finish("Active footer context disappeared behind background terminals."); return;
        }
        const auto* foreground=active_session();
        if(!foreground || footer_presentation().directory.empty()) {
            smoke_finish("Foreground footer lost its actual session directory/profile."); return;
        }
        std::fprintf(stderr,"Native footer smoke: active directory/context retained beside background restore targets.\n");
        const auto current_context=foreground->info().shell_context;
        if(current_context && footer_presentation().directory!=current_context->display_directory) {
            smoke_finish("Footer did not use the active actor's owned display directory."); return;
        }
        smoke_chrome_time_=GetTickCount64()+1000;
        smoke_motion_capture(L"footer-background",0);
        const auto original_project=projects_.active_id();const auto original_actor=active_session();const auto input_before=smoke_terminal_input_submissions_;
        if(!footer.background.chips.empty()||background_summary().terminals!=2){smoke_finish("Background summary did not collapse its owned terminal list.");return;}
        const Point button{footer.background.overflow.x+footer.background.overflow.width/2,footer.background.overflow.y+footer.background.overflow.height/2};
        pointer_down(button,false);pointer_up(button);
        if(!navigator_open_||!navigator_background_only_||navigator_.terminals().size()!=2||!navigator_.target()||!navigator_.target()->parked){smoke_finish("Background footer did not open its scoped Navigator list.");return;}
        smoke_motion_capture(L"background-navigator",0);
        MSG escape{};escape.hwnd=hwnd_;escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;key_message(escape);escape.message=WM_KEYUP;key_message(escape);
        if(navigator_open_||projects_.active_id()!=original_project||active_session()!=original_actor||smoke_terminal_input_submissions_!=input_before){smoke_finish("Background preview changed workspace, actor or terminal input.");return;}
        std::fputs("Native background summary: workspace-owned compact target, parked-only Navigator, Escape preserves context and frontend input.\n",stderr);
        smoke_chrome_time_.reset();
    }
    if (options_.smoke_projects && smoke_projects_live_verified_ && !smoke_background_command_verified_) {
        const auto actor=sessions_.at(smoke_parked_session_);
        if (!smoke_background_command_sent_) {
            const auto ready_frame=actor->frame();
            if (smoke_managed_sessions_.contains(smoke_parked_session_) && (!ready_frame || !ready_frame->bracketed_paste)) return;
            // Generate OSC inside the child, never by echoing raw control bytes
            // through terminal input. Keep the shell alive after prompt return.
            const std::string command=smoke_managed_sessions_.contains(smoke_parked_session_)?
                "printf '\\033]133;C\\007\\033]133;D;0\\007'":
                "powershell.exe -NoProfile -Command \"$e=[char]27; [Console]::Write($e+']133;C'+[char]7+$e+']133;D;0'+[char]7)\"";
            submit([actor,command]{return actor->paste(command);},command.size());
            KeyInput enter; enter.key=Key::enter;
            submit([actor,enter]{return actor->key(enter);});
            smoke_background_command_sent_=true;
            return;
        }
        if (actor->process_finished() || actor!=smoke_project_actors_.at(smoke_parked_session_)) {
            smoke_finish("Command completion replaced or exited the parked shell."); return;
        }
        const auto attention=parked_attention_.find(smoke_parked_session_);
        if (attention==parked_attention_.end() || !attention->second.unread_completion()) return;
        // Capture the actual unread footer while the original shell is alive,
        // before failed-restore/close checks replace the completion notice.
        if (smoke_background_capture_stage_==0) {
            activate_project(smoke_primary_project_);
            smoke_background_capture_stage_=1;
            changed(true); return; // A later presented frame must contain this workspace.
        }
        if (smoke_background_capture_stage_==1) {
            if (!presented || motion_timer_active_) return;
            const auto original_path=options_.capture_path;
            if (!original_path.empty()) {
                options_.capture_path=original_path.parent_path()/(original_path.stem().wstring()+L"-command-finished"+original_path.extension().wstring());
                try { capture_frame(); }
                catch (const std::exception& error) { options_.capture_path=original_path; smoke_finish(error.what()); return; }
                options_.capture_path=original_path;
            }
            const auto count_before=background_summary();const auto input_before=smoke_terminal_input_submissions_;
            open_navigator(true);
            if(!navigator_open_||!navigator_background_only_||count_before.terminals!=1||count_before.unread()!=1||!navigator_.target()||navigator_.target()->session!=smoke_parked_session_){smoke_finish("Unread background summary opened the wrong scope.");return;}
            const auto old_clock=smoke_chrome_time_;smoke_chrome_time_=GetTickCount64()+1000;smoke_motion_capture(L"background-finished-navigator",0);smoke_chrome_time_=old_clock;
            MSG escape{};escape.hwnd=hwnd_;escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;key_message(escape);escape.message=WM_KEYUP;key_message(escape);
            if(navigator_open_||background_summary().unread()!=1||smoke_terminal_input_submissions_!=input_before){smoke_finish("Background preview acknowledged unread completion or leaked input.");return;}
            std::fputs("Native background attention: real observed completion remains unread through Navigator preview and Escape.\n",stderr);
            smoke_background_capture_stage_=2;
            activate_project(smoke_secondary_project_);
            return;
        }
        restore_parked({smoke_primary_project_,smoke_parked_session_}); // Wrong active project.
        if (!parked_attention_.at(smoke_parked_session_).unread_completion()) {
            smoke_finish("Failed restore acknowledged background command completion."); return;
        }
        activate_project(smoke_primary_project_);
        close_parked({smoke_primary_project_,smoke_parked_session_});
        if (!modal_) { smoke_finish("Completed background terminal skipped close confirmation."); return; }
        answer_modal(ModalDecision::cancel);
        if (!parked_attention_.at(smoke_parked_session_).unread_completion() || sessions_.at(smoke_parked_session_)!=actor) {
            smoke_finish("Canceled close lost command attention or actor ownership."); return;
        }
        activate_project(smoke_secondary_project_);
        smoke_background_command_verified_=true;
    }
    if (!options_.smoke_agent && !options_.smoke_layout && !options_.smoke_projects && !options_.smoke_settings && !options_.smoke_modal && (!frame_observed || !smoke_typing_progress())) return;
    bool all_closed=true;
    for (const auto& [id,session]:sessions_) {
        const auto info=session->info();
        if (info.state==SessionState::failed) { smoke_finish(info.error.empty()?"Native session failed.":info.error); return; }
        if (!smoke_commands_sent_.contains(id) && info.state==SessionState::running && info.bytes_received) {
            const auto ready_frame=session->frame();
            if (smoke_managed_sessions_.contains(id) && (!ready_frame || !ready_frame->bracketed_paste)) { all_closed=false; continue; }
            smoke_phase(SmokeDeadline::Phase::exit);
            smoke_commands_sent_.insert(id);
            const std::string command=smoke_managed_sessions_.contains(id)?
                "[[ $ZSH_VERSION == 5.9.2 && ${STORMTTY_MANAGED_ZSH-} == 1 ]] && print -r -- " + std::string(smoke_marker) + "; exit $?":
                "echo " + std::string(smoke_marker) + " & exit /b 0";
            submit([session,command] { return session->paste(command); },command.size());
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
            open_navigator(true);
            if(!navigator_.target()||navigator_.target()->session!=smoke_parked_session_||!navigator_.target()->parked){smoke_finish("Background keyboard restore selected a stale actor.");return;}
            MSG enter{};enter.hwnd=hwnd_;enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;key_message(enter);enter.message=WM_KEYUP;key_message(enter);
            if (!active_session() || active_session()!=actor.get() || !projects_.active()->parked_sessions.empty() || layout_.panes.empty() || parked_attention_.contains(smoke_parked_session_)) {
                smoke_finish("Restore did not reattach the same exited actor and latest frame."); return;
            }
            const auto return_actors=sessions_.size(),return_input=smoke_terminal_input_submissions_;
            dispatch("terminal.previous");
            if(projects_.active_id()!=smoke_secondary_project_||!active_session()||!active_session()->process_finished()) {
                smoke_finish("Return did not accept the retained exited terminal in the other workspace.");return;
            }
            dispatch("terminal.previous");
            if(active_session()!=actor.get()||focused_session_!=smoke_parked_session_||sessions_.size()!=return_actors||smoke_terminal_input_submissions_!=return_input) {
                smoke_finish("Return replaced or submitted input to a retained exited actor.");return;
            }
            std::fputs("Native previous-terminal retained exit: same exited actors navigable in both directions without spawn, input or restore.\n",stderr);
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
                // Exercise the actual owned menu against a non-active workspace,
                // not the command-palette path that targets the active project.
                workspace_menu({navigation_.rail.x+16,chrome_.band.height+44},smoke_secondary_project_);
                const auto& panels=context_menu_.layout().panels;
                if (panels.empty()) { smoke_finish("Workspace context menu did not open."); return; }
                const auto& rows=panels.front().rows;
                const auto close=std::find_if(rows.begin(),rows.end(),[](const auto& row){return row.id=="workspace.close";});
                if (close==rows.end() || !close->visible || !close->enabled) {
                    smoke_finish("Close workspace is not directly available in the workspace menu."); return;
                }
                const Point close_point{close->bounds.x+close->bounds.width/2,close->bounds.y+close->bounds.height/2};
                context_menu_.pointer_down(close_point); context_menu_.pointer_up(close_point); menu_input_changed();
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
    if (!error.empty()) {
        smoke_typing_diagnostic("failure");
        std::fprintf(stderr,"Native smoke failure gates: frame_ready=%d marker_presented=%d motion_timer=%d material_requested=%d material_running=%d material_succeeded=%d presents=%llu pending_input=%zu\n",
            smoke_frame_ready(),smoke_marker_presented_,motion_timer_active_,
            material_probe_requested_,material_probe_running_,material_probe_succeeded_,
            static_cast<unsigned long long>(renderer_?renderer_->stats().presents:0),pending_input_.size());
        // Only isolated smoke-owned shells are eligible for fixture text evidence.
        // Never dump a normal user session or inherited history/configuration.
        if (!smoke_shell_state_root_.empty()) for (const auto& [id,session]:sessions_) {
            const auto info=session->info(); const auto frame=session->frame();
            std::fprintf(stderr,"Native smoke failed actor=%llu modal_stage=%u modal_presented=%d command_sent=%d rx=%llu tx=%llu bracketed=%d\n",
                static_cast<unsigned long long>(id.value),smoke_modal_stage_,smoke_modal_presented_,smoke_commands_sent_.contains(id),
                static_cast<unsigned long long>(info.bytes_received),static_cast<unsigned long long>(info.bytes_sent),frame&&frame->bracketed_paste);
            const auto diagnostics=session->diagnostics();
            const bool visible=std::any_of(layout_.panes.begin(),layout_.panes.end(),
                [&](const auto& placement){return placement.session==id;});
            // Failure-only numeric observations; never log shell paths or data.
            std::fprintf(stderr,"Native smoke actor state: actor=%llu state=%d finished=%d closed=%d exit_present=%d exit_code=%llu error_present=%d managed_requested=%d bundled_requested=%d provider_known=%d actual_system=%d actual_bundled=%d context_present=%d visible=%d frame_present=%d frame_generation=%llu columns=%u rows=%u marker=%d queued_input=%zu\n",
                static_cast<unsigned long long>(id.value),static_cast<int>(info.state),
                session->process_finished(),session->closed(),info.exit_code.has_value(),
                static_cast<unsigned long long>(info.exit_code.value_or(0)),!info.error.empty(),
                smoke_managed_sessions_.contains(id),smoke_bundled_sessions_.contains(id),
                !diagnostics.provider.empty(),diagnostics.provider=="Windows ConPTY (system)",
                diagnostics.bundled_provider,info.shell_context.has_value(),visible,bool(frame),
                static_cast<unsigned long long>(frame?frame->generation:0),
                unsigned(frame?frame->columns:0),unsigned(frame?frame->rows:0),
                frame_has_smoke_marker(frame),diagnostics.queued_input_bytes);
            std::fprintf(stderr,"Native smoke actor control: actor=%llu available=%d",
                static_cast<unsigned long long>(id.value),diagnostics.windows_control.has_value());
            if (diagnostics.windows_control) {
                const auto& control=*diagnostics.windows_control;
                std::fprintf(stderr," resize_requested=%llu resize_applied=%llu requested_columns=%u requested_rows=%u applied_grid_present=%d applied_columns=%u applied_rows=%u wait_calls=%llu control_wakes=%llu child_exit_wakes=%llu initialization_wakes=%llu resize_calls=%llu resize_failures=%llu",
                    static_cast<unsigned long long>(control.requested_resize_generation),
                    static_cast<unsigned long long>(control.applied_resize_generation),
                    unsigned(control.requested_grid.columns),unsigned(control.requested_grid.rows),
                    control.applied_grid.has_value(),unsigned(control.applied_grid?control.applied_grid->columns:0),
                    unsigned(control.applied_grid?control.applied_grid->rows:0),
                    static_cast<unsigned long long>(control.wait_calls),
                    static_cast<unsigned long long>(control.control_wakes),
                    static_cast<unsigned long long>(control.child_exit_wakes),
                    static_cast<unsigned long long>(control.initialization_wakes),
                    static_cast<unsigned long long>(control.resize_calls),
                    static_cast<unsigned long long>(control.resize_failures));
            }
            std::fputc('\n',stderr);
            if (frame) for (std::size_t row=0;row<frame->lines.size() && row<24;++row) {
                std::string text;
                for (const auto& cell:frame->lines[row]->cells) { text+=cell.grapheme.empty()?" ":cell.grapheme; if(text.size()>512)break; }
                while (!text.empty() && text.back()==' ') text.pop_back();
                if (!text.empty()) std::fprintf(stderr,"Native smoke isolated row=%zu text=%s\n",row,text.c_str());
            }
        }
        std::fprintf(stderr,"Native smoke deadline phase=%u phase_elapsed_ms=%llu budget_ms=%llu total_elapsed_ms=%llu\n",
            unsigned(smoke_deadline_.phase()),static_cast<unsigned long long>(smoke_deadline_.phase_elapsed(GetTickCount64())),
            static_cast<unsigned long long>(smoke_deadline_.phase_budget()),GetTickCount64()-smoke_started_at_);
    }
    if(options_.smoke_projects && !error.empty()) {
        const auto found=find_result_;std::fprintf(stderr,"Native Find failure: stage=%u open=%u focused=%llu request=%llu status=%d matches=%zu selected=%lld apply=%d frontend_delta=%llu transport_delta=%llu\n",smoke_find_stage_,unsigned(bool(find_session_)),static_cast<unsigned long long>(focused_session_.value),static_cast<unsigned long long>(find_request_),found?int(found->status):-1,found?found->matches.size():0,found&&found->selected_index?static_cast<long long>(*found->selected_index):-1,found&&found->last_apply_result?int(*found->last_apply_result):-1,static_cast<unsigned long long>(smoke_terminal_input_submissions_-smoke_find_input_baseline_),static_cast<unsigned long long>(active_session()?active_session()->info().bytes_sent-smoke_find_bytes_:0));
    }
            if(options_.smoke_layout&&!error.empty()){const auto* active=active_session();const auto frame=active?active->frame():FramePtr{};std::fprintf(stderr,"Native palette fixture: stage=%u stop_sent=%d mouse_mode=%d mouse_submissions=%llu capture=%d\n",smoke_palette_input_stage_,smoke_palette_disable_sent_,frame&&frame->mouse_reporting,static_cast<unsigned long long>(smoke_terminal_mouse_submissions_),GetCapture()==hwnd_);}
    smoke_finished_=true; exit_code_=error.empty()?0:2;
    if (error.empty()) {
        const auto diagnostics=active_session()?active_session()->diagnostics():ProcessDiagnostics{};
        std::fprintf(stderr,"Native CI smoke passed: real HWND, WARP renderer, ConPTY input/output marker, presented frame, child exit 0. Provider: %s. Not physical visual/IME/Snap QA.\n",diagnostics.provider.c_str());
        if (using_managed_shell()) std::fprintf(stderr,"Managed shell CI smoke: actual upstream Zsh 5.9.2 plus managed preset sentinel, builtin output and exit 0; HOME/CWD retained and child-only environment. Not a StormTTY Zsh fork or completed redistribution qualification.\n");
        if (options_.smoke_settings) std::fputs("Settings/menu CI smoke: async isolated snapshot/discovery, real styled frame, input trapping, midnight/custom ANSI hex and font preview rollback, Ribbon visibility/order Save/reopen, native Appearance/Ribbon captures, custom system menu/outside-click routing, confirmed Alt+], independent 25% opacity and extra blur 24 with WARP fallback, two atomic Save/reopen cycles, original live actor retained, explicit hot reload / repeat / draft refusal / invalid and missing file preservation. Not physical UI/accessibility QA.\n",stderr);
        if (options_.smoke_projects) std::fprintf(stderr,"Workspace CI smoke: two actual folder contexts; final-pane parking, canceled background close, cross-project isolation, live parked output and same retained actor restored after exit; scoped project close. Folder-picker interaction and physical UI remain manual QA.\n");
        if (options_.smoke_modal) std::fprintf(stderr,"Modal CI smoke: real styled confirmation frame, synthetic key trapping/Cancel/Escape/focus restoration and disposable actor close; not physical DWM activation/Snap proof.\n");
        if (options_.smoke_layout) std::fprintf(stderr,"Layout CI smoke: %zu real sessions, %zu tabs, %zu visible panes; settled chrome framebuffer, not desktop blur/corner proof.\n",sessions_.size(),workspace().state().tabs.size(),layout_.panes.size());
    } else std::fprintf(stderr,"Native CI smoke failed: %.*s Settings stage=%u elapsed_ms=%llu job_pending=%d saving=%d\n",
        static_cast<int>(error.size()),error.data(),smoke_settings_stage_,GetTickCount64()-smoke_started_at_,settings_job_,settings_panel_.saving());
    if (hwnd_) destroy_window();
}

void NativeWindow::position_ime() {
    if (!hwnd_ || !renderer_) return;
    POINT position{}; RECT exclusion{};
    if(navigator_open_) {
        const auto bounds=layout_workspace_navigator(width_,height_,navigator_,navigator_focus_).query;const auto visible=navigator_query_view();
        position={static_cast<LONG>((bounds.x+9+visible.second)*scale_),static_cast<LONG>((bounds.y+6)*scale_)};
        exclusion={position.x,position.y,position.x+1,position.y+static_cast<LONG>(24*scale_)};
    } else if(find_session_) {
        const auto bounds=find_layout().query;const auto visible=find_query_view();
        position={static_cast<LONG>((bounds.x+8+visible.second)*scale_),static_cast<LONG>((bounds.y+5)*scale_)};
        exclusion={position.x,position.y,position.x+1,position.y+static_cast<LONG>(20*scale_)};
    } else if (palette_open_ || rename_open_) {
        const auto panel=palette_layout({}).panel;
        ChromeText measure; measure.text=query_; measure.size_dip=16;
        measure.rect={0,0,static_cast<float>(panel.width*scale_),24*static_cast<float>(scale_)};
        const double start=panel.x+(rename_open_?20:46);
        const double caret=std::max(start,std::min(panel.x+panel.width-66,start+renderer_->measure_chrome_text(measure).width/scale_));
        position={static_cast<LONG>(caret*scale_),static_cast<LONG>((panel.y+16)*scale_)};
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
    const bool should_blink=active_ && !minimized_ && !navigator_open_ && !find_session_ && !palette_open_ && !rename_open_ && !modal_ && !overlay_open() && (cursor_blinks || text_blinks) && interval && interval!=INFINITE;
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
        typing_motion_.reset();
        minimized_=wparam==SIZE_MINIMIZED; maximized_=wparam==SIZE_MAXIMIZED;
        if (renderer_ && !minimized_) changed(true);
        refresh_clock(); caret_schedule(); motion_schedule(); return 0;
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
        typing_motion_.reset();
        active_=focused; text_input_.reset(); consumed_keys_.fill(false);
        if (!focused) { find_pressed_.reset(); navigator_switch_pressed_=false;navigator_pressed_target_.reset(); agent_panel_.pointer_cancel(); settings_panel_.pointer_cancel(); context_menu_.cancel(); menu_input_changed(); cancel_drag(); preedit_.clear(); composing_=false; }
        refresh_clock(); caret_schedule(focused); changed(); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; BeginPaint(hwnd_,&paint); EndPaint(hwnd_,&paint); dirty_=true;
        if (in_size_move_ && renderer_ && !minimized_) render();
        return 0;
    }
    case deferred_destroy_message:
        if(destroy_pending_) { destroy_pending_=false; destroy_window(); }
        return 0;
    case deferred_layout_message:
        layout_pending_=false;
        if (settings_pending_) { settings_pending_=false; settings_changed(); }
        changed(true); return 0;
    case smoke_settings_input_message:
        if (options_.smoke_test && options_.smoke_settings && smoke_settings_stage_==8 && smoke_settings_input_stage_==1) {
            if (settings_panel_.is_open() || !focused_session_) { smoke_finish("Settings-close input event did not retain terminal focus."); return 0; }
            smoke_settings_input_stage_=2;
            const auto* actor=active_session(); const auto info=actor?actor->info():SessionInfo{};
            const auto diagnostics=actor?actor->diagnostics():ProcessDiagnostics{};
            const auto frame=actor?actor->frame():FramePtr{};
            std::fprintf(stderr,"Native settings paste event: separate_dispatch=1 closed_scene_presented=1 rx=%llu tx=%llu queued=%zu bracketed=%d resize_requested=%llu resize_applied=%llu\n",
                static_cast<unsigned long long>(info.bytes_received),static_cast<unsigned long long>(info.bytes_sent),diagnostics.queued_input_bytes,frame&&frame->bracketed_paste,
                static_cast<unsigned long long>(diagnostics.windows_control?diagnostics.windows_control->requested_resize_generation:0),
                static_cast<unsigned long long>(diagnostics.windows_control?diagnostics.windows_control->applied_resize_generation:0));
            smoke_progress();
        }
        return 0;
    case wake_message:
        wake_->posted.store(false,std::memory_order_release); update_sessions(); InvalidateRect(hwnd_,nullptr,FALSE); return 0;
    case WM_TIMECHANGE: refresh_clock(); return 0;
    case WM_TIMER:
        if (wparam==workspace_save_timer) { KillTimer(hwnd_,workspace_save_timer); workspace_timer_=false; if (workspace_save_signal_.take_pending()) save_workspaces(); return 0; }
        if (wparam==local_clock_timer) { refresh_clock(); return 0; }
        if (wparam==typing_smoke_timer) { dirty_=true; InvalidateRect(hwnd_,nullptr,FALSE); return 0; }
        if (wparam==typing_motion_timer) {
            if (!typing_motion_.active(GetTickCount64())) { KillTimer(hwnd_,typing_motion_timer); typing_timer_active_=false; }
            dirty_=true; InvalidateRect(hwnd_,nullptr,FALSE); return 0;
        }
        if (wparam==material_probe_timer) {poll_material_probe();return 0;}
        if (wparam==smoke_timeout_timer) {
            const auto now=GetTickCount64();
            if (smoke_deadline_.expired(now)) smoke_finish("Native smoke phase deadline expired before real output, presentation and clean exit.");
            else if(!options_.smoke_workspaces.empty() && !smoke_finished_) {
                // Hidden actor readiness can change without changing the visible
                // scene. A normal unchanged render must not strand this explicit
                // test's presented-frame state machine. Repaint on the existing
                // bounded 250ms smoke timer; never poll/render recursively.
                force_redraw_=true; changed();
                if(now-workspace_smoke_diagnostic_at_>=1000) {
                    workspace_smoke_diagnostic_at_=now;
                    std::fprintf(stderr,"Workspace smoke progress: stage=%s seeded=%d menu=%d actors=%zu elapsed_ms=%llu\n",
                        options_.smoke_workspaces.c_str(),workspace_smoke_seeded_,workspace_smoke_menu_,sessions_.size(),now-smoke_started_at_);
                    for(const auto& [id,actor]:sessions_) {
                        const auto info=actor->info();
                        std::fprintf(stderr,"Workspace smoke actor: id=%llu state=%d rx=%llu frame=%d\n",
                            static_cast<unsigned long long>(id.value),static_cast<int>(info.state),
                            static_cast<unsigned long long>(info.bytes_received),static_cast<bool>(actor->frame()));
                    }
                }
            }
            return 0;
        }
        if (wparam==chrome_motion_timer) { motion_schedule(); ++chrome_generation_; dirty_=true; InvalidateRect(hwnd_,nullptr,FALSE); return 0; }
        if (wparam==cursor_timer && cursor_blink_.tick(GetTickCount64())) { dirty_=true; InvalidateRect(hwnd_,nullptr,FALSE); } return 0;
    case WM_CHAR: case WM_SYSCHAR: {
        if (settings_input_blocked()) return 0;
        if (message==WM_SYSCHAR && wparam==L' ' && (lparam & (1LL<<29))) return 0;
        if (composing_) return 0;
        const auto unit=static_cast<char16_t>(wparam);
        if ((agent_panel_.is_open() || palette_open_ || rename_open_ || context_menu_.is_open()) && unit<0x20) return 0;
        terminal_text(text_input_.feed(unit),physical_key(text_vk_,text_flags_),text_modifiers_); return 0;
    }
    case WM_UNICHAR:
        if (wparam==UNICODE_NOCHAR) return TRUE;
        { std::string text; Utf16Input::append(text,static_cast<std::uint32_t>(wparam)); terminal_text(std::move(text)); } return 0;
    case WM_IME_SETCONTEXT:
        if (overlay_open()) return 0;
        return DefWindowProcW(hwnd_,message,wparam,lparam & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
    case WM_IME_STARTCOMPOSITION:
        typing_motion_.reset();
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
        if (navigator_open_ || palette_open_ || rename_open_ || modal_ || overlay_open()) return 0;
        const auto p=point(lparam);
        for (const auto& tab:chrome_.tabs) if (tab.bounds.contains(p)) { if (const auto* model=workspace().tab(tab.tab)) activate(model->active_pane); return 0; }
        if (navigation_.rail.contains(p)) { for (const auto& row:navigation_.projects) if (row.bounds.contains(p)) { activate_project(row.project); break; } return 0; }
        if (workspace_chrome_contains(p)) return 0;
        for (const auto& placement:layout_.panes) if (placement.bounds.contains(p)) { const auto pane=placement.pane; activate(pane); break; }
        if (!(GetKeyState(VK_SHIFT)&0x8000)) terminal_mouse(p,MouseAction::press,MouseButton::right,true);
        return 0;
    }
    case WM_RBUTTONUP: {
        if (navigator_open_ || palette_open_ || rename_open_ || modal_ || overlay_open()) return 0;
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
        if (navigator_open_ || palette_open_ || rename_open_ || modal_ || overlay_open()) return 0;
        for (const auto& tab:chrome_.tabs) if (tab.bounds.contains(point(lparam))) { close_tab(tab.tab); return 0; }
        terminal_mouse(point(lparam),MouseAction::press,MouseButton::middle,true); return 0;
    case WM_MBUTTONUP: if (navigator_open_ || palette_open_ || rename_open_ || overlay_open()) return 0; terminal_mouse(point(lparam),MouseAction::release,MouseButton::middle,false); return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        if(palette_open_||rename_open_)return 0;
        if(navigator_open_){const auto nav=layout_workspace_navigator(width_,height_,navigator_,navigator_focus_);if(!nav.panel.contains(point(lparam,true)))return 0;const bool left=nav.workspace_body.contains(point(lparam,true));const int delta=GET_WHEEL_DELTA_WPARAM(wparam);for(int i=0;i<std::min(12,std::abs(delta)/WHEEL_DELTA);++i)navigator_.move(left,delta>0?-1:1);changed();return 0;}

        if(agent_panel_.is_open()) {agent_panel_.scroll(static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam))/WHEEL_DELTA*3);agent_input_changed();return 0;}
        if (settings_loading()) return 0;
        if (settings_input_blocked()) {
            // A horizontal wheel belongs to this modal too, but is not a
            // vertical gesture. Do not leak it to the terminal underneath.
            if (message==WM_MOUSEWHEEL) {
                UINT lines=3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
                settings_wheel(point(lparam,true),GET_WHEEL_DELTA_WPARAM(wparam),lines);
            }
            return 0;
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
        find_pressed_.reset();navigator_switch_pressed_=false;navigator_pressed_target_.reset();
        caption_pressed_=0; modal_pressed_.reset();
        agent_panel_.pointer_cancel(); settings_panel_.pointer_cancel(); context_menu_.pointer_cancel(); overlay_pointer_sequence_=false;
        if (overlay_open()) changed();
        if (mouse_capture_pane_) terminal_mouse(hover_,MouseAction::release,mouse_capture_button_,false);
        mouse_capture_pane_={}; mouse_capture_button_=MouseButton::none;
        if (drag_!=Drag::none) cancel_drag(); return 0;
    case WM_CANCELMODE: find_pressed_.reset(); navigator_switch_pressed_=false;navigator_pressed_target_.reset(); agent_panel_.pointer_cancel(); settings_panel_.pointer_cancel(); context_menu_.cancel(); menu_input_changed(); cancel_drag(); return 0;
    case WM_CONTEXTMENU:
        if (navigator_open_ || palette_open_ || rename_open_ || modal_ || overlay_open()) return 0;
        if (GET_X_LPARAM(lparam)==-1 && GET_Y_LPARAM(lparam)==-1) popup_menu({width_/2,chrome_.band.height+12},true);
        else popup_menu(point(lparam,true),true);
        return 0;
    case WM_CLOSE: {
        if(agent_panel_.is_open()) {agent_panel_.close();agent_input_changed();return 0;}
        if (settings_loading()) { settings_open_requested_=false; settings_binding_.clear(); notice_.clear(); caret_schedule(true); changed(); return 0; }
        if (context_menu_.is_open()) { context_menu_.cancel(); menu_input_changed(); }
        if (settings_input_blocked()) { settings_panel_.cancel(); settings_input_changed(); return 0; }
        const bool live=std::any_of(sessions_.begin(),sessions_.end(),[](const auto& pair) { return !pair.second->process_finished(); });
        if (modal_) { answer_modal(ModalDecision::cancel); return 0; }
        if (!live && !(agent_controller_&&agent_controller_->any_active())) destroy_window();
        else show_modal("Close StormTTY?",{"Its terminals or agents may have running work.","Closing disconnects agents and can discard unsaved work."},"Close window",[this]{if (hwnd_) destroy_window();});
        return 0;
    }
    case WM_DESTROY:
        wake_->window.store(nullptr,std::memory_order_release);
        KillTimer(hwnd_,local_clock_timer);
        KillTimer(hwnd_,workspace_save_timer); workspace_timer_=false;
        KillTimer(hwnd_,typing_smoke_timer); KillTimer(hwnd_,typing_motion_timer); KillTimer(hwnd_,cursor_timer); KillTimer(hwnd_,smoke_timeout_timer); KillTimer(hwnd_,chrome_motion_timer); KillTimer(hwnd_,material_probe_timer);
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
