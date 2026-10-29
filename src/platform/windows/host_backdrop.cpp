#include "host_backdrop.hpp"
#if defined(_MSC_VER)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <roapi.h>
#include <cstdio>
#include <d2d1effects.h>
#include <dispatcherqueue.h>
#include <windows.graphics.effects.interop.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <cwchar>
#include <cmath>

namespace stormtty::windows {
namespace {
namespace wc=winrt::Windows::UI::Composition;
namespace wf=winrt::Windows::Foundation;
namespace wg=winrt::Windows::Graphics::Effects;
namespace effect_abi=ABI::Windows::Graphics::Effects;
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

}
struct HostBackdrop::Impl {
    HWND window{};
    bool runtime_initialized{},shutdown_failed{};
    winrt::Windows::System::DispatcherQueueController queue{nullptr};
    wc::Compositor compositor{nullptr};
    wc::Desktop::DesktopWindowTarget target{nullptr};
    wc::ContainerVisual root{nullptr};
    wc::SpriteVisual background{nullptr},foreground{nullptr};
    wc::CompositionEffectBrush effect{nullptr};
    wc::CompositionSurfaceBrush surface{nullptr};
    void detach() noexcept {
        try { if(target) target.Root(nullptr); } catch(...) {}
        effect=nullptr;surface=nullptr;background=nullptr;foreground=nullptr;
        root=nullptr;target=nullptr;compositor=nullptr;
        if(IsWindow(window)) {
            const BOOL off=FALSE;
            DwmSetWindowAttribute(window,DWMWA_USE_HOSTBACKDROPBRUSH,&off,sizeof off);
        }
        window=nullptr;
    }
    bool shutdown() noexcept {
        detach();
        if(shutdown_failed) return false;
        // Final application shutdown only: the HWND is already destroyed. Keep
        // apartment/controller ownership until completion, including after a
        // slow-shutdown warning. Bounded batches retain message responsiveness;
        // native qualification adds an external owned-process deadline.
        bool quit=false;WPARAM quit_code{};
        try {
            if(queue) {
                auto operation=queue.ShutdownQueueAsync();
                const auto warning_at=GetTickCount64()+5000;
                bool warned=false;
                while(operation.Status()==wf::AsyncStatus::Started) {
                    MSG message{};
                    for(unsigned batch=0;batch<64 && PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++batch) {
                        if(message.message==WM_QUIT) {quit=true;quit_code=message.wParam;}
                        else {TranslateMessage(&message);DispatchMessageW(&message);}
                    }
                    if(!warned && GetTickCount64()>=warning_at) {
                        std::fputs("StormTTY host backdrop: dispatcher shutdown exceeds five seconds; still retaining ownership.\n",stderr);
                        warned=true;
                    }
                    MsgWaitForMultipleObjectsEx(0,nullptr,20,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                }
                operation.GetResults();
                queue=nullptr;
            }
            if(runtime_initialized) {RoUninitialize();runtime_initialized=false;}
            if(quit) PostQuitMessage(static_cast<int>(quit_code));
            return true;
        } catch(...) {
            if(quit) PostQuitMessage(static_cast<int>(quit_code));
            shutdown_failed=true;
            std::fputs("StormTTY host backdrop: dispatcher shutdown failed; ownership retained until process exit.\n",stderr);
            return false;
        }
    }

};
HostBackdrop::HostBackdrop():impl_(std::make_unique<Impl>()) {}
HostBackdrop::~HostBackdrop() {
    // Exceptional final-shutdown failure cannot safely balance WinRT or release
    // a live queue. Retain this one owner until process exit rather than destroy
    // live runtime state. Qualification treats false shutdown as a hard failure.
    if(!shutdown()) (void)impl_.release();
}
bool HostBackdrop::shutdown() noexcept {return impl_->shutdown();}
void HostBackdrop::detach() noexcept {impl_->detach();}
bool HostBackdrop::active() const noexcept {return bool(impl_->effect);}
bool HostBackdrop::attach(void* window,IUnknown* swapchain,float sigma,unsigned width,unsigned height) noexcept {
    return attach_impl(window,swapchain,sigma,width,height,false);
}
bool HostBackdrop::attach_owned_surface_for_test(void* window,IUnknown* swapchain,float sigma,unsigned width,unsigned height) noexcept {
    return attach_impl(window,swapchain,sigma,width,height,true);
}
bool HostBackdrop::attach_impl(void* window,IUnknown* swapchain,float sigma,unsigned width,unsigned height,bool owned_source) noexcept {
    detach();
    if(!window || !swapchain || !std::isfinite(sigma) || sigma<=0 || sigma>64) return false;
    auto& p=*impl_;if(p.shutdown_failed) return false;
    p.window=static_cast<HWND>(window);
    try {
        if(!p.runtime_initialized) {
            auto hr=RoInitialize(RO_INIT_MULTITHREADED);
            if(hr==RPC_E_CHANGED_MODE) hr=RoInitialize(RO_INIT_SINGLETHREADED);
            winrt::check_hresult(hr);p.runtime_initialized=true;
        }
        if(!p.queue && !winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
            DispatcherQueueOptions options{sizeof(options),DQTYPE_THREAD_CURRENT,DQTAT_COM_NONE};
            winrt::check_hresult(CreateDispatcherQueueController(options,
                reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(p.queue))));
        }
        const BOOL on=TRUE;
        if(!owned_source) winrt::check_hresult(DwmSetWindowAttribute(p.window,DWMWA_USE_HOSTBACKDROPBRUSH,&on,sizeof on));
        p.compositor=wc::Compositor();
        const auto interop=p.compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(interop->CreateDesktopWindowTarget(p.window,TRUE,
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(p.target))));
        p.root=p.compositor.CreateContainerVisual();p.target.Root(p.root);
        p.background=p.compositor.CreateSpriteVisual();p.foreground=p.compositor.CreateSpriteVisual();
        wc::ICompositionSurface surface{nullptr};
        winrt::check_hresult(p.compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>()->CreateCompositionSurfaceForSwapChain(
            swapchain,reinterpret_cast<ABI::Windows::UI::Composition::ICompositionSurface**>(winrt::put_abi(surface))));
        p.surface=p.compositor.CreateSurfaceBrush(surface);p.foreground.Brush(p.surface);
        auto descriptor=winrt::make_self<Gaussian>();
        descriptor->source=wc::CompositionEffectSourceParameter(L"Host");
        const auto factory=p.compositor.CreateEffectFactory(*descriptor,{L"Blur.BlurAmount"});
        p.effect=factory.CreateBrush();
        if(owned_source) p.effect.SetSourceParameter(L"Host",p.surface);
        else p.effect.SetSourceParameter(L"Host",p.compositor.CreateHostBackdropBrush());
        p.background.Brush(p.effect);
        p.root.Children().InsertAtBottom(p.background);p.root.Children().InsertAtTop(p.foreground);
        if(!update(sigma,width,height)) return false;
        return true;
    } catch(...) {detach();return false;}
}
bool HostBackdrop::update(float sigma,unsigned width,unsigned height) noexcept {
    if(!active() || !std::isfinite(sigma) || sigma<0 || sigma>64) return false;
    try {
        impl_->effect.Properties().InsertScalar(L"Blur.BlurAmount",sigma);
        impl_->background.Size({float(width),float(height)});
        impl_->foreground.Size({float(width),float(height)});
        return true;
    } catch(...) {detach();return false;}
}
}
#else
namespace stormtty::windows {
struct HostBackdrop::Impl {};
HostBackdrop::HostBackdrop():impl_(std::make_unique<Impl>()) {}
HostBackdrop::~HostBackdrop()=default;
bool HostBackdrop::attach(void*,IUnknown*,float,unsigned,unsigned) noexcept {return false;}
bool HostBackdrop::attach_impl(void*,IUnknown*,float,unsigned,unsigned,bool) noexcept {return false;}
bool HostBackdrop::attach_owned_surface_for_test(void*,IUnknown*,float,unsigned,unsigned) noexcept {return false;}
bool HostBackdrop::shutdown() noexcept {return true;}
bool HostBackdrop::update(float,unsigned,unsigned) noexcept {return false;}
void HostBackdrop::detach() noexcept {}
bool HostBackdrop::active() const noexcept {return false;}
}
#endif
