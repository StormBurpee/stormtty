// Native Windows WARP tests. Cross-compilation is not runtime evidence.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../src/platform/windows/image_cache.hpp"
#include "../src/platform/windows/com_owner.hpp"
#include "../src/platform/windows/graphics_error.hpp"
#include "stormtty/frame.hpp"

#include <d3d11.h>
#include <d2d1_1.h>

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace stormtty;
using namespace stormtty::windows;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Apartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};

    Apartment() {
        check(result, "Initialize test apartment");
    }

    ~Apartment() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

struct WarpFixture {
    Apartment apartment;
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> immediate;
    Com<ID2D1Factory1> factory;
    Com<ID2D1Device> drawing_device;
    Com<ID2D1DeviceContext> context;

    WarpFixture() {
        D3D_FEATURE_LEVEL level{};
        check(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), &level, immediate.put()),
            "Create image-test WARP device");
        const D2D1_FACTORY_OPTIONS options{};
        check(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
            reinterpret_cast<void**>(factory.put())), "Create image-test D2D factory");
        Com<IDXGIDevice> dxgi;
        check(device.as(dxgi), "Query image-test DXGI device");
        check(factory->CreateDevice(dxgi.get(), drawing_device.put()), "Create image-test D2D device");
        check(drawing_device->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context.put()), "Create image-test D2D context");
    }

    // Copy only this test's synthetic bitmap, without a window or desktop source.
    // Raw premultiplied BGRA avoids unpremultiplication hiding rounding errors.
    std::array<std::uint8_t, 4> first_pixel(ID2D1Bitmap1* bitmap) {
        require(bitmap != nullptr, "Expected image was rejected");
        Com<ID2D1Bitmap1> staging;
        const D2D1_BITMAP_PROPERTIES1 properties{
            {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
            96, 96, D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, nullptr};
        check(context->CreateBitmap(
            {1, 1}, nullptr, 0, &properties, staging.put()), "Create image-test staging bitmap");
        const D2D1_RECT_U source{0, 0, 1, 1};
        check(staging->CopyFromBitmap(nullptr, bitmap, &source), "Copy synthetic image pixel");
        D2D1_MAPPED_RECT mapped{};
        check(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped), "Map synthetic image pixel");
        struct Unmap {
            ID2D1Bitmap1* bitmap;
            ~Unmap() {
                bitmap->Unmap();
            }
        } unmap{staging.get()};
        return {mapped.bits[0], mapped.bits[1], mapped.bits[2], mapped.bits[3]};
    }
};

ImagePixels sample(std::uint32_t id = 1) {
    ImagePixels pixels;
    pixels.image_id = id;
    pixels.generation = 1;
    pixels.width = 1;
    pixels.height = 1;
    pixels.format = PixelFormat::rgba;
    pixels.bytes = {1, 127, 255, 128};
    return pixels;
}

void identity_test() {
    WarpFixture fixture;
    ImageCache cache({1024, 8192});
    const auto original = sample();
    require(cache.bitmap(*fixture.context.get(), 7, original, 1), "Initial upload failed");
    require(cache.stats().uploads == 1 && cache.stats().retained_bytes == 4, "Initial accounting changed");

    for (unsigned dimension = 0; dimension < 6; ++dimension) {
        auto changed = original;
        std::uint64_t pane_id = 7;
        switch (dimension) {
        case 0:
            ++pane_id;
            break;
        case 1:
            ++changed.generation;
            break;
        case 2:
            ++changed.image_id;
            break;
        case 3:
            ++changed.width;
            changed.bytes.resize(8);
            break;
        case 4:
            ++changed.height;
            changed.bytes.resize(8);
            break;
        case 5:
            changed.format = PixelFormat::rgb;
            changed.bytes.resize(3);
            break;
        }
        const auto before = cache.stats();
        require(cache.bitmap(*fixture.context.get(), pane_id, changed, 2 + dimension), "Changed key rejected");
        require(cache.stats().uploads == before.uploads + 1, "Key field did not produce independent upload");
        cache.bitmap(*fixture.context.get(), 7, original, 10 + dimension);
        require(cache.stats().uploads == before.uploads + 1, "Original key lost after independent upload");
    }

    auto malformed = original;
    malformed.bytes.clear();
    const auto before = cache.stats();
    auto* hit = cache.bitmap(*fixture.context.get(), 7, malformed, 20);
    require(fixture.first_pixel(hit) == std::array<std::uint8_t, 4>{128, 64, 1, 128},
            "Hit failed immutable-identity/lookup-before-validation contract");
    ++malformed.generation;
    malformed.image_id = 100;
    require(!cache.bitmap(*fixture.context.get(), 7, malformed, 21), "Malformed miss was accepted");
    require(cache.stats().uploads == before.uploads && cache.stats().retained_bytes == before.retained_bytes,
            "Hit or rejected miss changed accounting");
}

