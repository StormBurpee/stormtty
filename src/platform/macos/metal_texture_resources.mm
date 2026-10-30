#include "metal_texture_resources.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace stormtty::macos {
namespace {

constexpr std::size_t maximum_upload_bytes = 64u * 1024u * 1024u;
constexpr std::size_t maximum_row_bytes = 64u * 1024u * 1024u;
constexpr std::size_t maximum_image_bytes = 128u * 1024u * 1024u;
constexpr std::size_t maximum_rows = 16384;
constexpr std::size_t maximum_images = 1024;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

MetalTextureResources::MetalTextureResources(MetalTextureLimits limits)
    : limits_{
          std::min(limits.row_bytes, maximum_row_bytes),
          std::min(limits.image_bytes, maximum_image_bytes),
          std::min(limits.maximum_image_dimension, 8192u)
      } {}

void MetalTextureResources::begin_upload_batch() noexcept {
    uploaded_bytes_ = 0;
}

std::size_t MetalTextureResources::remaining_upload_bytes() const noexcept {
    return maximum_upload_bytes - uploaded_bytes_;
}

id<MTLTexture> MetalTextureResources::upload_bgra(
    id<MTLDevice> device,
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint8_t> pixels) {
    const auto allocation = std::uint64_t(width) * height * 4;
    if (allocation > remaining_upload_bytes()) {
        throw std::runtime_error("Metal per-frame upload budget exceeded");
    }
    uploaded_bytes_ += static_cast<std::size_t>(allocation);

    check(width && height && width <= 16384 && height <= 16384,
          "Metal texture dimensions exceed supported limits");
    check(pixels.size() == allocation, "Metal texture pixels do not match dimensions");

    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                    width:width
                                   height:height
                                mipmapped:NO];
    descriptor.storageMode = [device supportsFamily:MTLGPUFamilyApple1]
        ? MTLStorageModeShared : MTLStorageModeManaged;
    descriptor.usage = MTLTextureUsageShaderRead;

    id<MTLTexture> result = [device newTextureWithDescriptor:descriptor];
    check(result != nil, "Cannot allocate Metal texture");
    [result replaceRegion:MTLRegionMake2D(0, 0, width, height)
              mipmapLevel:0
                withBytes:pixels.data()
              bytesPerRow:std::size_t(width) * 4];
    return result;
}

void MetalTextureResources::trim_rows(std::size_t incoming) {
    while (!rows_.empty() &&
           (statistics_.row_bytes + incoming > limits_.row_bytes ||
            rows_.size() >= maximum_rows)) {
        const auto oldest = std::min_element(
            rows_.begin(), rows_.end(), [](const auto& left, const auto& right) {
                return left.second.used < right.second.used;
            });
        statistics_.row_bytes -= oldest->second.bytes;
        rows_.erase(oldest);
    }
}

id<MTLTexture> MetalTextureResources::row_texture(
    id<MTLDevice> device,
    RowTextureIdentity identity,
    const FrameRow& row,
    CTFontRef font,
    RowRasterOptions raster_options,
    std::uint64_t serial) {
    const bool has_blink = std::any_of(
        row.cells.begin(), row.cells.end(), [](const Cell& cell) {
            return cell.style.blink;
        });
    const RowKey key{
        identity.pane, row.id.words[0], row.id.words[1], row.generation,
        identity.font_generation, raster_options.width, raster_options.height,
        identity.variant, !has_blink || raster_options.text_blink_on
    };
    if (auto found = rows_.find(key); found != rows_.end()) {
        found->second.used = serial;
        ++statistics_.row_cache_hits;
        return found->second.texture;
    }

    const auto bytes = std::size_t(raster_options.width) * raster_options.height * 4;
    if (bytes > remaining_upload_bytes()) {
        throw std::runtime_error("Visible CoreText rows exceed transient texture budget");
    }

    // CPU rasterization sees an allowance snapshot, not a reservation.
    raster_options.maximum_output_bytes = remaining_upload_bytes();
    const auto pixels = CoreTextRowRasterizer{}.rasterize(row.cells, font, raster_options);
    id<MTLTexture> result = upload_bgra(
        device, raster_options.width, raster_options.height, pixels.bgra);
    ++statistics_.shaped_rows;
    ++statistics_.dirty_rectangles;

    // Row eviction follows successful upload, including oversized uncached rows.
    trim_rows(bytes);
    if (bytes <= limits_.row_bytes) {
        rows_.emplace(key, CachedTexture{result, bytes, serial});
        statistics_.row_bytes += bytes;
    }
    return result;
}

