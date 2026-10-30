#import <Foundation/Foundation.h>
#include "../src/platform/macos/metal_texture_resources.hpp"
#include "../src/platform/macos/core_text_font_resources.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {

using namespace stormtty;
using namespace stormtty::macos;

constexpr std::size_t upload_budget = 64u * 1024u * 1024u;

static_assert(!std::is_copy_constructible_v<MetalTextureResources>);

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class Operation>
void require_failure(Operation operation, std::string_view expected) {
    try {
        operation();
    } catch (const std::runtime_error& error) {
        require(std::string_view(error.what()) == expected, "Unexpected failure diagnostic");
        return;
    }
    throw std::runtime_error("Expected operation to fail");
}

ImagePixels image(std::uint32_t identifier = 1) {
    ImagePixels result;
    result.image_id = identifier;
    result.generation = 1;
    result.width = 1;
    result.height = 1;
    result.format = PixelFormat::rgba;
    result.bytes = {101, 51, 203, 128};
    return result;
}

std::vector<std::uint8_t> pixels(id<MTLTexture> texture) {
    require(texture != nil, "Missing texture");
    std::vector<std::uint8_t> result(texture.width * texture.height * 4);
    [texture getBytes:result.data()
         bytesPerRow:texture.width * 4
          fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
         mipmapLevel:0];
    return result;
}

struct RowFixture {
    CoreTextFontResources fonts;
    FrameRow row;
    RowRasterOptions options;

    RowFixture() {
        FontOptions font;
        font.family = "Menlo";
        fonts.reset(font, 96);
        row.id.words = {11, 12};
        row.generation = 1;
        row.cells.resize(1);
        row.cells[0].grapheme = "A";
        row.cells[0].foreground = {240, 160, 80, 255};
        const auto metrics = fonts.metrics();
        options = {16, 24, 16, metrics, 96, true, false, "en", upload_budget};
    }

    id<MTLTexture> get(MetalTextureResources& textures, id<MTLDevice> device,
                       RowTextureIdentity identity = {7, 1, 0},
                       std::uint64_t serial = 1) {
        return textures.row_texture(device, identity, row, fonts.borrow_font(), options, serial);
    }
};

void image_keys_and_pixels_test(id<MTLDevice> device) {
    MetalTextureResources textures({4096, 4096, 8192});
    auto source = image();
    id<MTLTexture> first = textures.image_texture(device, 7, source, 1);
    require(pixels(first) == std::vector<std::uint8_t>({102, 26, 51, 128}),
            "RGBA premultiplication/rounding changed");

    const auto allowance = textures.remaining_upload_bytes();
    source.format = PixelFormat::gray_alpha;
    source.bytes.clear();
    require(textures.image_texture(device, 7, source, 2) == first,
            "Image identity must hit before format/payload validation");
    require(textures.remaining_upload_bytes() == allowance,
            "Cache hit consumed upload bytes");
    require(textures.image_texture(device, 8, source, 2) == nil,
            "Different pane incorrectly reused malformed image");
    ++source.generation;
    require(textures.image_texture(device, 7, source, 2) == nil,
            "Different generation incorrectly reused image");

    source = image();
    for (const PixelFormat format : {PixelFormat::rgb, PixelFormat::gray_alpha,
                                    PixelFormat::gray, PixelFormat::rgba}) {
        ++source.image_id;
        source.format = format;
        source.bytes = format == PixelFormat::rgb ? std::vector<std::uint8_t>{101, 51, 203}
            : format == PixelFormat::gray_alpha ? std::vector<std::uint8_t>{101, 128}
            : format == PixelFormat::gray ? std::vector<std::uint8_t>{101}
            : std::vector<std::uint8_t>{255, 128, 64, 0};
        const auto expected = format == PixelFormat::rgb
            ? std::vector<std::uint8_t>{203, 51, 101, 255}
            : format == PixelFormat::gray_alpha ? std::vector<std::uint8_t>{51, 51, 51, 128}
            : format == PixelFormat::gray ? std::vector<std::uint8_t>{101, 101, 101, 255}
            : std::vector<std::uint8_t>{0, 0, 0, 0};
        require(pixels(textures.image_texture(device, 7, source, 3)) == expected,
                "Image channel conversion changed");
    }

    source = image();
    source.width = 2;
    source.bytes.insert(source.bytes.end(), {9, 8, 7, 255});
    id<MTLTexture> wider = textures.image_texture(device, 7, source, 4);
    require(wider.width == 2, "Width missing from image identity");
    source.width = 1;
    source.height = 2;
    id<MTLTexture> taller = textures.image_texture(device, 7, source, 4);
    require(taller.height == 2, "Height missing from image identity");
    source = image();
    require(textures.image_texture(device, 8, source, 5) != first,
            "Pane missing from image identity");
    ++source.generation;
    require(textures.image_texture(device, 7, source, 5) != first,
            "Generation missing from image identity");
}

