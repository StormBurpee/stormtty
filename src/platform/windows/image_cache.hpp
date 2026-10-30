#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

struct ID2D1Bitmap1;
struct ID2D1DeviceContext;

namespace stormtty {
struct ImagePixels;

namespace windows {

struct ImageCacheOptions {
    std::size_t byte_budget{};
    std::uint32_t maximum_dimension{};
};

struct ImageCacheStats {
    std::uint64_t uploads{};
    std::size_t retained_bytes{};
};

// Renderer-thread-confined owner of device-dependent image bitmaps. No context,
// scene, placement or pixel payload is retained. Clear before device replacement.
class ImageCache final {
public:
    explicit ImageCache(ImageCacheOptions options);
    ~ImageCache();
    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    // Context and pixels are borrowed for this call only. Use one D2D device
    // until clear(). The returned bitmap is borrowed: consume synchronously,
    // before another bitmap() call, clear(), or destruction. Rejected input
    // returns nullptr; native upload failures preserve their GraphicsError.
    // Hits precede validation: an unchanged key denotes immutable pixel content.
    ID2D1Bitmap1* bitmap(
        ID2D1DeviceContext& context,
        std::uint64_t pane_id,
        const ImagePixels& pixels,
        std::uint64_t render_tick);

    // Releases GPU residency, but preserves the cumulative upload count.
    void clear() noexcept;
    ImageCacheStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace windows
} // namespace stormtty