void conversion_test() {
    WarpFixture fixture;
    ImageCache cache({1024, 8192});
    struct Case {
        PixelFormat format;
        std::vector<std::uint8_t> source;
        std::array<std::uint8_t, 4> expected;
    };
    const std::array<Case, 6> cases{{
        {PixelFormat::rgba, {1, 127, 255, 128}, {128, 64, 1, 128}},
        {PixelFormat::rgba, {40, 60, 80, 0}, {0, 0, 0, 0}},
        {PixelFormat::rgba, {1, 127, 255, 255}, {255, 127, 1, 255}},
        {PixelFormat::rgb, {1, 127, 255}, {255, 127, 1, 255}},
        {PixelFormat::gray_alpha, {127, 128}, {64, 64, 64, 128}},
        {PixelFormat::gray, {127}, {127, 127, 127, 255}}
    }};
    std::uint32_t id = 0;
    for (const auto& test : cases) {
        auto pixels = sample(++id);
        pixels.format = test.format;
        pixels.bytes = test.source;
        auto* bitmap = cache.bitmap(*fixture.context.get(), 1, pixels, id);
        require(fixture.first_pixel(bitmap) == test.expected, "Premultiplied pixel conversion changed");
        const auto format = bitmap->GetPixelFormat();
        require(format.format == DXGI_FORMAT_B8G8R8A8_UNORM &&
                format.alphaMode == D2D1_ALPHA_MODE_PREMULTIPLIED,
                "Image bitmap native format changed");
        float dpi_x{};
        float dpi_y{};
        bitmap->GetDpi(&dpi_x, &dpi_y);
        require(dpi_x == 96 && dpi_y == 96, "Image bitmap coordinates became DPI-dependent");
    }
}

void rejection_and_budget_test() {
    WarpFixture fixture;
    ImageCache cache({4, 2});
    const auto valid = sample();
    cache.bitmap(*fixture.context.get(), 1, valid, 1);
    for (unsigned reason = 0; reason < 7; ++reason) {
        auto pixels = sample(2);
        switch (reason) {
        case 0:
            pixels.width = 0;
            break;
        case 1:
            pixels.height = 0;
            break;
        case 2:
            pixels.width = 3;
            break;
        case 3:
            pixels.height = 3;
            break;
        case 4:
            pixels.bytes.pop_back();
            break;
        case 5:
            pixels.bytes.push_back(0);
            break;
        case 6:
            pixels.width = 2;
            pixels.bytes.resize(8);
            break;
        }
        require(!cache.bitmap(*fixture.context.get(), 1, pixels, 2), "Invalid image accepted");
        require(cache.stats().uploads == 1 && cache.stats().retained_bytes == 4,
                "Rejected image changed residency or uploads");
    }
    for (const auto format : {
             PixelFormat::rgb, PixelFormat::rgba,
             PixelFormat::gray_alpha, PixelFormat::gray}) {
        const std::size_t channels = format == PixelFormat::rgba ? 4 :
            format == PixelFormat::rgb ? 3 : format == PixelFormat::gray_alpha ? 2 : 1;
        for (const auto length : {channels - 1, channels + 1}) {
            auto malformed = sample(3);
            malformed.format = format;
            malformed.bytes.resize(length);
            require(!cache.bitmap(*fixture.context.get(), 1, malformed, 3),
                    "Malformed channel count was accepted");
        }
    }
    require(cache.stats().uploads == 1 && cache.stats().retained_bytes == 4,
            "Malformed channel count changed accounting");
    cache.bitmap(*fixture.context.get(), 1, valid, 3);
    require(cache.stats().uploads == 1, "Rejected image evicted valid entry");
    ImageCache disabled({0, 8192});
    require(!disabled.bitmap(*fixture.context.get(), 1, valid, 1), "Zero budget retained image");
    require(disabled.stats().retained_bytes == 0 && disabled.stats().uploads == 0, "Zero budget accounting changed");

    ImageCache gray_cache({4, 8192});
    auto gray = sample();
    gray.format = PixelFormat::gray;
    gray.bytes = {128};
    require(gray_cache.bitmap(*fixture.context.get(), 1, gray, 1), "Exact decoded budget rejected gray");
    require(gray_cache.stats().retained_bytes == 4, "Residency counted encoded rather than decoded bytes");
}