void MetalTextureResources::trim_images(std::size_t incoming) {
    while (!images_.empty() &&
           (statistics_.image_bytes + incoming > limits_.image_bytes ||
            images_.size() >= maximum_images)) {
        const auto oldest = std::min_element(
            images_.begin(), images_.end(), [](const auto& left, const auto& right) {
                return left.second.used < right.second.used;
            });
        statistics_.image_bytes -= oldest->second.bytes;
        images_.erase(oldest);
    }
}

id<MTLTexture> MetalTextureResources::image_texture(
    id<MTLDevice> device,
    std::uint64_t pane,
    const ImagePixels& image,
    std::uint64_t serial) {
    const auto bytes = std::size_t(image.width) * image.height * 4;
    if (!image.width || !image.height ||
        image.width > limits_.maximum_image_dimension ||
        image.height > limits_.maximum_image_dimension ||
        bytes > limits_.image_bytes || bytes > maximum_upload_bytes) {
        return nil;
    }

    const ImageKey key{pane, image.generation, image.image_id, image.width, image.height};
    if (auto found = images_.find(key); found != images_.end()) {
        found->second.used = serial;
        return found->second.texture;
    }

    const unsigned channels = image.format == PixelFormat::rgba ? 4
        : image.format == PixelFormat::rgb ? 3
        : image.format == PixelFormat::gray_alpha ? 2 : 1;
    if (std::uint64_t(image.width) * image.height * channels != image.bytes.size()) {
        return nil;
    }
    if (bytes > remaining_upload_bytes()) {
        throw std::runtime_error("Image upload exceeds the remaining frame budget");
    }

    std::vector<std::uint8_t> bgra(bytes);
    for (std::size_t pixel = 0; pixel < std::size_t(image.width) * image.height; ++pixel) {
        const auto* source = image.bytes.data() + pixel * channels;
        const unsigned alpha = channels == 4 ? source[3] : channels == 2 ? source[1] : 255;
        const unsigned red = source[0];
        const unsigned green = channels >= 3 ? source[1] : red;
        const unsigned blue = channels >= 3 ? source[2] : red;
        bgra[pixel * 4] = static_cast<std::uint8_t>((blue * alpha + 127) / 255);
        bgra[pixel * 4 + 1] = static_cast<std::uint8_t>((green * alpha + 127) / 255);
        bgra[pixel * 4 + 2] = static_cast<std::uint8_t>((red * alpha + 127) / 255);
        bgra[pixel * 4 + 3] = static_cast<std::uint8_t>(alpha);
    }

    // Images evict before upload. Failure restores neither entries nor allowance.
    trim_images(bytes);
    id<MTLTexture> result = upload_bgra(device, image.width, image.height, bgra);
    images_.emplace(key, CachedTexture{result, bytes, serial});
    statistics_.image_bytes += bytes;
    ++statistics_.image_uploads;
    return result;
}

void MetalTextureResources::clear_rows() noexcept {
    rows_.clear();
    statistics_.row_bytes = 0;
}

void MetalTextureResources::clear_device_textures() noexcept {
    clear_rows();
    images_.clear();
    statistics_.image_bytes = 0;
}

MetalTextureStats MetalTextureResources::stats() const noexcept {
    return statistics_;
}

} // namespace stormtty::macos
