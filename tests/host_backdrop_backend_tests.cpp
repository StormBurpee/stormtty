// Actual production helper, with an owned WARP surface in place of desktop input.
// This exercises queue ownership, attach/update/detach and final shutdown only.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <winrt/base.h>
#include "../src/platform/windows/host_backdrop.hpp"
#include <cstdio>
#include <stdexcept>
#include <initializer_list>

int main() {
    HWND window{};
    try {
        WNDCLASSW klass{};klass.hInstance=GetModuleHandleW(nullptr);
        klass.lpfnWndProc=DefWindowProcW;klass.lpszClassName=L"StormTTY.HostBackendOwnedTest";
        if(!RegisterClassW(&klass)) winrt::throw_last_error();
        window=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,klass.lpszClassName,L"Owned backend lifecycle test",
            WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,klass.hInstance,nullptr);
        if(!window) winrt::throw_last_error();
        winrt::com_ptr<ID3D11Device> device;
        winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,nullptr));
        auto dxgi=device.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter> adapter;winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> factory;
        winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2),factory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=320;desc.Height=240;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
        desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
        winrt::com_ptr<IDXGISwapChain1> swapchain;
        winrt::check_hresult(factory->CreateSwapChainForComposition(device.get(),&desc,nullptr,swapchain.put()));
        stormtty::windows::HostBackdrop backend;
        for(unsigned cycle=0;cycle<3;++cycle) {
            if(!backend.attach_owned_surface_for_test(window,swapchain.get(),24,320,240) || !backend.active())
                throw std::runtime_error("Production backend owned graph attach failed");
            for(float sigma : {0.f,8.f,24.f,48.f,64.f})
                if(!backend.update(sigma,320,240)) throw std::runtime_error("Production backend scalar update failed");
            if(!backend.update(24,160,120)) throw std::runtime_error("Production backend visual resize failed");
            backend.detach();
            if(backend.active()) throw std::runtime_error("Production backend detach retained effect");
        }
        // Same ordering as NativeWindow: callbacks cannot reenter a dying host.
        DestroyWindow(window);window=nullptr;
        if(!backend.shutdown()) throw std::runtime_error("Production backend queue shutdown failed");
        std::puts("Production host backend: owned_graph=1 cycles=3 shutdown_completed=1 hardware_host=0 physical_blur_verified=0");
        return 0;
    } catch(const winrt::hresult_error& error) {
        if(IsWindow(window)) DestroyWindow(window);
        std::fprintf(stderr,"Production host backend failed HRESULT=0x%08lx\n",static_cast<unsigned long>(error.code().value));return 1;
    } catch(const std::exception& error) {
        if(IsWindow(window)) DestroyWindow(window);
        std::fprintf(stderr,"Production host backend failed: %s\n",error.what());return 1;
    } catch(...) {
        if(IsWindow(window)) DestroyWindow(window);
        std::fputs("Production host backend failed unexpectedly\n",stderr);return 1;
    }
}
