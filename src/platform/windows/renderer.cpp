#include "stormtty/renderer.hpp"
#include "stormtty/scene_damage.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_3.h>
#include <dcomp.h>
#include "smoke_teardown_trace.hpp"
#include "com_owner.hpp"
#include "graphics_error.hpp"
#include "text_resources.hpp"
#include "image_cache.hpp"
#include "chrome_painter.hpp"
#include "paint_surface.hpp"
#include "host_backdrop.hpp"
#include "liquid_glass/session.hpp"
#include "stormtty/build_features.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace stormtty {
namespace {
using windows::Com;
using windows::GraphicsError;
using windows::check;
using windows::GlyphCluster;
using windows::ShapedRow;

struct Apartment {
    HRESULT hr{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    bool smoke{};
    ~Apartment() {
        windows::smoke_teardown_trace(smoke,"renderer apartment unwind begin");
        if (SUCCEEDED(hr)) CoUninitialize();
        windows::smoke_teardown_trace(smoke,"renderer apartment unwind end");
    }
};
struct NativeHandle {
    HANDLE value{};
    ~NativeHandle() { reset(); }
    void reset(HANDLE v = nullptr) noexcept { if (value) CloseHandle(value); value = v; }
};
HRESULT win32_error() noexcept {
    const auto error=GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}
LONG_PTR extended_style(HWND window) {
    SetLastError(ERROR_SUCCESS);
    const auto style=GetWindowLongPtrW(window,GWL_EXSTYLE);
    const auto error=GetLastError();
    if(!style && error) check(HRESULT_FROM_WIN32(error),"Read renderer host extended style");
    return style;
}
// NOREDIRECTIONBITMAP is a creation-time choice, not a mutable style. Keep the
// shell HWND stable and give opaque fallback its own ordinary presentation HWND.
// This child is never an input/focus owner; all chrome and modals remain pixels
// in the same renderer scene and their input stays with the parent shell.
class OpaquePresentationWindow {
public:
    ~OpaquePresentationWindow() { reset(); }
    OpaquePresentationWindow()=default;
    OpaquePresentationWindow(const OpaquePresentationWindow&)=delete;
    OpaquePresentationWindow& operator=(const OpaquePresentationWindow&)=delete;
    HWND get() const noexcept { return window_; }
    void reset() noexcept {
        if(const auto window=std::exchange(window_,nullptr); window && IsWindow(window)) DestroyWindow(window);
    }
    void create(HWND parent, UINT width, UINT height) {
        constexpr const wchar_t* class_name=L"StormTTY.Renderer.OpaqueSurface.v1";
        static const bool registered=[] {
            WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);
            wc.lpszClassName=L"StormTTY.Renderer.OpaqueSurface.v1";wc.lpfnWndProc=dispatch;
            if(!RegisterClassW(&wc)) {
                const auto error=GetLastError();
                WNDCLASSW existing{};
                if(error!=ERROR_CLASS_ALREADY_EXISTS || !GetClassInfoW(wc.hInstance,wc.lpszClassName,&existing) ||
                   existing.lpfnWndProc!=dispatch)
                    check(error ? HRESULT_FROM_WIN32(error) : E_FAIL,"Register opaque presentation window");
            }
            return true;
        }();
        (void)registered;
        window_=CreateWindowExW(WS_EX_NOPARENTNOTIFY,class_name,L"",WS_CHILD|WS_VISIBLE|WS_DISABLED,
            0,0,static_cast<int>(width),static_cast<int>(height),parent,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!window_) check(win32_error(),"Create opaque presentation child");
        if(extended_style(window_) & WS_EX_NOREDIRECTIONBITMAP)
            throw std::runtime_error("Opaque presentation child unexpectedly lacks redirection");
    }
    void resize(UINT width, UINT height) {
        if(window_ && !SetWindowPos(window_,HWND_TOP,0,0,static_cast<int>(width),static_cast<int>(height),
            SWP_NOACTIVATE|SWP_NOOWNERZORDER))
            check(win32_error(),"Resize opaque presentation child");
    }
private:
    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        switch(message) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATEANDEAT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT paint{};BeginPaint(window,&paint);EndPaint(window,&paint);return 0; }
        default: return DefWindowProcW(window,message,wparam,lparam);
        }
    }
    HWND window_{};
};
bool device_lost(HRESULT hr) noexcept {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}
using windows::paint_color;
using windows::paint_rect;
using Clip = windows::PaintClip;
using render_geometry::intersect;
using render_geometry::nonempty;
using render_geometry::overlaps;
constexpr Color selection_background{58,79,91,255};
constexpr Color selection_foreground{236,238,242,255};
} // namespace

struct Renderer::Impl {
    Apartment apartment;
    windows::HostBackdrop host_backdrop;
    windows::liquid_glass::Session liquid_glass;
    Com<ID2D1Bitmap1> glass_bitmap;
    Com<ID2D1BitmapBrush1> glass_brush;
    ID3D11Texture2D* glass_key{}; // Identity only; bitmap owns the surface while used.
    std::uint64_t glass_generation{};
    bool glass_paint{},glass_canvas_tainted{};

    float additional_blur{};
    HWND window{};
    OpaquePresentationWindow opaque_window;
    // Font/cache settings below are construction inputs only. TextResources
    // owns the active font and retained-shape policy after initialization.
    RendererOptions options;
    RendererStats counters;
    UINT width{}, height{};
    float dpi{96};
    std::uint64_t tick{};
    bool pending_present{};
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> immediate;
    Com<IDXGISwapChain1> swapchain;
    Com<IDXGISwapChain2> swapchain2;
    NativeHandle latency;
    Com<IDCompositionDevice> composition;
    Com<IDCompositionTarget> composition_target;
    Com<IDCompositionVisual> composition_visual;
    Com<ID2D1Factory1> d2d_factory;
    Com<ID2D1Device> d2d_device;
    Com<ID2D1DeviceContext> dc;
    Com<ID2D1DeviceContext4> dc4;
    // Created after validation; survives GPU recreation. This slot preserves
    // DirectWrite destruction after GPU release and before the COM apartment.
    std::unique_ptr<windows::TextResources> text;
    Com<ID3D11Texture2D> canvas_texture, alpha_staging;
    UINT alpha_width{}, alpha_height{};
    RendererAlphaProbe alpha_metadata{};
    Com<ID2D1Bitmap1> canvas;
    Com<ID2D1SolidColorBrush> brush;
    std::unique_ptr<windows::ImageCache> image_cache;
    SceneDamageTracker scene_damage;

