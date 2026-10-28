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
#include "startup_notice.hpp"
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

namespace stormtty {
namespace {
constexpr wchar_t notice_class[] = L"StormTTYStartupNotice";
constexpr int body_id = 101, close_id = 102, chrome_close_id = 103, title_id = 104;
constexpr std::size_t body_byte_limit = 256 * 1024;
constexpr COLORREF background = RGB(0x11, 0x15, 0x1b);
constexpr COLORREF surface = RGB(0x18, 0x1d, 0x24);
constexpr COLORREF foreground = RGB(0xee, 0xed, 0xec);
constexpr COLORREF muted = RGB(0xaa, 0xb4, 0xc5);
constexpr COLORREF accent = RGB(0x8c, 0x9e, 0xff);
constexpr COLORREF border = RGB(0x38, 0x43, 0x55);

template<class T> class GdiObject {
public:
    explicit GdiObject(T value = nullptr) noexcept : value_(value) {}
    ~GdiObject() { if (value_) DeleteObject(value_); }
    GdiObject(const GdiObject&) = delete;
    GdiObject& operator=(const GdiObject&) = delete;
    T get() const noexcept { return value_; }
    void reset(T value) noexcept { if (value_) DeleteObject(value_); value_ = value; }
private:
    T value_;
};

// Bounded conversion also normalizes line endings for the standard EDIT control.
// Windows replaces malformed UTF-8 rather than throwing on a startup error path.
std::wstring display_text(std::string_view text, std::size_t limit, bool multiline) {
    const bool truncated = text.size() > limit;
    text = text.substr(0, limit);
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring decoded(static_cast<std::size_t>(std::max(0, count)), L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), decoded.data(), count);
    std::wstring result;
    result.reserve(decoded.size() + decoded.size() / 16);
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        const wchar_t c = decoded[i];
        if (c == L'\r' || c == L'\n') {
            if (c == L'\r' && i + 1 < decoded.size() && decoded[i + 1] == L'\n') ++i;
            result += multiline ? L"\r\n" : L" ";
        } else if (c < L' ' && c != L'\t') result += L'\ufffd';
        else result += c;
    }
    if (truncated) result += multiline ? L"\r\n\r\n[Message truncated to 256 KiB.]" : L"\u2026";
    return result;
}

void fill(HDC dc, RECT rect, COLORREF color) noexcept {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

class Notice {
public:
    Notice(std::wstring title, std::wstring body, StartupNoticeKind kind)
        : title_(std::move(title)), body_text_(std::move(body)), kind_(kind),
          background_brush_(CreateSolidBrush(background)), surface_brush_(CreateSolidBrush(surface)) {}
    ~Notice() {
        if (hwnd_) DestroyWindow(hwnd_);
        if (registered_) UnregisterClassW(notice_class, GetModuleHandleW(nullptr));
    }
    Notice(const Notice&) = delete;
    Notice& operator=(const Notice&) = delete;

    int run() noexcept {
        if (!background_brush_.get() || !surface_brush_.get()) return -1;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc); wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = dispatch; wc.lpszClassName = notice_class;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (RegisterClassExW(&wc)) registered_ = true;
        else {
            WNDCLASSEXW existing{}; existing.cbSize = sizeof(existing);
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS ||
                !GetClassInfoExW(wc.hInstance, notice_class, &existing) || existing.lpfnWndProc != dispatch) return -1;
        }
        // No WS_CAPTION, WS_SYSMENU, dialog template, or standard titlebar ever
        // exists, including the initial paint before WM_NCCALCSIZE is delivered.
        HWND created = CreateWindowExW(WS_EX_APPWINDOW, notice_class, title_.c_str(),
            WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
            640, 440, nullptr, nullptr, wc.hInstance, this);
        if (!created) return -1;
        hwnd_ = created;
        dpi_ = GetDpiForWindow(hwnd_); if (!dpi_) dpi_ = 96;
        if (!create_controls() || !set_fonts()) return -1;
        MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &monitor)) return -1;
        const RECT work = monitor.rcWork;
        const int width = std::min(scale(680), static_cast<int>(work.right - work.left));
        const int height = std::min(scale(460), static_cast<int>(work.bottom - work.top));
        SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - width) / 2,
                     work.top + (work.bottom - work.top - height) / 2, width, height,
                     SWP_NOZORDER | SWP_FRAMECHANGED);
        layout();
        ShowWindow(hwnd_, SW_SHOWNORMAL); UpdateWindow(hwnd_); SetFocus(close_);
        MSG message{};
        while (hwnd_) {
            const BOOL status = GetMessageW(&message, nullptr, 0, 0);
            if (status == -1) return -1;
            if (status == 0) { PostQuitMessage(static_cast<int>(message.wParam)); return -1; }
            const bool ours = message.hwnd == hwnd_ || IsChild(hwnd_, message.hwnd);
            if (ours && message.message == WM_KEYDOWN) {
                if (message.wParam == VK_ESCAPE || message.wParam == VK_RETURN) {
                    DestroyWindow(hwnd_); continue;
                }
                if (message.wParam == VK_TAB) {
                    const bool backwards = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    HWND next = GetNextDlgTabItem(hwnd_, GetFocus(), backwards);
                    if (next) SetFocus(next);
                    continue;
                }
            }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return failed_ ? -1 : 0;
    }

