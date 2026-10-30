#import <AppKit/AppKit.h>
#include "../src/platform/macos/metal_compositor_test_support.hpp"
#include "stormtty/renderer.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <cstdlib>
#include <thread>
#include <type_traits>

namespace {

using namespace stormtty;
using namespace stormtty::macos;
using Access = MetalCompositorTestAccess;
constexpr std::size_t upload_limit = 64u * 1024u * 1024u;

static_assert(!std::is_copy_constructible_v<MetalFrame>);
static_assert(!std::is_move_constructible_v<MetalFrame>);
static_assert(!std::is_copy_constructible_v<MetalCompositor>);

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

MetalTextureResources textures() {
    return MetalTextureResources({4096, 4096, 16});
}

void resize(MetalCompositor& owner, std::uint32_t width, std::uint32_t height) {
    MetalCompositor::validate_surface_size(width, height);
    owner.update_layer_geometry(96);
    if (!owner.surface_matches(width, height)) owner.replace_surface(width, height, 96);
}

bool near(unsigned actual, unsigned expected) {
    return std::abs(int(actual) - int(expected)) <= 2;
}

void require_pixel(const std::vector<std::uint8_t>& pixels, unsigned width,
                   unsigned x, unsigned y, Color expected) {
    const auto offset = (std::size_t(y) * width + x) * 4;
    require(near(pixels.at(offset), expected.r) && near(pixels.at(offset + 1), expected.g) &&
            near(pixels.at(offset + 2), expected.b) && near(pixels.at(offset + 3), expected.a),
            "Offscreen composition/readback pixel mismatch");
}

void wait_for_zero(const std::shared_ptr<std::atomic<unsigned>>& count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (count->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(count->load() == 0, "Native command completion exceeded test deadline");
}

// Every failure path signals the GPU before releasing this event. No held queue
// is left waiting on an assertion, allocation failure or test exception.
class HeldFrames final {
public:
    explicit HeldFrames(const MetalCompositor& owner) : event_(Access::make_event(owner)) {
        require(event_ != nil, "Native Metal shared event unavailable");
        Access::hold_frames(event_);
    }

    ~HeldFrames() { release(); }

    void release() noexcept {
        event_.signaledValue = 1;
        Access::hold_frames(nil);
    }

private:
    id<MTLSharedEvent> event_;
};

void composition_and_readback_test() {
    Access::reset();
    auto cache = textures();
    MetalCompositor owner(nullptr);
    owner.create_device(cache);
    require(!owner.has_surface() && owner.read_pixels().empty(), "Unallocated surface readback changed");
    const auto allowance = cache.remaining_upload_bytes();
    {
        auto absent = owner.begin_frame(cache);
        require(!absent, "Targetless compositor admitted a frame");
    }
    require(cache.remaining_upload_bytes() == allowance && Access::snapshot(owner).serial == 0,
            "Targetless admission reset accounting");
    resize(owner, 7, 5); // Exercises readback's 256-byte stride padding.
    {
        auto paint = owner.begin_frame(cache);
        require(bool(paint) && Access::retained_references(paint), "Expected retained-resource frame");
        paint.fill({0, 0, 7, 5}, {255, 0, 0, 255});
        require(paint.clip({1, 2, 4, 2}), "Valid clip rejected");
        paint.fill({0, 0, 7, 5}, {0, 0, 255, 128}, true);
        paint.submit();
    }
    auto pixels = owner.read_pixels();
    require(pixels.size() == 7u * 5u * 4u, "Aligned readback returned padded bytes");
    require_pixel(pixels, 7, 0, 4, {255, 0, 0, 255});
    require_pixel(pixels, 7, 2, 3, {0, 0, 255, 128});
    require_pixel(pixels, 7, 6, 4, {255, 0, 0, 255});
    {
        auto paint = owner.begin_frame(cache);
        paint.fill({0, 0, 7, 5}, {255, 0, 0, 255});
        const std::uint8_t bgra[] = {0, 0, 0, 0, 0, 255, 0, 255};
        paint.draw_bgra(2, 1, bgra, {0, 0, 2, 1}, {255, 255, 255, 255}, true);
        paint.submit();
    }
    pixels = owner.read_pixels();
    require_pixel(pixels, 7, 0, 0, {255, 0, 0, 255});
    require_pixel(pixels, 7, 1, 0, {0, 255, 0, 255});
    require(owner.surface_matches(7, 5), "Unchanged target dimensions not recognized");
    resize(owner, 0, 0);
    require(!owner.has_surface() && owner.read_pixels().empty(), "Zero-size target not suspended");
}

void held_generations_test() {
    Access::reset();
    auto cache = textures();
    MetalCompositor owner(nullptr);
    owner.create_device(cache);
    resize(owner, 8, 8);
    const auto old = Access::generation(owner);
    HeldFrames held(owner);
    const std::uint8_t bgra[] = {0, 0, 255, 255};
    for (unsigned index = 0; index < 2; ++index) {
        auto paint = owner.begin_frame(cache);
        require(bool(paint) && Access::retained_references(paint), "Held frame admission failed");
        paint.draw_bgra(1, 1, bgra, {0, 0, 8, 8});
        paint.submit();
    }
    const auto before = Access::snapshot(owner);
    const auto allowance = cache.remaining_upload_bytes();
    require(before.in_flight == 2 && owner.submission_busy(), "Two held buffers did not saturate generation");
    {
        auto busy = owner.begin_frame(cache);
        require(!busy, "Third in-flight frame admitted");
    }
    require(Access::snapshot(owner).serial == before.serial && cache.remaining_upload_bytes() == allowance,
            "Busy return changed serial or upload allowance");

    // Reconstruct while old retained command buffers still wait. No owner/cache
    // capture enters their callbacks, and no wait is added to reconstruction.
    owner.create_device(cache);
    resize(owner, 8, 8);
    auto fresh = Access::generation(owner);
    require(fresh.count != old.count && fresh.failed != old.failed, "Reconstruction reused callback generation");
    require(Access::snapshot(owner).serial == before.serial && fresh.count->load() == 0,
            "Reconstruction reset serial or inherited old count");
    Access::hold_frames(nil);
    {
        auto paint = owner.begin_frame(cache);
        paint.fill({0, 0, 8, 8}, {0, 255, 0, 255});
        paint.submit();
    }
    require_pixel(owner.read_pixels(), 8, 4, 4, {0, 255, 0, 255});
    wait_for_zero(fresh.count);
    held.release();
    wait_for_zero(old.count);
    require(fresh.count->load() == 0 && !fresh.failed->load() && !old.failed->load(),
            "Old completion damaged new generation or lost retained resources");
    require(Access::snapshot(owner).serial == before.serial + 1, "Frame serial not monotonic across reconstruction");
}

void abandoned_frame_test() {
    Access::reset();
    auto cache = textures();
    MetalCompositor owner(nullptr);
    owner.create_device(cache);
    resize(owner, 8, 8);
    const auto initial = Access::snapshot(owner);
    bool abandoned = false;
    @autoreleasepool {
        try {
            auto paint = owner.begin_frame(cache);
            require(bool(paint), "Abandonment fixture did not admit a frame");
            const std::uint8_t bgra[] = {0, 0, 255, 255};
            paint.draw_bgra(1, 1, bgra, {0, 0, 8, 8});
            std::cerr << "abandonment: uploaded; throwing with active encoder" << std::endl;
            throw std::runtime_error("Synthetic paint abandonment");
        } catch (const std::runtime_error& error) {
            abandoned = std::string_view(error.what()) == "Synthetic paint abandonment";
            std::cerr << "abandonment: caught; draining autorelease pool" << std::endl;
        }
    }
    std::cerr << "abandonment: pool drained" << std::endl;
    const auto after = Access::snapshot(owner);
    require(abandoned && after.serial == initial.serial + 1 &&
            after.in_flight == 0 && after.completed == initial.completed,
            "Abandoned frame submitted work or rolled back admission");
    require(cache.remaining_upload_bytes() == upload_limit - 4,
            "Abandoned frame rolled back consumed upload bytes");
    {
        auto paint = owner.begin_frame(cache);
        require(bool(paint), "Abandoned frame blocked later admission");
        require(cache.remaining_upload_bytes() == upload_limit,
                "Later accepted frame did not begin a fresh upload batch");
        paint.fill({0, 0, 8, 8}, {0, 0, 255, 255});
        paint.submit();
    }
    require_pixel(owner.read_pixels(), 8, 3, 3, {0, 0, 255, 255});
    wait_for_zero(Access::generation(owner).count);
}

void failure_order_test() {
    Access::reset();
    auto cache = textures();
    MetalCompositor owner(nullptr);
    owner.create_device(cache);
    resize(owner, 8, 8);
    const auto initial = Access::snapshot(owner);
    Access::fail_at(MetalTestPoint::before_command);
    bool failed = false;
    try {
        auto paint = owner.begin_frame(cache);
        (void)paint;
    } catch (const std::runtime_error&) {
        failed = true;
    }
    require(failed, "Pre-command checkpoint did not fail");
    const auto admitted = Access::snapshot(owner);
    require(admitted.serial == initial.serial + 1 && admitted.in_flight == 0 &&
            admitted.completed == 0 && cache.remaining_upload_bytes() == upload_limit,
            "Pre-command failure rolled back admission or fabricated submission");

    Access::fail_at(MetalTestPoint::before_encoder);
    failed = false;
    try {
        auto paint = owner.begin_frame(cache);
        (void)paint;
    } catch (const std::runtime_error&) {
        failed = true;
    }
    require(failed && Access::snapshot(owner).in_flight == 0,
            "Encoder creation failure submitted work");
    Access::fail_at(MetalTestPoint::none);

    const auto old = Access::generation(owner);
    const auto serial = Access::snapshot(owner).serial;
    Access::fail_at(MetalTestPoint::queue);
    failed = false;
    try {
        owner.create_device(cache);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    const auto replacement = Access::generation(owner);
    require(failed && replacement.count != old.count && old.presentation.expired(),
            "Reconstruction did not replace observer state before acquisition failure");
    require(owner.has_surface() && Access::snapshot(owner).serial == serial &&
            cache.remaining_upload_bytes() == upload_limit,
            "Partial reconstruction cleared target/serial or failed to begin upload batch");
    Access::fail_at(MetalTestPoint::none);
    owner.create_device(cache);
    require(!owner.has_surface(), "Successful reconstruction did not clear old target");
}

void renderer_unwind_test() {
    const auto verify_trace = [] {
        const auto trace = Access::trace();
        require(!trace.overflow && trace.size >= 2, "Missing lifecycle trace");
        require(trace.points[trace.size - 2] == MetalTestPoint::generation_released &&
                trace.points[trace.size - 1] == MetalTestPoint::native_owner_unwind,
                "Owner generations were not released before native graph unwind");
    };
    RendererOptions options;
    options.font.family = "Menlo";
    Access::reset();
    {
        Renderer renderer(nullptr, options);
        renderer.resize(8, 8);
    }
    verify_trace();

    Access::reset();
    options.force_warp = true;
    bool failed = false;
    try {
        Renderer renderer(nullptr, options);
    } catch (const std::invalid_argument&) {
        failed = true;
    }
    require(failed && Access::trace().size == 2, "Validation acquired native resources");
    verify_trace();
    options.force_warp = false;

    for (const auto point : {MetalTestPoint::device, MetalTestPoint::queue, MetalTestPoint::library,
                             MetalTestPoint::pipeline, MetalTestPoint::replacement_pipeline,
                             MetalTestPoint::white_texture, MetalTestPoint::before_cache_clear}) {
        Access::reset();
        Access::fail_at(point);
        failed = false;
        try {
            Renderer renderer(nullptr, options);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed, "Acquisition checkpoint did not fail renderer construction");
        verify_trace();
    }
    Access::reset();
    options.font.family.clear();
    failed = false;
    try {
        Renderer renderer(nullptr, options);
    } catch (const std::invalid_argument&) {
        failed = true;
    }
    require(failed, "Late font validation did not fail renderer construction");
    verify_trace();
    Access::reset();
}

void unattached_view_status_test() {
    Access::reset();
    auto cache = textures();
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 8, 8)];
    MetalCompositor owner((__bridge void*)view);
    owner.create_device(cache);
    resize(owner, 8, 8);
    const auto allowance = cache.remaining_upload_bytes();
    owner.check_gpu_health();
    require(owner.is_occluded(), "Unattached view should be occluded");
    require(cache.remaining_upload_bytes() == allowance && Access::snapshot(owner).serial == 0,
            "Occlusion observation changed admission accounting");
}

} // namespace