void row_keys_and_pixels_test(id<MTLDevice> device) {
    MetalTextureResources textures({1024 * 1024, 4096, 8192});
    RowFixture fixture;
    id<MTLTexture> first = fixture.get(textures, device);
    const auto reference = CoreTextRowRasterizer{}.rasterize(
        fixture.row.cells, fixture.fonts.borrow_font(), fixture.options);
    require(pixels(first) == reference.bgra, "Row upload changed CPU raster bytes");

    const auto allowance = textures.remaining_upload_bytes();
    fixture.options.text_blink_on = false;
    require(fixture.get(textures, device) == first, "Nonblinking row needs stable blink key");
    require(textures.remaining_upload_bytes() == allowance, "Row hit consumed upload bytes");

    // Identity hits precede CPU font validation as in the renderer's original map.
    require(textures.row_texture(device, {7, 1, 0}, fixture.row, nullptr,
                                 fixture.options, 2) == first,
            "Row hit unexpectedly invoked CPU rasterizer");

    const auto expect_miss = [&](RowTextureIdentity identity) {
        id<MTLTexture> next = fixture.get(textures, device, identity);
        require(next != first, "Row identity component was omitted");
    };
    expect_miss({8, 1, 0});
    expect_miss({7, 2, 0});
    expect_miss({7, 1, 1});
    ++fixture.row.id.words[0];
    expect_miss({7, 1, 0});
    --fixture.row.id.words[0];
    ++fixture.row.id.words[1];
    expect_miss({7, 1, 0});
    --fixture.row.id.words[1];
    ++fixture.row.generation;
    expect_miss({7, 1, 0});
    --fixture.row.generation;
    ++fixture.options.width;
    expect_miss({7, 1, 0});
    --fixture.options.width;
    ++fixture.options.height;
    expect_miss({7, 1, 0});
    --fixture.options.height;

    fixture.row.cells[0].style.blink = true;
    id<MTLTexture> hidden = fixture.get(textures, device);
    require(hidden != first, "Blinking row reused visible texture while blink is off");
    fixture.options.text_blink_on = true;
    require(fixture.get(textures, device) == first,
            "Effective visible blink identity changed");
}

