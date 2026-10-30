#pragma once

#if !defined(STORMTTY_METAL_COMPOSITOR_TESTS)
#error "Metal compositor test support must not be included by production targets"
#endif

#include "metal_compositor.hpp"
#include <array>

namespace stormtty::macos {

enum class MetalTestPoint {
    none,
    device,
    queue,
    library,
    pipeline,
    replacement_pipeline,
    white_texture,
    layer,
    before_cache_clear,
    before_command,
    before_encoder,
    generation_released,
    native_owner_unwind
};

struct MetalTestSnapshot {
    std::uint64_t serial{};
    std::uint64_t submitted{};
    std::uint64_t completed{};
    unsigned in_flight{};
    bool failed{};
    bool has_target{};
};

struct MetalGenerationProbe {
    std::shared_ptr<std::atomic<unsigned>> count;
    std::shared_ptr<std::atomic<bool>> failed;
    std::weak_ptr<void> presentation;
};

struct MetalTestTrace {
    std::array<MetalTestPoint, 128> points{};
    std::size_t size{};
    bool overflow{};
};

// Compiled only in the isolated native test executable. No production resource
// getters, checkpoints, held commands or test policy switches are emitted.
class MetalCompositorTestAccess final {
public:
    static void reset() noexcept;
    static void fail_at(MetalTestPoint point) noexcept;
    static MetalTestTrace trace() noexcept;
    static MetalTestSnapshot snapshot(const MetalCompositor& owner);
    static MetalGenerationProbe generation(const MetalCompositor& owner);
    static id<MTLSharedEvent> make_event(const MetalCompositor& owner);
    static void hold_frames(id<MTLSharedEvent> event) noexcept;
    static bool retained_references(const MetalFrame& frame) noexcept;
};

} // namespace stormtty::macos
