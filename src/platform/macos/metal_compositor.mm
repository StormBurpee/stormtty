#include "metal_compositor.hpp"
#include "cf_owner.hpp"
#if defined(STORMTTY_METAL_COMPOSITOR_TESTS)
#include "metal_compositor_test_support.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace stormtty::macos {
namespace {

constexpr std::size_t maximum_surface_bytes = 128u * 1024u * 1024u;

#if defined(STORMTTY_METAL_COMPOSITOR_TESTS)
MetalTestPoint failure_point = MetalTestPoint::none;
MetalTestTrace test_trace;
id<MTLSharedEvent> held_frames;

void record_test_point(MetalTestPoint point) noexcept {
    if (test_trace.size < test_trace.points.size()) {
        test_trace.points[test_trace.size++] = point;
    } else {
        test_trace.overflow = true;
    }
}

void checkpoint(MetalTestPoint point) {
    record_test_point(point);
    if (failure_point == point) {
        throw std::runtime_error("Injected Metal acquisition failure");
    }
}
#define STORMTTY_METAL_CHECKPOINT(point) checkpoint(MetalTestPoint::point)
#else
#define STORMTTY_METAL_CHECKPOINT(point) ((void)0)
#endif

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

NSString* string(std::string_view text) {
    NSString* result = [[NSString alloc] initWithBytes:text.data() length:text.size()
                                            encoding:NSUTF8StringEncoding];
    return result ? result : @"\uFFFD";
}

bool finite(PixelRect rect) {
    return std::isfinite(rect.x) && std::isfinite(rect.y) &&
           std::isfinite(rect.width) && std::isfinite(rect.height) &&
           std::abs(rect.x) <= 1e7f && std::abs(rect.y) <= 1e7f &&
           std::abs(rect.width) <= 1e7f && std::abs(rect.height) <= 1e7f;
}

struct alignas(16) Quad {
    float destination[4];
    float uv[4];
    float viewport[2];
};
static_assert(sizeof(Quad) == 48, "Metal constant-buffer ABI requires 16-byte rounded alignment");

struct Tint {
    float value[4];
};

constexpr const char* shader_source=R"metal(
#include <metal_stdlib>
using namespace metal;
struct Quad { float4 destination; float4 uv; float2 viewport; };
struct Vertex { float4 position [[position]]; float2 uv; };
vertex Vertex quad_vertex(uint n [[vertex_id]],constant Quad& q [[buffer(0)]]) {
    const float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    float2 p=q.destination.xy+corners[n]*q.destination.zw;
    Vertex v;v.position=float4(p.x/q.viewport.x*2-1,1-p.y/q.viewport.y*2,0,1);
    v.uv=q.uv.xy+corners[n]*q.uv.zw;return v;
}
fragment float4 quad_fragment(Vertex v [[stage_in]],texture2d<float> t [[texture(0)]],
    constant float4& tint [[buffer(0)]]) {
    constexpr sampler s(coord::normalized,filter::nearest,address::clamp_to_edge);
    return t.sample(s,v.uv)*tint;
}
fragment float4 quad_replace_fragment(Vertex v [[stage_in]],texture2d<float> t [[texture(0)]],
    constant float4& tint [[buffer(0)]]) {
    constexpr sampler s(coord::normalized,filter::nearest,address::clamp_to_edge);
    const float4 pixel=t.sample(s,v.uv);
    // Bounded CoreGraphics overlays include transparent padding. COPY applies
    // to their covered shape, not its otherwise-transparent bounding rectangle.
    if(pixel.a==0)discard_fragment();
    return pixel*tint;
}
)metal";

} // namespace

struct MetalPresentation {
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t serial{};
    std::uint64_t submitted{};
    std::uint64_t scheduled{};
    std::uint64_t completed{};
    std::uint64_t callbacks{};
    std::uint64_t zero_time_callbacks{};
    double last_time{};
    bool gpu_error{};
    std::string surface;
};

MetalCompositor::MetalCompositor(void* view)
    : view_((__bridge NSView*)view),
      in_flight_(std::make_shared<std::atomic<unsigned>>(0)),
      gpu_failed_(std::make_shared<std::atomic<bool>>(false)),
      presented_(std::make_shared<MetalPresentation>()) {}