void eviction_and_failure_order_test(id<MTLDevice> device) {
    MetalTextureResources images({4096, 8, 8192});
    auto one = image(1);
    auto two = image(2);
    auto three = image(3);
    id<MTLTexture> first = images.image_texture(device, 7, one, 1);
    id<MTLTexture> second = images.image_texture(device, 7, two, 1);
    images.image_texture(device, 7, three, 1);
    require(images.image_texture(device, 7, two, 2) == second,
            "Equal-tick image eviction did not use ordered key traversal");
    require(images.image_texture(device, 7, one, 3) != first,
            "Oldest ordered image was not evicted");

    MetalTextureResources recency_images({4096, 8, 8192});
    first = recency_images.image_texture(device, 7, one, 1);
    second = recency_images.image_texture(device, 7, two, 2);
    recency_images.image_texture(device, 7, one, 3);
    recency_images.image_texture(device, 7, three, 4);
    require(recency_images.image_texture(device, 7, one, 5) == first,
            "Image hit did not refresh frame-tick recency");
    require(recency_images.image_texture(device, 7, two, 6) != second,
            "Image eviction ignored refreshed recency");

    MetalTextureResources failure_images({4096, 4, 8192});
    first = failure_images.image_texture(device, 7, one, 1);
    const auto before_image_failure = failure_images.remaining_upload_bytes();
    require_failure([&] {
        failure_images.image_texture(nil, 7, two, 2);
    }, "Cannot allocate Metal texture");
    require(failure_images.stats().image_bytes == 0,
            "Image upload failure restored pre-upload eviction");
    require(failure_images.stats().image_uploads == 1,
            "Failed image upload incremented success counter");
    require(failure_images.remaining_upload_bytes() == before_image_failure - 4,
            "Image allocation failure rolled back reservation");

    RowFixture fixture;
    const auto row_bytes = std::size_t(fixture.options.width) * fixture.options.height * 4;
    MetalTextureResources rows({row_bytes * 2, 4096, 8192});
    first = fixture.get(rows, device, {1, 1, 0}, 1);
    second = fixture.get(rows, device, {2, 1, 0}, 1);
    fixture.get(rows, device, {3, 1, 0}, 1);
    require(fixture.get(rows, device, {2, 1, 0}, 2) == second,
            "Equal-tick row eviction did not use ordered keys");
    require(fixture.get(rows, device, {1, 1, 0}, 3) != first,
            "Oldest ordered row was not evicted");

    MetalTextureResources recency_rows({row_bytes * 2, 4096, 8192});
    first = fixture.get(recency_rows, device, {1, 1, 0}, 1);
    second = fixture.get(recency_rows, device, {2, 1, 0}, 2);
    fixture.get(recency_rows, device, {1, 1, 0}, 3);
    fixture.get(recency_rows, device, {3, 1, 0}, 4);
    require(fixture.get(recency_rows, device, {1, 1, 0}, 5) == first,
            "Row hit did not refresh frame-tick recency");
    require(fixture.get(recency_rows, device, {2, 1, 0}, 6) != second,
            "Row eviction ignored refreshed recency");

    MetalTextureResources failure_rows({row_bytes, 4096, 8192});
    first = fixture.get(failure_rows, device);
    require_failure([&] {
        fixture.get(failure_rows, nil, {8, 1, 0});
    }, "Cannot allocate Metal texture");
    require(failure_rows.stats().row_bytes == row_bytes &&
                fixture.get(failure_rows, device) == first,
            "Failed row upload evicted before success");
    ++fixture.options.width;
    fixture.get(failure_rows, device, {8, 1, 0});
    require(failure_rows.stats().row_bytes == 0,
            "Oversized uncached row no longer trims old rows");
}

// Reserve through the real native allocation-failure path, using only 64 KiB
// of authored pixels. No giant textures, fake budgets or production test hooks.
void exhaust_upload_allowance(MetalTextureResources& textures) {
    const std::vector<std::uint8_t> authored(16384 * 4, 0);
    while (textures.remaining_upload_bytes()) {
        const auto bytes = std::min(textures.remaining_upload_bytes(), authored.size());
        const auto width = static_cast<std::uint32_t>(bytes / 4);
        require_failure([&] {
            textures.upload_bgra(nil, width, 1,
                                 std::span<const std::uint8_t>(authored.data(), bytes));
        }, "Cannot allocate Metal texture");
    }
}

void aggregate_budget_and_clear_test(id<MTLDevice> device) {
    MetalTextureResources textures({4096, 4096, 8192});
    RowFixture fixture;
    const auto source = image();
    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    textures.upload_bgra(device, 1, 1, white);
    id<MTLTexture> row = fixture.get(textures, device);
    id<MTLTexture> cached_image = textures.image_texture(device, 7, source, 1);
    const auto row_bytes = std::size_t(fixture.options.width) * fixture.options.height * 4;
    require(textures.remaining_upload_bytes() == upload_budget - row_bytes - 8,
            "White, row and image uploads do not share one allowance");

    exhaust_upload_allowance(textures);
    require(fixture.get(textures, device) == row, "Exhausted allowance rejected row hit");
    require(textures.image_texture(device, 7, source, 2) == cached_image,
            "Exhausted allowance rejected image hit");
    require_failure([&] {
        fixture.get(textures, device, {8, 1, 0});
    }, "Visible CoreText rows exceed transient texture budget");
    require_failure([&] {
        textures.image_texture(device, 8, source, 2);
    }, "Image upload exceeds the remaining frame budget");
    require_failure([&] {
        textures.upload_bgra(device, 1, 1, white);
    }, "Metal per-frame upload budget exceeded");

    const auto counters = textures.stats();
    textures.clear_rows();
    require(textures.stats().row_bytes == 0 && textures.stats().image_bytes == 4,
            "Font invalidation cleared images");
    textures.clear_device_textures();
    require(textures.stats().image_bytes == 0 && textures.remaining_upload_bytes() == 0,
            "Residency clear reset upload batch");
    require(textures.stats().shaped_rows == counters.shaped_rows &&
                textures.stats().image_uploads == counters.image_uploads &&
                textures.stats().row_cache_hits == counters.row_cache_hits &&
                textures.stats().dirty_rectangles == counters.dirty_rectangles,
            "Residency clear reset cumulative counters");
    textures.begin_upload_batch();
    require(textures.remaining_upload_bytes() == upload_budget,
            "New upload batch did not restore allowance");
    require(fixture.get(textures, device) != row, "Device clear retained old row");
    require(textures.image_texture(device, 7, source, 3) != cached_image,
            "Device clear retained old image");

    const auto before = textures.remaining_upload_bytes();
    require_failure([&] {
        textures.upload_bgra(device, 16385, 1, {});
    }, "Metal texture dimensions exceed supported limits");
    require(textures.remaining_upload_bytes() == before - 16385 * 4,
            "Dimension failure did not reserve before validation");
}

