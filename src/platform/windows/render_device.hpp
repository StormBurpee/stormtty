#pragma once

#include "paint_surface.hpp"

#include <memory>

namespace stormtty::windows {

class ImageCache;
#ifdef STORMTTY_RENDER_DEVICE_TESTS
class RenderDevice;
namespace testing {
void taint_canvas(RenderDevice& device) noexcept;
}
#endif

struct DeviceOptions {
    void* window{};
    bool force_warp{};
    bool composition{};
};

struct SurfaceChanges {
    bool repaint{};
    bool canvas_installed{};
};

struct DeviceCapabilities {
    bool warp{};
    bool color_glyphs{};
    bool composition{};
    bool host_backdrop{};
};

enum class DeviceRelease { replacement, final_shutdown };

// A noncopyable draw borrow. The owning RenderDevice and its canvas must outlive
// this scope and all PaintSurface/clip borrows. No resize, reset or presentation
// is valid during the scope. finish() ends drawing exactly once; unwinding calls
// EndDraw without replacing the original paint exception.
class CanvasDraw final {
public:
    ~CanvasDraw();
    CanvasDraw(const CanvasDraw&) = delete;
    CanvasDraw& operator=(const CanvasDraw&) = delete;

    PaintSurface& surface() noexcept;
    void clear() noexcept;
    HRESULT finish(bool full_repaint) noexcept;

private:
    friend class RenderDevice;
    CanvasDraw(
        ID2D1DeviceContext& context,
        ID2D1Bitmap1& target,
        ID2D1SolidColorBrush& brush,
        ID2D1Brush* replacement,
        ID2D1DeviceContext4* color_context,
        bool& tainted) noexcept;

    ID2D1DeviceContext& context_;
    PaintSurface surface_;
    bool& tainted_;
    bool glass_paint_;
    bool active_{true};
};

// Renderer-thread native resource graph. Keeps canvas, device and presentation
// lifetimes together; owns no scene, damage history, text or image-cache policy.
// Its caller's COM apartment outlives it. Construction allocates no GPU objects;
// initialize() preserves the renderer's validation/font initialization ordering.
class RenderDevice final {
public:
    explicit RenderDevice(DeviceOptions options);
    ~RenderDevice();
    RenderDevice(const RenderDevice&) = delete;
    RenderDevice& operator=(const RenderDevice&) = delete;

    void initialize(SurfaceChanges& changes);
    void recreate(ImageCache& images, SurfaceChanges& changes);
    void release(DeviceRelease mode, ImageCache& images, SurfaceChanges& changes) noexcept;

    // Constructor-failure unwinding only: release original pre-TextResources
    // slots without unbinding, flushing or waiting. Caller destroys text next;
    // this owner's remaining slots then unwind in original declaration order.
    void release_failed_initialization_canvas() noexcept;
    void enable_teardown_trace() noexcept;
    void set_teardown_trace(bool enabled) noexcept;

    bool ready() const noexcept;
    bool matches_size(std::uint32_t width, std::uint32_t height) const noexcept;
    PixelRect bounds() const noexcept;
    DeviceCapabilities capabilities() const noexcept;
    bool material_enabled() const noexcept;
    void* frame_latency_wait_handle() const noexcept;

    // SurfaceChanges is a synchronous output, never retained. Renderer applies
    // it on success AND exception paths; a failed native replacement may have
    // stopped material even though no new canvas was installed.
    void resize(std::uint32_t width, std::uint32_t height, SurfaceChanges& changes);
    void set_additional_blur(float sigma, SurfaceChanges& changes);
    void prepare_background(float dpi, SurfaceChanges& changes);
    void mark_canvas_write() noexcept;
    CanvasDraw begin_canvas_draw();
    RenderResult present_canvas();

    bool liquid_glass_available() const noexcept;
    bool start_liquid_glass(void* monitor, SurfaceChanges& changes);
    void stop_liquid_glass(const char* reason, SurfaceChanges& changes) noexcept;
    bool liquid_glass_active() const noexcept;
    std::string liquid_glass_status() const;
    bool poll_liquid_glass(float dpi, SurfaceChanges& changes);

    // The native canvas itself enforces active/stale-glass readback rejection.
    // Probe metadata is a snapshot supplied by the transaction coordinator.
    bool begin_alpha_probe(PixelRect area, RendererAlphaProbe metadata);
    std::optional<RendererAlphaProbe> poll_alpha_probe();
    void cancel_alpha_probe() noexcept;
    std::vector<std::uint8_t> read_pixels();

private:
#ifdef STORMTTY_RENDER_DEVICE_TESTS
    friend void testing::taint_canvas(RenderDevice& device) noexcept;
#endif
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stormtty::windows
