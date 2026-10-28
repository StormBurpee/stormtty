// Native Windows UI test. A cross-build/link is not a runtime or visual pass.
// No renderer, Ghostty, COM initialization, hooks, or external input injection.
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
#include "../src/platform/windows/startup_notice.hpp"
#include <cstdio>
#include <string>

namespace {
constexpr int body_id = 101, close_id = 102, chrome_close_id = 103;
struct Scenario {
    const char* failure{};
    int mode{}, ticks{}, phase{};
    bool seen{}, long_message{};
    UINT_PTR timer{};
    HWND window{}, body{}, button{};
    DWORD thread = GetCurrentThreadId();
    void check(bool value, const char* message) noexcept { if (!value && !failure) failure = message; }
};
Scenario* active{};
BOOL CALLBACK find_notice(HWND hwnd, LPARAM parameter) noexcept {
    wchar_t name[64]{};
    GetClassNameW(hwnd, name, 64);
    if (lstrcmpW(name, L"StormTTYStartupNotice") == 0) {
        reinterpret_cast<Scenario*>(parameter)->window = hwnd;
        return FALSE;
    }
    return TRUE;
}
bool child_inside(HWND parent, HWND child) noexcept {
    RECT bounds{}, rect{}; GetClientRect(parent, &bounds); GetWindowRect(child, &rect);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect.left >= 0 && rect.top >= 0 && rect.right <= bounds.right && rect.bottom <= bounds.bottom &&
        rect.right > rect.left && rect.bottom > rect.top;
}
void CALLBACK inspect_notice(HWND, UINT, UINT_PTR, DWORD) noexcept {
    if (!active) return;
    auto& test = *active;
    if (++test.ticks > 50) {
        test.check(false, "Notice did not finish within bounded native test timer");
        if (test.window && IsWindow(test.window)) PostMessageW(test.window, WM_CLOSE, 0, 0);
        else PostQuitMessage(1);
        return;
    }
    if (!test.window) EnumThreadWindows(test.thread, find_notice, reinterpret_cast<LPARAM>(&test));
    if (!test.window || !IsWindowVisible(test.window)) return;
    HWND window = test.window;
    if (test.phase == 0) {
        test.seen = true;
        const auto style = GetWindowLongPtrW(window, GWL_STYLE);
        test.check((style & (WS_CAPTION | WS_DLGFRAME | WS_SYSMENU)) == 0, "Notice has native titlebar/dialog chrome");
        test.body = GetDlgItem(window, body_id); test.button = GetDlgItem(window, close_id);
        test.check(test.body && test.button && GetDlgItem(window, chrome_close_id), "Styled notice controls missing");
        const auto body_style = GetWindowLongPtrW(test.body, GWL_STYLE);
        test.check((body_style & (ES_READONLY | ES_MULTILINE | WS_TABSTOP)) == (ES_READONLY | ES_MULTILINE | WS_TABSTOP),
                   "Notice body is not selectable read-only multiline text");
        test.check((body_style & (WS_VSCROLL | WS_HSCROLL | WS_BORDER)) == 0, "Notice body has native scrollbars/border");
        test.check((GetWindowLongPtrW(test.button, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW, "Close button is not styled");
        test.check(GetFocus() == test.button, "Safe Close button was not initially focused");
        test.check(GetWindowTextLengthW(test.body) > 0, "Notice body text missing");
        test.check(child_inside(window, test.body) && child_inside(window, test.button), "Initial notice layout clips its controls");

        // Preserve real Win32 resize/DPI lifecycle coverage without changing
        // system settings. This synthetic DPI message tests our handler only.
        RECT outer{}; GetWindowRect(window, &outer);
        SetWindowPos(window, nullptr, 0, 0, 540, 360, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        test.check(child_inside(window, test.body) && child_inside(window, test.button), "Resize clips controls");
        RECT suggested{outer.left, outer.top, outer.left + 810, outer.top + 540};
        SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested));
        test.check(child_inside(window, test.body) && child_inside(window, test.button), "DPI transition clips controls");
        LOGFONTW font{};
        HFONT body_font = reinterpret_cast<HFONT>(SendMessageW(test.body, WM_GETFONT, 0, 0));
        test.check(GetObjectW(body_font, sizeof(font), &font) != 0 && font.lfHeight == -24, "DPI transition did not scale body font");
        if (test.long_message) {
            test.check(GetWindowTextLengthW(test.body) < 530000, "Long message is not bounded");
            test.check(SendMessageW(test.body, EM_GETLINECOUNT, 0, 0) > 100, "Long message lacks scrollable lines");
            SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
            UINT wheel_lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheel_lines, 0);
            test.check(wheel_lines == 0 || SendMessageW(test.body, EM_GETFIRSTVISIBLELINE, 0, 0) > 0,
                       "Custom mousewheel scrolling did not move text");
        }
        PostMessageW(test.button, WM_KEYDOWN, VK_TAB, 0);
        test.phase = 1;
        return;
    }
    if (test.phase == 1) {
        test.check(GetFocus() == GetDlgItem(window, chrome_close_id), "Tab did not reach accessible chrome Close button");
        PostMessageW(GetFocus(), WM_KEYDOWN, VK_TAB, 0);
        test.phase = 2;
        return;
    }
    if (test.phase == 2) {
        test.check(GetFocus() == test.body, "Tab did not wrap into selectable message body");
        if (test.mode == 0) PostMessageW(test.body, WM_KEYDOWN, VK_ESCAPE, 0);
        else if (test.mode == 1) PostMessageW(test.body, WM_KEYDOWN, VK_RETURN, 0);
        else if (test.mode == 2) SendMessageW(test.button, BM_CLICK, 0, 0);
        else if (test.mode == 3) SendMessageW(GetDlgItem(window, chrome_close_id), BM_CLICK, 0, 0);
        else if (test.mode == 4) PostQuitMessage(37);
        else PostMessageW(window, WM_SYSKEYDOWN, VK_F4, 0);
        test.phase = 3;
    }
}
}

