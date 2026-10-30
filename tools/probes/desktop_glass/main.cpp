// Opt-in source feasibility harness. No app linkage, capture on launch, recording or uploads.
#include "capture_source.hpp"
#include "preview_renderer.hpp"
#include <winrt/Windows.Foundation.h>
#include <algorithm>
#include <cwchar>
#include <cstdio>
#include <stdexcept>
#include <memory>
#include <string>

namespace stormtty::glass_probe {
namespace {
std::wstring widen(const char* message) {
    const int length=MultiByteToWideChar(CP_UTF8,0,message,-1,nullptr,0);
    if(length<=0) return L"Unknown error";
    std::wstring result(std::size_t(length),L'\0');
    MultiByteToWideChar(CP_UTF8,0,message,-1,result.data(),length); result.pop_back(); return result;
}
// Separate authored source window. No capture/display-affinity APIs are called here.
class PatternWindow final {
public:
    static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) noexcept {
        if(message==WM_CREATE) { SetTimer(hwnd,1,100,nullptr); return 0; }
        if(message==WM_PAINT) {
            PAINTSTRUCT paint{}; HDC dc=BeginPaint(hwnd,&paint); RECT bounds{}; GetClientRect(hwnd,&bounds);
            const auto tick=GetTickCount64()/100;
            const int shift=int(tick%64);
            for(int y=0;y<bounds.bottom;y+=64) for(int x=-64;x<bounds.right;x+=64) {
                RECT cell{x+shift,y,x+shift+64,y+64};
                HBRUSH brush=CreateSolidBrush(((x/64+y/64)&1)?RGB(20,150,210):RGB(245,170,30));
                FillRect(dc,&cell,brush); DeleteObject(brush);
                SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(0,0,0));
                wchar_t label[96]{};
                std::swprintf(label,96,L"%llu:%d,%d",tick,x/64,y/64);
                DrawTextW(dc,label,-1,&cell,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
            }
            EndPaint(hwnd,&paint); return 0;
        }
        if(message==WM_TIMER) { InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        if(message==WM_DESTROY) { KillTimer(hwnd,1); PostQuitMessage(0); return 0; }
        return DefWindowProcW(hwnd,message,wparam,lparam);
    }
};
class ProbeWindow final {
    HWND hwnd_{};
    CaptureSource source_;
    std::unique_ptr<PreviewRenderer> renderer_;
    std::wstring status_=L"SOURCE FEASIBILITY ONLY — no lens shader yet\nS: select this display and Start   Esc: Stop\nNo capture on launch. Run a second copy with --pattern behind this window.";
    bool shutdown_pending_{};
    ULONGLONG started_{};
    unsigned long long display_generation_{};
    void stop() noexcept {
        if(hwnd_) KillTimer(hwnd_,1);
        const bool restored=source_.stop();
        if(!restored) {
            shutdown_pending_=true;
            SetWindowTextW(hwnd_,L"STOPPED — affinity restoration failed; close window to remove exclusion");
        }
    }
    void stopped_status(std::wstring text) {
        status_=shutdown_pending_ ? L"Capture closed, but affinity restoration failed. Close this window; restart is blocked." : std::move(text);
    }
    void fail(const std::wstring& message) noexcept {
        stop();
        renderer_.reset(); // A failed GPU device must not create a repaint/error loop.
        // Allocation-free fallback if reporting itself runs out of memory.
        if(!shutdown_pending_) SetWindowTextW(hwnd_,L"Desktop glass source probe — STOPPED (error)");
        try { status_=L"STOPPED: "+message+(shutdown_pending_ ?
            L"\nCapture is closed, but affinity restoration failed. Close this window; restart is blocked." :
            L"\nNo capture active. S: select/start again; Esc: stop."); } catch(...) {}
        InvalidateRect(hwnd_,nullptr,FALSE);
    }
    void start() {
        stop();
        if(shutdown_pending_) throw std::runtime_error("Display affinity restoration failed. Close the probe before retrying.");
        const auto monitor=MonitorFromWindow(hwnd_,MONITOR_DEFAULTTONULL);
        MONITORINFOEXW info{}; info.cbSize=sizeof info;
        winrt::check_bool(monitor && GetMonitorInfoW(monitor,&info));
        const auto selected_generation=display_generation_;
        const auto prompt=std::wstring(L"Selected display: ")+info.szDevice+
            L"\n\nStart local capture of this ENTIRE display for the desktop-glass source experiment?"
            L"\nPixels stay in GPU memory. No recording, files or uploads. The system capture indicator remains visible."
            L"\n\nThis probe will be excluded from supported screenshots and screen shares to prevent self-feedback."
            L"\nEsc stops capture and restores the previous capture setting. Capture also stops on minimize, display changes or errors."
            L"\n\nMove this window to another display BEFORE starting to select that display."
            L"\n\nStart capture now?";
        if(MessageBoxW(hwnd_,prompt.c_str(),L"Experimental Liquid Glass — display capture consent",
                       MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) {
            status_=L"Canceled — no capture active. S: select/start; Esc: stop."; return;
        }
        MONITORINFOEXW current{}; current.cbSize=sizeof current;
        if(!IsWindow(hwnd_) || display_generation_!=selected_generation ||
           !GetMonitorInfoW(monitor,&current) || std::wcscmp(info.szDevice,current.szDevice)!=0 ||
           !EqualRect(&info.rcMonitor,&current.rcMonitor))
            throw std::runtime_error("Display selection changed during confirmation; select and confirm again.");
        if(!renderer_) renderer_=std::make_unique<PreviewRenderer>(hwnd_);
        source_.start(hwnd_,monitor,renderer_->device());
        started_=GetTickCount64();
        winrt::check_bool(SetTimer(hwnd_,1,33,nullptr)!=0);
        status_=L"CAPTURING selected display locally — Esc: Stop\nRaw desktop crop; no optical processing. Observe the moving pattern below.\nThis window should NOT appear in its own preview or in supported screen shares.";
    }
    void tick() {
        if(!source_.active()) return;
        RECT client{}; winrt::check_bool(GetClientRect(hwnd_,&client));
        POINT origin{0,0}; winrt::check_bool(ClientToScreen(hwnd_,&origin));
        OffsetRect(&client,origin.x,origin.y);
        if(!source_.frames() && GetTickCount64()-started_>5000)
            throw std::runtime_error("No capture frame arrived within five seconds; stopped safely.");
        if(source_.update(client)) {
            status_=L"CAPTURING — Esc: Stop | S: stop and select again\nRaw background frames: "+
                std::to_wstring(source_.frames())+L" | source timestamp (100ns): "+std::to_wstring(source_.timestamp())+
                L"\nSource proof only: pattern must move behind this crisp text without black/self-feedback.";
            renderer_->draw(source_.texture(),status_);
        }
    }
    LRESULT dispatch(UINT message,WPARAM wparam,LPARAM lparam) {
        switch(message) {
        case WM_CREATE:
            renderer_=std::make_unique<PreviewRenderer>(hwnd_);
            return 0;
        case WM_KEYDOWN:
            if(wparam=='S') { start(); InvalidateRect(hwnd_,nullptr,FALSE); }
            else if(wparam==VK_ESCAPE) { stop(); stopped_status(L"Stopped — capture closed and GPU source released. S: select/start."); InvalidateRect(hwnd_,nullptr,FALSE); }
            return 0;
        case WM_TIMER: tick(); return 0;
        case WM_SIZE:
            if(wparam==SIZE_MINIMIZED) { stop(); stopped_status(L"Stopped on minimize. S: explicitly restart."); }
            else InvalidateRect(hwnd_,nullptr,FALSE);
            return 0;
        case WM_SETTINGCHANGE:
        case WM_DISPLAYCHANGE:
            ++display_generation_;
            stop(); stopped_status(L"Stopped: display/settings configuration changed. S: select again.");
            InvalidateRect(hwnd_,nullptr,FALSE); return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC dc=BeginPaint(hwnd_,&paint);
            if(!renderer_) {
                RECT area{}; GetClientRect(hwnd_,&area); FillRect(dc,&area,GetSysColorBrush(COLOR_WINDOW));
                SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT)); SetBkMode(dc,TRANSPARENT);
                DrawTextW(dc,status_.c_str(),-1,&area,DT_WORDBREAK);
            }
            EndPaint(hwnd_,&paint);
            if(renderer_ && !IsIconic(hwnd_)) renderer_->draw(source_.texture(),status_);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE:
            stop(); renderer_.reset(); DestroyWindow(hwnd_); return 0;
        case WM_DESTROY:
            KillTimer(hwnd_,1); stop(); PostQuitMessage(0); return 0;
        default: return DefWindowProcW(hwnd_,message,wparam,lparam);
        }
    }
public:
    static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) noexcept {
        auto self=reinterpret_cast<ProbeWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<ProbeWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->hwnd_=hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self) return DefWindowProcW(hwnd,message,wparam,lparam);
        try { return self->dispatch(message,wparam,lparam); }
        catch(const winrt::hresult_error& error) { try { self->fail(std::wstring(error.message())); } catch(...) { self->stop(); } }
        catch(const std::exception& error) { try { self->fail(widen(error.what())); } catch(...) { self->stop(); } }
        catch(...) { self->stop(); }
        return message==WM_CREATE ? -1 : 0;
    }
};
}
int run(bool pattern) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
       !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        throw std::runtime_error("Per-monitor-v2 DPI awareness is required for exact capture coordinates.");
    const auto instance=GetModuleHandleW(nullptr);
    WNDCLASSW cls{}; cls.hInstance=instance; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.lpszClassName=pattern?L"StormTTY.DesktopGlass.Pattern":L"StormTTY.DesktopGlass.Source";
    cls.lpfnWndProc=pattern?PatternWindow::procedure:ProbeWindow::procedure;
    winrt::check_bool(RegisterClassW(&cls)!=0);
    ProbeWindow probe;
    HWND hwnd=CreateWindowExW(0,cls.lpszClassName,
        pattern?L"Authored moving source — put behind probe":L"StormTTY experimental desktop glass — source feasibility",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,960,640,nullptr,nullptr,instance,&probe);
    winrt::check_bool(hwnd!=nullptr); ShowWindow(hwnd,SW_SHOW);
    MSG message{}; int result{};
    while((result=GetMessageW(&message,nullptr,0,0))>0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if(result<0) { DestroyWindow(hwnd); return 1; }
    return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc>2 || (argc==2 && std::wcscmp(argv[1],L"--pattern")!=0)) {
            std::fputws(L"Usage: stormtty_desktop_glass_probe [--pattern]\nNo capture starts automatically.\n",stderr); return 2;
        }
        return stormtty::glass_probe::run(argc==2);
    } catch(const std::exception& error) { std::fprintf(stderr,"Desktop-glass probe failed: %s\n",error.what()); return 1; }
    catch(...) { std::fputs("Desktop-glass probe failed.\n",stderr); return 1; }
}
