#import <AppKit/AppKit.h>
#include "../src/platform/macos/chrome_painter.hpp"
#include "../src/platform/macos/metal_compositor.hpp"
#include "macos_chrome_reference.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using namespace stormtty;
using namespace stormtty::macos;
constexpr std::uint32_t width = 256;
constexpr std::uint32_t height = 160;
constexpr std::size_t upload_limit = 64u * 1024u * 1024u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Result {
    std::vector<std::uint8_t> pixels;
    std::size_t remaining{};
};

Result render(const WindowScene& scene, float dpi, bool reference,
              std::size_t allowance = upload_limit) {
    MetalTextureResources textures({4096, 4096, 16});
    MetalCompositor owner(nullptr);
    owner.create_device(textures);
    owner.update_layer_geometry(dpi);
    owner.replace_surface(width, height, dpi);
    std::size_t remaining = 0;
    {
        auto frame = owner.begin_frame(textures);
        require(bool(frame), "Offscreen chrome frame not admitted");
        frame.fill({0, 0, width, height}, {23, 25, 29, 128});
        if (allowance != upload_limit) {
            // Exercise real aggregate accounting without allocating 64 MiB.
            // upload_bgra charges before rejecting these oversized dimensions.
            bool charged_failure = false;
            try {
                frame.draw_bgra(static_cast<std::uint32_t>((upload_limit - allowance) / 4),
                                1, {}, {0, 0, 1, 1});
            } catch (const std::runtime_error& error) {
                charged_failure = std::string_view(error.what()) == "Metal texture dimensions exceed supported limits";
            }
            require(charged_failure && textures.remaining_upload_bytes() == allowance,
                    "Synthetic precharge changed native failure/accounting order");
        }
        frame.clip({0, 0, width, height});
        if (reference) {
            test::chrome_reference::draw(scene, frame, textures, width, height, dpi);
        } else {
            ChromePainter painter(frame, {0, 0, width, height}, dpi);
            for (const auto& item : chrome_paint_order(scene)) {
                if (item.kind == ChromeDrawKind::primitive) {
                    painter.draw(scene.primitives[item.index], textures.remaining_upload_bytes());
                } else {
                    painter.draw(scene.texts[item.index], textures.remaining_upload_bytes());
                }
            }
        }
        remaining = textures.remaining_upload_bytes();
        frame.submit();
    }
    return {owner.read_pixels(), remaining};
}

Result compare(const WindowScene& scene, float dpi, std::size_t allowance = upload_limit) {
    std::cerr << "chrome comparison dpi=" << dpi << " allowance=" << allowance
              << " primitives=" << scene.primitives.size() << " labels=" << scene.texts.size()
              << std::endl;
    const auto original = render(scene, dpi, true, allowance);
    auto extracted = render(scene, dpi, false, allowance);
    require(original.pixels == extracted.pixels, "Chrome extraction differs from original native pixels");
    require(original.remaining == extracted.remaining, "Chrome extraction changed upload accounting");
    return extracted;
}

ChromePrimitive rectangle(PixelRect rect, Color color) {
    ChromePrimitive primitive;
    primitive.rect = rect;
    primitive.fill = color;
    return primitive;
}

ChromeText label(PixelRect rect, std::string text) {
    ChromeText result;
    result.rect = rect;
    result.text = std::move(text);
    result.family = "Menlo";
    result.color = {220, 230, 240, 191};
    return result;
}

void primitive_and_layer_test() {
    WindowScene scene;
    scene.primitives.push_back(rectangle({2.25f, 4.5f, 100.5f, 75.25f}, {170, 40, 60, 170}));
    auto rounded = rectangle({25.5f, 20.25f, 100.5f, 65.5f}, {20, 180, 50, 130});
    rounded.radius = 11.5f;
    rounded.stroke_width = 2.5f;
    rounded.stroke = {230, 220, 60, 210};
    rounded.background_replace = true;
    rounded.z = {ChromeLayer::modal, 2};
    scene.primitives.push_back(rounded);
    auto line = rectangle({170.5f, 110.25f, -120.5f, -70.75f}, {100, 140, 220, 140});
    line.kind = ChromePrimitiveKind::line;
    line.stroke_width = -1; // Existing fallback uses fill and a one-pixel stroke.
    line.z = {ChromeLayer::toast, 0};
    scene.primitives.push_back(line);
    auto text = label({8, 15, 200, 30}, "Layer text");
    text.z = {ChromeLayer::base, 0};
    scene.texts.push_back(text);
    text.rect.y = 60;
    text.z = {ChromeLayer::context_menu, 1};
    scene.texts.push_back(text);
    for (float dpi : {96.f, 144.f, 192.f}) {
        const auto painted = compare(scene, dpi);
        const auto empty = render({}, dpi, false);
        require(painted.pixels != empty.pixels && painted.remaining < upload_limit,
                "Primitive/layer comparison produced no paint or uploads");
    }
}

