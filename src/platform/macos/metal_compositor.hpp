#pragma once

#include "metal_texture_resources.hpp"
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <atomic>
#include <memory>

namespace stormtty::macos {

class MetalFrame;
struct MetalPresentation;
#if defined(STORMTTY_METAL_COMPOSITOR_TESTS)
class MetalCompositorTestAccess;
#endif

// Renderer-thread owner of native composition and submission resources. The
// host view is weak. No scene, font, texture cache or painter is retained.
class MetalCompositor final {
public:
    explicit MetalCompositor(void* view);
    ~MetalCompositor();

    MetalCompositor(const MetalCompositor&) = delete;
    MetalCompositor& operator=(const MetalCompositor&) = delete;

    // Explicit acquisition preserves renderer option-validation ordering.
    void create_device(MetalTextureResources& textures);

    // Shutdown/unwind only: drop owner references before external texture/font
    // owners unwind. Captured generations survive; no callbacks or native work.
    void release_generation_references_for_unwind() noexcept;

    static void validate_surface_size(std::uint32_t width, std::uint32_t height);
    void update_layer_geometry(float dpi);
    bool has_surface() const noexcept;
    bool surface_matches(std::uint32_t width, std::uint32_t height) const noexcept;
    void replace_surface(std::uint32_t width, std::uint32_t height, float dpi);

    // Call after scene bounds, before previous-scene comparison. GPU failure
    // precedes occlusion just as it does in the native host contract.
    void check_gpu_health() const;
    bool is_occluded() const;
    bool submission_busy() const noexcept;

    // Caller keeps its autorelease pool around admission, paint and submission.
    // No other frame, resize/recreation/readback/destruction is allowed during this borrow.
    MetalFrame begin_frame(MetalTextureResources& textures);

    // Host grants exclusive access on its readback worker. No AppKit callbacks
    // wait here; onscreen readback still requires positive presentedTime.
    std::vector<std::uint8_t> read_pixels();

private:
    friend class MetalFrame;
#if defined(STORMTTY_METAL_COMPOSITOR_TESTS)
    friend class MetalCompositorTestAccess;
#endif

    __weak NSView* view_;
    CAMetalLayer* layer_;
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLRenderPipelineState> pipeline_;
    id<MTLRenderPipelineState> replace_pipeline_;
    id<MTLTexture> target_;
    id<MTLTexture> white_;
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::uint64_t serial_{};
    std::shared_ptr<std::atomic<unsigned>> in_flight_;
    std::shared_ptr<std::atomic<bool>> gpu_failed_;
    std::shared_ptr<MetalPresentation> presented_;
};

// Synchronous, nonescaping native paint borrow. ARC command buffers retain
// encoded resources after submit; callbacks retain only generation state.
class MetalFrame final {
public:
    ~MetalFrame();
    MetalFrame(const MetalFrame&) = delete;
    MetalFrame& operator=(const MetalFrame&) = delete;
    MetalFrame(MetalFrame&&) = delete;
    MetalFrame& operator=(MetalFrame&&) = delete;

    explicit operator bool() const noexcept;
    bool clip(PixelRect rect);
    void fill(PixelRect rect, Color tint = {255, 255, 255, 255}, bool replace = false);
    void draw_row(RowTextureIdentity identity, const FrameRow& row, CTFontRef font,
                  RowRasterOptions options, PixelRect destination);
    void draw_image(std::uint64_t pane, const ImagePixels& pixels,
                    PixelRect destination, PixelRect uv);
    void draw_bgra(std::uint32_t width, std::uint32_t height,
                   std::span<const std::uint8_t> pixels, PixelRect destination,
                   Color tint = {255, 255, 255, 255}, bool replace = false);
    void submit();

private:
    friend class MetalCompositor;
#if defined(STORMTTY_METAL_COMPOSITOR_TESTS)
    friend class MetalCompositorTestAccess;
#endif
    MetalFrame(MetalCompositor& owner, MetalTextureResources& textures);

    void end_encoding() noexcept;

    void draw(id<MTLTexture> texture, PixelRect destination, Color tint,
              bool replace, PixelRect uv = {0, 0, 1, 1});

    MetalCompositor& owner_;
    MetalTextureResources& textures_;
    id<CAMetalDrawable> drawable_;
    id<MTLCommandBuffer> command_;
    id<MTLRenderCommandEncoder> encoder_;
    bool encoding_ended_{};
    bool submitted_{};
};

} // namespace stormtty::macos
