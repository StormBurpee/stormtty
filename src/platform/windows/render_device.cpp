#include "render_device.hpp"

#include "com_owner.hpp"
#include "graphics_error.hpp"
#include "host_backdrop.hpp"
#include "image_cache.hpp"
#include "liquid_glass/session.hpp"
#include "smoke_teardown_trace.hpp"
#include "stormtty/build_features.hpp"

#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

#ifdef STORMTTY_RENDER_DEVICE_TESTS
#include "render_device_test_support.hpp"
namespace stormtty::windows::testing {
namespace {
thread_local AcquisitionPoint failure_point = AcquisitionPoint::none;
}
void fail_after(AcquisitionPoint point) noexcept {
    failure_point = point;
}
void checkpoint(AcquisitionPoint point) {
    if (failure_point == point) {
        failure_point = AcquisitionPoint::none;
        throw std::runtime_error("Synthetic native acquisition failure");
    }
}
} // namespace stormtty::windows::testing
#define DEVICE_CHECKPOINT(point) testing::checkpoint(testing::AcquisitionPoint::point)
#else
#define DEVICE_CHECKPOINT(point) ((void)0)
#endif

namespace stormtty::windows {
namespace {
struct NativeHandle {
    HANDLE value{};

    ~NativeHandle() {
        reset();
    }