    Impl(void* hwnd, RendererOptions opts) : window(static_cast<HWND>(hwnd)), options(std::move(opts)) {
        if (FAILED(apartment.hr) && apartment.hr != RPC_E_CHANGED_MODE) check(apartment.hr, "COM initialization");
        windows::TextResources::validate_font(options.font);
        if (!options.maximum_image_dimension || options.maximum_image_dimension > 8192 ||
            options.shape_cache_bytes > 256u * 1024u * 1024u || options.image_cache_bytes > 512u * 1024u * 1024u)
            throw std::invalid_argument("Renderer cache or image dimensions exceed safety limits");
        if (!std::isfinite(options.background_opacity)) throw std::invalid_argument("Invalid background opacity");
        options.background_opacity = std::clamp(options.background_opacity, 0.0f, 1.0f);
        text = std::make_unique<windows::TextResources>(options.shape_cache_bytes);
        image_cache = std::make_unique<windows::ImageCache>(windows::ImageCacheOptions{
            options.image_cache_bytes,
            options.maximum_image_dimension});
        D2D1_FACTORY_OPTIONS factory_options{};
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                &factory_options, reinterpret_cast<void**>(d2d_factory.put())), "D2D1CreateFactory");
        create_device();
        text->configure(options.font, dpi);
    }
    bool tracing_teardown{};
    ~Impl() {
        tracing_teardown=options.smoke_teardown_diagnostics;
        apartment.smoke=tracing_teardown;
        if (tracing_teardown) {
#define TRACE_RELEASE(member) member.teardown_label="renderer " #member " Release"
            TRACE_RELEASE(device); TRACE_RELEASE(immediate); TRACE_RELEASE(swapchain);
            TRACE_RELEASE(swapchain2); TRACE_RELEASE(composition); TRACE_RELEASE(composition_target);
            TRACE_RELEASE(composition_visual); TRACE_RELEASE(d2d_factory); TRACE_RELEASE(d2d_device);
            TRACE_RELEASE(dc); TRACE_RELEASE(dc4);
            text->enable_teardown_trace();
            TRACE_RELEASE(canvas_texture); TRACE_RELEASE(alpha_staging); TRACE_RELEASE(canvas); TRACE_RELEASE(brush);
#undef TRACE_RELEASE
        }
        windows::smoke_teardown_trace(tracing_teardown,"renderer release_gpu begin");
        release_gpu(true);
        windows::smoke_teardown_trace(tracing_teardown,"renderer release_gpu end; implicit cache/font members begin");
    }
    void clear_and_flush() noexcept {
        if (!immediate) return;
        windows::smoke_teardown_trace(tracing_teardown,"renderer ClearState begin");
        immediate->ClearState();
        windows::smoke_teardown_trace(tracing_teardown,"renderer ClearState end; Flush begin");
        immediate->Flush();
        windows::smoke_teardown_trace(tracing_teardown,"renderer Flush end");
    }
    void stop_glass(const char* reason) noexcept {
        glass_paint=false;glass_key=nullptr;glass_brush.reset();glass_bitmap.reset();
        liquid_glass.stop(reason);scene_damage.invalidate();
    }
    bool update_glass() {
        if(!liquid_glass.active())return false;
        if(dc)dc->SetTarget(nullptr);
        auto* texture=liquid_glass.background(dpi/96.f);
        try {
            if(texture!=glass_key) {
                glass_brush.reset();glass_bitmap.reset();glass_key=nullptr;
                if(texture) {
                    Com<IDXGISurface> surface;
                    check(texture->QueryInterface(__uuidof(IDXGISurface),reinterpret_cast<void**>(surface.put())),"Glass surface");
                    const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
                    check(dc->CreateBitmapFromDxgiSurface(surface.get(),&properties,glass_bitmap.put()),"Glass bitmap");
                    const auto bitmap_properties=D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                    check(dc->CreateBitmapBrush(glass_bitmap.get(),bitmap_properties,D2D1::BrushProperties(),glass_brush.put()),"Glass brush");
                    glass_key=texture;
                }
            }
        }catch(...) {stop_glass("Experimental glass surface failed; original material restored.");}
        const bool changed=glass_generation!=liquid_glass.generation();
        glass_generation=liquid_glass.generation();if(changed)scene_damage.invalidate();
        return changed;
    }
    void release_swapchain(bool replacing = true) noexcept {
        stop_glass("Stopped for renderer surface replacement. Start again explicitly in Settings.");
        host_backdrop.detach();counters.host_backdrop=false;
        composition_visual.reset(); composition_target.reset(); composition.reset();
        windows::smoke_teardown_trace(tracing_teardown,"renderer latency handle close begin");
        latency.reset();
        windows::smoke_teardown_trace(tracing_teardown,"renderer latency handle close end");
        swapchain2.reset(); swapchain.reset();
        // Only replacement needs synchronous deferred-resource destruction.
        // Final shutdown releases the immediate context and device next; forcing
        // ClearState after a destroyed HWND can stall the WARP presentation path.
        if(replacing) clear_and_flush();
        else windows::smoke_teardown_trace(tracing_teardown,"renderer final device release; no swapchain replacement drain");
        windows::smoke_teardown_trace(tracing_teardown,"renderer opaque HWND destruction begin");
        opaque_window.reset();
        windows::smoke_teardown_trace(tracing_teardown,"renderer opaque HWND destruction end");
        counters.composition = false;
    }
    void finish_submitted_work() noexcept {
        if (!device || !immediate) return;
        // Flush alone only submits work. Keep all D2D/canvas/presentation
        // owners alive until the final submitted work completes, rather than
        // entering WARP deferred destruction with commands still outstanding.
        windows::smoke_teardown_trace(tracing_teardown,"renderer completion query begin");
        Com<ID3D11Query> completion;
        const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT,0};
        auto result=device->CreateQuery(&description,completion.put());
        if (SUCCEEDED(result)) {
            immediate->End(completion.get());
            immediate->Flush();
            const auto deadline=GetTickCount64()+2000;
            do {
                result=immediate->GetData(completion.get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if(result!=S_FALSE) break;
                Sleep(1);
            } while(GetTickCount64()<deadline);
        }
        windows::smoke_teardown_trace(tracing_teardown,result==S_OK ?
            "renderer completion query completed" : "renderer completion query incomplete; normal ownership release follows");
        // No detached/leaked device on timeout or removal. The native outer
        // deadline still fails qualification if normal driver release stalls.
    }
    void release_gpu(bool final_shutdown = false) noexcept {
        stop_glass("Stopped for graphics device replacement. Start again explicitly in Settings.");
        if(final_shutdown) finish_submitted_work();
        windows::smoke_teardown_trace(tracing_teardown,"renderer SetTarget null begin");
        if (dc) dc->SetTarget(nullptr);
        windows::smoke_teardown_trace(tracing_teardown,"renderer SetTarget null end");
        alpha_staging.reset(); canvas.reset(); canvas_texture.reset(); brush.reset();
        windows::smoke_teardown_trace(tracing_teardown,"renderer image cache clear begin");
        image_cache->clear();
        windows::smoke_teardown_trace(tracing_teardown,"renderer image cache clear end");
        // Release every D2D owner before the swapchain/context/device. Device
        // recovery still needs the documented flip-chain replacement drain;
        // A live caller-owned HWND may be reused even after this Renderer dies,
        // so retain the replacement drain there. Only a destroyed HWND (or no
        // HWND) makes final release unambiguously non-replacing.
        dc4.reset(); dc.reset(); d2d_device.reset();
        const bool replacing=!final_shutdown || (window && IsWindow(window));
        release_swapchain(replacing);
        immediate.reset(); device.reset();
    }
    void create_device() {
        D3D_FEATURE_LEVEL level{};
        constexpr std::array<D3D_FEATURE_LEVEL, 3> levels{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        auto create = [&](D3D_DRIVER_TYPE type) {
            return D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
                                    device.put(), &level, immediate.put());
        };
        auto hr = options.force_warp ? E_FAIL : create(D3D_DRIVER_TYPE_HARDWARE);
        counters.warp = FAILED(hr);
        if (FAILED(hr)) { device.reset(); immediate.reset(); hr = create(D3D_DRIVER_TYPE_WARP); }
        check(hr, "D3D11CreateDevice hardware/WARP");
        Com<IDXGIDevice> dxgi_device;
        check(device.as(dxgi_device), "Query IDXGIDevice");
        check(d2d_factory->CreateDevice(dxgi_device.get(), d2d_device.put()), "D2D CreateDevice");
        // D2D's internal caches are separate from our explicit image/shape caches.
        // This is a soft purge threshold (D2D may exceed it within one frame).
        d2d_device->SetMaximumTextureMemory(32u*1024u*1024u);
        Com<ID2D1Device4> d2d4;
        if(SUCCEEDED(d2d_device.as(d2d4))) d2d4->SetMaximumColorGlyphCacheMemory(16u*1024u*1024u);
        check(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()), "D2D CreateDeviceContext");
        dc.as(dc4);
        counters.color_fonts = bool(dc4) && text->supports_color_glyphs();
        dc->SetDpi(96, 96); // All geometry and fonts are already physical pixels.
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        check(dc->CreateSolidColorBrush(D2D1_COLOR_F{1,1,1,1}, brush.put()), "Create brush");
        if (width && height) create_surfaces();
    }
    void create_surfaces() {
        if (!width || !height) return;
        try {
        if (window && !swapchain) {
            Com<IDXGIDevice> dxgi_device; check(device.as(dxgi_device), "Query DXGI device");
            Com<IDXGIAdapter> adapter; check(dxgi_device->GetAdapter(adapter.put()), "Get adapter");
            Com<IDXGIFactory2> factory;
            check(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put())), "Get DXGI factory");
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2; desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            counters.composition = false;
            const bool no_redirection=(extended_style(window) & WS_EX_NOREDIRECTIONBITMAP)!=0;
            // Never compose translucent pixels above an ordinary backing surface.
            // An ordinary caller-provided HWND safely selects the opaque path.
            if(options.composition && no_redirection && additional_blur>0 && !counters.warp) {
                desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
                if(SUCCEEDED(factory->CreateSwapChainForComposition(device.get(),&desc,nullptr,swapchain.put())) &&
                    host_backdrop.attach(window,swapchain.get(),additional_blur,width,height)) {
                    counters.composition=true;counters.host_backdrop=true;
                } else {host_backdrop.detach();swapchain.reset();}
            }
            if (options.composition && no_redirection && !swapchain) {
                desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
                const char* stage = "DCompositionCreateDevice";
                auto hr = DCompositionCreateDevice(dxgi_device.get(), __uuidof(IDCompositionDevice),
                                                  reinterpret_cast<void**>(composition.put()));
                auto step = [&](const char* action, auto&& operation) {
                    if (SUCCEEDED(hr)) { stage = action; hr = operation(); }
                };
                step("CreateSwapChainForComposition", [&] { return factory->CreateSwapChainForComposition(device.get(), &desc, nullptr, swapchain.put()); });
                step("CreateTargetForHwnd", [&] { return composition->CreateTargetForHwnd(window, TRUE, composition_target.put()); });
                step("CreateVisual", [&] { return composition->CreateVisual(composition_visual.put()); });
                step("SetContent", [&] { return composition_visual->SetContent(swapchain.get()); });
                step("SetRoot", [&] { return composition_target->SetRoot(composition_visual.get()); });
                step("Commit", [&] { return composition->Commit(); });
                counters.composition = SUCCEEDED(hr);
                if (!counters.composition) {
                    SetLastError(ERROR_SUCCESS);
                    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
                    const auto style_error = GetLastError();
                    char diagnostic[320]{};
                    std::snprintf(diagnostic, sizeof diagnostic,
                        "StormTTY renderer: DComp fallback stage=%s hr=0x%08lx warp=%u exstyle=0x%08lx style_error=%lu visible=%u size=%ux%u\n",
                        stage, static_cast<unsigned long>(hr), unsigned(counters.warp),
                        static_cast<unsigned long>(style), static_cast<unsigned long>(style_error),
                        unsigned(IsWindowVisible(window)!=FALSE), width, height);
                    OutputDebugStringA(diagnostic);
                    std::fputs(diagnostic, stderr);
                    release_swapchain();
                }
            }
            if (!swapchain) {
                // A no-redirection host cannot be switched back at runtime. The
                // fallback target is an ordinary disabled child, not that host.
                HWND presentation_window=window;
                if(no_redirection) {
                    opaque_window.create(window,width,height);
                    presentation_window=opaque_window.get();
                }
                desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                check(factory->CreateSwapChainForHwnd(device.get(), presentation_window, &desc, nullptr, nullptr, swapchain.put()), "Create HWND swapchain");
            }
            factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
            check(swapchain.as(swapchain2), "Query swapchain2");
            check(swapchain2->SetMaximumFrameLatency(1), "Set frame latency");
            latency.reset(swapchain2->GetFrameLatencyWaitableObject());
            if (!latency.value) throw std::runtime_error("Swapchain did not provide frame latency handle");
        }
        opaque_window.resize(width,height);
        if(counters.host_backdrop && !host_backdrop.update(additional_blur,width,height)) {
            release_swapchain();
            // Failed graph update must not leave an invisible presentation. Use
            // the already-qualified ordinary renderer path for this generation.
            const auto requested=additional_blur;additional_blur=0;
            try {create_surfaces();} catch(...) {additional_blur=requested;throw;}
            additional_blur=requested;return;
        }
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = width; texture.Height = height; texture.MipLevels = 1; texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM; texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT; texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&texture, nullptr, canvas_texture.put()), "Create persistent canvas");
        Com<IDXGISurface> surface; check(canvas_texture.as(surface), "Query canvas DXGI surface");
        const D2D1_BITMAP_PROPERTIES1 properties{{DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED},96,96,
                                                D2D1_BITMAP_OPTIONS_TARGET,nullptr};
        check(dc->CreateBitmapFromDxgiSurface(surface.get(), &properties, canvas.put()), "Create canvas bitmap");
        dc->SetTarget(canvas.get()); scene_damage.invalidate(); pending_present = false;
        } catch (...) {
            dc->SetTarget(nullptr); canvas.reset(); canvas_texture.reset();
            release_swapchain();
            throw;
        }
    }
    bool material_enabled() const noexcept {
        // Offscreen composition surfaces preserve alpha so WARP can verify its
        // math. composition=false explicitly tests the opaque fallback there.
        // A real WARP HWND never advertises an accelerated translucent material.
        return options.composition && (!window || (counters.composition && !counters.warp));
    }
    float backdrop_opacity() const noexcept {
        return material_enabled() ? options.background_opacity : 1.0f;
    }
    void fill(PixelRect bounds, Color color, float opacity = 1, bool replace = false) {
        windows::PaintSurface surface(
            *dc.get(), *brush.get(), glass_paint ? glass_brush.get() : nullptr);
        surface.fill(bounds, color, opacity, replace);
    }
    bool selected(const FrameRow& row, std::size_t column) const noexcept {
        return (column < row.cells.size() && row.cells[column].selected) ||
               (row.selection && column >= row.selection->first && column <= row.selection->last);
    }
    Color foreground(const FrameRow& row, std::size_t column) const noexcept {
        if (column >= row.cells.size()) return {236,238,242,255};
        const auto& c = row.cells[column];
        if (selected(row,column)) return selection_foreground;
        auto result = c.style.inverse ? c.background : c.foreground;
        if (c.style.faint) { result.r /= 2; result.g /= 2; result.b /= 2; }
        return result;
    }
    Color background(const FrameRow& row, std::size_t column) const noexcept {
        const auto& c = row.cells[column];
        return selected(row,column) ? selection_background : (c.style.inverse ? c.foreground : c.background);
    }
    void glyph_run(const GlyphCluster& cluster, D2D1_POINT_2F origin, Color fg) {
        if (cluster.glyphs.empty()) return;
        DWRITE_GLYPH_RUN run{};
        run.fontFace = cluster.face.get(); run.fontEmSize = cluster.size;
        run.glyphCount = static_cast<UINT32>(cluster.glyphs.size()); run.glyphIndices = cluster.glyphs.data();
        run.glyphAdvances = cluster.advances.data(); run.glyphOffsets = cluster.offsets.data();
        run.isSideways = cluster.sideways; run.bidiLevel = cluster.bidi;
        brush->SetColor(paint_color(fg));
        if (text->supports_color_glyphs() && dc4) {
            Com<IDWriteColorGlyphRunEnumerator1> layers;
            const auto hr = text->color_glyph_layers(run, origin.x, origin.y, layers);
            if (SUCCEEDED(hr)) {
                BOOL available{};
                while (SUCCEEDED(layers->MoveNext(&available)) && available) {
                    const DWRITE_COLOR_GLYPH_RUN1* layer{};
                    if (FAILED(layers->GetCurrentRun(&layer)) || !layer) break;
                    const D2D1_POINT_2F position{layer->baselineOriginX,layer->baselineOriginY};
                    switch (layer->glyphImageFormat) {
                    case DWRITE_GLYPH_IMAGE_FORMATS_PNG:
                    case DWRITE_GLYPH_IMAGE_FORMATS_JPEG:
                    case DWRITE_GLYPH_IMAGE_FORMATS_TIFF:
                    case DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8:
                        dc4->DrawColorBitmapGlyphRun(layer->glyphImageFormat, position, &layer->glyphRun,
                            DWRITE_MEASURING_MODE_NATURAL, D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT);
                        break;
                    case DWRITE_GLYPH_IMAGE_FORMATS_SVG:
                        brush->SetColor(paint_color(fg));
                        dc4->DrawSvgGlyphRun(position, &layer->glyphRun, brush.get(), nullptr, 0, DWRITE_MEASURING_MODE_NATURAL);
                        break;
                    default:
                        brush->SetColor(layer->paletteIndex == 0xffff ? paint_color(fg) : layer->runColor);
                        dc->DrawGlyphRun(position, &layer->glyphRun, brush.get(), DWRITE_MEASURING_MODE_NATURAL);
                        break;
                    }
                }
                return;
            }
            if (device_lost(hr)) check(hr,"Translate color glyphs");
            // Unsupported color font/API uses explicit monochrome fallback.
        }
        brush->SetColor(paint_color(fg));
        dc->DrawGlyphRun(origin, &run, brush.get(), DWRITE_MEASURING_MODE_NATURAL);
    }
    void draw_text(const RenderPane& pane, const FrameRow& row, UINT row_index, UINT cw, UINT ch,
                   std::optional<Color> override_color = std::nullopt) {
        ShapedRow temporary;
        const auto& shape = text->shape({pane.id, pane.frame->columns, cw, ch}, row, tick, temporary);
        const auto y = pane.content.y + float(row_index * ch);
        Clip row_clip(dc.get(), intersect(pane.content, {pane.content.x,y,pane.content.width,float(ch)}));
        for (const auto& cluster : shape.clusters) {
            if (cluster.column >= row.cells.size()) continue;
            const auto left = pane.content.x + float(cluster.column * cw);
            const auto right = pane.content.x + float(cluster.end * cw);
            // At most half a cell of horizontal ink overhang, never outside row
            // or pane. Multi-cell ligatures remain one shaped glyph cluster.
            const PixelRect ink{left - cw * .5f, y, right - left + cw, float(ch)};
            Clip ink_clip(dc.get(), ink);
            const D2D1_POINT_2F origin{pane.content.x + cluster.x,y + cluster.baseline};
            // Paint one cluster multiple times only when paint/selection changes
            // inside it. Each pass is clipped to exact cell backgrounds; shaping
            // is unchanged, so selection never tears apart grapheme processing.
            auto start = cluster.column;
            while (start < cluster.end && start < row.cells.size()) {
                const auto fg = override_color.value_or(foreground(row,start));
                const bool invisible = row.cells[start].style.invisible ||
                    (row.cells[start].style.blink && !pane.text_blink_on);
                auto end = start + 1;
                while (end < cluster.end && end < row.cells.size() && override_color.value_or(foreground(row,end)) == fg &&
                       (row.cells[end].style.invisible || (row.cells[end].style.blink && !pane.text_blink_on)) == invisible) ++end;
                if (!invisible) {
                    const float x0 = start == cluster.column ? ink.x : pane.content.x + float(start * cw);
                    const float x1 = end == cluster.end ? ink.x + ink.width : pane.content.x + float(end * cw);
                    Clip paint_clip(dc.get(), {x0,y,x1-x0,float(ch)});
                    glyph_run(cluster,origin,fg);
                }
                start = end;
            }
        }
    }
    void decorations(const RenderPane& pane, const FrameRow& row, UINT row_index, UINT cw, UINT ch) {
        const auto y = pane.content.y + float(row_index * ch);
        const auto thickness = std::max(1.0f,std::floor(dpi / 96));
        const auto count = std::min<std::size_t>(row.cells.size(),pane.frame->columns);
        for (std::size_t column = 0; column < count; ++column) {
            const auto& c = row.cells[column];
            if (!c.span || c.style.invisible || (c.style.blink && !pane.text_blink_on)) continue;
            const auto x = pane.content.x + float(column * cw);
            const auto length = float(std::min<std::size_t>(c.span,count-column) * cw);
            auto underline_color = selected(row,column) ? selection_foreground : c.underline_color;
            if (c.style.faint) { underline_color.r /= 2; underline_color.g /= 2; underline_color.b /= 2; }
            const float uy = y + float(ch) - thickness * 2;
            if (c.style.underline == 1 || c.style.underline == 2) {
                fill({x,uy,length,thickness},underline_color);
                if (c.style.underline == 2) fill({x,uy-thickness*2,length,thickness},underline_color);
            } else if (c.style.underline >= 3) {
                brush->SetColor(paint_color(underline_color));
                const float step = c.style.underline == 5 ? thickness * 4 : thickness * 2;
                for (float p = 0; p < length; p += step) {
                    if (c.style.underline == 3) {
                        const auto end = std::min(length,p+step);
                        dc->DrawLine({x+p,uy},{x+(p+end)/2,uy-thickness},brush.get(),thickness);
                        dc->DrawLine({x+(p+end)/2,uy-thickness},{x+end,uy},brush.get(),thickness);
                    } else fill({x+p,uy,std::min(length-p,c.style.underline==5?step*.65f:thickness),thickness},underline_color);
                }
            }
            const auto fg = foreground(row,column);
            if (c.style.strikethrough) fill({x,y+ch*.52f,length,thickness},fg);
            if (c.style.overline) fill({x,y+thickness,length,thickness},fg);
        }
    }
    void draw_images(const RenderPane& pane, ImageLayer layer, UINT cw, UINT ch, PixelRect damage) {
        for (const auto& placement : pane.frame->images) {
            if (placement.layer != layer || !placement.pixels) continue;
            const auto& pixels = *placement.pixels;
            if (placement.source_x >= pixels.width || placement.source_y >= pixels.height) continue;
            const auto sw = std::min(placement.source_width ? placement.source_width : pixels.width-placement.source_x,
                                     pixels.width-placement.source_x);
            const auto sh = std::min(placement.source_height ? placement.source_height : pixels.height-placement.source_y,
                                     pixels.height-placement.source_y);
            const auto dw = placement.pixel_width ? float(placement.pixel_width) : placement.columns ? float(placement.columns)*float(cw) : float(sw);
            const auto dh = placement.pixel_height ? float(placement.pixel_height) : placement.rows ? float(placement.rows)*float(ch) : float(sh);
            const PixelRect target{pane.content.x+float(placement.column)*cw+placement.offset_x,
                                   pane.content.y+float(placement.row)*ch+placement.offset_y,dw,dh};
            if (!sw || !sh || !overlaps(target,damage)) continue;
            auto* bitmap = image_cache->bitmap(
                *dc.get(), pane.id, pixels, tick);
            if (!bitmap) continue;
            const auto destination = paint_rect(target);
            const D2D1_RECT_F source{float(placement.source_x),float(placement.source_y),
                                    float(placement.source_x+sw),float(placement.source_y+sh)};
            dc->DrawBitmap(bitmap,&destination,1,D2D1_INTERPOLATION_MODE_LINEAR,&source,nullptr);
        }
    }
    void draw_cursor(const RenderPane& pane, UINT cw, UINT ch) {
        auto bounds = render_geometry::cursor_bounds(pane, text->metrics());
        if (!nonempty(bounds)) return;
        const auto& cursor = pane.frame->cursor;
        const auto thickness = std::max(1.0f,std::round(dpi / 96));
        const auto shape = pane.focused ? cursor.shape : CursorShape::hollow_block;
        if (shape == CursorShape::bar) bounds.width = thickness*2;
        if (shape == CursorShape::underline) { bounds.y += bounds.height-thickness*2; bounds.height = thickness*2; }
        const auto reveal=std::clamp(pane.cursor_reveal,0.0f,1.0f);
        if (pane.focused && shape != CursorShape::hollow_block) {
            if (shape == CursorShape::bar) { const auto inset=bounds.height*(1-reveal); bounds.y+=inset; bounds.height-=inset; }
            else bounds.width*=reveal;
        }
        if (shape == CursorShape::hollow_block) {
            brush->SetColor(paint_color(cursor.color));
            const D2D1_RECT_F outline{bounds.x+thickness*.5f,bounds.y+thickness*.5f,
                                      bounds.x+bounds.width-thickness*.5f,bounds.y+bounds.height-thickness*.5f};
            dc->DrawRectangle(outline,brush.get(),thickness);
        } else {
            fill(bounds,cursor.color);
            if (shape == CursorShape::block && cursor.row < pane.frame->lines.size() && pane.frame->lines[cursor.row]) {
                Clip clip(dc.get(),bounds);
                const auto& row=*pane.frame->lines[cursor.row];
                const std::size_t column=cursor.wide_tail && cursor.column?cursor.column-1:cursor.column;
                const auto text_color=column<row.cells.size()?background(row,column):pane.frame->background;
                draw_text(pane,row,cursor.row,cw,ch,text_color);
            }
        }
    }
    void draw_pane(const RenderPane& pane, PixelRect damage) {
        if (!pane.frame || !nonempty(pane.content) || !overlaps(pane.content,damage)) return;
        const auto& frame = *pane.frame;
        const auto cell = text->metrics();
        const UINT cw = frame.cell_width ? frame.cell_width : cell.width;
        const UINT ch = frame.cell_height ? frame.cell_height : cell.height;
        Clip clip(dc.get(),pane.content);
        fill(pane.content,frame.background,backdrop_opacity(),true);
        draw_images(pane,ImageLayer::below_background,cw,ch,damage);
        const auto rows = std::min<std::size_t>(frame.rows,frame.lines.size());
        for (std::size_t r = 0; r < rows; ++r) {
            if (!frame.lines[r] || !overlaps({pane.content.x,pane.content.y+float(r*ch),pane.content.width,float(ch)},damage)) continue;
            const auto& row = *frame.lines[r];
            const auto columns = std::min<std::size_t>(frame.columns,row.cells.size());
            for (std::size_t column = 0; column < columns;) {
                const auto& c = row.cells[column];
                const auto bg = background(row,column);
                const bool default_bg = c.background_is_default && !c.style.inverse && !selected(row,column);
                auto end = column+1;
                while (end < columns && background(row,end) == bg &&
                    (row.cells[end].background_is_default && !row.cells[end].style.inverse && !selected(row,end)) == default_bg) ++end;
                // Default backgrounds were painted once; skipping them also lets
                // Kitty's below-background layer show through. Explicit cells,
                // inverse and selection remain opaque even at matching RGB.
                if (!default_bg) fill({pane.content.x+float(column*cw),pane.content.y+float(r*ch),float((end-column)*cw),float(ch)},bg);
                column = end;
            }
        }
        draw_images(pane,ImageLayer::below_text,cw,ch,damage);
        for (std::size_t r = 0; r < rows; ++r) {
            if (!frame.lines[r] || !overlaps({pane.content.x,pane.content.y+float(r*ch),pane.content.width,float(ch)},damage)) continue;
            draw_text(pane,*frame.lines[r],static_cast<UINT>(r),cw,ch);
            decorations(pane,*frame.lines[r],static_cast<UINT>(r),cw,ch);
        }
        draw_images(pane,ImageLayer::above_text,cw,ch,damage);
        draw_cursor(pane,cw,ch);
    }
    RenderResult render(const WindowScene& scene) {
        if (!width || !height || !canvas) return RenderResult::suspended;
        update_glass();glass_paint=bool(glass_brush);
        ++tick;
        const auto dirty = scene_damage.damage(scene, text->metrics(), {0, 0, float(width), float(height)});
        if (dirty.empty() && !pending_present) {
            scene_damage.commit(scene);
            return RenderResult::unchanged;
        }
        if(!dirty.empty()) {
            const bool full_repaint=dirty.size()==1 && dirty[0].x<=0 && dirty[0].y<=0 && dirty[0].x+dirty[0].width>=float(width) && dirty[0].y+dirty[0].height>=float(height);
            if(glass_paint)glass_canvas_tainted=true; // Before any desktop pixels enter canvas.
            const auto chrome_order=chrome_paint_order(scene);
            dc->SetTarget(canvas.get());
            dc->BeginDraw();
            try {
                for (auto r:dirty) {
                    // Snap damage outward to physical pixels to prevent partially
                    // accumulated alpha at fractional layout boundaries.
                    const auto right=std::ceil(r.x+r.width),bottom=std::ceil(r.y+r.height);
                    r.x=std::floor(r.x);r.y=std::floor(r.y);r.width=right-r.x;r.height=bottom-r.y;
                    Clip clip(dc.get(),r);
                    dc->Clear(D2D1_COLOR_F{0,0,0,0});
                    fill({0,0,float(width),float(height)},scene.background,backdrop_opacity(),true);
                    for(const auto& pane:scene.panes) draw_pane(pane,r);
                    windows::PaintSurface surface(
                        *dc.get(), *brush.get(), glass_paint ? glass_brush.get() : nullptr);
                    windows::ChromePainter{}.paint(
                        scene.primitives, scene.texts, chrome_order, r,
                        {0, 0, float(width), float(height)}, material_enabled(), surface, *text);
                }
            } catch (...) { dc->EndDraw(); scene_damage.invalidate(); throw; }
            check(dc->EndDraw(),"Render canvas");
            if(!glass_paint&&full_repaint)glass_canvas_tainted=false;
            counters.dirty_rectangles+=dirty.size();
            scene_damage.commit(scene);
            pending_present=true;
        }
        if(swapchain) {
            // Flip buffers are never assumed to retain old pixels. All damage is
            // accumulated into canvas, then the complete canvas is copied to the
            // current backbuffer. Shapes/uploads remain damage-driven.
            Com<ID3D11Texture2D> backbuffer;
            check(swapchain->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(backbuffer.put())),"Get swapchain buffer");
            immediate->CopyResource(backbuffer.get(),canvas_texture.get());
            const DXGI_PRESENT_PARAMETERS parameters{};
            const auto hr=swapchain->Present1(1,DXGI_PRESENT_DO_NOT_WAIT,&parameters);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING) return RenderResult::busy;
            if(hr==DXGI_STATUS_OCCLUDED) return RenderResult::occluded;
            check(hr,"Present");
        } else immediate->Flush();
        pending_present=false;++counters.presents;
        return RenderResult::presented;
    }
};

