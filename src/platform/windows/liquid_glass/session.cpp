#include "session.hpp"
#include <cstdint>
#include <stdexcept>
#if defined(_MSC_VER)
#include "capture_source.hpp"
#include "material_pass.hpp"
#include <winrt/base.h>
namespace stormtty::windows::liquid_glass {
struct Session::Impl {
    HWND window{};
    CaptureSource source;
    std::unique_ptr<MaterialPass> material;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11Texture2D> output;
    unsigned width{},height{};
    ULONGLONG started{};
    std::string status="Off — experimental desktop glass is session-only.";
    bool restore_failed{};
    std::uint64_t generation{};
};
Session::Session():impl_(std::make_unique<Impl>()){}
Session::~Session(){stop();}
void Session::stop(const char* reason) noexcept {
    auto& p=*impl_;
    const bool restored=p.source.stop();
    p.output=nullptr;p.material.reset();p.device=nullptr;p.width=p.height=0;
    p.restore_failed=!restored;++p.generation;
    try {p.status=restored?reason:"Capture stopped, but exclusion restoration failed. Close this window before retrying.";}catch(...){}
}
bool Session::start(void* window,void* monitor,ID3D11Device* device,ID3D11DeviceContext* context) noexcept {
    stop();auto& p=*impl_;
    if(p.restore_failed)return false;
    try {
        p.window=static_cast<HWND>(window);p.device.copy_from(device);
        p.material=std::make_unique<MaterialPass>(device,context);
        p.source.start(p.window,static_cast<HMONITOR>(monitor),device);
        p.started=GetTickCount64();++p.generation;p.status="On — local display capture; excluded from screen shares. Waiting for first frame.";
        return true;
    } catch(const winrt::hresult_error& error) {
        try {const auto detail=winrt::to_string(error.message());stop(detail.c_str());}catch(...){stop("Experimental glass API failure.");}
    } catch(const std::exception& error) {stop(error.what());}
    catch(...) {stop("Experimental glass could not start.");}
    return false;
}
bool Session::active()const noexcept{return impl_->source.active();}
std::string Session::status()const{return impl_->status;}
std::uint64_t Session::generation()const noexcept{return impl_->generation;}
ID3D11Texture2D* Session::background(float scale) noexcept {
    auto& p=*impl_;if(!active())return nullptr;
    try {
        RECT client{};winrt::check_bool(GetClientRect(p.window,&client));
        POINT origin{};winrt::check_bool(ClientToScreen(p.window,&origin));OffsetRect(&client,origin.x,origin.y);
        const auto width=unsigned(client.right-client.left),height=unsigned(client.bottom-client.top);
        if(!width||!height||std::uint64_t(width)*height>4194304 || width>4096 || height>4096)
            throw std::runtime_error("Experimental glass supports client areas up to 4,194,304 pixels and 4096 per axis.");
        const bool changed=p.source.update(client);
        if(!p.source.texture()) {
            if(GetTickCount64()-p.started>5000)throw std::runtime_error("Capture produced no frame within five seconds.");
            return nullptr;
        }
        bool resize=!p.output||p.width!=width||p.height!=height;
        if(resize) {
            p.output=nullptr;D3D11_TEXTURE2D_DESC desc{};
            desc.Width=width;desc.Height=height;desc.MipLevels=1;desc.ArraySize=1;
            desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
            desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            winrt::check_hresult(p.device->CreateTexture2D(&desc,nullptr,p.output.put()));
            p.width=width;p.height=height;
        }
        if(changed||resize) {p.material->draw(p.source.texture(),p.output.get(),scale);++p.generation;}
        p.status="On — experimental live desktop lens; local capture only. Hidden from supported screen shares.";
        return p.output.get();
    }catch(const winrt::hresult_error& error){try {const auto detail=winrt::to_string(error.message());stop(detail.c_str());}catch(...){stop("Experimental glass API failure.");}}
    catch(const std::exception& error){stop(error.what());}
    catch(...){stop("Experimental glass failed; original material restored.");}
    return nullptr;
}
}
#else
namespace stormtty::windows::liquid_glass {
struct Session::Impl{};
Session::Session():impl_(std::make_unique<Impl>()){} Session::~Session()=default;
bool Session::start(void*,void*,ID3D11Device*,ID3D11DeviceContext*)noexcept{return false;}
void Session::stop(const char*)noexcept{} bool Session::active()const noexcept{return false;}
std::string Session::status()const{return "Unavailable: experimental glass requires a native MSVC/Windows SDK build.";}
ID3D11Texture2D* Session::background(float)noexcept{return nullptr;}
std::uint64_t Session::generation()const noexcept{return 0;}
}
#endif