    void reset(HANDLE replacement = nullptr) noexcept {
        if (value) {
            CloseHandle(value);
        }
        value = replacement;
    }
};

HRESULT win32_error() noexcept {
    const auto error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

LONG_PTR extended_style(HWND window) {
    SetLastError(ERROR_SUCCESS);
    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const auto error = GetLastError();
    if (!style && error) {
        check(HRESULT_FROM_WIN32(error), "Read renderer host extended style");
    }
    return style;
}

// NOREDIRECTIONBITMAP is immutable. Opaque fallback owns a disabled child;
// the stable shell HWND remains the sole input/focus owner.
class OpaquePresentationWindow final {
public:
    OpaquePresentationWindow() = default;
    ~OpaquePresentationWindow() {
        reset();
    }

    OpaquePresentationWindow(const OpaquePresentationWindow&) = delete;
    OpaquePresentationWindow& operator=(const OpaquePresentationWindow&) = delete;

    HWND get() const noexcept {
        return window_;
    }

    void reset() noexcept {
        const auto window = std::exchange(window_, nullptr);
        if (window && IsWindow(window)) {
            DestroyWindow(window);
        }
    }

    void create(HWND parent, UINT width, UINT height) {
        constexpr const wchar_t* class_name = L"StormTTY.Renderer.OpaqueSurface.v1";
        static const bool registered = [] {
            WNDCLASSW description{};
            description.hInstance = GetModuleHandleW(nullptr);
            description.lpszClassName = L"StormTTY.Renderer.OpaqueSurface.v1";
            description.lpfnWndProc = dispatch;
            if (!RegisterClassW(&description)) {
                const auto error = GetLastError();
                WNDCLASSW existing{};
                if (error != ERROR_CLASS_ALREADY_EXISTS ||
                    !GetClassInfoW(description.hInstance, description.lpszClassName, &existing) ||
                    existing.lpfnWndProc != dispatch) {
                    check(error ? HRESULT_FROM_WIN32(error) : E_FAIL,
                          "Register opaque presentation window");
                }
            }
            return true;
        }();
        (void)registered;
        window_ = CreateWindowExW(
            WS_EX_NOPARENTNOTIFY, class_name, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED,
            0, 0, static_cast<int>(width), static_cast<int>(height),
            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_) {
            check(win32_error(), "Create opaque presentation child");
        }
        if (extended_style(window_) & WS_EX_NOREDIRECTIONBITMAP) {
            throw std::runtime_error("Opaque presentation child unexpectedly lacks redirection");
        }
    }

    void resize(UINT width, UINT height) {
        if (window_ && !SetWindowPos(
                window_, HWND_TOP, 0, 0, static_cast<int>(width), static_cast<int>(height),
                SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
            check(win32_error(), "Resize opaque presentation child");
        }
    }

private:
    static LRESULT CALLBACK dispatch(
        HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATEANDEAT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    HWND window_{};
};
} // namespace

struct RenderDevice::Impl {
    // Native fields retain the original relative declaration order, including
    // dormant material subobjects. Failed initialization uses member unwinding,
    // not the normal shutdown/drain path.
    HostBackdrop host_backdrop;
    liquid_glass::Session liquid_glass;
    Com<ID2D1Bitmap1> glass_bitmap;
    Com<ID2D1BitmapBrush1> glass_brush;
    ID3D11Texture2D* glass_key{};
    std::uint64_t glass_generation{};
    bool glass_paint{};
    bool glass_canvas_tainted{};
    float additional_blur{};
    HWND window{};
    OpaquePresentationWindow opaque_window;
    DeviceOptions options;
    DeviceCapabilities counters;
    UINT width{};
    UINT height{};
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
    Com<ID3D11Texture2D> canvas_texture;
    Com<ID3D11Texture2D> alpha_staging;
    UINT alpha_width{};
    UINT alpha_height{};
    RendererAlphaProbe alpha_metadata{};
    Com<ID2D1Bitmap1> canvas;
    Com<ID2D1SolidColorBrush> brush;
    bool tracing_teardown{};

    explicit Impl(DeviceOptions requested)
        : window(static_cast<HWND>(requested.window)), options(requested) {}

    void clear_and_flush() noexcept {
        if (!immediate) {
            return;
        }
        smoke_teardown_trace(tracing_teardown, "renderer ClearState begin");
        immediate->ClearState();
        smoke_teardown_trace(tracing_teardown, "renderer ClearState end; Flush begin");
        immediate->Flush();
        smoke_teardown_trace(tracing_teardown, "renderer Flush end");
    }

    void stop_glass(const char* reason, SurfaceChanges& changes) noexcept {
        glass_paint = false;
        glass_key = nullptr;
        glass_brush.reset();
        glass_bitmap.reset();
        liquid_glass.stop(reason);
        changes.repaint = true;
    }

    bool update_glass(float dpi, SurfaceChanges& changes) {
        if (!liquid_glass.active()) {
            return false;
        }
        if (dc) {
            dc->SetTarget(nullptr);
        }
        auto* texture = liquid_glass.background(dpi / 96.f);
        try {
            if (texture != glass_key) {
                glass_brush.reset();
                glass_bitmap.reset();
                glass_key = nullptr;
                if (texture) {
                    Com<IDXGISurface> surface;
                    check(texture->QueryInterface(
                        __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.put())),
                        "Glass surface");
                    const auto properties = D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_NONE,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                        96, 96);
                    check(dc->CreateBitmapFromDxgiSurface(
                        surface.get(), &properties, glass_bitmap.put()), "Glass bitmap");
                    const auto bitmap_properties = D2D1::BitmapBrushProperties1(
                        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                        D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                    check(dc->CreateBitmapBrush(
                        glass_bitmap.get(), bitmap_properties, D2D1::BrushProperties(),
                        glass_brush.put()), "Glass brush");
                    glass_key = texture;
                }
            }
        } catch (...) {
            stop_glass("Experimental glass surface failed; original material restored.", changes);
        }
        const bool changed = glass_generation != liquid_glass.generation();
        glass_generation = liquid_glass.generation();
        if (changed) {
            changes.repaint = true;
        }
        return changed;
    }

    void release_swapchain(SurfaceChanges& changes, bool replacing = true) noexcept {
        stop_glass("Stopped for renderer surface replacement. Start again explicitly in Settings.", changes);
        host_backdrop.detach();
        counters.host_backdrop = false;
        composition_visual.reset();
        composition_target.reset();
        composition.reset();
        smoke_teardown_trace(tracing_teardown, "renderer latency handle close begin");
        latency.reset();
        smoke_teardown_trace(tracing_teardown, "renderer latency handle close end");
        swapchain2.reset();
        swapchain.reset();
        // Only replacement needs a synchronous deferred-resource drain. Forcing
        // ClearState after a destroyed HWND can stall WARP presentation release.
        if (replacing) {
            clear_and_flush();
        } else {
            smoke_teardown_trace(
                tracing_teardown, "renderer final device release; no swapchain replacement drain");
        }
        smoke_teardown_trace(tracing_teardown, "renderer opaque HWND destruction begin");
        opaque_window.reset();
        smoke_teardown_trace(tracing_teardown, "renderer opaque HWND destruction end");
        counters.composition = false;
    }

    void finish_submitted_work() noexcept {
        if (!device || !immediate) {
            return;
        }
        // Flush submits only. Keep D2D/canvas/presentation owners alive while
        // awaiting the final completion query, before normal ownership release.
        smoke_teardown_trace(tracing_teardown, "renderer completion query begin");
        Com<ID3D11Query> completion;
        const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
        auto result = device->CreateQuery(&description, completion.put());
        if (SUCCEEDED(result)) {
            immediate->End(completion.get());
            immediate->Flush();
            const auto deadline = GetTickCount64() + 2000;
            do {
                result = immediate->GetData(
                    completion.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (result != S_FALSE) {
                    break;
                }
                Sleep(1);
            } while (GetTickCount64() < deadline);
        }
        smoke_teardown_trace(tracing_teardown, result == S_OK ?
            "renderer completion query completed" :
            "renderer completion query incomplete; normal ownership release follows");
        // Timeout/removal never detaches or leaks a device. Native outer
        // deadlines still detect a stall during normal driver release.
    }

    void release_gpu(ImageCache& images, SurfaceChanges& changes, bool final_shutdown) noexcept {
        stop_glass("Stopped for graphics device replacement. Start again explicitly in Settings.", changes);
        if (final_shutdown) {
            finish_submitted_work();
        }
        smoke_teardown_trace(tracing_teardown, "renderer SetTarget null begin");
        if (dc) {
            dc->SetTarget(nullptr);
        }
        smoke_teardown_trace(tracing_teardown, "renderer SetTarget null end");
        alpha_staging.reset();
        canvas.reset();
        canvas_texture.reset();
        brush.reset();
        smoke_teardown_trace(tracing_teardown, "renderer image cache clear begin");
        images.clear();
        smoke_teardown_trace(tracing_teardown, "renderer image cache clear end");
        dc4.reset();
        dc.reset();
        d2d_device.reset();
        // A live caller-owned HWND may be reused even after Renderer destruction.
        // Only an already-destroyed HWND (or no HWND) skips replacement draining.
        const bool replacing = !final_shutdown || (window && IsWindow(window));
        release_swapchain(changes, replacing);
        immediate.reset();
        device.reset();
    }

    void create_device(SurfaceChanges& changes) {
        D3D_FEATURE_LEVEL level{};
        constexpr std::array<D3D_FEATURE_LEVEL, 3> levels{
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        auto create = [&](D3D_DRIVER_TYPE type) {
            return D3D11CreateDevice(
                nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
                device.put(), &level, immediate.put());
        };
        auto result = options.force_warp ? E_FAIL : create(D3D_DRIVER_TYPE_HARDWARE);
        counters.warp = FAILED(result);
        if (FAILED(result)) {
            device.reset();
            immediate.reset();
            result = create(D3D_DRIVER_TYPE_WARP);
        }
        check(result, "D3D11CreateDevice hardware/WARP");
        DEVICE_CHECKPOINT(device);
        Com<IDXGIDevice> dxgi_device;
        check(device.as(dxgi_device), "Query IDXGIDevice");
        check(d2d_factory->CreateDevice(dxgi_device.get(), d2d_device.put()), "D2D CreateDevice");
        DEVICE_CHECKPOINT(drawing_device);
        // Native caches have separate soft thresholds, not ImageCache accounting.
        d2d_device->SetMaximumTextureMemory(32u * 1024u * 1024u);
        Com<ID2D1Device4> d2d4;
        if (SUCCEEDED(d2d_device.as(d2d4))) {
            d2d4->SetMaximumColorGlyphCacheMemory(16u * 1024u * 1024u);
        }
        check(d2d_device->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()), "D2D CreateDeviceContext");
        DEVICE_CHECKPOINT(context);
        dc.as(dc4);
        counters.color_glyphs = bool(dc4);
        dc->SetDpi(96, 96);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        check(dc->CreateSolidColorBrush(D2D1_COLOR_F{1, 1, 1, 1}, brush.put()), "Create brush");
        DEVICE_CHECKPOINT(brush);
        if (width && height) {
            create_surfaces(changes);
        }
    }

    void create_presentation(SurfaceChanges& changes) {
        Com<IDXGIDevice> dxgi_device;
        check(device.as(dxgi_device), "Query DXGI device");
        Com<IDXGIAdapter> adapter;
        check(dxgi_device->GetAdapter(adapter.put()), "Get adapter");
        Com<IDXGIFactory2> factory;
        check(adapter->GetParent(
            __uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put())), "Get DXGI factory");

        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width;
        description.Height = height;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        counters.composition = false;
        const bool no_redirection = (extended_style(window) & WS_EX_NOREDIRECTIONBITMAP) != 0;

        // Never compose translucent pixels over an ordinary backing surface.
        if (options.composition && no_redirection && additional_blur > 0 && !counters.warp) {
            description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            if (SUCCEEDED(factory->CreateSwapChainForComposition(
                    device.get(), &description, nullptr, swapchain.put())) &&
                host_backdrop.attach(window, swapchain.get(), additional_blur, width, height)) {
                counters.composition = true;
                counters.host_backdrop = true;
            } else {
                host_backdrop.detach();
                swapchain.reset();
            }
        }
        if (options.composition && no_redirection && !swapchain) {
            description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            const char* stage = "DCompositionCreateDevice";
            auto result = DCompositionCreateDevice(
                dxgi_device.get(), __uuidof(IDCompositionDevice),
                reinterpret_cast<void**>(composition.put()));
            auto step = [&](const char* action, auto&& operation) {
                if (SUCCEEDED(result)) {
                    stage = action;
                    result = operation();
                }
            };
            step("CreateSwapChainForComposition", [&] {
                return factory->CreateSwapChainForComposition(
                    device.get(), &description, nullptr, swapchain.put());
            });
            step("CreateTargetForHwnd", [&] {
                return composition->CreateTargetForHwnd(window, TRUE, composition_target.put());
            });
            step("CreateVisual", [&] {
                return composition->CreateVisual(composition_visual.put());
            });
            step("SetContent", [&] {
                return composition_visual->SetContent(swapchain.get());
            });
            step("SetRoot", [&] {
                return composition_target->SetRoot(composition_visual.get());
            });
            step("Commit", [&] {
                return composition->Commit();
            });
            counters.composition = SUCCEEDED(result);
            if (!counters.composition) {
                SetLastError(ERROR_SUCCESS);
                const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
                const auto style_error = GetLastError();
                char diagnostic[320]{};
                std::snprintf(
                    diagnostic, sizeof diagnostic,
                    "StormTTY renderer: DComp fallback stage=%s hr=0x%08lx warp=%u exstyle=0x%08lx style_error=%lu visible=%u size=%ux%u\n",
                    stage, static_cast<unsigned long>(result), unsigned(counters.warp),
                    static_cast<unsigned long>(style), static_cast<unsigned long>(style_error),
                    unsigned(IsWindowVisible(window) != FALSE), width, height);
                OutputDebugStringA(diagnostic);
                std::fputs(diagnostic, stderr);
                release_swapchain(changes);
            }
        }
        if (!swapchain) {
            HWND presentation_window = window;
            if (no_redirection) {
                opaque_window.create(window, width, height);
                presentation_window = opaque_window.get();
            }
            description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            check(factory->CreateSwapChainForHwnd(
                device.get(), presentation_window, &description, nullptr, nullptr, swapchain.put()),
                "Create HWND swapchain");
        }
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        check(swapchain.as(swapchain2), "Query swapchain2");
        check(swapchain2->SetMaximumFrameLatency(1), "Set frame latency");
        latency.reset(swapchain2->GetFrameLatencyWaitableObject());
        if (!latency.value) {
            throw std::runtime_error("Swapchain did not provide frame latency handle");
        }
    }

    void create_surfaces(SurfaceChanges& changes) {
        if (!width || !height) {
            return;
        }
        try {
            if (window && !swapchain) {
                create_presentation(changes);
            }
            opaque_window.resize(width, height);
            if (counters.host_backdrop && !host_backdrop.update(additional_blur, width, height)) {
                release_swapchain(changes);
                // Failed graph update must restore ordinary visible presentation.
                // Suppress only this generation's blur retry, not the request.
                const auto requested = additional_blur;
                additional_blur = 0;
                try {
                    create_surfaces(changes);
                } catch (...) {
                    additional_blur = requested;
                    throw;
                }
                additional_blur = requested;
                return;
            }

            D3D11_TEXTURE2D_DESC description{};
            description.Width = width;
            description.Height = height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            check(device->CreateTexture2D(
                &description, nullptr, canvas_texture.put()), "Create persistent canvas");
            DEVICE_CHECKPOINT(canvas_texture);
            Com<IDXGISurface> surface;
            check(canvas_texture.as(surface), "Query canvas DXGI surface");
            const D2D1_BITMAP_PROPERTIES1 properties{
                {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
                96, 96, D2D1_BITMAP_OPTIONS_TARGET, nullptr};
            check(dc->CreateBitmapFromDxgiSurface(
                surface.get(), &properties, canvas.put()), "Create canvas bitmap");
            DEVICE_CHECKPOINT(canvas_bitmap);
            dc->SetTarget(canvas.get());
            changes.repaint = true;
            changes.canvas_installed = true;
        } catch (...) {
            dc->SetTarget(nullptr);
            canvas.reset();
            canvas_texture.reset();
            release_swapchain(changes);
            throw;
        }
    }

};

CanvasDraw::CanvasDraw(
    ID2D1DeviceContext& context,
    ID2D1Bitmap1& target,
    ID2D1SolidColorBrush& brush,
    ID2D1Brush* replacement,
    ID2D1DeviceContext4* color_context,
    bool& tainted) noexcept
    : context_(context),
      surface_(context, brush, replacement, color_context),
      tainted_(tainted),
      glass_paint_(replacement != nullptr) {
    context_.SetTarget(&target);
    context_.BeginDraw();
}

CanvasDraw::~CanvasDraw() {
    if (active_) {
        context_.EndDraw();
    }
}

PaintSurface& CanvasDraw::surface() noexcept {
    return surface_;
}

void CanvasDraw::clear() noexcept {
    context_.Clear(D2D1_COLOR_F{0, 0, 0, 0});
}

HRESULT CanvasDraw::finish(bool full_repaint) noexcept {
    if (!active_) {
        return D2DERR_WRONG_STATE;
    }
    active_ = false;
    const auto result = context_.EndDraw();
    if (SUCCEEDED(result) && !glass_paint_ && full_repaint) {
        tainted_ = false;
    }
    return result;
}

RenderDevice::RenderDevice(DeviceOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

RenderDevice::~RenderDevice() = default;

#ifdef STORMTTY_RENDER_DEVICE_TESTS
void testing::taint_canvas(RenderDevice& device) noexcept {
    device.impl_->glass_canvas_tainted = true;
}
#endif

void RenderDevice::initialize(SurfaceChanges& changes) {
    const D2D1_FACTORY_OPTIONS factory_options{};
    check(D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
        &factory_options, reinterpret_cast<void**>(impl_->d2d_factory.put())),
        "D2D1CreateFactory");
    DEVICE_CHECKPOINT(factory);
    impl_->create_device(changes);
}

void RenderDevice::release_failed_initialization_canvas() noexcept {
    // This is the original reverse member order around the text-owner slot.
    // Deliberately no SetTarget, completion query, ClearState or Flush.
    impl_->brush.reset();
    impl_->canvas.reset();
    impl_->alpha_staging.reset();
    impl_->canvas_texture.reset();
}

void RenderDevice::enable_teardown_trace() noexcept {
#define TRACE_RELEASE(member) impl_->member.teardown_label = "renderer " #member " Release"
    TRACE_RELEASE(device);
    TRACE_RELEASE(immediate);
    TRACE_RELEASE(swapchain);
    TRACE_RELEASE(swapchain2);
    TRACE_RELEASE(composition);
    TRACE_RELEASE(composition_target);
    TRACE_RELEASE(composition_visual);
    TRACE_RELEASE(d2d_factory);
    TRACE_RELEASE(d2d_device);
    TRACE_RELEASE(dc);
    TRACE_RELEASE(dc4);
    TRACE_RELEASE(canvas_texture);
    TRACE_RELEASE(alpha_staging);
    TRACE_RELEASE(canvas);
    TRACE_RELEASE(brush);
#undef TRACE_RELEASE
}

void RenderDevice::set_teardown_trace(bool enabled) noexcept {
    impl_->tracing_teardown = enabled;
}

void RenderDevice::release(
    DeviceRelease mode,
    ImageCache& images,
    SurfaceChanges& changes) noexcept {
    impl_->release_gpu(images, changes, mode == DeviceRelease::final_shutdown);
}

void RenderDevice::recreate(ImageCache& images, SurfaceChanges& changes) {
    impl_->release_gpu(images, changes, false);
    impl_->create_device(changes);
    changes.repaint = true;
}

bool RenderDevice::ready() const noexcept {
    return impl_->width && impl_->height && impl_->canvas;
}

bool RenderDevice::matches_size(std::uint32_t width, std::uint32_t height) const noexcept {
    return width == impl_->width && height == impl_->height && impl_->canvas;
}

PixelRect RenderDevice::bounds() const noexcept {
    return {0, 0, float(impl_->width), float(impl_->height)};
}

DeviceCapabilities RenderDevice::capabilities() const noexcept {
    return impl_->counters;
}

bool RenderDevice::material_enabled() const noexcept {
    // Offscreen composition keeps alpha for WARP math tests. A real WARP HWND
    // does not advertise an accelerated translucent material.
    const auto& device = *impl_;
    return device.options.composition &&
        (!device.window || (device.counters.composition && !device.counters.warp));
}

void* RenderDevice::frame_latency_wait_handle() const noexcept {
    return impl_->latency.value;
}

void RenderDevice::resize(
    std::uint32_t width,
    std::uint32_t height,
    SurfaceChanges& changes) {
    auto& device = *impl_;
    device.stop_glass("Stopped for resize. Start again explicitly in Settings.", changes);
    device.width = width;
    device.height = height;
    device.dc->SetTarget(nullptr);
    device.canvas.reset();
    device.canvas_texture.reset();
    changes.repaint = true;
    if (!width || !height) {
        device.opaque_window.resize(width, height);
        return;
    }
    if (device.swapchain) {
        check(device.swapchain->ResizeBuffers(
            2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT), "Resize swapchain");
    }
    device.create_surfaces(changes);
}

void RenderDevice::set_additional_blur(float sigma, SurfaceChanges& changes) {
    auto& device = *impl_;
    if (device.additional_blur == sigma) {
        return;
    }
    const auto previous = device.additional_blur;
    device.additional_blur = sigma;
    if (device.host_backdrop.active() && sigma > 0 &&
        device.host_backdrop.update(sigma, device.width, device.height)) {
        changes.repaint = true;
        return;
    }
    // Preserve suppression of unsupported graph rebuilds during slider movement.
    if (previous > 0 && sigma > 0 && !device.counters.host_backdrop) {
        return;
    }
    device.dc->SetTarget(nullptr);
    device.canvas.reset();
    device.canvas_texture.reset();
    device.release_swapchain(changes);
    device.create_surfaces(changes);
    changes.repaint = true;
}

void RenderDevice::prepare_background(float dpi, SurfaceChanges& changes) {
    impl_->update_glass(dpi, changes);
    impl_->glass_paint = bool(impl_->glass_brush);
}

void RenderDevice::mark_canvas_write() noexcept {
    if (impl_->glass_paint) {
        impl_->glass_canvas_tainted = true;
    }
}

CanvasDraw RenderDevice::begin_canvas_draw() {
    auto& device = *impl_;
    return CanvasDraw(
        *device.dc.get(), *device.canvas.get(), *device.brush.get(),
        device.glass_paint ? device.glass_brush.get() : nullptr,
        device.dc4.get(), device.glass_canvas_tainted);
}

RenderResult RenderDevice::present_canvas() {
    auto& device = *impl_;
    if (device.swapchain) {
        // Flip buffers do not retain pixels. Copy the full persistent canvas.
        Com<ID3D11Texture2D> backbuffer;
        check(device.swapchain->GetBuffer(
            0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.put())),
            "Get swapchain buffer");
        device.immediate->CopyResource(backbuffer.get(), device.canvas_texture.get());
        const DXGI_PRESENT_PARAMETERS parameters{};
        const auto result = device.swapchain->Present1(1, DXGI_PRESENT_DO_NOT_WAIT, &parameters);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
            return RenderResult::busy;
        }
        if (result == DXGI_STATUS_OCCLUDED) {
            return RenderResult::occluded;
        }
        check(result, "Present");
    } else {
        device.immediate->Flush();
    }
    return RenderResult::presented;
}

bool RenderDevice::liquid_glass_available() const noexcept {
#if defined(_MSC_VER) && STORMTTY_EXPERIMENTAL_LIQUID_GLASS
    return impl_->window && impl_->counters.composition && !impl_->counters.warp;
#else
    return false;
#endif
}

bool RenderDevice::start_liquid_glass(void* monitor, SurfaceChanges& changes) {
    if (!liquid_glass_available()) {
        return false;
    }
    auto& device = *impl_;
    device.stop_glass("Starting experimental glass.", changes);
    const bool started = device.liquid_glass.start(
        device.window, monitor, device.device.get(), device.immediate.get());
    changes.repaint = true;
    return started;
}

void RenderDevice::stop_liquid_glass(const char* reason, SurfaceChanges& changes) noexcept {
    impl_->stop_glass(reason, changes);
}

bool RenderDevice::liquid_glass_active() const noexcept {
    return impl_->liquid_glass.active();
}

std::string RenderDevice::liquid_glass_status() const {
    return impl_->liquid_glass.status();
}

bool RenderDevice::poll_liquid_glass(float dpi, SurfaceChanges& changes) {
    return impl_->update_glass(dpi, changes);
}

void RenderDevice::cancel_alpha_probe() noexcept {
    impl_->alpha_staging.reset();
}

bool RenderDevice::begin_alpha_probe(PixelRect area, RendererAlphaProbe metadata) {
    auto& device = *impl_;
    if (device.liquid_glass.active() || device.glass_canvas_tainted) {
        return false;
    }
    device.alpha_staging.reset();
    if (!device.canvas_texture ||
        !std::isfinite(area.x) || !std::isfinite(area.y) ||
        !std::isfinite(area.width) || !std::isfinite(area.height) ||
        area.x < 0 || area.y < 0 || area.width < 1 || area.height < 1 ||
        area.x >= device.width || area.y >= device.height) {
        return false;
    }
    const UINT x = static_cast<UINT>(area.x);
    const UINT y = static_cast<UINT>(area.y);
    device.alpha_width = std::min({
        16u, static_cast<UINT>(std::min(area.width, 16.f)), device.width - x});
    device.alpha_height = std::min({
        16u, static_cast<UINT>(std::min(area.height, 16.f)), device.height - y});
    D3D11_TEXTURE2D_DESC description{};
    description.Width = device.alpha_width;
    description.Height = device.alpha_height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device.device->CreateTexture2D(
        &description, nullptr, device.alpha_staging.put()), "Create alpha diagnostic staging texture");
    const D3D11_BOX source{x, y, 0, x + device.alpha_width, y + device.alpha_height, 1};
    device.immediate->CopySubresourceRegion(
        device.alpha_staging.get(), 0, 0, 0, 0, device.canvas_texture.get(), 0, &source);
    metadata.minimum = 255;
    metadata.maximum = 0;
    metadata.samples = device.alpha_width * device.alpha_height;
    device.alpha_metadata = metadata;
    device.immediate->Flush(); // Submit this tiny copy; never wait for completion.
    return true;
}

std::optional<RendererAlphaProbe> RenderDevice::poll_alpha_probe() {
    auto& device = *impl_;
    if (device.liquid_glass.active() || device.glass_canvas_tainted) {
        device.alpha_staging.reset();
        return std::nullopt;
    }
    if (!device.alpha_staging) {
        return std::nullopt;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto mapped_result = device.immediate->Map(
        device.alpha_staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (mapped_result == DXGI_ERROR_WAS_STILL_DRAWING) {
        return std::nullopt;
    }
    if (FAILED(mapped_result)) {
        device.alpha_staging.reset();
        check(mapped_result, "Map alpha diagnostic staging texture");
    }
    auto result = device.alpha_metadata;
    for (UINT y = 0; y < device.alpha_height; ++y) {
        const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData) +
            std::size_t(y) * mapped.RowPitch;
        for (UINT x = 0; x < device.alpha_width; ++x) {
            const auto alpha = bytes[x * 4 + 3];
            result.minimum = std::min(result.minimum, alpha);
            result.maximum = std::max(result.maximum, alpha);
        }
    }
    device.immediate->Unmap(device.alpha_staging.get(), 0);
    device.alpha_staging.reset();
    return result;
}

std::vector<std::uint8_t> RenderDevice::read_pixels() {
    auto& device = *impl_;
    if (device.liquid_glass.active() || device.glass_canvas_tainted) {
        throw std::runtime_error(
            "Desktop glass is GPU-only; readback stays blocked until a clean non-glass repaint.");
    }
    if (!device.canvas_texture) {
        return {};
    }
    D3D11_TEXTURE2D_DESC description{};
    device.canvas_texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    check(device.device->CreateTexture2D(&description, nullptr, staging.put()), "Create readback texture");
    device.immediate->CopyResource(staging.get(), device.canvas_texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(device.immediate->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped), "Readback map");
    struct Unmap {
        ID3D11DeviceContext* context;
        ID3D11Texture2D* texture;
        ~Unmap() {
            context->Unmap(texture, 0);
        }
    } unmap{device.immediate.get(), staging.get()};

    std::vector<std::uint8_t> result(std::size_t(device.width) * device.height * 4);
    for (UINT y = 0; y < device.height; ++y) {
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData) +
            std::size_t(y) * mapped.RowPitch;
        for (UINT x = 0; x < device.width; ++x) {
            const auto target = (std::size_t(y) * device.width + x) * 4;
            const auto alpha = source[x * 4 + 3];
            // Public readback is straight RGBA, not the premultiplied canvas.
            auto straight = [alpha](std::uint8_t value) -> std::uint8_t {
                return alpha ? static_cast<std::uint8_t>(
                    std::min(255u, (unsigned(value) * 255u + alpha / 2) / alpha)) : 0;
            };
            result[target] = straight(source[x * 4 + 2]);
            result[target + 1] = straight(source[x * 4 + 1]);
            result[target + 2] = straight(source[x * 4]);
            result[target + 3] = alpha;
        }
    }
    return result;
}

} // namespace stormtty::windows