void limits_and_entry_counts_test(id<MTLDevice> device) {
    auto source = image();
    MetalTextureResources disabled({0, 0, 0});
    require(disabled.image_texture(device, 7, source, 1) == nil,
            "Zero image budget unexpectedly accepted pixels");
    RowFixture fixture;
    fixture.get(disabled, device);
    require(disabled.stats().row_bytes == 0 && disabled.stats().shaped_rows == 1,
            "Zero row residency budget must still allow uncached upload");
    MetalTextureResources bounded_images({4096, 4, 8192});
    source.width = 2;
    require(bounded_images.image_texture(device, 7, source, 1) == nil,
            "Image larger than residency budget was accepted");
    require(bounded_images.remaining_upload_bytes() == upload_budget,
            "Rejected oversized image consumed upload allowance");
    source = image();
    MetalTextureResources zero_dimension({4096, 4096, 0});
    require(zero_dimension.image_texture(device, 7, source, 1) == nil,
            "Zero image dimension changed semantics");
    MetalTextureResources clamped({SIZE_MAX, SIZE_MAX, UINT32_MAX});
    source.width = 8193;
    require(clamped.image_texture(device, 7, source, 1) == nil,
            "Maximum image dimension was not clamped");

    MetalTextureResources images({4096, 128 * 1024 * 1024, 8192});
    source = image(0);
    id<MTLTexture> first = images.image_texture(device, 7, source, 1);
    for (std::uint32_t identifier = 1; identifier <= 1024; ++identifier) {
        source.image_id = identifier;
        images.image_texture(device, 7, source, 1);
    }
    require(images.stats().image_bytes == 1024 * 4,
            "Image entry cap changed");
    source.image_id = 0;
    require(images.image_texture(device, 7, source, 2) != first,
            "Image entry cap failed to evict oldest ordered entry");

    // Tiny blank rows exercise the entry cap without allocating a large raster.
    MetalTextureResources rows({64 * 1024 * 1024, 4096, 8192});
    fixture.options.width = 1;
    fixture.options.height = 1;
    fixture.row.cells.clear();
    first = fixture.get(rows, device, {0, 1, 0}, 1);
    for (std::uint64_t pane = 1; pane <= 16384; ++pane) {
        @autoreleasepool {
            fixture.get(rows, device, {pane, 1, 0}, 1);
        }
    }
    require(rows.stats().row_bytes == 16384 * 4, "Row entry cap changed");
    require(fixture.get(rows, device, {0, 1, 0}, 2) != first,
            "Row entry cap failed to evict oldest ordered entry");
}

struct SignalOnExit {
    id<MTLSharedEvent> event;

    ~SignalOnExit() {
        // Never strand a submitted wait if a test assertion throws.
        event.signaledValue = 1;
    }
};

