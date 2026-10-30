#include "preview_renderer.hpp"
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <algorithm>
namespace stormtty::glass_probe {
using winrt::check_hresult;
struct PreviewRenderer::Impl {
    HWND window;
    UINT width{}, height{};
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<IDXGISwapChain1> swapchain;
    winrt::com_ptr<ID2D1DeviceContext> dc;
    winrt::com_ptr<ID2D1Bitmap1> target;
    winrt::com_ptr<ID2D1SolidColorBrush> ink;
    winrt::com_ptr<IDWriteTextFormat> font;
    explicit Impl(HWND hwnd) : window(hwnd) {
        D3D_FEATURE_LEVEL level{};
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device.put(), &level, nullptr));
        auto dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter> adapter; check_hresult(dxgi->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width=1; desc.Height=1; desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        check_hresult(factory->CreateSwapChainForHwnd(device.get(), hwnd, &desc, nullptr, nullptr, swapchain.put()));
        check_hresult(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
        winrt::com_ptr<ID2D1Factory1> d2d;
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, d2d.put_void()));
        winrt::com_ptr<ID2D1Device> d2d_device; check_hresult(d2d->CreateDevice(dxgi.get(), d2d_device.put()));
        check_hresult(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()));
        dc->SetDpi(96,96); dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        check_hresult(dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), ink.put()));
        winrt::com_ptr<IDWriteFactory> write;
        check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write.put())));
        check_hresult(write->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16, L"en-US", font.put()));
    }
    void resize() {
        RECT rect{}; winrt::check_bool(GetClientRect(window,&rect));
        const UINT w=UINT(std::max(1L,rect.right)), h=UINT(std::max(1L,rect.bottom));
        if(target && w==width && h==height) return;
        dc->SetTarget(nullptr); target=nullptr;
        check_hresult(swapchain->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0));
        winrt::com_ptr<IDXGISurface> surface;
        check_hresult(swapchain->GetBuffer(0,__uuidof(IDXGISurface),surface.put_void()));
        const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE));
        check_hresult(dc->CreateBitmapFromDxgiSurface(surface.get(),&properties,target.put()));
        dc->SetTarget(target.get()); width=w; height=h;
    }
};
PreviewRenderer::PreviewRenderer(HWND window) : impl_(std::make_unique<Impl>(window)) {}
PreviewRenderer::~PreviewRenderer()=default;
ID3D11Device* PreviewRenderer::device() const noexcept { return impl_->device.get(); }
void PreviewRenderer::draw(ID3D11Texture2D* texture,std::wstring_view status) {
    auto& p=*impl_; p.resize();
    // Resolve every fallible setup operation before BeginDraw.
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
    if(texture) {
        winrt::com_ptr<IDXGISurface> surface; check_hresult(texture->QueryInterface(__uuidof(IDXGISurface),surface.put_void()));
        const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE));
        check_hresult(p.dc->CreateBitmapFromDxgiSurface(surface.get(),&properties,bitmap.put()));
    }
    p.dc->BeginDraw(); p.dc->Clear(D2D1::ColorF(0.06f,0.07f,0.09f));
    if(bitmap) p.dc->DrawBitmap(bitmap.get()); // Exact-size source: deliberately no optics yet.
    p.ink->SetColor(D2D1::ColorF(0.04f,0.05f,0.07f,0.96f));
    p.dc->FillRectangle(D2D1::RectF(0,0,float(p.width),130),p.ink.get());
    p.ink->SetColor(D2D1::ColorF(0.92f,0.95f,1.f));
    const auto area=D2D1::RectF(16,12,float(p.width)-16,126);
    p.dc->DrawTextW(status.data(),UINT32(status.size()),p.font.get(),area,p.ink.get());
    check_hresult(p.dc->EndDraw()); check_hresult(p.swapchain->Present(1,0));
}
}
