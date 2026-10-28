// Experimental standalone probe. Not linked into StormTTY. See host-backdrop-blur.md.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <dispatcherqueue.h>
#include <windows.graphics.effects.interop.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>

namespace wc = winrt::Windows::UI::Composition;
namespace wf = winrt::Windows::Foundation;
namespace wg = winrt::Windows::Graphics::Effects;
namespace effect_abi = ABI::Windows::Graphics::Effects;
using winrt::check_hresult;

// Native effect metadata, not an ID2D1Effect instance or Win2D runtime object.
// make_self wraps implementations in a derived heap_implements; do not mark final.
struct Gaussian : winrt::implements<Gaussian, wg::IGraphicsEffect,
    wg::IGraphicsEffectSource, effect_abi::IGraphicsEffectD2D1Interop> {
    winrt::hstring name{L"Blur"};
    wg::IGraphicsEffectSource source{nullptr};
    winrt::hstring Name() const { return name; }
    void Name(winrt::hstring const& value) { name = value; }
    HRESULT __stdcall GetEffectId(GUID* value) noexcept override {
        if (!value) return E_POINTER; *value = CLSID_D2D1GaussianBlur; return S_OK;
    }
    HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR property, UINT* index,
        effect_abi::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
        if (!property || !index || !mapping) return E_POINTER;
        if (wcscmp(property, L"BlurAmount") != 0) return E_INVALIDARG;
        *index = D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION;
        *mapping = effect_abi::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT; return S_OK;
    }
    HRESULT __stdcall GetPropertyCount(UINT* value) noexcept override {
        if (!value) return E_POINTER; *value = 3; return S_OK;
    }
    HRESULT __stdcall GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value) return E_POINTER; *value = nullptr;
        try {
            wf::IInspectable property{nullptr};
            switch (index) {
            case D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION: property = wf::PropertyValue::CreateSingle(0); break;
            case D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION: property = wf::PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED); break;
            case D2D1_GAUSSIANBLUR_PROP_BORDER_MODE: property = wf::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD); break;
            default: return E_INVALIDARG;
            }
            property.as<ABI::Windows::Foundation::IPropertyValue>().copy_to(value); return S_OK;
        } catch (...) { return winrt::to_hresult(); }
    }
    HRESULT __stdcall GetSourceCount(UINT* value) noexcept override {
        if (!value) return E_POINTER; *value = 1; return S_OK;
    }
    HRESULT __stdcall GetSource(UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** value) noexcept override {
        if (!value) return E_POINTER; *value = nullptr;
        if (index != 0) return E_INVALIDARG;
        try { source.as<ABI::Windows::Graphics::Effects::IGraphicsEffectSource>().copy_to(value); return S_OK; }
        catch (...) { return winrt::to_hresult(); }
    }
};

struct Probe {
    HWND hwnd{};
    bool force_warp{}, warp{}, active{}, failed{}, blocked_test{}, high_contrast{}, smoke{}, require_host{};
    unsigned smoke_step{};
    int smoke_result{};
    float opacity{.25f}, sigma{};
    UINT width{960}, height{640};
    unsigned recovery_count{};
    std::wstring status{L"initializing"};
    wc::Compositor compositor{nullptr};
    wc::Desktop::DesktopWindowTarget target{nullptr};
    wc::ContainerVisual root{nullptr};
    wc::SpriteVisual background{nullptr}, foreground{nullptr};
    wc::CompositionEffectBrush effect{nullptr};
    wc::CompositionSurfaceBrush surface_brush{nullptr};
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> immediate;
    winrt::com_ptr<IDXGISwapChain1> swapchain;
    winrt::com_ptr<ID2D1Device> d2d_device;
    winrt::com_ptr<ID2D1DeviceContext> dc;
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
    winrt::com_ptr<ID2D1SolidColorBrush> ink;
    winrt::com_ptr<IDWriteFactory> write;
    winrt::com_ptr<IDWriteTextFormat> font;