int main() {
    @autoreleasepool {
        try {
            std::cerr << "BEGIN composition_and_readback" << std::endl;
            composition_and_readback_test();
            std::cerr << "PASS composition_and_readback" << std::endl;
            std::cerr << "BEGIN held_generations" << std::endl;
            held_generations_test();
            std::cerr << "PASS held_generations" << std::endl;
            std::cerr << "BEGIN abandoned_frame" << std::endl;
            abandoned_frame_test();
            std::cerr << "PASS abandoned_frame" << std::endl;
            std::cerr << "BEGIN failure_order" << std::endl;
            failure_order_test();
            std::cerr << "PASS failure_order" << std::endl;
            std::cerr << "BEGIN renderer_unwind" << std::endl;
            renderer_unwind_test();
            std::cerr << "PASS renderer_unwind" << std::endl;
            std::cerr << "BEGIN unattached_view_status" << std::endl;
            unattached_view_status_test();
            std::cerr << "PASS unattached_view_status" << std::endl;
            std::cout << "PASS native macOS Metal compositor ownership/admission/readback\n";
            return 0;
        } catch (const std::exception& error) {
            // HeldFrames signals on unwinding before reaching this handler.
            Access::reset();
            std::cerr << "FAIL macOS Metal compositor: " << error.what() << '\n';
            return 1;
        }
    }
}