void eviction_test() {
    WarpFixture fixture;
    ImageCache cache({8, 8192});
    const auto first = sample(1);
    const auto second = sample(2);
    const auto third = sample(3);
    cache.bitmap(*fixture.context.get(), 1, first, 10);
    cache.bitmap(*fixture.context.get(), 1, second, 20);
    cache.bitmap(*fixture.context.get(), 1, first, 30);
    cache.bitmap(*fixture.context.get(), 1, third, 40);
    require(cache.stats().uploads == 3 && cache.stats().retained_bytes == 8, "Eviction budget changed");
    cache.bitmap(*fixture.context.get(), 1, first, 50);
    require(cache.stats().uploads == 3, "Cache hit did not refresh recency");
    cache.bitmap(*fixture.context.get(), 1, second, 60);
    require(cache.stats().uploads == 4 && cache.stats().retained_bytes == 8, "Oldest entry was not evicted");

    cache.clear();
    for (const auto& pixels : {first, second, third}) {
        auto* bitmap = cache.bitmap(*fixture.context.get(), 1, pixels, 70);
        require(fixture.first_pixel(bitmap)[3] == 128, "Same-tick pressure lost uploaded image");
        require(cache.stats().retained_bytes <= 8, "Same-tick pressure exceeded budget");
    }
    // Deliberately do not assert which same-tick entry survives.
}

void clear_and_device_test() {
    ImageCache cache({4, 8192});
    const auto pixels = sample();
    for (std::uint64_t generation = 1; generation <= 2; ++generation) {
        WarpFixture fixture;
        struct ClearBeforeDevice {
            ImageCache& cache;
            ~ClearBeforeDevice() {
                cache.clear();
            }
        } clear_before_device{cache};
        auto* bitmap = cache.bitmap(*fixture.context.get(), 1, pixels, generation);
        require(fixture.first_pixel(bitmap)[3] == 128, "Fresh-device image not uploaded");
        require(cache.stats().uploads == generation && cache.stats().retained_bytes == 4,
                "Device generation reused stale bitmap or reset upload count");
        cache.clear();
        cache.clear();
        require(cache.stats().uploads == generation && cache.stats().retained_bytes == 0,
                "Clear reset cumulative uploads or retained bytes");
    }
}

void constructor_test() {
    for (const auto options : {
             ImageCacheOptions{4, 0},
             ImageCacheOptions{4, 8193},
             ImageCacheOptions{512u * 1024u * 1024u + 1, 8192}}) {
        bool rejected = false;
        try {
            ImageCache cache(options);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "Unsafe cache options accepted");
    }
    ImageCache disabled({0, 8192});
    require(disabled.stats().uploads == 0, "Disabled cache construction changed counters");
}

} // namespace

int main() {
    try {
        constructor_test();
        identity_test();
        conversion_test();
        rejection_and_budget_test();
        eviction_test();
        clear_and_device_test();
        std::cout << "PASS native WARP image owner: identity, pixels, budget, eviction, device lifetime\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL image cache: " << error.what() << '\n';
        return 1;
    }
}
