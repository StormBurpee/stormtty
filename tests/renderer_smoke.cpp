// Native Windows runtime test. Cross-compiling this file is NOT a passing test.
#include "stormtty/renderer.hpp"
#include "stormtty/cursor_motion.hpp"
#include <fstream>
#include "chrome_layer_raster.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace stormtty;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct HostWindow {
    HWND hwnd{};
    unsigned style_changes{};
    static constexpr const wchar_t* class_name = L"StormTTYRendererHostSmoke";
    static LRESULT CALLBACK dispatch(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<HostWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && message == WM_STYLECHANGING && static_cast<int>(wparam) == GWL_EXSTYLE)
            ++self->style_changes;
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    explicit HostWindow(bool no_redirection, DWORD base_style=WS_EX_TOOLWINDOW) {
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=class_name;wc.lpfnWndProc=dispatch;
        require(RegisterClassW(&wc)!=0,"Register renderer host class failed");
        hwnd=CreateWindowExW(base_style | (no_redirection ? WS_EX_NOREDIRECTIONBITMAP : 0),
            class_name,L"Renderer host smoke",WS_POPUP,0,0,96,48,nullptr,nullptr,wc.hInstance,this);
        if (!hwnd) {
            UnregisterClassW(class_name,wc.hInstance);
            throw std::runtime_error("Create hidden renderer host failed");
        }
    }
    ~HostWindow() { if(hwnd)DestroyWindow(hwnd);UnregisterClassW(class_name,GetModuleHandleW(nullptr)); }
    HostWindow(const HostWindow&)=delete;
    HostWindow& operator=(const HostWindow&)=delete;
    LONG_PTR style() const { return GetWindowLongPtrW(hwnd,GWL_EXSTYLE); }
    void add_unrelated_style() {
        SetLastError(ERROR_SUCCESS);
        const auto previous=SetWindowLongPtrW(hwnd,GWL_EXSTYLE,style() | WS_EX_ACCEPTFILES);
        require(previous!=0 || GetLastError()==ERROR_SUCCESS,"Add unrelated host style failed");
    }
};
template<class T> struct ProbeCom {
    T* value{};
    ~ProbeCom() { reset(); }
    void reset() { if(value) {value->Release();value=nullptr;} }
    ProbeCom()=default;
    ProbeCom(const ProbeCom&)=delete;
    ProbeCom& operator=(const ProbeCom&)=delete;
};
void save_motion_png(const std::filesystem::path& path,const std::vector<std::uint8_t>& rgba,UINT width,UINT height) {
    require(width && height && width<=std::numeric_limits<UINT>::max()/4 &&
        std::uint64_t(width)*height*4==rgba.size() && rgba.size()<=std::numeric_limits<UINT>::max(),
        "Motion PNG framebuffer dimensions are invalid");
    auto check=[](HRESULT hr){require(SUCCEEDED(hr),"Motion PNG encoding failed");};
    ProbeCom<IWICImagingFactory> factory; ProbeCom<IWICStream> stream;
    ProbeCom<IWICBitmapEncoder> encoder; ProbeCom<IWICBitmapFrameEncode> frame;
    check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory.value)));
    check(factory.value->CreateStream(&stream.value)); check(stream.value->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    check(factory.value->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder.value));
    check(encoder.value->Initialize(stream.value,WICBitmapEncoderNoCache));
    check(encoder.value->CreateNewFrame(&frame.value,nullptr)); check(frame.value->Initialize(nullptr));
    check(frame.value->SetSize(width,height));
    // Match NativeWindow::capture_frame: WIC PNG negotiates BGRA, not RGBA.
    // Readback is straight RGBA. Swap only R/B, preserving alpha and the original
    // capture used by the incremental-versus-full-render assertions below.
    auto pixels=rgba;
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    check(frame.value->SetPixelFormat(&format));
    if (IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA)) {
        for (std::size_t i=0;i<pixels.size();i+=4) std::swap(pixels[i],pixels[i+2]);
    } else {
        require(IsEqualGUID(format,GUID_WICPixelFormat32bppRGBA),"Motion PNG negotiated an unsupported pixel format");
    }
    check(frame.value->WritePixels(height,width*4,static_cast<UINT>(pixels.size()),pixels.data()));
    check(frame.value->Commit()); check(encoder.value->Commit());
    // Close the writer before reopening the committed PNG. Every real renderer
    // sample must decode to identical straight RGBA, not merely exist on disk.
    frame.reset(); encoder.reset(); stream.reset();
    std::ifstream input(path,std::ios::binary);
    require(input.is_open(),"Motion PNG could not be reopened");
    const std::vector<std::uint8_t> encoded{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    const auto decoded=decode_windows_png(encoded,rgba.size());
    require(decoded && decoded->width==width && decoded->height==height && decoded->rgba==rgba,
        "Motion PNG round-trip changed framebuffer pixels");
}
void cursor_motion_raster_test() {
    RendererOptions options; options.force_warp=true; options.font.family="Consolas";
    Renderer renderer(nullptr,options); renderer.resize(480,96);
    const auto metrics=renderer.metrics();
    auto f=std::make_shared<RenderFrame>(); f->columns=40; f->rows=3; f->cell_width=metrics.width; f->cell_height=metrics.height;
    f->background={23,25,29,255}; f->foreground={230,235,242,255}; f->cursor.visible=true; f->cursor.color={180,210,250,255};
    auto row=std::make_shared<FrameRow>(); row->cells.resize(40);
    const std::string text="stormtty > beautiful typing";
    for (size_t i=0;i<row->cells.size();++i) { auto& c=row->cells[i]; c.grapheme=i<text.size()?text.substr(i,1):" "; c.foreground=f->foreground; c.background=f->background; }
    f->lines.push_back(row); f->cursor.column=15;
    WindowScene scene; scene.panes.push_back({1,{8,20,464,76},f}); CursorMotion motion;
    motion.observe(scene.panes[0],100,true); motion.input(110); ++row->generation; f->cursor.column=16;
    wchar_t executable[32768]{}; require(GetModuleFileNameW(nullptr,executable,32768)>0,"Motion capture executable path missing");
    const auto directory=std::filesystem::path(executable).parent_path()/L"artifacts";
    std::filesystem::create_directories(directory);
    std::vector<std::vector<std::uint8_t>> captures;
    const std::array<std::uint64_t,7> times{120,134,148,162,176,190,205};
    const std::array<const wchar_t*,7> names{L"typing-initial-000ms.png",L"typing-intermediate-014ms.png",L"typing-intermediate-028ms.png",L"typing-intermediate-042ms.png",L"typing-intermediate-056ms.png",L"typing-intermediate-070ms.png",L"typing-settled-085ms.png"};
    for (size_t i=0;i<times.size();++i) {
        scene.panes[0].cursor_reveal=motion.observe(scene.panes[0],times[i],true);
        require(renderer.render(scene)==RenderResult::presented,"Cursor presentation damage was not rendered");
        captures.push_back(renderer.read_pixels()); save_motion_png(directory/names[i],captures.back(),480,96);
        scene.force_redraw=true; renderer.render(scene);
        require(renderer.read_pixels()==captures.back(),"Intermediate cursor damage differs from full redraw");
        scene.force_redraw=false;
    }
    require(captures[0]!=captures[2] && captures[2]!=captures.back(),"Cursor settle did not produce distinct native frames");
    scene.panes[0].cursor_reveal=1; require(renderer.render(scene)==RenderResult::unchanged,"Settled cursor keeps rendering at idle");
    // Incremental damage must be identical to a fresh full render, including inverse glyphs.
    scene.force_redraw=true; renderer.render(scene); require(renderer.read_pixels()==captures.back(),"Cursor damage left stale inverse glyph pixels");
    std::cout<<"Cursor motion captures: all 7 PNGs round-trip verified; initial 0ms, intermediate 28ms, settled 85ms; input-gated destination reveal, WARP native pixels\n";
}
void direct_composition_probe(bool initial_no_redirection, bool waitable, D3D_DRIVER_TYPE driver) {
    // Failure-only diagnostic, independent of Renderer and its dynamic style
    // guard. The enclosing HostWindow keeps this class registered. Each probe
    // uses a separate HWND/device and releases everything before the next one.
    struct ProbeWindow { HWND value{}; ~ProbeWindow(){if(value)DestroyWindow(value);} } window;
    window.value=CreateWindowExW(initial_no_redirection ? WS_EX_NOREDIRECTIONBITMAP : 0,
        HostWindow::class_name,L"DComp baseline probe",WS_POPUP,0,0,96,48,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    const char* stage="CreateWindowExW";
    const auto window_error=GetLastError();
    auto hr=window.value ? S_OK : (window_error ? HRESULT_FROM_WIN32(window_error) : E_FAIL);
    auto step=[&](const char* action,auto&& operation) {
        if(SUCCEEDED(hr)) {stage=action;hr=operation();}
    };
    ProbeCom<ID3D11Device> device;
    ProbeCom<IDXGIDevice> dxgi;
    ProbeCom<IDXGIAdapter> adapter;
    ProbeCom<IDXGIAdapter1> adapter1;
    ProbeCom<IDXGIFactory2> factory;
    ProbeCom<IDXGISwapChain1> swapchain;
    ProbeCom<IDCompositionDevice> composition;
    ProbeCom<IDCompositionTarget> target;
    ProbeCom<IDCompositionVisual> visual;
    constexpr std::array<D3D_FEATURE_LEVEL,3> levels{D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level{};
    step("D3D11CreateDevice",[&]{return D3D11CreateDevice(nullptr,driver,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels.data(),static_cast<UINT>(levels.size()),D3D11_SDK_VERSION,&device.value,&level,nullptr);});
    step("Query IDXGIDevice",[&]{return device.value->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(&dxgi.value));});
    step("GetAdapter",[&]{return dxgi.value->GetAdapter(&adapter.value);});
    DXGI_ADAPTER_DESC1 adapter_description{};bool have_adapter_description=false;
    if(adapter.value && SUCCEEDED(adapter.value->QueryInterface(__uuidof(IDXGIAdapter1),reinterpret_cast<void**>(&adapter1.value))))
        have_adapter_description=SUCCEEDED(adapter1.value->GetDesc1(&adapter_description));
    step("Get DXGI factory",[&]{return adapter.value->GetParent(__uuidof(IDXGIFactory2),reinterpret_cast<void**>(&factory.value));});
    step("DCompositionCreateDevice",[&]{return DCompositionCreateDevice(dxgi.value,__uuidof(IDCompositionDevice),reinterpret_cast<void**>(&composition.value));});
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width=96;desc.Height=48;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.Scaling=DXGI_SCALING_STRETCH;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Flags=waitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
    step("CreateSwapChainForComposition",[&]{return factory.value->CreateSwapChainForComposition(device.value,&desc,nullptr,&swapchain.value);});
    step("CreateTargetForHwnd",[&]{return composition.value->CreateTargetForHwnd(window.value,TRUE,&target.value);});
    step("CreateVisual",[&]{return composition.value->CreateVisual(&visual.value);});
    step("SetContent",[&]{return visual.value->SetContent(swapchain.value);});
    step("SetRoot",[&]{return target.value->SetRoot(visual.value);});
    step("Commit",[&]{return composition.value->Commit();});
    DWORD session{};const bool have_session=ProcessIdToSessionId(GetCurrentProcessId(),&session)!=FALSE;
    char description[512]{};
    if(have_adapter_description)
        WideCharToMultiByte(CP_UTF8,0,adapter_description.Description,-1,description,sizeof description,nullptr,nullptr);
    std::cerr<<"DComp independent baseline: initial_no_redirection="<<initial_no_redirection<<" waitable="<<waitable
             <<" driver="<<(driver==D3D_DRIVER_TYPE_WARP ? "WARP" : "HARDWARE")
             <<" stage="<<stage<<" hr=0x"<<std::hex<<static_cast<unsigned long>(hr)
             <<" exstyle=0x"<<(window.value ? GetWindowLongPtrW(window.value,GWL_EXSTYLE) : 0)
             <<" feature_level=0x"<<unsigned(level)<<" adapter_flags=0x"<<adapter_description.Flags
             <<" vendor=0x"<<adapter_description.VendorId<<" device=0x"<<adapter_description.DeviceId<<std::dec
             <<" adapter_known="<<have_adapter_description<<" adapter="<<description
             <<" session_known="<<have_session<<" session="<<session<<'\n';
}
struct ReservedCompositionTarget {
    ProbeCom<ID3D11Device> device;
    ProbeCom<IDXGIDevice> dxgi;
    ProbeCom<IDCompositionDevice> composition;
    ProbeCom<IDCompositionTarget> target;
    explicit ReservedCompositionTarget(HWND window) {
        // A real API failure fixture: a window can have only one topmost target.
        // No production failpoint and no mutation of the immutable host style.
        auto checked=[](HRESULT hr,const char* action) {
            if(FAILED(hr)) {
                std::cerr<<"Target reservation failed: "<<action<<" hr=0x"<<std::hex<<static_cast<unsigned long>(hr)<<std::dec<<'\n';
                throw std::runtime_error(action);
            }
        };
        checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device.value,nullptr,nullptr),"Reserve D3D device");
        checked(device.value->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(&dxgi.value)),"Reserve DXGI device");
        checked(DCompositionCreateDevice(dxgi.value,__uuidof(IDCompositionDevice),reinterpret_cast<void**>(&composition.value)),"Reserve DComp device");
        checked(composition.value->CreateTargetForHwnd(window,TRUE,&target.value),"Reserve topmost DComp target");
    }
    void release() { target.reset();composition.reset();dxgi.reset();device.reset(); }
};
HWND require_opaque_child(HWND host, UINT width, UINT height) {
    const auto child=GetWindow(host,GW_CHILD);
    require(child!=nullptr,"No ordinary child presentation HWND under no-redirection host");
    require(GetParent(child)==host,"Opaque child changed top-level host identity");
    require((GetWindowLongPtrW(child,GWL_EXSTYLE)&WS_EX_NOREDIRECTIONBITMAP)==0,"Opaque HWND target has no-redirection style");
    const auto style=GetWindowLongPtrW(child,GWL_STYLE);
    require((style&(WS_CHILD|WS_DISABLED|WS_VISIBLE))==(WS_CHILD|WS_DISABLED|WS_VISIBLE),"Opaque child can take input or is not visible");
    require(!IsWindowEnabled(child),"Opaque child must not own keyboard focus");
    require(SendMessageW(child,WM_NCHITTEST,0,0)==HTTRANSPARENT,"Opaque child intercepts parent chrome hit tests");
    require(SendMessageW(child,WM_MOUSEACTIVATE,0,0)==MA_NOACTIVATEANDEAT,"Opaque child can activate instead of the shell");
    require(GetDpiForWindow(child)==GetDpiForWindow(host),"Opaque child DPI context differs from parent");
    RECT bounds{};require(GetWindowRect(child,&bounds)!=FALSE,"Get opaque child bounds failed");
    POINT origin{bounds.left,bounds.top};require(ScreenToClient(host,&origin)!=FALSE,"Map opaque child bounds failed");
    require(origin.x==0 && origin.y==0 && bounds.right-bounds.left==static_cast<LONG>(width) &&
            bounds.bottom-bounds.top==static_cast<LONG>(height),"Opaque child does not cover the renderer's physical viewport");
    require(GetWindow(child,GW_HWNDNEXT)==nullptr,"Unexpected renderer-owned child siblings");
    return child;
}
void present_visible_host(Renderer& renderer,const WindowScene& scene) {
    const auto deadline=GetTickCount64()+3000;
    for(;;) {
        const auto result=renderer.render(scene);
        if(result==RenderResult::presented) return;
        require(result==RenderResult::busy || result==RenderResult::occluded,"Visible host did not have a presentable frame");
        require(GetTickCount64()<deadline,"Visible native HWND presentation remained busy/occluded");
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessageW(&message);}
        const auto handle=static_cast<HANDLE>(renderer.frame_latency_wait_handle());
        if(handle) WaitForSingleObject(handle,16); else Sleep(16);
    }
}
void native_host_surface_test() {
    // The immutable creation style, not an optimistic request, determines which
    // HWND may host DComp. Valid static DComp must initialize; it is not skipped.
    for(const bool no_redirection : {false,true}) {
        for(const bool requested_composition : {false,true}) {
            HostWindow host(no_redirection,requested_composition ? 0 : WS_EX_TOOLWINDOW);
            const auto original=host.style();const auto initial_changes=host.style_changes;
            const bool expected_composition=no_redirection && requested_composition;
            RendererOptions options;options.force_warp=true;options.composition=requested_composition;
            {
                Renderer renderer(host.hwnd,options);renderer.resize(96,48);
                std::cerr<<"Renderer immutable HWND: no_redirection="<<no_redirection
                         <<" requested="<<requested_composition<<" actual="<<renderer.stats().composition<<'\n';
                if(renderer.stats().composition!=expected_composition) {
                    direct_composition_probe(true,true,D3D_DRIVER_TYPE_WARP);
                    direct_composition_probe(true,false,D3D_DRIVER_TYPE_WARP);
                    direct_composition_probe(true,true,D3D_DRIVER_TYPE_HARDWARE);
                }
                require(renderer.stats().composition==expected_composition,"Valid creation-time composition/opaque path did not initialize");
                require(host.style()==original && host.style_changes==initial_changes,"Renderer attempted to mutate host extended styles");
                if(no_redirection && !expected_composition) require_opaque_child(host.hwnd,96,48);
                else require(GetWindow(host.hwnd,GW_CHILD)==nullptr,"Unexpected child HWND on direct host presentation");
                host.add_unrelated_style();const auto shell_changes=host.style_changes;
                WindowScene scene;renderer.render(scene);
                require(renderer.read_pixels().size()==96*48*4,"Native host surface did not render");
                const auto unblurred=renderer.read_pixels();
                for(const float sigma : {24.f,64.f,0.f}) {
                    renderer.set_additional_blur(sigma);renderer.render(scene);
                    require(!renderer.stats().host_backdrop,"WARP must never enable the hardware host blur graph");
                    require(renderer.stats().composition==expected_composition,"Blur preference changed safe fallback presentation");
                    require(renderer.read_pixels()==unblurred,"Unavailable blur altered foreground pixels or faked blur with opacity");
                }
                // A real WARP HWND deliberately fails closed, even if DComp
                // itself exists. Exact clear-alpha math is tested offscreen below.
                renderer.set_background_opacity(0);renderer.render(scene);
                const auto fallback=renderer.read_pixels();
                bool opaque=true;
                for(std::size_t i=3;i<fallback.size();i+=4) opaque=opaque && fallback[i]==255;
                require(opaque && !renderer.stats().host_backdrop,"Real WARP HWND ignored opaque fallback at zero blur/tint");
                renderer.set_background_opacity(1);renderer.render(scene);
                bool rejected=false;
                try {renderer.set_additional_blur(65);} catch(const std::invalid_argument&) {rejected=true;}
                require(rejected,"Additional blur accepted out-of-range sigma");
                renderer.recreate_device();renderer.render(scene);
                require(renderer.stats().composition==expected_composition && renderer.stats().device_recoveries==1,
                        "Native host did not reconstruct its valid presentation path");
                require(host.style()==(original|WS_EX_ACCEPTFILES) && host.style_changes==shell_changes,
                        "Recovery mutated host creation style or shell-owned flags");
                if(no_redirection && !expected_composition) require_opaque_child(host.hwnd,96,48);
            }
            require(GetWindow(host.hwnd,GW_CHILD)==nullptr,"Renderer destruction leaked an opaque child HWND");
            require(host.style()==(original|WS_EX_ACCEPTFILES),"Renderer destruction changed immutable host style");
        }
    }
    {
        HostWindow host(true);const auto identity=host.hwnd;const auto original=host.style();
        RendererOptions options;options.force_warp=true;
        Renderer renderer(host.hwnd,options);
        ReservedCompositionTarget blocker(host.hwnd);
        renderer.resize(96,48);
        require(!renderer.stats().composition,"Occupied DComp target did not trigger real opaque fallback");
        require_opaque_child(host.hwnd,96,48);
        const auto active=GetActiveWindow();ShowWindow(host.hwnd,SW_SHOWNOACTIVATE);
        require(GetActiveWindow()==active,"Opaque fallback host stole activation");
        WindowScene scene;scene.background={21,27,32,255};scene.chrome_generation=1;
        ChromePrimitive modal;modal.rect={16,12,48,24};modal.fill={190,60,110,255};scene.primitives.push_back(modal);
        present_visible_host(renderer,scene);
        auto rgba=renderer.read_pixels();const auto center=(24*96+32)*4;
        require(rgba[center]==190 && rgba[center+1]==60 && rgba[center+2]==110 && rgba[center+3]==255,
                "Opaque fallback did not render custom modal above canvas");
        require(SetWindowPos(host.hwnd,nullptr,0,0,128,80,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"Resize test host failed");
        renderer.resize(128,80,144);require_opaque_child(host.hwnd,128,80);present_visible_host(renderer,scene);
        renderer.resize(0,0);require_opaque_child(host.hwnd,0,0);
        require(renderer.render(scene)==RenderResult::suspended,"Zero-size child fallback did not suspend");
        renderer.resize(128,80,144);present_visible_host(renderer,scene);
        // Retaining the reservation forces a real opaque-to-opaque recovery.
        renderer.recreate_device();require(!renderer.stats().composition,"Reserved target disappeared during recovery");
        require_opaque_child(host.hwnd,128,80);present_visible_host(renderer,scene);
        blocker.release();renderer.recreate_device();
        require(renderer.stats().composition,"Opaque-to-DComp recovery did not bind the now-free target");
        require(GetWindow(host.hwnd,GW_CHILD)==nullptr,"DComp recovery left stale opaque child above content");
        present_visible_host(renderer,scene);
        require(host.hwnd==identity && host.style()==original,"Fallback/recovery replaced the shell HWND or changed its style");
        require(GetActiveWindow()==active,"Fallback/recovery moved focus away from the previous active window");
    }
    {
        // Parent-first teardown is permitted by app shutdown. The child may
        // already be gone when the renderer releases its swapchain/resources.
        HostWindow host(true);RendererOptions options;options.force_warp=true;options.composition=false;
        Renderer renderer(host.hwnd,options);renderer.resize(96,48);
        require_opaque_child(host.hwnd,96,48);
        require(DestroyWindow(host.hwnd)!=FALSE,"Destroy renderer host failed");host.hwnd=nullptr;
    }
}
Color pixel(const std::vector<std::uint8_t>& rgba, std::size_t width, std::size_t x, std::size_t y) {
    const auto offset=(y*width+x)*4;
    require(offset+3<rgba.size(),"pixel coordinate outside readback");
    return {rgba[offset],rgba[offset+1],rgba[offset+2],rgba[offset+3]};
}
bool colors_near(Color a, Color b, unsigned tolerance=2) {
    auto close=[tolerance](unsigned x,unsigned y) { return x>y?x-y<=tolerance:y-x<=tolerance; };
    return close(a.r,b.r)&&close(a.g,b.g)&&close(a.b,b.b)&&close(a.a,b.a);
}
std::shared_ptr<RenderFrame> frame() {
    auto result=std::make_shared<RenderFrame>();
    result->generation=1;result->columns=12;result->rows=4;result->cell_width=16;result->cell_height=24;
    result->foreground={236,238,242,255};result->background={23,25,29,255};
    for(unsigned r=0;r<4;++r) {
        auto row=std::make_shared<FrameRow>();row->id.words={42,r+1};row->generation=1;row->cells.resize(12);
        for(auto& c:row->cells) {c.foreground=result->foreground;c.background=result->background;c.underline_color=result->foreground;}
        result->lines.push_back(row);
    }
    return result;
}
void liquid_glass_off_test() {
    // App-owned offscreen pixels only: never call Start or acquire a desktop source.
    // This contract also applies when the optional capture service is stubbed out.
    RendererOptions options;options.force_warp=true;options.font.family="Consolas";
    Renderer renderer(nullptr,options);
    require(!renderer.liquid_glass_active(),"New renderer started Liquid Glass without consent");
    renderer.resize(96,48);
    WindowScene scene;scene.panes.push_back({1,{0,0,96,48},frame()});
    require(renderer.render(scene)==RenderResult::presented,"Glass-off fixture did not render");
    const auto original=renderer.read_pixels();
    require(original.size()==96*48*4,"Glass-off fixture readback unavailable");
    renderer.stop_liquid_glass();
    renderer.stop_liquid_glass(); // Inactive Stop must be safe and idempotent.
    require(!renderer.liquid_glass_active(),"Inactive Stop activated Liquid Glass");
    require(renderer.read_pixels()==original,"Inactive Stop blocked or altered app-only readback");
    renderer.render(scene);
    require(renderer.read_pixels()==original,"Inactive Stop changed normal rendering");
}
void native_render_test() {
    RendererOptions options;options.force_warp=true;options.image_cache_bytes=4096;
    Renderer renderer(nullptr,options);renderer.resize(256,160);
    require(renderer.stats().bundled_font,"Packaged Cascadia Mono upright/italic private collection was not selected");
    auto source=frame();
    auto first=std::make_shared<FrameRow>(*source->lines[0]);
    first->cells[0].grapheme="A";
    first->cells[4].background={200,30,40,255};first->cells[4].background_is_default=false;
    first->cells[5].foreground={20,190,50,255};first->cells[5].style.inverse=true;
    source->lines[0]=first;
    auto unicode=std::make_shared<FrameRow>(*source->lines[1]);
    unicode->cells[0].grapheme="e\xCC\x81"; // combining sequence, one terminal cell
    unicode->cells[1].grapheme="\xE7\x95\x8C";unicode->cells[1].span=2;unicode->cells[2].span=0;
    unicode->cells[3].grapheme="\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB";
    unicode->cells[3].span=2;unicode->cells[4].span=0;
    // Contextual Latin shaping and Arabic exercise fallback/cluster capture; this
    // test deliberately does not assert font-specific glyph counts or full bidi.
    for(unsigned i=0;i<6;++i) unicode->cells[6+i].grapheme=std::string(1,"office"[i]);
    source->lines[1]=unicode;
    auto selection=std::make_shared<FrameRow>(*source->lines[2]);selection->cells[0].selected=true;
    source->lines[2]=selection;
    auto pixels=std::make_shared<ImagePixels>();pixels->image_id=7;pixels->generation=1;pixels->width=2;pixels->height=1;
    pixels->bytes={255,0,0,255,0,80,240,255};
    ImagePlacement placement;placement.pixels=pixels;placement.column=8;placement.row=2;
    placement.pixel_width=16;placement.pixel_height=24;placement.source_x=1;placement.source_width=1;placement.source_height=1;
    placement.kind=ImagePlacementKind::virtual_placeholder; // Geometry is already resolved by the adapter.
    source->images.push_back(placement);source->image_placements_generation=1;
    WindowScene scene;scene.panes.push_back({100,{16,24,192,96},source,true,true});
    require(renderer.render(scene)==RenderResult::presented,"first offscreen frame not presented");
    auto stats=renderer.stats();require(stats.warp,"smoke test did not use WARP");require(stats.shaped_rows==4,"expected four shaped rows");
    auto rgba=renderer.read_pixels();require(rgba.size()==256*160*4,"readback size wrong");
    require(colors_near(pixel(rgba,256,16+4*16+8,24+12),{200,30,40,255}),"explicit background paint wrong");
    require(colors_near(pixel(rgba,256,16+5*16+8,24+12),{20,190,50,255}),"inverse background paint wrong");
    require(colors_near(pixel(rgba,256,24,24+2*24+12),{58,79,91,255}),"selection background paint wrong");
    require(colors_near(pixel(rgba,256,16+8*16+8,24+2*24+12),{0,80,240,255}),"cropped image placement not drawn");
    unsigned ink=0;
    for(unsigned y=24;y<48;++y) for(unsigned x=16;x<32;++x)
        if(!colors_near(pixel(rgba,256,x,y),source->background,8)) ++ink;
    require(ink>4,"DirectWrite did not rasterize visible text");
    require(renderer.render(scene)==RenderResult::unchanged,"identical scene was repainted");
    require(renderer.stats().shaped_rows==stats.shaped_rows,"identical frame reshaped text");

    auto changed=std::make_shared<RenderFrame>(*source);changed->generation++;
    auto row=std::make_shared<FrameRow>(*changed->lines[1]);row->generation++;row->cells[6].grapheme="O";
    changed->lines[1]=row;scene.panes[0].frame=changed;
    const auto before_damage=renderer.stats().dirty_rectangles;
    renderer.render(scene);
    require(renderer.stats().shaped_rows==stats.shaped_rows+1,"single-row edit reshaped unchanged rows");
    require(renderer.stats().dirty_rectangles==before_damage+1,"single-row edit did not produce one damage band");

    auto cursor_frame=std::make_shared<RenderFrame>(*changed);cursor_frame->generation++;
    cursor_frame->cursor={1,3,true,false,false,false,CursorShape::block,{220,230,240,255}};
    scene.panes[0].frame=cursor_frame;renderer.render(scene);
    const auto cursor_shapes=renderer.stats().shaped_rows;
    scene.panes[0].cursor_on=false;renderer.render(scene);
    require(renderer.stats().shaped_rows==cursor_shapes,"cursor blink reshaped unchanged rows");
    rgba=renderer.read_pixels();
    require(colors_near(pixel(rgba,256,16+24,24+3*24+12),source->background),"old cursor was not erased");

    auto animation=std::make_shared<RenderFrame>(*cursor_frame);animation->generation++;
    auto next_pixels=std::make_shared<ImagePixels>(*pixels);next_pixels->generation++;next_pixels->bytes={255,0,0,255,30,210,60,255};
    animation->images[0].pixels=next_pixels;scene.panes[0].frame=animation;renderer.render(scene);
    rgba=renderer.read_pixels();
    require(colors_near(pixel(rgba,256,16+8*16+8,24+2*24+12),{30,210,60,255}),"image animation generation used stale pixels");
    require(renderer.stats().image_uploads==2,"unchanged image was reuploaded or new frame was missed");
    require(renderer.stats().image_cache_bytes<=options.image_cache_bytes,"image cache exceeded budget");

    const auto old_metrics=renderer.metrics();renderer.resize(256,160,144);
    require(renderer.metrics().width>old_metrics.width,"DPI did not remeasure font");
    renderer.render(scene);renderer.recreate_device();renderer.render(scene);
    require(renderer.stats().device_recoveries==1,"device reconstruction not exercised");
    rgba=renderer.read_pixels();
    require(colors_near(pixel(rgba,256,16+8*16+8,24+2*24+12),{30,210,60,255}),"owned image lost after device reconstruction");
    renderer.resize(0,0);require(renderer.render(scene)==RenderResult::suspended,"zero-size surface did not suspend");
    renderer.resize(256,160,96);require(renderer.render(scene)==RenderResult::presented,"restore from zero size failed");
}
void font_override_test() {
    RendererOptions options;options.force_warp=true;options.font.family="Consolas";
    Renderer renderer(nullptr,options);renderer.resize(96,48);
    require(!renderer.stats().bundled_font,"Explicit system font override was replaced with bundled Cascadia");
    auto source=frame();WindowScene scene;scene.panes.push_back({104,{0,0,96,48},source});
    require(renderer.render(scene)==RenderResult::presented,"System font override did not render");
    options.font.family="Cascadia Mono";renderer.set_font(options.font);
    require(renderer.stats().bundled_font,"Switching back to bundled font failed");
    require(renderer.render(scene)==RenderResult::presented,"Bundled font switch did not invalidate layout");
    options.font.family="StormTTY missing font fixture";renderer.set_font(options.font);
    require(!renderer.stats().bundled_font,"Unknown user font must preserve system fallback policy");
    require(renderer.render(scene)==RenderResult::presented,"Missing user font fallback did not render");
}
void transparency_test() {
    RendererOptions options;options.force_warp=true;options.background_opacity=.9f;
    Renderer renderer(nullptr,options);renderer.resize(96,48);
    auto source=frame();source->columns=4;source->rows=1;
    auto row=std::make_shared<FrameRow>(*source->lines[0]);
    row->cells[1].background_is_default=false; // Same RGB but explicit TUI background must stay opaque.
    source->lines={row};
    WindowScene scene;scene.panes.push_back({101,{0,0,64,24},source});renderer.render(scene);
    auto rgba=renderer.read_pixels();
    require(std::abs(int(pixel(rgba,96,8,12).a)-230)<=1,"default background opacity not preserved");
    require(pixel(rgba,96,24,12).a==255,"explicit background was made translucent");
    auto changed=std::make_shared<RenderFrame>(*source);changed->generation++;
    auto changed_row=std::make_shared<FrameRow>(*row);changed_row->generation++;changed_row->cells[3].grapheme="A";
    changed->lines={changed_row};scene.panes[0].frame=changed;renderer.render(scene);
    require(std::abs(int(pixel(renderer.read_pixels(),96,8,12).a)-230)<=1,"damage repaint accumulated background alpha");
    // Unlike a real software HWND, this offscreen composition fixture retains
    // exact alpha. Exercise the same positive-to-zero renderer transition.
    renderer.set_additional_blur(24);renderer.set_additional_blur(0);
    renderer.set_background_opacity(0);renderer.render(scene);
    const auto clear=renderer.read_pixels();
    require(pixel(clear,96,8,12).a==0 && pixel(clear,96,80,36).a==0,
            "Zero blur/tint did not clear default cell and canvas alpha");
    require(pixel(clear,96,24,12).a==255,"Clear background faded explicit ANSI background");
    bool opaque_glyph=false;
    for(unsigned y=0;y<24;++y)for(unsigned x=48;x<64;++x)
        opaque_glyph=opaque_glyph || pixel(clear,96,x,y).a==255;
    require(opaque_glyph,"Clear background faded foreground glyph coverage");
    renderer.set_background_opacity(1);renderer.render(scene);
    require(pixel(renderer.read_pixels(),96,8,12).a==255,"opaque fallback not applied");
}
void chrome_material_test() {
    RendererOptions options;options.force_warp=true;options.background_opacity=.9f;
    Renderer renderer(nullptr,options);renderer.resize(128,96);
    auto source=frame();source->rows=1;source->columns=8;
    auto row=std::make_shared<FrameRow>(*source->lines[0]);row->cells[0].grapheme="M";source->lines={row};
    WindowScene scene;scene.chrome_generation=1;scene.panes.push_back({105,{0,48,128,24},source});
    ChromePrimitive band;band.rect={0,0,128,32};band.fill={27,38,48,204};band.background_replace=true;
    scene.primitives.push_back(band);
    ChromePrimitive tab;tab.rect={80,4,36,24};tab.fill={52,65,74,255};tab.radius=7;scene.primitives.push_back(tab);
    renderer.render(scene);const auto first=renderer.read_pixels();
    require(colors_near(pixel(first,128,20,16),{27,38,48,204}),"chrome material alpha compounded over canvas");
    require(colors_near(pixel(first,128,96,16),{52,65,74,255}),"active tab must stay opaque");
    require(std::abs(int(pixel(first,128,20,80).a)-230)<=1,"terminal canvas opacity changed under chrome");
    require(colors_near(pixel(first,128,80,4),{27,38,48,204}),"rounded tab corner was filled as a square");
    const auto before=renderer.stats();scene.primitives[0].fill.a=180;++scene.chrome_generation;
    renderer.render(scene);const auto second=renderer.read_pixels();
    require(std::abs(int(pixel(second,128,20,16).a)-180)<=1,"material alpha did not replace old frame alpha");
    require(renderer.stats().shaped_rows==before.shaped_rows && renderer.stats().shape_cache_hits==before.shape_cache_hits,
            "chrome-only animation touched terminal rows");
    for(unsigned y=48;y<72;++y)for(unsigned x=0;x<128;++x)
        require(pixel(first,128,x,y)==pixel(second,128,x,y),"chrome transition altered terminal pixels");
    options.composition=false;Renderer fallback(nullptr,options);fallback.resize(128,96);fallback.render(scene);
    require(pixel(fallback.read_pixels(),128,20,16).a==255,"opaque fallback left material band translucent");
    require(pixel(fallback.read_pixels(),128,20,80).a==255,"opaque fallback left canvas translucent");
}
void alpha_probe_test() {
    RendererOptions options;options.force_warp=true;options.background_opacity=.8f;
    Renderer renderer(nullptr,options);renderer.resize(96,64);
    WindowScene scene;scene.chrome_generation=1;scene.background={31,48,72,255};renderer.render(scene);
    require(!renderer.begin_alpha_probe({-1,0,8,8}),"negative probe coordinates accepted");
    require(!renderer.begin_alpha_probe({0,0,0,8}),"zero-width probe accepted");
    require(renderer.begin_alpha_probe({8,8,8000,8000}),"alpha probe did not start");
    const auto deadline=GetTickCount64()+2000;
    std::optional<RendererAlphaProbe> result;
    do { result=renderer.poll_alpha_probe();if(!result)Sleep(1); } while(!result && GetTickCount64()<deadline);
    require(result && result->samples==256,"probe failed or exceeded 16x16 bound");
    require(result->minimum==204 && result->maximum==204,"GPU alpha-only sample disagrees with requested 80 percent canvas");
    require(std::abs(result->effective_background_opacity-.8f)<.001f && result->software,"probe metadata lost actual renderer policy");
    require(!renderer.poll_alpha_probe(),"completed alpha probe kept emitting stale samples");
    require(renderer.begin_alpha_probe({8,8,8,8}),"second probe did not start");renderer.cancel_alpha_probe();
    require(!renderer.poll_alpha_probe(),"cancelled alpha probe returned stale data");
    renderer.set_background_opacity(1);renderer.render(scene);require(renderer.begin_alpha_probe({8,8,8,8}),"opaque probe did not start");
    const auto opaque_deadline=GetTickCount64()+2000;
    do { result=renderer.poll_alpha_probe();if(!result)Sleep(1); } while(!result && GetTickCount64()<opaque_deadline);
    require(result && result->minimum==255 && result->maximum==255,"opaque GPU canvas misreported translucent");
    require(renderer.begin_alpha_probe({8,8,8,8}),"recovery probe did not start");renderer.recreate_device();
    require(!renderer.poll_alpha_probe(),"device recreation retained invalid GPU probe");
}
void chrome_alignment_test() {
    RendererOptions options;options.font.family="Consolas";
    Renderer renderer(nullptr,options);
    for(const float dpi:{96.f,192.f}) {
        renderer.resize(256,160,dpi);
        WindowScene scene;scene.background={0,0,0,255};scene.chrome_generation=1;
        for(unsigned row=0;row<4;++row) {
            ChromeText text;text.rect={12,float(row*40),220,36};text.text="Save";text.color={255,255,255,255};
            text.alignment=row==1?ChromeTextAlignment::center:row==2?ChromeTextAlignment::trailing:ChromeTextAlignment::leading;
            scene.texts.push_back(text);
        }
        renderer.render(scene);const auto pixels=renderer.read_pixels();
        std::array<unsigned,4> left{256,256,256,256},right{};
        for(unsigned row=0;row<4;++row) for(unsigned y=row*40;y<row*40+36;++y) for(unsigned x=0;x<256;++x) {
            if(pixels[(y*256+x)*4]>80) {left[row]=std::min(left[row],x);right[row]=std::max(right[row],x);}
        }
        require(left[0]<24 && right[0]>left[0],"Leading chrome has no readable label");
        require(left[1]>left[0]+40 && left[2]>left[1]+40,"Centered/trailing labels remained leading");
        require(std::abs(int(left[1]+right[1])-244)<=6,"Button text is not horizontally centered");
        require(left[3]==left[0] && right[3]==right[0],"Centered button contaminated subsequent leading label");
    }
}
void chrome_ellipsis_and_opacity_test() {
    RendererOptions options;options.force_warp=true;options.font.family="Consolas";options.font.size_dip=20;
    options.background_opacity=0;
    Renderer renderer(nullptr,options);renderer.resize(192,48);
    WindowScene scene;scene.chrome_generation=1;
    ChromeText label;label.rect={8,4,float(renderer.metrics().width)*6,36};label.family="Consolas";label.size_dip=20;
    label.text="MMMMMMMMMMMMMMMMMMMM";label.color={255,255,255,255};scene.texts.push_back(label);
    renderer.render(scene);const auto truncated=renderer.read_pixels();
    scene.texts[0].text="MMMMM\xE2\x80\xA6";++scene.chrome_generation;
    renderer.render(scene);const auto reference=renderer.read_pixels();
    unsigned differences=0;
    for(std::size_t i=0;i<reference.size();++i)if(std::abs(int(reference[i])-int(truncated[i]))>2)++differences;
    require(differences<=16,"long chrome label did not match native end-ellipsis layout");
    std::uint8_t opaque=0;for(std::size_t i=3;i<reference.size();i+=4)opaque=std::max(opaque,reference[i]);
    require(opaque>200,"ellipsis fixture had no visible text ink");
    scene.texts[0].color.a=128;++scene.chrome_generation;renderer.render(scene);auto faded=renderer.read_pixels();
    std::uint8_t maximum=0;for(std::size_t i=3;i<faded.size();i+=4)maximum=std::max(maximum,faded[i]);
    require(maximum<=129 && maximum>60,"finite text opacity did not scale all glyph coverage");
    scene.texts[0].color.a=0;++scene.chrome_generation;renderer.render(scene);faded=renderer.read_pixels();
    for(std::size_t i=3;i<faded.size();i+=4)require(faded[i]==0,"completed hidden palette left text pixels behind");
    const auto presents=renderer.stats().presents;
    require(renderer.render(scene)==RenderResult::unchanged && renderer.stats().presents==presents,
            "settled palette continued rendering unchanged frames");
}
void cache_pressure_test() {
    RendererOptions options;options.force_warp=true;options.image_cache_bytes=8;options.shape_cache_bytes=512;
    Renderer renderer(nullptr,options);renderer.resize(96,64);
    auto source=frame();source->rows=1;source->columns=4;
    auto red=std::make_shared<ImagePixels>();red->image_id=1;red->generation=1;red->width=2;red->height=1;
    red->bytes={240,20,30,255,240,20,30,255};
    auto blue=std::make_shared<ImagePixels>(*red);blue->image_id=2;blue->bytes={20,30,240,255,20,30,240,255};
    ImagePlacement a;a.pixels=red;a.column=-1;a.pixel_width=32;a.pixel_height=24;
    ImagePlacement b;b.pixels=blue;b.column=1;b.pixel_width=16;b.pixel_height=24;
    source->images={a,b};source->image_placements_generation=1;
    WindowScene scene;scene.background={100,110,120,255};scene.panes.push_back({103,{16,16,64,24},source});
    renderer.render(scene);const auto rgba=renderer.read_pixels();
    require(colors_near(pixel(rgba,96,8,24),scene.background),"negative image placement escaped pane clip");
    require(colors_near(pixel(rgba,96,24,24),{240,20,30,255}),"evicted texture lost in-flight drawing reference");
    require(colors_near(pixel(rgba,96,40,24),{20,30,240,255}),"second cache-pressure image not drawn");
    require(renderer.stats().image_uploads==2 && renderer.stats().image_cache_bytes<=8,"image eviction budget failed");
    require(renderer.stats().shape_cache_bytes<=512,"shape cache budget failed");
}
void chrome_notice_measurement_test() {
    RendererOptions options; options.force_warp=true;
    Renderer renderer(nullptr,options); renderer.resize(800,600,96);
    ChromeText label; label.text="build · Command finished"; label.size_dip=12; label.wrap=true; label.rect={0,0,536,500};
    const auto compact=renderer.measure_chrome_text(label);
    require(compact.width>100 && compact.width<220 && compact.height>0 && compact.height<24,"short notice uses real chrome-font measurement");
    label.text="A long process diagnostic must remain readable and wrap within the active workspace rather than spanning the navigation rail. ";
    label.text+=label.text; label.rect.width=160;
    const auto wrapped=renderer.measure_chrome_text(label);
    require(wrapped.height>compact.height*2 && wrapped.width<=161,"long notice wraps to measured width");
    WindowScene scene; scene.texts.push_back(label); renderer.render(scene);
    const auto pixels=renderer.read_pixels(); unsigned ink_rows{};
    for(unsigned y=0;y<500;++y) {
        bool ink{};
        for(unsigned x=0;x<160;++x) if(!colors_near(pixel(pixels,800,x,y),scene.background,16)) { ink=true; break; }
        if(ink)++ink_rows;
        require(colors_near(pixel(pixels,800,180,y),scene.background),"wrapped notice escaped its text clip");
    }
    require(ink_rows>2*compact.height,"wrapped notice did not rasterize multiple readable lines");
    label.text="Approval required · 1 pending · PageDown for scope\nRead source files\nRead-only fixture scope: src/platform/windows/window.cpp";label.rect.width=600;
    const auto approval_lines=renderer.wrap_chrome_text(label);
    require(approval_lines.size()==3,"native approval scope wasted available line width");
    require(approval_lines.back()=="Read-only fixture scope: src/platform/windows/window.cpp","native scope split an ordinary path unnecessarily");
    label.text="Unicode café 文件 😀 and a longer path src/platform/windows/window.cpp";label.rect.width=160;
    const auto unicode_lines=renderer.wrap_chrome_text(label);std::string reconstructed;
    for(const auto& line:unicode_lines){reconstructed+=line;auto probe=label;probe.text=line;probe.wrap=false;require(renderer.measure_chrome_text(probe).width<=161,"native wrapped line exceeds measured width");}
    require(reconstructed==label.text,"native wrapping lost or split Unicode evidence");
    label.text.clear();for(unsigned n=0;n<80;++n)label.text+="e\xcc\x81";
    label.rect.width=160;const auto clusters=renderer.wrap_chrome_text(label);reconstructed.clear();
    for(const auto& line:clusters) {
        require(!line.starts_with("\xcc\x81"),"native context wrapping split a combining cluster");
        reconstructed+=line;
    }
    require(reconstructed==label.text,"native context wrapping lost combining evidence");
    renderer.resize(1200,900,144); label.text="build · Command finished"; label.rect.width=804;
    const auto scaled=renderer.measure_chrome_text(label);
    require(std::abs(scaled.width/1.5f-compact.width)<1 && std::abs(scaled.height/1.5f-compact.height)<1,"notice measurement respects DPI");
}
void rejected_image_test() {
    RendererOptions options;options.force_warp=true;options.image_cache_bytes=4;
    Renderer renderer(nullptr,options);renderer.resize(64,48);
    auto source=frame();auto pixels=std::make_shared<ImagePixels>();pixels->width=8193;pixels->height=8193;
    ImagePlacement image;image.pixels=pixels;image.pixel_width=16;image.pixel_height=16;
    source->images.push_back(image);
    WindowScene scene;scene.panes.push_back({102,{0,0,64,48},source});renderer.render(scene);
    require(renderer.stats().image_uploads==0,"oversized malformed image was uploaded");
    require(renderer.stats().image_cache_bytes==0,"rejected image consumed cache budget");
}
void final_teardown_test() {
    unsigned cases=0;
    for(bool composition : {false,true}) for(bool hwnd_first : {false,true}) for(unsigned frames : {1u,16u,64u}) {
        HostWindow host(true);
        RendererOptions options;options.force_warp=true;options.composition=composition;
        options.smoke_teardown_diagnostics=true;
        auto renderer=std::make_unique<Renderer>(host.hwnd,options);
        renderer->resize(640,400);ShowWindow(host.hwnd,SW_SHOWNOACTIVATE);
        WindowScene scene;scene.panes.push_back({101,{0,64,600,300},frame()});
        ChromeText text;text.rect={16,8,600,40};text.text="Settings lifecycle: Unicode café 文件";scene.texts.push_back(text);
        std::cerr<<"Final teardown case begin: composition="<<composition<<" hwnd_first="<<hwnd_first<<" frames="<<frames<<std::endl;
        for(unsigned i=0;i<frames;++i) {
            ++scene.chrome_generation;scene.background={static_cast<std::uint8_t>(23+i%16),25,29,255};
            renderer->set_background_opacity(i%2?0.f:.25f);
            renderer->render(scene);
            if(i%8==0) {
                require(renderer->read_pixels().size()==640*400*4,"Final teardown fixture readback failed");
                renderer->resize(800,500);renderer->render(scene);renderer->resize(640,400);
                FontOptions font;font.size_dip=14+float(i%3);renderer->set_font(font);
            }
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if(message.message!=WM_QUIT) {TranslateMessage(&message);DispatchMessageW(&message);}
            }
        }
        if(hwnd_first) {
            renderer->prepare_window_destruction();
            renderer->prepare_window_destruction(); // Explicit preparation is idempotent.
            require(renderer->frame_latency_wait_handle()==nullptr && !renderer->stats().composition,
                    "Prepared renderer retained presentation ownership");
            require(DestroyWindow(host.hwnd)!=FALSE,"Destroy app-order HWND failed");host.hwnd=nullptr;
        }
        renderer.reset();
        if(host.hwnd) {require(GetWindow(host.hwnd,GW_CHILD)==nullptr,"Final teardown leaked opaque child");}
        std::cerr<<"Final teardown case complete: composition="<<composition<<" hwnd_first="<<hwnd_first<<" frames="<<frames<<std::endl;
        ++cases;
    }
    require(cases==12,"Final teardown fixture matrix incomplete");
    std::cout<<"PASS renderer final teardown: 12/12 cases, opaque/composition, prepared-HWND-first/renderer-first, 1/16/64 frames"<<std::endl;
}

} // namespace
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string_view(argv[1])=="--final-teardown") {final_teardown_test();return 0;}
        if(argc!=1) throw std::runtime_error("Unknown renderer test arguments");
        // Asset resolution must follow the executable, not the process/shell CWD.
        const auto previous_directory=std::filesystem::current_path();
        struct RestoreDirectory {std::filesystem::path path;~RestoreDirectory(){std::error_code ignored;std::filesystem::current_path(path,ignored);}} restore{previous_directory};
        std::filesystem::current_path(std::filesystem::temp_directory_path());
        RendererOptions layer_options;layer_options.force_warp=true;layer_options.font.family="Consolas";
        stormtty::test::chrome_layer_raster_tests(layer_options);
        stormtty::test::chrome_viewport_raster_tests(layer_options);
        wchar_t executable[32768]{};require(GetModuleFileNameW(nullptr,executable,32768)>0,"Settings capture executable path missing");
        const auto captures=std::filesystem::path(executable).parent_path()/L"artifacts";std::filesystem::create_directories(captures);
        stormtty::test::settings_viewport_raster_tests(layer_options,[&](std::string_view name,unsigned width,unsigned height,const auto& pixels) {
            save_motion_png(captures/std::string(name),pixels,width,height);
        });
        cursor_motion_raster_test();
        native_host_surface_test();liquid_glass_off_test();native_render_test();font_override_test();transparency_test();chrome_material_test();chrome_ellipsis_and_opacity_test();chrome_alignment_test();alpha_probe_test();cache_pressure_test();rejected_image_test();chrome_notice_measurement_test();
        std::cout<<"PASS Windows WARP renderer: HWND/DComp style lifecycle and fallback, private Cascadia/overrides, pixels, Unicode path, damage/cache, images, alpha, DPI, recovery\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL renderer smoke: "<<error.what()<<'\n';return 1; }
}