void text_and_clip_test() {
    for (float dpi : {96.f, 144.f, 192.f}) {
        WindowScene scene;
        for (unsigned index = 0; index < 4; ++index) {
            auto text = label({8.25f, float(index * 36) + .5f, 235.5f, 35.25f},
                              index == 3 ? std::string("\xFF") : "A e\xCC\x81 界 👩‍💻");
            text.alignment = index == 1 ? ChromeTextAlignment::center :
                             index == 2 ? ChromeTextAlignment::trailing : ChromeTextAlignment::leading;
            text.family = index == 1 ? "" : index == 2 ? "StormTTY Missing Face" : "Menlo";
            text.semibold = index != 0;
            text.color.a = index == 3 ? 1 : 191;
            text.clip = index == 0 ? std::optional<PixelRect>{{20.5f, 12.25f, 80.25f, 18.5f}} : std::nullopt;
            scene.texts.push_back(text);
        }
        compare(scene, dpi);
        auto wrapped = scene;
        for (auto& text : wrapped.texts) text.wrap = true;
        require(compare(wrapped, dpi).pixels == compare(scene, dpi).pixels,
                "Chrome wrap flag gained unsupported multiline semantics");
    }

    WindowScene clipped;
    auto scroll = rectangle({0, 0, 256, 160}, {255, 0, 0, 255});
    scroll.clip = PixelRect{10.5f, 10.5f, 20, 20};
    clipped.primitives.push_back(scroll);
    auto fixed = rectangle({100, 100, 20, 20}, {0, 255, 0, 255});
    clipped.primitives.push_back(fixed);
    const auto result = compare(clipped, 144);
    const auto offset = (110u * width + 110u) * 4;
    require(result.pixels[offset + 1] == 255, "Scroll clip leaked into subsequent fixed item");
}

void partial_failure_test() {
    for (bool reference : {true, false}) {
        MetalTextureResources textures({4096, 4096, 16});
        MetalCompositor owner(nullptr);
        owner.create_device(textures);
        owner.update_layer_geometry(96);
        owner.replace_surface(width, height, 96);
        WindowScene scene;
        scene.texts.push_back(label({0, 0, 16, 16}, "A"));
        // Within the CPU item budget, but exceeds the Metal texture width limit.
        scene.texts.push_back(label({0, 0, 20000, 1}, "B"));
        bool failed = false;
        @autoreleasepool {
            try {
                auto frame = owner.begin_frame(textures);
                frame.clip({0, 0, width, height});
                if (reference) {
                    test::chrome_reference::draw(scene, frame, textures, width, height, 96);
                } else {
                    ChromePainter painter(frame, {0, 0, width, height}, 96);
                    for (const auto& text : scene.texts) {
                        painter.draw(text, textures.remaining_upload_bytes());
                    }
                }
            } catch (const std::runtime_error& error) {
                failed = std::string_view(error.what()) == "Metal texture dimensions exceed supported limits";
            }
        }
        require(failed && textures.remaining_upload_bytes() == upload_limit - 1024 - 80000,
                "Chrome failure changed charged partial progress or error propagation");
        {
            auto frame = owner.begin_frame(textures);
            require(bool(frame) && textures.remaining_upload_bytes() == upload_limit,
                    "Chrome unwind blocked subsequent frame or fresh budget");
            frame.fill({0, 0, width, height}, {0, 255, 0, 255});
            frame.submit();
        }
        const auto pixels = owner.read_pixels();
        require(pixels[0] == 0 && pixels[1] == 255 && pixels[3] == 255,
                "Chrome unwind prevented subsequent successful rendering");
    }
}