int main() {
    // Deliberately do not initialize COM or a graphics device.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    for (int mode = 0; mode < 6; ++mode) {
        Scenario test; test.mode = mode; test.long_message = mode == 2;
        std::string body = "Readable startup notice.\nSelect text and copy it with Ctrl+C.\nUnicode: \xe2\x9c\x93";
        if (test.long_message) {
            body.clear(); body.reserve(300000);
            for (int i = 0; i < 10000; ++i) body += "A bounded long startup message, line " + std::to_string(i) + "\n";
        }
        active = &test;
        test.timer = SetTimer(nullptr, 0, 50, inspect_notice);
        if (!test.timer) { std::fprintf(stderr, "Could not create native UI test timer.\n"); return 1; }
        const int result = stormtty::show_startup_notice("Startup notice native test", body,
            mode == 1 ? stormtty::StartupNoticeKind::error : stormtty::StartupNoticeKind::information);
        KillTimer(nullptr, test.timer); active = nullptr;
        test.check(test.seen && test.phase == 3, "Notice closed before native lifecycle checks ran");
        test.check(result == (mode == 4 ? -1 : 0), "Notice returned the wrong dismissal status");
        test.check(!IsWindow(test.window), "Notice leaked its top-level window");
        WNDCLASSEXW remaining{}; remaining.cbSize = sizeof(remaining);
        test.check(!GetClassInfoExW(GetModuleHandleW(nullptr), L"StormTTYStartupNotice", &remaining),
                   "Notice leaked its window class registration");
        MSG quit{};
        const bool has_quit = PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE;
        if (mode == 4) test.check(has_quit && quit.wParam == 37, "Notice swallowed the caller's WM_QUIT");
        else test.check(!has_quit, "Notice unexpectedly posted WM_QUIT");
        if (test.failure) { std::fprintf(stderr, "startup_notice_tests mode %d: %s\n", mode, test.failure); return 1; }
    }
    std::puts("startup_notice_tests: native lifecycle, text bounds, resize/DPI, scroll, keyboard and dismissal passed");
    return 0;
}