private:
    int scale(int value) const noexcept { return MulDiv(value, static_cast<int>(dpi_), 96); }
    int visible_lines() const noexcept {
        RECT rect{}; if (body_) SendMessageW(body_, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&rect));
        return std::max(1, static_cast<int>(rect.bottom - rect.top) / std::max(1, line_height_));
    }
    int line_count() const noexcept { return body_ ? static_cast<int>(SendMessageW(body_, EM_GETLINECOUNT, 0, 0)) : 1; }
    int first_line() const noexcept { return body_ ? static_cast<int>(SendMessageW(body_, EM_GETFIRSTVISIBLELINE, 0, 0)) : 0; }
    int last_line() const noexcept { return std::max(0, line_count() - visible_lines()); }
    void scroll_to(int line) noexcept {
        line = std::clamp(line, 0, last_line());
        SendMessageW(body_, EM_LINESCROLL, 0, line - first_line());
        InvalidateRect(hwnd_, &scroll_rail_, FALSE);
    }
    RECT thumb_rect() const noexcept {
        RECT thumb = scroll_rail_;
        if (!last_line()) return {};
        const int track = static_cast<int>(scroll_rail_.bottom - scroll_rail_.top);
        const int height = std::min(track, std::max(scale(28), MulDiv(track, visible_lines(), line_count())));
        thumb.top += MulDiv(track - height, std::min(first_line(), last_line()), last_line());
        thumb.bottom = thumb.top + height;
        thumb.left += scale(3); thumb.right -= scale(3);
        return thumb;
    }
    void scroll_wheel(WPARAM wparam) noexcept {
        wheel_remainder_ += GET_WHEEL_DELTA_WPARAM(wparam);
        const int steps = wheel_remainder_ / WHEEL_DELTA;
        wheel_remainder_ %= WHEEL_DELTA;
        UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const int increment = lines == WHEEL_PAGESCROLL ? visible_lines() : static_cast<int>(std::min(lines, UINT(100)));
        scroll_to(first_line() - steps * increment);
    }
    bool create_controls() noexcept {
        const auto instance = GetModuleHandleW(nullptr);
        title_control_ = CreateWindowExW(0, L"STATIC", title_.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
            0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(title_id), instance, nullptr);
        // An actual read-only EDIT retains text selection, Ctrl+C, and the
        // platform's accessibility provider; only its presentation is replaced.
        body_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL,
            0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(body_id), instance, nullptr);
        close_ = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(close_id), instance, nullptr);
        chrome_close_ = CreateWindowExW(0, L"BUTTON", L"Close window", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(chrome_close_id), instance, nullptr);
        if (!title_control_ || !body_ || !close_ || !chrome_close_) return false;
        SendMessageW(body_, EM_SETLIMITTEXT, body_byte_limit * 2, 0);
        SetWindowLongPtrW(body_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        original_edit_proc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(body_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(edit_dispatch)));
        if (!original_edit_proc_) return false;
        return SetWindowTextW(body_, body_text_.c_str()) != FALSE;
    }
    bool set_fonts() noexcept {
        HFONT body_font = CreateFontW(-scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT heading_font = CreateFontW(-scale(23), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!body_font || !heading_font) {
            if (body_font) DeleteObject(body_font);
            if (heading_font) DeleteObject(heading_font);
            return false;
        }
        // Controls stop borrowing the old fonts before those handles are freed.
        for (HWND control : {body_, close_, chrome_close_}) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(body_font), TRUE);
        SendMessageW(title_control_, WM_SETFONT, reinterpret_cast<WPARAM>(heading_font), TRUE);
        body_font_.reset(body_font); heading_font_.reset(heading_font);
        HDC dc = GetDC(hwnd_);
        if (!dc) return false;
        const auto old = SelectObject(dc, body_font_.get()); TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics)) line_height_ = metrics.tmHeight;
        else line_height_ = scale(20);
        SelectObject(dc, old); ReleaseDC(hwnd_, dc);
        return true;
    }
    void layout() noexcept {
        if (!body_) return;
        RECT client{}; GetClientRect(hwnd_, &client);
        const int width = client.right, height = client.bottom;
        const int margin = scale(28), body_top = scale(126), body_bottom = std::max(body_top + scale(24), height - scale(88));
        MoveWindow(title_control_, margin, scale(77), std::max(1, width - margin * 2), scale(34), TRUE);
        MoveWindow(body_, margin + scale(12), body_top + scale(12), std::max(1, width - margin * 2 - scale(40)),
                   std::max(1, body_bottom - body_top - scale(24)), TRUE);
        SendMessageW(body_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        MoveWindow(close_, std::max(margin, width - margin - scale(100)), height - scale(62), scale(100), scale(36), TRUE);
        MoveWindow(chrome_close_, width - scale(48), scale(6), scale(36), scale(32), TRUE);
        scroll_rail_ = {width - margin - scale(22), body_top + scale(12), width - margin - scale(8), body_bottom - scale(12)};
        body_panel_ = {margin, body_top, width - margin, body_bottom};
        scroll_to(first_line()); InvalidateRect(hwnd_, nullptr, TRUE);
    }
    void paint() noexcept {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd_, &paint);
        if (!dc) return;
        RECT rect{}; GetClientRect(hwnd_, &rect);
        fill(dc, rect, background);
        RECT header{1, 1, rect.right - 1, scale(44)}; fill(dc, header, surface);
        fill(dc, {0, 0, rect.right, 1}, accent);
        fill(dc, {0, rect.bottom - 1, rect.right, rect.bottom}, border);
        fill(dc, {0, 1, 1, rect.bottom}, border);
        fill(dc, {rect.right - 1, 1, rect.right, rect.bottom}, border);
        fill(dc, {1, scale(44), rect.right - 1, scale(45)}, border);
        fill(dc, body_panel_, surface);
        SetBkMode(dc, TRANSPARENT); const auto old = SelectObject(dc, body_font_.get());
        SetTextColor(dc, foreground);
        RECT brand{scale(28), scale(8), rect.right - scale(64), scale(36)};
        DrawTextW(dc, L"StormTTY", -1, &brand, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SetTextColor(dc, kind_ == StartupNoticeKind::error ? RGB(0xff, 0xb7, 0xae) : accent);
        RECT label{scale(28), scale(53), rect.right - scale(28), scale(75)};
        DrawTextW(dc, kind_ == StartupNoticeKind::error ? L"STARTUP ERROR" : L"APPLICATION HELP", -1,
                  &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SetTextColor(dc, muted);
        RECT hint{scale(28), rect.bottom - scale(60), rect.right - scale(160), rect.bottom - scale(28)};
        DrawTextW(dc, L"Esc to close  \u00b7  Ctrl+C to copy selected text", -1, &hint,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (last_line()) {
            fill(dc, scroll_rail_, background);
            fill(dc, thumb_rect(), dragging_scroll_ ? accent : border);
        }
        SelectObject(dc, old); EndPaint(hwnd_, &paint);
    }
    void draw_button(const DRAWITEMSTRUCT& item) noexcept {
        const bool chrome = item.CtlID == chrome_close_id;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        fill(item.hDC, item.rcItem, chrome ? (pressed ? border : surface) : (pressed ? RGB(0x73, 0x86, 0xe8) : accent));
        SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, chrome ? foreground : background);
        const auto old = SelectObject(item.hDC, body_font_.get());
        RECT label = item.rcItem;
        if (chrome) {
            GdiObject<HPEN> pen(CreatePen(PS_SOLID, std::max(1, scale(1)), foreground));
            const auto old_pen = SelectObject(item.hDC, pen.get());
            const int x = (label.left + label.right) / 2, y = (label.top + label.bottom) / 2, d = scale(5);
            MoveToEx(item.hDC, x - d, y - d, nullptr); LineTo(item.hDC, x + d + 1, y + d + 1);
            MoveToEx(item.hDC, x + d, y - d, nullptr); LineTo(item.hDC, x - d - 1, y + d + 1);
            SelectObject(item.hDC, old_pen);
        } else DrawTextW(item.hDC, L"Close", -1, &label, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        if (item.itemState & ODS_FOCUS) {
            InflateRect(&label, -scale(3), -scale(3));
            SetDCBrushColor(item.hDC, chrome ? accent : background);
            FrameRect(item.hDC, &label, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
        SelectObject(item.hDC, old);
    }
    LRESULT hit_test(LPARAM lparam) const noexcept {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd_, &point);
        RECT rect{}; GetClientRect(hwnd_, &rect);
        const int edge = scale(7);
        const bool left = point.x < edge, right = point.x >= rect.right - edge;
        const bool top = point.y < edge, bottom = point.y >= rect.bottom - edge;
        if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (point.y < scale(44) && point.x < rect.right - scale(54)) return HTCAPTION;
        return HTCLIENT;
    }
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        switch (message) {
        case WM_NCCALCSIZE: return 0;
        case WM_NCPAINT: return 0;
        case WM_NCACTIVATE: return TRUE;
        case WM_NCLBUTTONDBLCLK:
            // This is a dismiss-only notice, not a maximizable app window.
            if (wparam == HTCAPTION) return 0;
            break;
        case WM_NCHITTEST: return hit_test(lparam);
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_SIZE: layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
            const bool found = GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
            info->ptMinTrackSize.x = found ? std::min(LONG(scale(420)), monitor.rcWork.right - monitor.rcWork.left) : scale(420);
            info->ptMinTrackSize.y = found ? std::min(LONG(scale(300)), monitor.rcWork.bottom - monitor.rcWork.top) : scale(300);
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam); if (!dpi_) dpi_ = 96;
            if (body_ && !set_fonts()) { failed_ = true; DestroyWindow(hwnd_); return 0; }
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout(); return 0;
        }
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            const bool body = reinterpret_cast<HWND>(lparam) == body_;
            SetTextColor(dc, foreground); SetBkColor(dc, body ? surface : background);
            return reinterpret_cast<LRESULT>(body ? surface_brush_.get() : background_brush_.get());
        }
        case WM_DRAWITEM: draw_button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam)); return TRUE;
        case WM_COMMAND:
            if ((LOWORD(wparam) == close_id || LOWORD(wparam) == chrome_close_id) && HIWORD(wparam) == BN_CLICKED) {
                DestroyWindow(hwnd_); return 0;
            }
            if (LOWORD(wparam) == body_id) InvalidateRect(hwnd_, &scroll_rail_, FALSE);
            break;
        case WM_MOUSEWHEEL: scroll_wheel(wparam); return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (last_line() && PtInRect(&scroll_rail_, point)) {
                RECT thumb = thumb_rect();
                if (PtInRect(&thumb, point)) {
                    dragging_scroll_ = true; scroll_drag_offset_ = point.y - thumb.top; SetCapture(hwnd_);
                } else scroll_to(first_line() + (point.y < thumb.top ? -visible_lines() : visible_lines()));
                InvalidateRect(hwnd_, &scroll_rail_, FALSE); return 0;
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (dragging_scroll_) {
                const RECT thumb = thumb_rect();
                const int range = static_cast<int>((scroll_rail_.bottom - scroll_rail_.top) - (thumb.bottom - thumb.top));
                if (range > 0) scroll_to(MulDiv(GET_Y_LPARAM(lparam) - scroll_rail_.top - scroll_drag_offset_, last_line(), range));
                return 0;
            }
            break;
        case WM_LBUTTONUP: if (dragging_scroll_) ReleaseCapture(); break;
        case WM_CAPTURECHANGED: dragging_scroll_ = false; InvalidateRect(hwnd_, &scroll_rail_, FALSE); return 0;
        case WM_SYSKEYDOWN:
            if (wparam == VK_F4) { DestroyWindow(hwnd_); return 0; }
            break;
        case WM_CLOSE: DestroyWindow(hwnd_); return 0;
        case WM_NCDESTROY: {
            const HWND destroyed = hwnd_; hwnd_ = nullptr;
            SetWindowLongPtrW(destroyed, GWLP_USERDATA, 0);
            return DefWindowProcW(destroyed, message, wparam, lparam);
        }
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }
    static LRESULT CALLBACK dispatch(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto* self = reinterpret_cast<Notice*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Notice*>(reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }
    static LRESULT CALLBACK edit_dispatch(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto* self = reinterpret_cast<Notice*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self || !self->original_edit_proc_) return DefWindowProcW(hwnd, message, wparam, lparam);
        // Avoid the EDIT class's stock popup menu; selection + Ctrl+C remain.
        if (message == WM_CONTEXTMENU) return 0;
        if (message == WM_MOUSEWHEEL) { self->scroll_wheel(wparam); return 0; }
        if (message == WM_KEYDOWN && wparam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0;
        }
        const LRESULT result = CallWindowProcW(self->original_edit_proc_, hwnd, message, wparam, lparam);
        if (message == WM_KEYDOWN || message == WM_LBUTTONUP || message == EM_LINESCROLL || message == EM_SCROLLCARET)
            InvalidateRect(self->hwnd_, &self->scroll_rail_, FALSE);
        return result;
    }

    std::wstring title_, body_text_;
    StartupNoticeKind kind_;
    GdiObject<HBRUSH> background_brush_, surface_brush_;
    GdiObject<HFONT> body_font_, heading_font_;
    HWND hwnd_{}, title_control_{}, body_{}, close_{}, chrome_close_{};
    WNDPROC original_edit_proc_{};
    UINT dpi_ = 96;
    int line_height_ = 20, wheel_remainder_{}, scroll_drag_offset_{};
    RECT scroll_rail_{}, body_panel_{};
    bool registered_{}, failed_{}, dragging_scroll_{};
};
} // namespace

int show_startup_notice(std::string_view title_utf8, std::string_view body_utf8, StartupNoticeKind kind) noexcept {
    try {
        auto title = display_text(title_utf8, 512, false);
        if (title.empty()) title = L"StormTTY";
        Notice notice(std::move(title), display_text(body_utf8, body_byte_limit, true), kind);
        const int result = notice.run();
        if (result == 0) return 0;
    } catch (...) {
        // Startup failures can include allocation/graphics/COM failures. Do not
        // recurse into another UI, and never let an exception cross a callback.
    }
    std::fprintf(stderr, "StormTTY could not show its startup notice.\n%.*s\n%.*s\n",
        static_cast<int>(std::min(title_utf8.size(), std::size_t(512))), title_utf8.empty() ? "" : title_utf8.data(),
        static_cast<int>(std::min(body_utf8.size(), body_byte_limit)), body_utf8.empty() ? "" : body_utf8.data());
    return -1;
}

} // namespace stormtty