MetalCompositor::~MetalCompositor() {
#if defined(STORMTTY_METAL_COMPOSITOR_TESTS)
    record_test_point(MetalTestPoint::native_owner_unwind);
#endif
}

void MetalCompositor::release_generation_references_for_unwind() noexcept {
    presented_.reset();
    gpu_failed_.reset();
    in_flight_.reset();
#if defined(STORMTTY_METAL_COMPOSITOR_TESTS)
    record_test_point(MetalTestPoint::generation_released);
#endif
}

void MetalCompositor::create_device(MetalTextureResources& textures) {
    // Intentionally nontransactional, matching the original reconstruction.
    // Late callbacks hold the old shared states, never this native owner.
    textures.begin_upload_batch();
    in_flight_ = std::make_shared<std::atomic<unsigned>>(0);
    gpu_failed_ = std::make_shared<std::atomic<bool>>(false);
    presented_ = std::make_shared<MetalPresentation>();
    device_ = MTLCreateSystemDefaultDevice();
    check(device_ != nil, "A supported Metal device is required");
    STORMTTY_METAL_CHECKPOINT(device);
    queue_ = [device_ newCommandQueue];
    check(queue_ != nil, "Cannot create Metal command queue");
    STORMTTY_METAL_CHECKPOINT(queue);

    NSError* error = nil;
    id<MTLLibrary> library = [device_ newLibraryWithSource:string(shader_source)
                                                options:nil error:&error];
    if (!library) {
        const char* reason = error.localizedDescription.UTF8String;
        throw std::runtime_error("Metal shader compile: " + std::string(reason ? reason : "unknown error"));
    }
    STORMTTY_METAL_CHECKPOINT(library);
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"quad_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"quad_fragment"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    auto attachment = descriptor.colorAttachments[0];
    attachment.blendingEnabled = YES;
    attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
    attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
    attachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeline_ = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
    check(pipeline_ != nil, "Cannot create Metal composition pipeline");
    STORMTTY_METAL_CHECKPOINT(pipeline);
    attachment.blendingEnabled = NO;
    descriptor.fragmentFunction = [library newFunctionWithName:@"quad_replace_fragment"];
    replace_pipeline_ = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
    check(replace_pipeline_ != nil, "Cannot create Metal replacement pipeline");
    STORMTTY_METAL_CHECKPOINT(replacement_pipeline);

    const std::uint8_t pixel[4] = {255, 255, 255, 255};
    white_ = textures.upload_bgra(device_, 1, 1, pixel);
    STORMTTY_METAL_CHECKPOINT(white_texture);
    if (view_) {
        layer_ = [CAMetalLayer layer];
        layer_.device = device_;
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer_.framebufferOnly = NO;
        layer_.opaque = NO;
        layer_.maximumDrawableCount = 3;
        layer_.allowsNextDrawableTimeout = YES;
        layer_.displaySyncEnabled = YES;
        CfOwner<CGColorSpaceRef> color(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
        layer_.colorspace = color.get();
        // This is a layer-hosting view; retain its explicit native geometry.
        layer_.frame = view_.bounds;
        view_.layer = layer_;
        view_.wantsLayer = YES;
        STORMTTY_METAL_CHECKPOINT(layer);
    }
    STORMTTY_METAL_CHECKPOINT(before_cache_clear);
    textures.clear_device_textures();
    target_ = nil;
    gpu_failed_->store(false);
}

void MetalCompositor::validate_surface_size(std::uint32_t width, std::uint32_t height) {
    if (width > 16384 || height > 16384 || std::uint64_t(width) * height * 4 > maximum_surface_bytes) {
        throw std::invalid_argument("Metal window surface exceeds 128 MiB budget");
    }
}

void MetalCompositor::update_layer_geometry(float dpi) {
    if (layer_) {
        layer_.contentsScale = dpi / 96.;
        if (view_) {
            layer_.frame = view_.bounds;
        }
    }
}

bool MetalCompositor::has_surface() const noexcept {
    return target_ != nil;
}

bool MetalCompositor::surface_matches(std::uint32_t width, std::uint32_t height) const noexcept {
    return width_ == width && height_ == height && target_ != nil;
}

void MetalCompositor::replace_surface(std::uint32_t width, std::uint32_t height, float dpi) {
    width_ = width;
    height_ = height;
    target_ = nil;
    if (layer_) {
        layer_.contentsScale = dpi / 96.;
        layer_.drawableSize = CGSizeMake(width, height);
    }
    if (!width || !height) {
        return;
    }
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                      width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    target_ = [device_ newTextureWithDescriptor:descriptor];
    check(target_ != nil, "Cannot allocate Metal composition surface");
}

void MetalCompositor::check_gpu_health() const {
    if (gpu_failed_->load()) {
        throw std::runtime_error("Metal command buffer failed; renderer reconstruction is required");
    }
}

bool MetalCompositor::is_occluded() const {
    if (view_ && (!view_.window.visible ||
                  (view_.window.occlusionState & NSWindowOcclusionStateVisible) == 0)) {
        return true;
    }
    return false;
}

bool MetalCompositor::submission_busy() const noexcept {
    return in_flight_->load() >= 2;
}

MetalFrame MetalCompositor::begin_frame(MetalTextureResources& textures) {
    return MetalFrame(*this, textures);
}

MetalFrame::MetalFrame(MetalCompositor& owner, MetalTextureResources& textures)
    : owner_(owner), textures_(textures) {
    // The renderer checks before entering its pool; this also keeps direct
    // callers from acquiring a drawable while the generation is saturated.
    if (!owner_.has_surface() || owner_.submission_busy()) {
        return;
    }
    drawable_ = owner_.layer_ ? [owner_.layer_ nextDrawable] : nil;
    if (owner_.layer_ && !drawable_) {
        return;
    }
    ++owner_.serial_;
    textures_.begin_upload_batch();
    if (owner_.layer_) {
        std::ostringstream details;
        const auto frame = owner_.layer_.frame;
        const auto bounds = owner_.layer_.bounds;
        details << "attached=" << (owner_.layer_.superlayer != nil)
                << " hidden=" << bool(owner_.layer_.hidden)
                << " opacity=" << owner_.layer_.opacity
                << " frame=" << frame.origin.x << ',' << frame.origin.y << ',' << frame.size.width << ',' << frame.size.height
                << " bounds=" << bounds.origin.x << ',' << bounds.origin.y << ',' << bounds.size.width << ',' << bounds.size.height
                << " drawable=" << owner_.layer_.drawableSize.width << 'x' << owner_.layer_.drawableSize.height
                << " scale=" << owner_.layer_.contentsScale;
        std::lock_guard lock(owner_.presented_->mutex);
        // Preserve the pre-command diagnostic meaning, including later failures.
        owner_.presented_->submitted = owner_.serial_;
        owner_.presented_->surface = details.str();
    }
    STORMTTY_METAL_CHECKPOINT(before_command);
    command_ = [owner_.queue_ commandBuffer];
    check(command_ != nil, "Cannot create Metal command buffer");
#if defined(STORMTTY_METAL_COMPOSITOR_TESTS)
    if (held_frames) {
        [command_ encodeWaitForEvent:held_frames value:1];
    }
#endif
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = owner_.target_;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    STORMTTY_METAL_CHECKPOINT(before_encoder);
    encoder_ = [command_ renderCommandEncoderWithDescriptor:pass];
    check(encoder_ != nil, "Cannot create Metal render encoder");
}

// Abandonment only releases native references. Do not silently end encoding,
// submit, wait, restore upload accounting or update renderer scene history.
MetalFrame::~MetalFrame() = default;

MetalFrame::operator bool() const noexcept {
    return encoder_ != nil;
}

void MetalFrame::draw(id<MTLTexture> texture, PixelRect rect, Color tint, bool replace, PixelRect uv) {
    if (!finite(rect) || !finite(uv) || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    Quad quad{{rect.x, rect.y, rect.width, rect.height}, {uv.x, uv.y, uv.width, uv.height},
              {float(owner_.width_), float(owner_.height_)}};
    const float alpha = tint.a / 255.f;
    Tint color{{tint.r / 255.f * alpha, tint.g / 255.f * alpha, tint.b / 255.f * alpha, alpha}};
    [encoder_ setRenderPipelineState:replace ? owner_.replace_pipeline_ : owner_.pipeline_];
    [encoder_ setVertexBytes:&quad length:sizeof(quad) atIndex:0];
    [encoder_ setFragmentBytes:&color length:sizeof(color) atIndex:0];
    [encoder_ setFragmentTexture:texture atIndex:0];
    [encoder_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

bool MetalFrame::clip(PixelRect rect) {
    if (!finite(rect) || rect.width <= 0 || rect.height <= 0) {
        return false;
    }
    const auto x = std::clamp(std::floor(rect.x), 0.f, float(owner_.width_));
    const auto y = std::clamp(std::floor(rect.y), 0.f, float(owner_.height_));
    const auto right = std::clamp(std::ceil(rect.x + rect.width), 0.f, float(owner_.width_));
    const auto bottom = std::clamp(std::ceil(rect.y + rect.height), 0.f, float(owner_.height_));
    if (right <= x || bottom <= y) {
        return false;
    }
    [encoder_ setScissorRect:MTLScissorRect{std::size_t(x), std::size_t(y),
                                         std::size_t(right - x), std::size_t(bottom - y)}];
    return true;
}

void MetalFrame::fill(PixelRect rect, Color tint, bool replace) {
    draw(owner_.white_, rect, tint, replace);
}

void MetalFrame::draw_row(RowTextureIdentity identity, const FrameRow& row, CTFontRef font,
                          RowRasterOptions options, PixelRect destination) {
    id<MTLTexture> texture = textures_.row_texture(owner_.device_, identity, row, font, options, owner_.serial_);
    draw(texture, destination, {255, 255, 255, 255}, false);
}

void MetalFrame::draw_image(std::uint64_t pane, const ImagePixels& pixels,
                            PixelRect destination, PixelRect uv) {
    id<MTLTexture> texture = textures_.image_texture(owner_.device_, pane, pixels, owner_.serial_);
    if (texture) {
        draw(texture, destination, {255, 255, 255, 255}, false, uv);
    }
}

void MetalFrame::draw_bgra(std::uint32_t width, std::uint32_t height,
                           std::span<const std::uint8_t> pixels, PixelRect destination,
                           Color tint, bool replace) {
    id<MTLTexture> texture = textures_.upload_bgra(owner_.device_, width, height, pixels);
    draw(texture, destination, tint, replace);
}

void MetalFrame::submit() {
    check(encoder_ != nil && !submitted_, "Metal frame must be admitted and submitted only once");
    [encoder_ endEncoding];
    if (drawable_) {
        auto presentation = owner_.presented_;
        const auto frame_serial = owner_.serial_;
        [drawable_ addPresentedHandler:^(id<MTLDrawable> shown) {
            {
                std::lock_guard lock(presentation->mutex);
                ++presentation->callbacks;
                presentation->last_time = shown.presentedTime;
                if (shown.presentedTime > 0) {
                    presentation->serial = std::max(presentation->serial, frame_serial);
                } else {
                    ++presentation->zero_time_callbacks;
                }
            }
            presentation->changed.notify_all();
        }];
        [command_ addScheduledHandler:^(id<MTLCommandBuffer> scheduled) {
            (void)scheduled;
            std::lock_guard lock(presentation->mutex);
            presentation->scheduled = std::max(presentation->scheduled, frame_serial);
        }];
        id<MTLBlitCommandEncoder> blit = [command_ blitCommandEncoder];
        [blit copyFromTexture:owner_.target_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(owner_.width_, owner_.height_, 1)
                    toTexture:drawable_.texture destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [command_ presentDrawable:drawable_];
    }
    auto count = owner_.in_flight_;
    auto failure = owner_.gpu_failed_;
    auto presentation = owner_.presented_;
    const auto frame_serial = owner_.serial_;
    count->fetch_add(1);
    [command_ addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        const bool failed = buffer.status == MTLCommandBufferStatusError;
        if (failed) {
            failure->store(true);
        }
        count->fetch_sub(1);
        {
            std::lock_guard lock(presentation->mutex);
            presentation->completed = std::max(presentation->completed, frame_serial);
            presentation->gpu_error |= failed;
        }
        presentation->changed.notify_all();
    }];
    [command_ commit];
    submitted_ = true;
}

std::vector<std::uint8_t> MetalCompositor::read_pixels() {
    if (!target_) {
        return {};
    }
    const auto stride = (std::size_t(width_) * 4 + 255) & ~std::size_t(255);
    const bool shared = [device_ supportsFamily:MTLGPUFamilyApple1];
    auto buffer = [device_ newBufferWithLength:stride * height_
                                      options:shared ? MTLResourceStorageModeShared : MTLResourceStorageModeManaged];
    check(buffer != nil, "Cannot allocate Metal readback buffer");
    auto command = [queue_ commandBuffer];
    auto blit = [command blitCommandEncoder];
    [blit copyFromTexture:target_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(width_, height_, 1) toBuffer:buffer destinationOffset:0
       destinationBytesPerRow:stride destinationBytesPerImage:stride * height_];
    if (!shared) {
        [blit synchronizeResource:buffer];
    }
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    check(command.status == MTLCommandBufferStatusCompleted, "Metal readback failed");
    if (layer_) {
        std::unique_lock lock(presented_->mutex);
        auto& observed = *presented_;
        const bool signalled = observed.changed.wait_for(lock, std::chrono::seconds(3), [&] {
            return observed.serial >= serial_ || observed.gpu_error;
        });
        if (!signalled || observed.gpu_error || observed.serial < serial_) {
            std::ostringstream error;
            error << "Metal drawable was not observed presented onscreen: requested=" << serial_
                  << " submitted=" << observed.submitted << " scheduled=" << observed.scheduled
                  << " completed=" << observed.completed << " presented=" << observed.serial
                  << " callbacks=" << observed.callbacks << " zeroTimeCallbacks=" << observed.zero_time_callbacks
                  << " lastPresentedTime=" << observed.last_time << " gpuError=" << observed.gpu_error
                  << " layer{" << observed.surface << '}';
            throw std::runtime_error(error.str());
        }
    }
    std::vector<std::uint8_t> rgba(std::size_t(width_) * height_ * 4);
    auto source = static_cast<const std::uint8_t*>(buffer.contents);
    for (std::uint32_t y = 0; y < height_; ++y) {
        for (std::uint32_t x = 0; x < width_; ++x) {
            const auto from = std::size_t(y) * stride + x * 4;
            const auto to = (std::size_t(y) * width_ + x) * 4;
            const unsigned alpha = source[from + 3];
            rgba[to + 3] = static_cast<std::uint8_t>(alpha);
            const unsigned components[3] = {source[from + 2], source[from + 1], source[from]};
            for (unsigned component = 0; component < 3; ++component) {
                rgba[to + component] = alpha
                    ? static_cast<std::uint8_t>(std::min(255u, (components[component] * 255 + alpha / 2) / alpha))
                    : 0;
            }
        }
    }
    return rgba;
}

#if defined(STORMTTY_METAL_COMPOSITOR_TESTS)
void MetalCompositorTestAccess::reset() noexcept {
    failure_point = MetalTestPoint::none;
    test_trace = {};
    held_frames = nil;
}

void MetalCompositorTestAccess::fail_at(MetalTestPoint point) noexcept {
    failure_point = point;
}

MetalTestTrace MetalCompositorTestAccess::trace() noexcept {
    return test_trace;
}

MetalTestSnapshot MetalCompositorTestAccess::snapshot(const MetalCompositor& owner) {
    std::lock_guard lock(owner.presented_->mutex);
    return {owner.serial_, owner.presented_->submitted, owner.presented_->completed,
            owner.in_flight_->load(), owner.gpu_failed_->load(), owner.target_ != nil};
}

MetalGenerationProbe MetalCompositorTestAccess::generation(const MetalCompositor& owner) {
    return {owner.in_flight_, owner.gpu_failed_, owner.presented_};
}

id<MTLSharedEvent> MetalCompositorTestAccess::make_event(const MetalCompositor& owner) {
    return [owner.device_ newSharedEvent];
}

void MetalCompositorTestAccess::hold_frames(id<MTLSharedEvent> event) noexcept {
    held_frames = event;
}

bool MetalCompositorTestAccess::retained_references(const MetalFrame& frame) noexcept {
    return frame.command_.retainedReferences;
}
#endif

} // namespace stormtty::macos
