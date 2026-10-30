#pragma once

#include "core_text_row_rasterizer.hpp"
#import <Metal/Metal.h>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>

namespace stormtty::macos {

struct MetalTextureLimits {
    std::size_t row_bytes{};
    std::size_t image_bytes{};
    std::uint32_t maximum_image_dimension{};
};

struct RowTextureIdentity {
    std::uint64_t pane{};
    std::uint64_t font_generation{};
    std::uint32_t variant{};
};

struct MetalTextureStats {
    std::uint64_t shaped_rows{};
    std::uint64_t row_cache_hits{};
    std::uint64_t image_uploads{};
    std::uint64_t dirty_rectangles{};
    std::size_t row_bytes{};
    std::size_t image_bytes{};
};

// Single renderer-thread owner of residency and the shared upload allowance.
// Device, font, pixels and locale are borrowed only during each call. No device,
// encoder, host, scene or asynchronous completion state is retained.
class MetalTextureResources final {
public:
    explicit MetalTextureResources(MetalTextureLimits limits);

    MetalTextureResources(const MetalTextureResources&) = delete;
    MetalTextureResources& operator=(const MetalTextureResources&) = delete;

    // Begin at device creation and after drawable acquisition, not on busy,
    // unchanged or occluded returns. Clearing residency does not begin a batch.
    void begin_upload_batch() noexcept;
    std::size_t remaining_upload_bytes() const noexcept;

    // The caller clears residency at its existing device-replacement boundary.
    // ARC callers retain returned textures while encoding. Ordinary retained-
    // resource command buffers own encoded textures through GPU completion even
    // after this cache is cleared/destroyed. No callback may borrow this owner.
    id<MTLTexture> row_texture(id<MTLDevice> device,
                               RowTextureIdentity identity,
                               const FrameRow& row,
                               CTFontRef font,
                               RowRasterOptions raster_options,
                               std::uint64_t serial);
    id<MTLTexture> image_texture(id<MTLDevice> device,
                                 std::uint64_t pane,
                                 const ImagePixels& image,
                                 std::uint64_t serial);

    // Tightly packed premultiplied BGRA. White/chrome uploads share the row/image
    // allowance. Reservation precedes dimension/allocation checks; no rollback.
    id<MTLTexture> upload_bgra(id<MTLDevice> device,
                               std::uint32_t width,
                               std::uint32_t height,
                               std::span<const std::uint8_t> pixels);

    void clear_rows() noexcept;
    void clear_device_textures() noexcept;
    MetalTextureStats stats() const noexcept;

private:
    struct RowKey {
        std::uint64_t pane{};
        std::uint64_t id0{};
        std::uint64_t id1{};
        std::uint64_t generation{};
        std::uint64_t font{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t variant{};
        bool blink{};

        auto operator<=>(const RowKey&) const = default;
    };

    struct ImageKey {
        std::uint64_t pane{};
        std::uint64_t generation{};
        std::uint32_t id{};
        std::uint32_t width{};
        std::uint32_t height{};

        // PixelFormat deliberately is not part of existing macOS identity.
        auto operator<=>(const ImageKey&) const = default;
    };

    struct CachedTexture {
        id<MTLTexture> texture;
        std::size_t bytes{};
        std::uint64_t used{};
    };

    void trim_rows(std::size_t incoming);
    void trim_images(std::size_t incoming);

    MetalTextureLimits limits_;
    MetalTextureStats statistics_;
    std::map<RowKey, CachedTexture> rows_;
    std::map<ImageKey, CachedTexture> images_;
    std::size_t uploaded_bytes_{};
};

} // namespace stormtty::macos
