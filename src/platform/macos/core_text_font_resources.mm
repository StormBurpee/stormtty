#include "core_text_font_resources.hpp"

#import <Foundation/Foundation.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace stormtty::macos {

void CoreTextFontResources::reset(const FontOptions& options, float dpi) {
    if (options.family.empty() || options.family.size() > 256 ||
        options.locale.size() > 128 || !std::isfinite(options.size_dip) ||
        options.size_dip < 6 || options.size_dip > 72) {
        throw std::invalid_argument("Invalid CoreText font options");
    }

    const float pixels = options.size_dip * dpi / 96.f;
    NSString* family = [[NSString alloc] initWithBytes:options.family.data()
                                              length:options.family.size()
                                            encoding:NSUTF8StringEncoding] ?: @"\uFFFD";
    font_ = CfOwner<CTFontRef>(CTFontCreateWithName((__bridge CFStringRef)family,
                                                 pixels, nullptr));
    if (!font_) {
        throw std::runtime_error("Cannot create terminal CoreText font");
    }

    UniChar letter = 'M';
    CGGlyph glyph{};
    CGSize advance{};
    CTFontGetGlyphsForCharacters(font_.get(), &letter, &glyph, 1);
    CTFontGetAdvancesForGlyphs(font_.get(), kCTFontOrientationHorizontal,
                             &glyph, &advance, 1);

    metrics_.width = static_cast<std::uint16_t>(
        std::clamp(std::ceil(advance.width), 1., 512.));
    metrics_.height = static_cast<std::uint16_t>(std::clamp(
        std::ceil(CTFontGetAscent(font_.get()) + CTFontGetDescent(font_.get()) +
                  CTFontGetLeading(font_.get()) + 2 * dpi / 96.),
        1., 1024.));
    metrics_.baseline = static_cast<float>(CTFontGetAscent(font_.get()) + dpi / 96.);
}

CellMetrics CoreTextFontResources::metrics() const noexcept {
    return metrics_;
}

CTFontRef CoreTextFontResources::borrow_font() const noexcept {
    return font_.get();
}

} // namespace stormtty::macos