Renderer::Renderer(void* window,RendererOptions options) : impl_(std::make_unique<Impl>(window,std::move(options))) {}
Renderer::~Renderer() {
    const bool smoke=impl_ && impl_->options.smoke_teardown_diagnostics;
    windows::smoke_teardown_trace(smoke,"renderer destructor begin");
    impl_.reset();
    windows::smoke_teardown_trace(smoke,"renderer destructor end");
}
void Renderer::prepare_window_destruction() noexcept {
    if (!impl_) return;
    impl_->tracing_teardown=impl_->options.smoke_teardown_diagnostics;
    windows::smoke_teardown_trace(impl_->tracing_teardown,"renderer prepare HWND destruction begin");
    impl_->release_gpu(true);
    windows::smoke_teardown_trace(impl_->tracing_teardown,"renderer prepare HWND destruction end");
}
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;
void Renderer::resize(std::uint32_t width,std::uint32_t height,float dpi) {
    if (!std::isfinite(dpi) || dpi<48 || dpi>768) throw std::invalid_argument("Invalid surface DPI");
    if(width>16384 || height>16384 || std::uint64_t(width)*height>64u*1024u*1024u)
        throw std::length_error("Surface exceeds bounded renderer dimensions");
    auto& p=*impl_;
    if (p.dpi != dpi) {
        p.dpi = dpi;
        if (p.text->set_dpi(dpi)) p.scene_damage.invalidate();
    }
    if(width==p.width && height==p.height && p.canvas) return;
    p.stop_glass("Stopped for resize. Start again explicitly in Settings.");
    p.width=width;p.height=height;
    p.dc->SetTarget(nullptr);p.canvas.reset();p.canvas_texture.reset();p.scene_damage.invalidate();
    if(!width || !height) { p.opaque_window.resize(width,height); return; }
    try {
        if(p.swapchain) check(p.swapchain->ResizeBuffers(2,width,height,DXGI_FORMAT_B8G8R8A8_UNORM,
                                DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),"Resize swapchain");
        p.create_surfaces();
    } catch(const GraphicsError& error) { if(!device_lost(error.result)) throw;recreate_device(); }
}
void Renderer::set_font(FontOptions font) {
    if (impl_->text->set_font(font)) impl_->scene_damage.invalidate();
}
void Renderer::set_background_opacity(float opacity) {
    if(!std::isfinite(opacity) || opacity<0 || opacity>1) throw std::invalid_argument("Invalid background opacity");
    if(impl_->options.background_opacity!=opacity) {impl_->options.background_opacity=opacity;impl_->scene_damage.invalidate();}
}
void Renderer::set_additional_blur(float sigma) {
    if(!std::isfinite(sigma) || sigma<0 || sigma>64) throw std::invalid_argument("Invalid additional blur");
    auto& p=*impl_;
    if(p.additional_blur==sigma) return;
    const auto previous=p.additional_blur;p.additional_blur=sigma;
    if(p.host_backdrop.active() && sigma>0 && p.host_backdrop.update(sigma,p.width,p.height)) {
        p.scene_damage.invalidate();return;
    }
    // Avoid rebuilding an unsupported graph on every pointer move. A return to
    // zero, policy transition or device recovery permits another explicit try.
    if(previous>0 && sigma>0 && !p.counters.host_backdrop) return;
    p.dc->SetTarget(nullptr);p.canvas.reset();p.canvas_texture.reset();
    p.release_swapchain();p.create_surfaces();p.scene_damage.invalidate();
}
bool Renderer::liquid_glass_available() const noexcept {
#if defined(_MSC_VER) && STORMTTY_EXPERIMENTAL_LIQUID_GLASS
    return impl_->window && impl_->counters.composition && !impl_->counters.warp;
#else
    return false;
#endif
}
bool Renderer::start_liquid_glass(void* monitor) {
    if(!liquid_glass_available())return false;
    auto& p=*impl_;p.stop_glass("Starting experimental glass.");
    const bool started=p.liquid_glass.start(p.window,monitor,p.device.get(),p.immediate.get());
    p.scene_damage.invalidate();return started;
}
void Renderer::stop_liquid_glass(const char* reason) noexcept {impl_->stop_glass(reason);}
bool Renderer::liquid_glass_active()const noexcept{return impl_->liquid_glass.active();}
std::string Renderer::liquid_glass_status()const{return impl_->liquid_glass.status();}
bool Renderer::poll_liquid_glass(){return impl_->update_glass();}
CellMetrics Renderer::metrics() const noexcept {return impl_->text->metrics();}
RenderResult Renderer::render(const WindowScene& scene) {
    try {return impl_->render(scene);}
    catch(const GraphicsError& error) {
        if(!device_lost(error.result)) throw;
        recreate_device();
        return impl_->render(scene); // One bounded real recovery, never infinite retry.
    }
}
void* Renderer::frame_latency_wait_handle() const noexcept {return impl_->latency.value;}
RendererStats Renderer::stats() const noexcept {
    auto result = impl_->counters;
    const auto text = impl_->text->stats();
    result.shaped_rows = text.shaped_rows;
    result.shape_cache_hits = text.cache_hits;
    result.shape_cache_bytes = text.retained_bytes;
    result.bundled_font = text.bundled_font;
    const auto images = impl_->image_cache->stats();
    result.image_uploads = images.uploads;
    result.image_cache_bytes = images.retained_bytes;
    return result;
}
void Renderer::recreate_device() {
    auto& p=*impl_;
    p.release_gpu();
    p.create_device();p.scene_damage.invalidate();++p.counters.device_recoveries;
}
void Renderer::cancel_alpha_probe() noexcept { impl_->alpha_staging.reset(); }
bool Renderer::begin_alpha_probe(PixelRect area) {
    if(impl_->liquid_glass.active()||impl_->glass_canvas_tainted)return false; // Includes stale post-Stop canvas.
    auto& p=*impl_;p.alpha_staging.reset();
    if(!p.canvas_texture || !std::isfinite(area.x) || !std::isfinite(area.y) || !std::isfinite(area.width) || !std::isfinite(area.height) ||
        area.x<0 || area.y<0 || area.width<1 || area.height<1 || area.x>=p.width || area.y>=p.height) return false;
    const UINT x=static_cast<UINT>(area.x),y=static_cast<UINT>(area.y);
    p.alpha_width=std::min({16u,static_cast<UINT>(std::min(area.width,16.f)),p.width-x});
    p.alpha_height=std::min({16u,static_cast<UINT>(std::min(area.height,16.f)),p.height-y});
    D3D11_TEXTURE2D_DESC desc{};desc.Width=p.alpha_width;desc.Height=p.alpha_height;desc.MipLevels=1;desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(p.device->CreateTexture2D(&desc,nullptr,p.alpha_staging.put()),"Create alpha diagnostic staging texture");
    const D3D11_BOX source{x,y,0,x+p.alpha_width,y+p.alpha_height,1};
    p.immediate->CopySubresourceRegion(p.alpha_staging.get(),0,0,0,0,p.canvas_texture.get(),0,&source);
    p.alpha_metadata={255,0,p.alpha_width*p.alpha_height,p.backdrop_opacity(),p.counters.composition,p.counters.warp,p.counters.device_recoveries,p.counters.presents};
    p.immediate->Flush(); // Submit the tiny copy, never wait for completion.
    return true;
}
std::optional<RendererAlphaProbe> Renderer::poll_alpha_probe() {
    if(impl_->liquid_glass.active()||impl_->glass_canvas_tainted){impl_->alpha_staging.reset();return std::nullopt;}
    auto& p=*impl_;if(!p.alpha_staging)return std::nullopt;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto hr=p.immediate->Map(p.alpha_staging.get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
    if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return std::nullopt;
    if(FAILED(hr)){p.alpha_staging.reset();check(hr,"Map alpha diagnostic staging texture");}
    auto result=p.alpha_metadata;
    for(UINT y=0;y<p.alpha_height;++y) {
        const auto* bytes=static_cast<const std::uint8_t*>(mapped.pData)+std::size_t(y)*mapped.RowPitch;
        for(UINT x=0;x<p.alpha_width;++x) {const auto alpha=bytes[x*4+3];result.minimum=std::min(result.minimum,alpha);result.maximum=std::max(result.maximum,alpha);}
    }
    p.immediate->Unmap(p.alpha_staging.get(),0);p.alpha_staging.reset();return result;
}
PixelRect Renderer::measure_chrome_text(const ChromeText& label) const {
    return impl_->text->measure_chrome_text(label);
}
std::vector<std::string> Renderer::wrap_chrome_text(const ChromeText& label) const {
    return impl_->text->wrap_chrome_text(label);
}
std::vector<std::uint8_t> Renderer::read_pixels() {
    if(impl_->liquid_glass.active()||impl_->glass_canvas_tainted)throw std::runtime_error("Desktop glass is GPU-only; readback stays blocked until a clean non-glass repaint.");
    auto& p=*impl_;if(!p.canvas_texture) return {};
    D3D11_TEXTURE2D_DESC desc{};p.canvas_texture->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    check(p.device->CreateTexture2D(&desc,nullptr,staging.put()),"Create readback texture");
    p.immediate->CopyResource(staging.get(),p.canvas_texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(p.immediate->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped),"Readback map");
    struct Unmap {ID3D11DeviceContext* context;ID3D11Texture2D* texture;~Unmap(){context->Unmap(texture,0);}} unmap{p.immediate.get(),staging.get()};
    std::vector<std::uint8_t> result(std::size_t(p.width)*p.height*4);
    for(UINT y=0;y<p.height;++y) {
        const auto* source=static_cast<const std::uint8_t*>(mapped.pData)+std::size_t(y)*mapped.RowPitch;
        for(UINT x=0;x<p.width;++x) {
            const auto target=(std::size_t(y)*p.width+x)*4;
            const auto alpha=source[x*4+3];
            // Public readback is straight RGBA, unlike the premultiplied canvas.
            auto straight=[alpha](std::uint8_t value)->std::uint8_t {
                return alpha?static_cast<std::uint8_t>(std::min(255u,(unsigned(value)*255u+alpha/2)/alpha)):0;
            };
            result[target]=straight(source[x*4+2]);result[target+1]=straight(source[x*4+1]);
            result[target+2]=straight(source[x*4]);result[target+3]=alpha;
        }
    }
    return result;
}
} // namespace stormtty