    void disable_host() noexcept {
        BOOL off = FALSE;
        if (IsWindow(hwnd)) DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &off, sizeof off);
        active = false;
    }
    void release_graphics() noexcept {
        disable_host();
        try { if (target) target.Root(nullptr); } catch (...) {}
        effect = nullptr; surface_brush = nullptr; foreground = nullptr;
        background = nullptr; root = nullptr; target = nullptr; compositor = nullptr;
        if (dc) dc->SetTarget(nullptr);
        bitmap = nullptr; ink = nullptr; font = nullptr; write = nullptr;
        dc = nullptr; d2d_device = nullptr; swapchain = nullptr; immediate = nullptr; device = nullptr;
    }
    ~Probe() { release_graphics(); }
    void create_bitmap() {
        winrt::com_ptr<IDXGISurface> surface;
        check_hresult(swapchain->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void()));
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        check_hresult(dc->CreateBitmapFromDxgiSurface(surface.get(), &properties, bitmap.put()));
        dc->SetTarget(bitmap.get());
    }
    void initialize() {
        release_graphics(); failed = false;
        RECT r{}; GetClientRect(hwnd, &r); width = std::max(1L, r.right); height = std::max(1L, r.bottom);
        D3D_FEATURE_LEVEL level{};
        auto create = [&](D3D_DRIVER_TYPE type) {
            return D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, device.put(), &level, immediate.put());
        };
        HRESULT hr = force_warp ? E_FAIL : create(D3D_DRIVER_TYPE_HARDWARE);
        warp = FAILED(hr);
        if (warp) { device = nullptr; immediate = nullptr; hr = create(D3D_DRIVER_TYPE_WARP); }
        check_hresult(hr);
        auto dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter> adapter; check_hresult(dxgi->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2; desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        check_hresult(factory->CreateSwapChainForComposition(device.get(), &desc, nullptr, swapchain.put()));
        winrt::com_ptr<ID2D1Factory1> d2d;
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, d2d.put_void()));
        check_hresult(d2d->CreateDevice(dxgi.get(), d2d_device.put()));
        check_hresult(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        check_hresult(dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), ink.put()));
        check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write.put())));
        check_hresult(write->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20, L"en-US", font.put()));
        create_bitmap();
        compositor = wc::Compositor();
        auto desktop = compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        check_hresult(desktop->CreateDesktopWindowTarget(hwnd, TRUE,
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target))));
        root = compositor.CreateContainerVisual(); target.Root(root);
        background = compositor.CreateSpriteVisual(); foreground = compositor.CreateSpriteVisual();
        background.Size({float(width), float(height)}); foreground.Size({float(width), float(height)});
        wc::ICompositionSurface surface{nullptr};
        check_hresult(compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>()->CreateCompositionSurfaceForSwapChain(
            swapchain.get(), reinterpret_cast<ABI::Windows::UI::Composition::ICompositionSurface**>(winrt::put_abi(surface))));
        surface_brush = compositor.CreateSurfaceBrush(surface); foreground.Brush(surface_brush);
        root.Children().InsertAtBottom(background); root.Children().InsertAtTop(foreground);
        // Do not stack the system acrylic recipe below our own host brush.
        const DWORD none = DWMSBT_NONE;
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &none, sizeof none);
        policy();
    }
    void policy() {
        if (!background) return;
        HIGHCONTRASTW contrast{sizeof(HIGHCONTRASTW)};
        const bool contrast_known = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0) != FALSE;
        DWORD transparency = 0, bytes = sizeof transparency;
        const auto transparency_result = RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency",
            RRF_RT_REG_DWORD, nullptr, &transparency, &bytes);
        const bool enabled = transparency_result == ERROR_SUCCESS && transparency != 0;
        high_contrast = !contrast_known || (contrast.dwFlags & HCF_HIGHCONTRASTON);
        disable_host(); effect = nullptr;
        background.Brush(compositor.CreateColorBrush({255, 22, 28, 38}));
        if (warp || blocked_test || high_contrast || !enabled) {
            status = warp ? L"opaque: WARP" : L"opaque: accessibility/transparency policy (or test override)";
            return;
        }
        BOOL yes = TRUE;
        const auto hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &yes, sizeof yes);
        if (FAILED(hr)) { status = L"opaque: host backdrop unavailable (Windows 11 required)"; return; }
        try {
            auto descriptor = winrt::make_self<Gaussian>();
            descriptor->source = wc::CompositionEffectSourceParameter(L"Host");
            const auto factory = compositor.CreateEffectFactory(*descriptor, {L"Blur.BlurAmount"});
            effect = factory.CreateBrush();
            effect.SetSourceParameter(L"Host", compositor.CreateHostBackdropBrush());
            effect.Properties().InsertScalar(L"Blur.BlurAmount", sigma);
            background.Brush(effect); active = true;
            status = L"host graph active; visual correctness NOT established by HRESULT";
        } catch (...) {
            const auto error = winrt::to_hresult(); disable_host(); effect = nullptr;
            status = L"opaque: effect graph failed HRESULT=" + std::to_wstring(static_cast<unsigned long>(error));
        }
    }
    void resize() {
        RECT r{}; GetClientRect(hwnd, &r);
        if (!r.right || !r.bottom || !swapchain) return;
        width = r.right; height = r.bottom;
        dc->SetTarget(nullptr); bitmap = nullptr;
        check_hresult(swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
        create_bitmap(); background.Size({float(width), float(height)}); foreground.Size({float(width), float(height)});
    }
    void draw() {
        if (!dc) { InvalidateRect(hwnd, nullptr, TRUE); return; }
        dc->BeginDraw();
        const auto system_color = [](int index) {
            const auto c = GetSysColor(index);
            return D2D1::ColorF(GetRValue(c)/255.f, GetGValue(c)/255.f, GetBValue(c)/255.f);
        };
        dc->Clear(high_contrast ? system_color(COLOR_WINDOW) : D2D1::ColorF(.055f, .075f, .105f, active ? opacity : 1.f));
        const auto text = L"StormTTY HOST BACKDROP PROBE — not production\n\n" + status +
            L"\nAdditional sigma: " + std::to_wstring(sigma) + L" (0 retains OS blur)\nBackground opacity: " +
            std::to_wstring(opacity) + L"\n\nLeft/Right: sigma 0..64; Up/Down: opacity 0..1\nHome/End: sigma 0/64; F: opaque-policy test; R: recreate\n\n" +
            L"Opaque foreground: ABC 0123456789 []{} /\\ <>\nMove another window behind this one; inspect spatial blur.\nNo capture, readback, private API, or global window alpha.";
        ink->SetColor(high_contrast ? system_color(COLOR_WINDOWTEXT) : D2D1::ColorF(D2D1::ColorF::White));
        dc->DrawText(text.c_str(), static_cast<UINT32>(text.size()), font.get(),
            D2D1::RectF(28, 28, float(width)-28, float(height)-28), ink.get());
        check_hresult(dc->EndDraw()); check_hresult(swapchain->Present(1, 0));
    }
    void safe_update(bool rebuild = false) noexcept {
        try { if (rebuild) initialize(); draw(); }
        catch (...) {
            const auto hr = winrt::to_hresult();
            // One bounded automatic recreation for a real lost graphics device.
            if (!rebuild && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET)) {
                ++recovery_count; safe_update(true); return;
            }
            release_graphics(); failed = true;
            status = L"Opaque GDI fallback: graphics initialization/render failed HRESULT=" +
                std::to_wstring(static_cast<unsigned long>(hr));
            InvalidateRect(hwnd, nullptr, TRUE);
        }
    }
};

LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* probe = reinterpret_cast<Probe*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        probe = static_cast<Probe*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        probe->hwnd = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(probe));
    }
    try {
        if (probe) switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{}; auto hdc = BeginPaint(hwnd, &paint);
            if (probe->failed) {
                FillRect(hdc, &paint.rcPaint, GetSysColorBrush(COLOR_WINDOW));
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT)); SetBkMode(hdc, TRANSPARENT);
                RECT rect{}; GetClientRect(hwnd, &rect);
                DrawTextW(hdc, probe->status.c_str(), -1, &rect, DT_WORDBREAK);
            }
            EndPaint(hwnd, &paint); if (!probe->failed) probe->safe_update(); return 0;
        }
        case WM_SIZE: probe->resize(); probe->safe_update(); return 0;
        case WM_TIMER:
            if (wparam == 1 && probe->smoke) {
                ++probe->smoke_step;
                if (probe->failed || (probe->warp && probe->active)) probe->smoke_result = 1;
                if (probe->require_host && !probe->blocked_test && !probe->active) probe->smoke_result = 1;
                if (probe->smoke_step == 1) {
                    probe->sigma = 24; probe->opacity = 0;
                    if (probe->effect) probe->effect.Properties().InsertScalar(L"Blur.BlurAmount", probe->sigma);
                    probe->safe_update();
                } else if (probe->smoke_step == 2) {
                    probe->blocked_test = true; probe->policy(); probe->safe_update();
                    if (probe->active) probe->smoke_result = 1;
                } else if (probe->smoke_step == 3) {
                    probe->blocked_test = false; probe->safe_update(true);
                    SetWindowPos(hwnd, nullptr, 0, 0, 800, 500, SWP_NOMOVE | SWP_NOZORDER);
                } else {
                    std::printf("Host backdrop smoke: result=%d software=%u host_active=%u. No physical blur verification.\n",
                        probe->smoke_result, unsigned(probe->warp), unsigned(probe->active));
                    KillTimer(hwnd, 1); DestroyWindow(hwnd);
                }
            }
            return 0;
        case WM_SETTINGCHANGE: case WM_THEMECHANGED: probe->policy(); probe->safe_update(); return 0;
        case WM_DPICHANGED: {
            const auto r = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0;
        }
        case WM_KEYDOWN:
            switch (wparam) {
            case VK_LEFT: probe->sigma = std::max(0.f, probe->sigma-2); break;
            case VK_RIGHT: probe->sigma = std::min(64.f, probe->sigma+2); break;
            case VK_HOME: probe->sigma = 0; break;
            case VK_END: probe->sigma = 64; break;
            case VK_UP: probe->opacity = std::min(1.f, probe->opacity+.05f); break;
            case VK_DOWN: probe->opacity = std::max(0.f, probe->opacity-.05f); break;
            case 'F': probe->blocked_test = !probe->blocked_test; probe->policy(); break;
            case 'R': probe->safe_update(true); return 0;
            case VK_ESCAPE: DestroyWindow(hwnd); return 0;
            default: return DefWindowProcW(hwnd, message, wparam, lparam);
            }
            if (probe->effect) probe->effect.Properties().InsertScalar(L"Blur.BlurAmount", probe->sigma);
            probe->safe_update(); return 0;
        case WM_DESTROY: probe->release_graphics(); PostQuitMessage(0); return 0;
        }
    } catch (...) { if (probe) probe->safe_update(true); }
    // In particular WM_NCACTIVATE always receives the normal DWM/default path.
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int main(int argc, char** argv) {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        winrt::Windows::System::DispatcherQueueController queue{nullptr};
        DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
        check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
        int exit_code = 0;
        {
            Probe probe;
            for (int i = 1; i < argc; ++i) {
                if (std::string(argv[i]) == "--warp") probe.force_warp = true;
                else if (std::string(argv[i]) == "--smoke") probe.smoke = true;
                else if (std::string(argv[i]) == "--require-host") probe.require_host = true;
                else { std::fputs("Usage: host-backdrop-blur [--warp] [--smoke] [--require-host]\n", stderr); return 2; }
            }
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            WNDCLASSW klass{}; klass.hInstance = GetModuleHandleW(nullptr); klass.lpfnWndProc = procedure;
            klass.lpszClassName = L"StormTTY.HostBackdropProbe"; klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            check_hresult(RegisterClassW(&klass) ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
            // Ordinary redirection HWND permits a true GDI fallback if composition creation fails.
            // DesktopWindowTarget is topmost, so the swapchain overlays its backing surface.
            HWND hwnd = CreateWindowExW(0, klass.lpszClassName, L"Experimental host backdrop — additional blur only",
                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 640, nullptr, nullptr, klass.hInstance, &probe);
            check_hresult(hwnd ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
            probe.safe_update(true); ShowWindow(hwnd, SW_SHOW);
            if (probe.smoke && !SetTimer(hwnd, 1, 250, nullptr)) winrt::throw_last_error();
            MSG message{}; int result;
            while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (IsWindow(hwnd)) DestroyWindow(hwnd);
            exit_code = result == -1 ? 1 : probe.smoke_result;
        }
        // Shutdown requires a live message pump; do not synchronously .get() on this thread.
        auto shutdown = queue.ShutdownQueueAsync();
        while (shutdown.Status() == wf::AsyncStatus::Started) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message != WM_QUIT) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        shutdown.GetResults(); queue = nullptr; winrt::uninit_apartment(); return exit_code;
    } catch (winrt::hresult_error const& error) {
        std::fprintf(stderr, "Host backdrop probe failed: 0x%08lx\n", static_cast<unsigned long>(error.code().value));
        return 1;
    } catch (...) { std::fputs("Host backdrop probe failed unexpectedly\n", stderr); return 1; }
}
