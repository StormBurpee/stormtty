#include "image_cache.hpp"

#include "com_owner.hpp"
#include "graphics_error.hpp"
#include "stormtty/renderer.hpp"

#include <d2d1_1.h>

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>

namespace stormtty::windows {
namespace {

struct ImageKey {
    std::uint64_t pane{};
    std::uint64_t generation{};
    std::uint32_t id{};
    std::uint32_t width{};
    std::uint32_t height{};
    PixelFormat format{};

    bool operator==(const ImageKey&) const = default;
};

struct ImageHash {
    std::size_t operator()(const ImageKey& key) const noexcept {
        std::size_t hash = 0;
        const std::array<std::uint64_t, 8> values{
            key.pane, key.generation, key.id, key.width, key.height,
            static_cast<std::uint32_t>(key.format), 0, 0};
        for (const auto value : values) {
            hash ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b9u +
                (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

struct CachedImage {
    Com<ID2D1Bitmap1> bitmap;
    std::size_t bytes{};
    std::uint64_t touched{};
};

} // namespace

struct ImageCache::Impl {
    ImageCacheOptions options;
    ImageCacheStats counters;
    std::unordered_map<ImageKey, CachedImage, ImageHash> images;

    explicit Impl(ImageCacheOptions requested) : options(requested) {
        if (!options.maximum_dimension || options.maximum_dimension > 8192 ||
            options.byte_budget > 512u * 1024u * 1024u) {
            throw std::invalid_argument("Renderer cache or image dimensions exceed safety limits");
        }
    }
};

ImageCache::ImageCache(ImageCacheOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

ImageCache::~ImageCache() = default;

ID2D1Bitmap1* ImageCache::bitmap(
    ID2D1DeviceContext& context,
    std::uint64_t pane_id,
    const ImagePixels& pixels,
    std::uint64_t render_tick) {
    auto& cache = *impl_;
    const ImageKey key{
        pane_id, pixels.generation, pixels.image_id,
        pixels.width, pixels.height, pixels.format};
    if (auto found = cache.images.find(key); found != cache.images.end()) {
        found->second.touched = render_tick;
        return found->second.bitmap.get();
    }

    const std::size_t channels = pixels.format == PixelFormat::rgba ? 4 :
        pixels.format == PixelFormat::rgb ? 3 :
        pixels.format == PixelFormat::gray_alpha ? 2 : 1;
    const auto pixel_count = std::uint64_t(pixels.width) * pixels.height;
    const auto decoded_bytes = pixel_count * 4;
    if (!pixels.width || !pixels.height ||
        pixels.width > cache.options.maximum_dimension ||
        pixels.height > cache.options.maximum_dimension ||
        decoded_bytes > cache.options.byte_budget ||
        decoded_bytes > std::numeric_limits<UINT>::max() ||
        pixel_count * channels != pixels.bytes.size()) {
        return nullptr;
    }

    // Eviction precedes conversion and upload, including when either throws.
    // Recency is the caller's frame tick; ties retain unordered-map behavior.
    while (!cache.images.empty() &&
           cache.counters.retained_bytes + decoded_bytes > cache.options.byte_budget) {
        const auto oldest = std::min_element(
            cache.images.begin(), cache.images.end(),
            [](const auto& left, const auto& right) {
                return left.second.touched < right.second.touched;
            });
        cache.counters.retained_bytes -= oldest->second.bytes;
        cache.images.erase(oldest);
    }

    std::vector<std::uint8_t> premultiplied(static_cast<std::size_t>(decoded_bytes));
    for (std::size_t index = 0; index < static_cast<std::size_t>(pixel_count); ++index) {
        const auto* source = pixels.bytes.data() + index * channels;
        const auto red = source[0];
        const auto green = channels >= 3 ? source[1] : red;
        const auto blue = channels >= 3 ? source[2] : red;
        const std::uint8_t alpha = channels == 4 ? source[3] : channels == 2 ? source[1] : 255;
        premultiplied[index * 4] = static_cast<std::uint8_t>((std::uint32_t(blue) * alpha + 127) / 255);
        premultiplied[index * 4 + 1] = static_cast<std::uint8_t>((std::uint32_t(green) * alpha + 127) / 255);
        premultiplied[index * 4 + 2] = static_cast<std::uint8_t>((std::uint32_t(red) * alpha + 127) / 255);
        premultiplied[index * 4 + 3] = alpha;
    }

    CachedImage cached;
    const D2D1_BITMAP_PROPERTIES1 properties{
        {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
        96, 96, D2D1_BITMAP_OPTIONS_NONE, nullptr};
    const auto pitch = pixels.width * 4;
    check(context.CreateBitmap(
        {pixels.width, pixels.height}, premultiplied.data(), pitch,
        &properties, cached.bitmap.put()), "Upload image");
    cached.bytes = static_cast<std::size_t>(decoded_bytes);
    cached.touched = render_tick;

    auto [entry, inserted] = cache.images.emplace(key, std::move(cached));
    if (inserted) {
        cache.counters.retained_bytes += static_cast<std::size_t>(decoded_bytes);
        ++cache.counters.uploads;
    }
    return entry->second.bitmap.get();
}

void ImageCache::clear() noexcept {
    impl_->images.clear();
    impl_->counters.retained_bytes = 0;
}

ImageCacheStats ImageCache::stats() const noexcept {
    return impl_->counters;
}

} // namespace stormtty::windows
