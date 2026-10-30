#import <Foundation/Foundation.h>
#include "../src/platform/macos/core_text_font_resources.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {

using stormtty::CellMetrics;
using stormtty::FontOptions;
using stormtty::macos::CfOwner;
using stormtty::macos::CoreTextFontResources;

static_assert(!std::is_copy_constructible_v<CoreTextFontResources>);
static_assert(!std::is_copy_assignable_v<CoreTextFontResources>);

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

bool same_metrics(CellMetrics left, CellMetrics right) {
    return left.width == right.width && left.height == right.height &&
           left.baseline == right.baseline;
}

CellMetrics expected_metrics(float size, float dpi) {
    CfOwner<CTFontRef> font(CTFontCreateWithName(CFSTR("Menlo"), size * dpi / 96.f, nullptr));
    require(bool(font), "Cannot create independent reference font");
    UniChar letter = 'M';
    CGGlyph glyph{};
    CGSize advance{};
    CTFontGetGlyphsForCharacters(font.get(), &letter, &glyph, 1);
    CTFontGetAdvancesForGlyphs(font.get(), kCTFontOrientationHorizontal, &glyph, &advance, 1);

    return {
        static_cast<std::uint16_t>(std::clamp(std::ceil(advance.width), 1., 512.)),
        static_cast<std::uint16_t>(std::clamp(std::ceil(
            CTFontGetAscent(font.get()) + CTFontGetDescent(font.get()) +
            CTFontGetLeading(font.get()) + 2 * dpi / 96.), 1., 1024.)),
        static_cast<float>(CTFontGetAscent(font.get()) + dpi / 96.)
    };
}

void font_metrics_and_reset_test() {
    FontOptions options;
    options.family = "Menlo";
    CoreTextFontResources resources;

    for (const float dpi : {96.f, 192.f, 96.f}) {
        for (const float size : {6.f, 14.f, 72.f}) {
            options.size_dip = size;
            resources.reset(options, dpi);
            require(resources.borrow_font() != nullptr, "Reset did not own a font");
            require(same_metrics(resources.metrics(), expected_metrics(size, dpi)),
                    "Physical font metrics changed from original formula");

            // Trait copies own their reference independently; the base is only
            // borrowed synchronously while creating the copy.
            CfOwner<CTFontRef> bold(CTFontCreateCopyWithSymbolicTraits(
                resources.borrow_font(), 0, nullptr, kCTFontBoldTrait, kCTFontBoldTrait));
            require(bool(bold), "Menlo bold trait copy unavailable");
            resources.reset(options, dpi);
            require(same_metrics(resources.metrics(), expected_metrics(size, dpi)),
                    "Same-font reset changed metrics");
            require(CTFontGetSize(bold.get()) > 0, "Owned trait font did not survive base reset");
        }
    }
}

void invalid_options_test() {
    FontOptions valid;
    valid.family = "Menlo";
    CoreTextFontResources resources;
    resources.reset(valid, 96);
    const auto before = resources.metrics();
    const auto borrowed_before = resources.borrow_font();

    const auto rejects = [&](const FontOptions& invalid) {
        bool rejected = false;
        try {
            resources.reset(invalid, 96);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "Invalid font options accepted");
        require(resources.borrow_font() == borrowed_before &&
                    same_metrics(resources.metrics(), before),
                "Validation failure replaced existing resources");
    };

    auto invalid = valid;
    invalid.family.clear();
    rejects(invalid);
    invalid = valid;
    invalid.family.assign(257, 'a');
    rejects(invalid);
    invalid = valid;
    invalid.locale.assign(129, 'a');
    rejects(invalid);

    for (const float size : {5.f, 73.f, std::numeric_limits<float>::infinity(),
                             std::numeric_limits<float>::quiet_NaN()}) {
        invalid = valid;
        invalid.size_dip = size;
        rejects(invalid);
    }
}

} // namespace

int main() {
    @autoreleasepool {
        try {
            font_metrics_and_reset_test();
            invalid_options_test();
            std::cout << "PASS native macOS CoreText font ownership/metrics/reset\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL macOS font resources: " << error.what() << '\n';
            return 1;
        }
    }
}
