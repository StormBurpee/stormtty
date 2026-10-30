#include "capture_source.hpp"
#include <dxgi1_6.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <cstdint>
#include <cwchar>
#include <string>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <atomic>
#include <stdexcept>

namespace stormtty::windows::liquid_glass {
namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;
using winrt::check_hresult;
namespace {
constexpr std::uint64_t max_pixels = 3840ull * 2160ull;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
winrt::com_ptr<IDXGIOutput6> verify_sdr(IDXGIAdapter* adapter, HMONITOR monitor) {
    winrt::com_ptr<IDXGIOutput6> found;
    for (UINT i = 0;; ++i) {
        winrt::com_ptr<IDXGIOutput> output;
        HRESULT result = adapter->EnumOutputs(i, output.put());
        if (result == DXGI_ERROR_NOT_FOUND) break;
        check_hresult(result);
        DXGI_OUTPUT_DESC description{}; check_hresult(output->GetDesc(&description));
        if (description.Monitor != monitor) continue;
        auto output6 = output.try_as<IDXGIOutput6>();
        require(bool(output6), "Cannot verify SDR output; capture was not started.");
        DXGI_OUTPUT_DESC1 extended{}; check_hresult(output6->GetDesc1(&extended));
        require(extended.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                "This source-only probe supports SDR displays only.");
        found = output6; break;
    }
    require(bool(found), "Selected monitor is not on the rendering adapter; cross-adapter capture is deferred.");
    return found;
}
}
struct CaptureSource::Impl {
    HWND window{};
    HMONITOR monitor{};
    RECT bounds{};
    std::wstring device_name;
    winrt::com_ptr<IDXGIOutput6> output;
    DWORD previous_affinity{};
    bool affinity_changed{};
    capture::GraphicsCaptureItem item{nullptr};
    capture::Direct3D11CaptureFramePool pool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};
    winrt::event_token closed_token{};
    bool subscribed{};
    std::shared_ptr<std::atomic<bool>> closed;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11Texture2D> crop, latest;
    RECT crop_rect{};
    UINT width{}, height{};
    unsigned long long frames{};
    long long timestamp{};
};
CaptureSource::CaptureSource() : impl_(std::make_unique<Impl>()) {}
CaptureSource::~CaptureSource() { stop(); }
void CaptureSource::start(HWND window, HMONITOR monitor, ID3D11Device* device) {
    require(stop(), "Previous display affinity could not be restored; close the probe.");
    auto& p = *impl_;
    try {
        require(winrt::Windows::Foundation::Metadata::ApiInformation::IsApiContractPresent(
            L"Windows.Foundation.UniversalApiContract", 10), "Windows 10 2004 or newer is required for capture exclusion.");
        require(capture::GraphicsCaptureSession::IsSupported(), "Windows Graphics Capture is unavailable.");
        MONITORINFOEXW info{}; info.cbSize=sizeof info;
        require(GetMonitorInfoW(monitor, &info) != FALSE, "Selected monitor is no longer available.");
        const auto w = info.rcMonitor.right - info.rcMonitor.left;
        const auto h = info.rcMonitor.bottom - info.rcMonitor.top;
        require(w > 0 && h > 0 && std::uint64_t(w) * std::uint64_t(h) <= max_pixels,
                "Probe display limit is 8,294,400 pixels (one 4K display).");
        p.device.copy_from(device); device->GetImmediateContext(p.context.put());
        auto dxgi_device = p.device.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter> adapter; check_hresult(dxgi_device->GetAdapter(adapter.put()));
        p.output = verify_sdr(adapter.get(), monitor);
        p.device_name = info.szDevice;
        p.window = window; p.monitor = monitor; p.bounds = info.rcMonitor;
        require(GetWindowDisplayAffinity(window, &p.previous_affinity) != FALSE, "Cannot read prior capture affinity.");
        require(SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE) != FALSE, "Cannot exclude this window from capture.");
        p.affinity_changed = true;
        DWORD affinity{};
        require(GetWindowDisplayAffinity(window, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE,
                "Capture exclusion could not be verified.");
        auto interop = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(p.item)));
        const auto size = p.item.Size();
        require(size.Width == w && size.Height == h, "Capture and selected monitor geometry differ.");
        winrt::com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()));
        auto runtime_device = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        p.pool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(runtime_device,
            directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        p.closed = std::make_shared<std::atomic<bool>>(false);
        p.closed_token = p.item.Closed([closed = p.closed](auto const&, auto const&) noexcept { closed->store(true); });
        p.subscribed = true;
        p.session = p.pool.CreateCaptureSession(p.item);
        p.session.IsCursorCaptureEnabled(false);
        // Leave the system capture border enabled. No borderless permission request.
        p.session.StartCapture();
    } catch (...) { stop(); throw; }
}
bool CaptureSource::update(const RECT& rect) {
    auto& p = *impl_;
    require(bool(p.session), "Capture is not active.");
    require(!p.closed->load(), "Capture source closed.");
    MONITORINFOEXW info{}; info.cbSize=sizeof info;
    require(GetMonitorInfoW(p.monitor, &info) && EqualRect(&info.rcMonitor, &p.bounds) && p.device_name == info.szDevice,
            "Monitor geometry changed; select it again explicitly.");
    DXGI_OUTPUT_DESC1 output{}; check_hresult(p.output->GetDesc1(&output));
    require(output.Monitor == p.monitor && output.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
            "Output identity/color mode changed; capture stopped.");
    require(rect.left >= p.bounds.left && rect.top >= p.bounds.top && rect.right <= p.bounds.right &&
            rect.bottom <= p.bounds.bottom && rect.right > rect.left && rect.bottom > rect.top,
            "Probe left the selected display; capture stopped. Move it fully inside and Start again.");
    bool refreshed=false;
    auto frame = p.pool.TryGetNextFrame();
    if(frame) {
        if(auto newer=p.pool.TryGetNextFrame()) {frame.Close();frame=std::move(newer);}
        try {
            const auto size=frame.ContentSize();
            require(size.Width==p.bounds.right-p.bounds.left && size.Height==p.bounds.bottom-p.bounds.top,
                    "Capture size changed; restart after display selection.");
            auto access=frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> source;
            check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),source.put_void()));
            D3D11_TEXTURE2D_DESC description{};source->GetDesc(&description);
            require(description.Format==DXGI_FORMAT_B8G8R8A8_UNORM && description.SampleDesc.Count==1 &&
                    description.Width==UINT(size.Width) && description.Height==UINT(size.Height),
                    "Unexpected capture texture format or dimensions.");
            if(!p.latest) {
                description.Usage=D3D11_USAGE_DEFAULT;description.BindFlags=0;
                description.CPUAccessFlags=0;description.MiscFlags=0;
                check_hresult(p.device->CreateTexture2D(&description,nullptr,p.latest.put()));
            }
            p.context->CopyResource(p.latest.get(),source.get());
            p.timestamp=frame.SystemRelativeTime().count();++p.frames;refreshed=true;
            source=nullptr;access=nullptr;frame.Close();
        } catch(...) {frame.Close();throw;}
    }
    if(!p.latest || (!refreshed && p.crop && EqualRect(&rect,&p.crop_rect))) return false;
    // Excluded-window movement need not produce a new desktop frame. Keep one
    // bounded GPU snapshot and recrop it, never reuse a crop from old coordinates.
    const UINT width=UINT(rect.right-rect.left),height=UINT(rect.bottom-rect.top);
    if(!p.crop || width!=p.width || height!=p.height) {
        p.crop=nullptr;
        D3D11_TEXTURE2D_DESC description{};
        description.Width=width;description.Height=height;description.MipLevels=1;description.ArraySize=1;
        description.Format=DXGI_FORMAT_B8G8R8A8_UNORM;description.SampleDesc.Count=1;
        description.Usage=D3D11_USAGE_DEFAULT;description.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        check_hresult(p.device->CreateTexture2D(&description,nullptr,p.crop.put()));
        p.width=width;p.height=height;
    }
    D3D11_BOX box{UINT(rect.left-p.bounds.left),UINT(rect.top-p.bounds.top),0,
                  UINT(rect.right-p.bounds.left),UINT(rect.bottom-p.bounds.top),1};
    p.context->CopySubresourceRegion(p.crop.get(),0,0,0,0,p.latest.get(),0,&box);
    p.crop_rect=rect;
    return true;
}
bool CaptureSource::stop() noexcept {
    auto& p = *impl_;
    if (p.subscribed) { try { p.item.Closed(p.closed_token); } catch (...) {} p.subscribed = false; }
    if (p.session) { try { p.session.Close(); } catch (...) {} p.session = nullptr; }
    if (p.pool) { try { p.pool.Close(); } catch (...) {} p.pool = nullptr; }
    p.item = nullptr; p.crop = nullptr; p.latest = nullptr; p.output = nullptr; p.device_name.clear(); p.context = nullptr; p.device = nullptr; p.closed.reset();
    p.frames = 0; p.timestamp = 0; p.width = p.height = 0;
    if (p.affinity_changed) {
        if (!SetWindowDisplayAffinity(p.window, p.previous_affinity)) return false;
        p.affinity_changed = false;
    }
    p.window = nullptr; p.monitor = nullptr;
    return true;
}
bool CaptureSource::active() const noexcept { return bool(impl_->session); }
ID3D11Texture2D* CaptureSource::texture() const noexcept { return impl_->crop.get(); }
unsigned long long CaptureSource::frames() const noexcept { return impl_->frames; }
long long CaptureSource::timestamp() const noexcept { return impl_->timestamp; }
}