void guard_and_budget_test() {
    WindowScene scene;
    auto text = label({4, 4, 16, 16}, "A"); // Exactly 1,024 bytes per item.
    scene.texts.push_back(text);
    text.rect.x = 30;
    scene.texts.push_back(text);
    const auto zero = compare(scene, 96, 0);
    const auto short_budget = compare(scene, 96, 1020);
    const auto exact = compare(scene, 96, 1024);
    const auto both = compare(scene, 96, 2048);
    require(zero.remaining == 0 && short_budget.remaining == 1020 &&
            exact.remaining == 0 && both.remaining == 0,
            "Per-item allowance boundary or successive upload accounting changed");
    require(zero.pixels == short_budget.pixels && exact.pixels != zero.pixels && both.pixels != exact.pixels,
            "Budget guards skipped valid text or reused a stale allowance");

    WindowScene rounded_scene;
    auto rounded = rectangle({10, 10, 20, 20}, {80, 120, 160, 128});
    rounded.radius = 4;
    rounded.background_replace = true;
    rounded_scene.primitives.push_back(rounded); // 24 x 24 padded raster.
    const auto rounded_short = compare(rounded_scene, 96, 2300);
    const auto rounded_exact = compare(rounded_scene, 96, 2304);
    require(rounded_short.remaining == 2300 && rounded_exact.remaining == 0 &&
            rounded_short.pixels == zero.pixels && rounded_exact.pixels != zero.pixels,
            "Primitive allowance boundary changed");
    const auto padding = (8u * width + 8u) * 4;
    require(rounded_exact.pixels[padding + 3] == zero.pixels[padding + 3],
            "Transparent rounded replacement padding cleared the underlying canvas");

    WindowScene item_limit;
    item_limit.texts.push_back(label({0, 0, 1024, 1024}, "Item limit"));
    require(compare(item_limit, 96).remaining == upload_limit - 4u * 1024u * 1024u,
            "Exact per-item byte limit was rejected");

    WindowScene guarded;
    for (unsigned index = 0; index < 7; ++index) {
        auto invalid = label({0, 0, 16, 16}, "A");
        if (index == 0) invalid.clip = PixelRect{0, 0, 0, 20};
        if (index == 1) invalid.rect.x = std::numeric_limits<float>::quiet_NaN();
        if (index == 2) invalid.color.a = 0;
        if (index == 3) invalid.size_dip = 257;
        if (index == 4) invalid.text.assign(65537, 'A');
        if (index == 5) invalid.rect = {0, 0, 1025, 1024};
        if (index == 6) invalid.text.clear();
        guarded.texts.push_back(std::move(invalid));
    }
    auto oversized = rectangle({0, 0, 2048, 2048}, {255, 0, 0, 255});
    oversized.radius = 4;
    guarded.primitives.push_back(oversized);
    auto invalid = oversized;
    invalid.stroke_width = std::numeric_limits<float>::infinity();
    guarded.primitives.push_back(invalid);
    const auto skipped = compare(guarded, 96);
    require(skipped.remaining == upload_limit && skipped.pixels == render({}, 96, false).pixels,
            "Rejected chrome consumed uploads or painted");

    auto fast = rectangle({10, 10, 20, 20}, {80, 120, 160, 64});
    fast.background_replace = true;
    WindowScene fills;
    fills.primitives.push_back(fast);
    const auto filled = compare(fills, 96, 0);
    require(filled.pixels != zero.pixels && filled.remaining == 0,
            "Direct fill incorrectly requires upload allowance");
}

} // namespace

int main() {
    @autoreleasepool {
        try {
            std::cerr << "BEGIN primitive_and_layer" << std::endl;
            primitive_and_layer_test();
            std::cerr << "BEGIN text_and_clip" << std::endl;
            text_and_clip_test();
            std::cerr << "BEGIN guard_and_budget" << std::endl;
            guard_and_budget_test();
            std::cerr << "BEGIN partial_failure" << std::endl;
            partial_failure_test();
            std::cout << "PASS native macOS original/extracted chrome pixels, clipping and budgets\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL macOS chrome painter: " << error.what() << '\n';
            return 1;
        }
    }
}