void submitted_texture_lifetime_test(id<MTLDevice> device) {
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLSharedEvent> event = [device newSharedEvent];
    require(queue != nil && event != nil, "Cannot create lifetime test queue/event");
    SignalOnExit unblock{event};
    id<MTLBuffer> result = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    require(result != nil, "Cannot allocate authored-pixel result buffer");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    require(command != nil, "Cannot allocate lifetime test command buffer");
    require(command.retainedReferences, "Lifetime test requires ordinary retained resources");
    [command encodeWaitForEvent:event value:1];

    @autoreleasepool {
        MetalTextureResources textures({4096, 4, 8192});
        id<MTLTexture> source = textures.image_texture(device, 7, image(), 1);
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(blit != nil, "Cannot allocate lifetime blit encoder");
        [blit copyFromTexture:source
                 sourceSlice:0
                 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(1, 1, 1)
                    toBuffer:result
           destinationOffset:0
      destinationBytesPerRow:256
    destinationBytesPerImage:256];
        [blit endEncoding];
        [command commit];
        source = nil;
        textures.image_texture(device, 7, image(2), 2);
        textures.clear_device_textures();
    }

    // The submitted buffer was held behind the event until all cache/local
    // texture ownership disappeared. It alone must keep its source alive.
    event.signaledValue = 1;
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted,
            "Submitted texture did not survive cache eviction/destruction");
    const std::array<std::uint8_t, 4> expected{102, 26, 51, 128};
    require(std::memcmp(result.contents, expected.data(), expected.size()) == 0,
            "Retained command buffer lost authored texture pixels");
}

void renderer_invalidation_test() {
    RendererOptions options;
    options.font.family = "Menlo";
    Renderer renderer(nullptr, options);
    renderer.resize(64, 64);
    auto frame = std::make_shared<RenderFrame>();
    frame->generation = 1;
    frame->columns = 1;
    frame->rows = 1;
    frame->cell_width = 16;
    frame->cell_height = 24;
    frame->cursor.visible = false;
    RowFixture fixture;
    frame->lines.push_back(std::make_shared<FrameRow>(fixture.row));
    ImagePlacement placement;
    placement.pixels = std::make_shared<ImagePixels>(image());
    placement.pixel_width = 8;
    placement.pixel_height = 8;
    frame->images.push_back(placement);
    WindowScene scene;
    scene.force_redraw = true;
    scene.panes.push_back({7, {0, 0, 64, 64}, frame});
    const auto submit = [&] {
        require(renderer.render(scene) == RenderResult::presented,
                "Offscreen cache integration did not submit");
        require(!renderer.read_pixels().empty(), "Missing offscreen authored pixels");
    };
    submit();
    const auto initial = renderer.stats();
    require(initial.shaped_rows == 1 && initial.image_uploads == 1,
            "Initial integration upload counts changed");
    renderer.resize(80, 64);
    submit();
    renderer.set_background_opacity(.5f);
    submit();
    require(renderer.stats().shaped_rows == initial.shaped_rows &&
                renderer.stats().image_uploads == initial.image_uploads,
            "Size/opacity mutation discarded cached textures");
    renderer.set_font(options.font);
    submit();
    require(renderer.stats().shaped_rows == initial.shaped_rows + 1 &&
                renderer.stats().image_uploads == initial.image_uploads,
            "Same-font reset did not clear rows only");
    renderer.resize(80, 64, 192);
    submit();
    require(renderer.stats().shaped_rows == initial.shaped_rows + 2 &&
                renderer.stats().image_uploads == initial.image_uploads,
            "DPI reset did not clear rows only");
    renderer.recreate_device();
    submit();
    require(renderer.stats().shaped_rows == initial.shaped_rows + 3 &&
                renderer.stats().image_uploads == initial.image_uploads + 1 &&
                renderer.stats().device_recoveries == 1,
            "Device replacement did not clear both caches with cumulative counters");
}

} // namespace

int main() {
    @autoreleasepool {
        try {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            require(device != nil, "Native texture tests require a Metal device");
            image_keys_and_pixels_test(device);
            std::cout << "image_keys_and_pixels_test passed\n";
            row_keys_and_pixels_test(device);
            std::cout << "row_keys_and_pixels_test passed\n";
            eviction_and_failure_order_test(device);
            std::cout << "eviction_and_failure_order_test passed\n";
            aggregate_budget_and_clear_test(device);
            std::cout << "aggregate_budget_and_clear_test passed\n";
            limits_and_entry_counts_test(device);
            std::cout << "limits_and_entry_counts_test passed\n";
            submitted_texture_lifetime_test(device);
            std::cout << "submitted_texture_lifetime_test passed\n";
            renderer_invalidation_test();
            std::cout << "renderer_invalidation_test passed\n";
            std::cout << "macOS Metal texture resource tests passed\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
