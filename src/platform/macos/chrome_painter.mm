#include "chrome_painter.hpp"
#include "cf_owner.hpp"
#include "metal_compositor.hpp"
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace stormtty::macos {
namespace {

NSString* string(std::string_view text) {
    return [[NSString alloc] initWithBytes:text.data() length:text.size()
                                  encoding:NSUTF8StringEncoding] ?: @"\uFFFD";
}

bool finite(PixelRect rect) {
    return std::isfinite(rect.x) && std::isfinite(rect.y) &&
           std::isfinite(rect.width) && std::isfinite(rect.height) &&
           std::abs(rect.x) <= 1e7f && std::abs(rect.y) <= 1e7f &&
           std::abs(rect.width) <= 1e7f && std::abs(rect.height) <= 1e7f;
}

struct Bitmap {
    std::vector<std::uint8_t> bytes;
    CfOwner<CGContextRef> context;

    Bitmap(std::uint32_t width, std::uint32_t height)
        : bytes(std::size_t(width) * height * 4) {
        CfOwner<CGColorSpaceRef> colors(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
        context = CfOwner<CGContextRef>(CGBitmapContextCreate(
            bytes.data(), width, height, 8, std::size_t(width) * 4, colors.get(),
            kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little));
        if (!context) {
            throw std::runtime_error("Cannot allocate CoreText row bitmap");
        }
        CGContextSetShouldAntialias(context.get(), true);
        CGContextSetShouldSmoothFonts(context.get(), false);
    }
};

} // namespace

ChromePainter::ChromePainter(MetalFrame& frame, PixelRect viewport, float dpi) noexcept
    : frame_(frame), viewport_(viewport), dpi_(dpi) {}

void ChromePainter::draw(const ChromePrimitive& primitive, std::size_t remaining_upload_bytes) {
    // Reset even for skipped items: a previous scroll clip must not leak.
    if (!frame_.clip(primitive.clip.value_or(viewport_))) {
        return;
    }
    if (!finite(primitive.rect) || !std::isfinite(primitive.stroke_width) ||
        !std::isfinite(primitive.radius)) {
        return;
    }
    const bool line = primitive.kind == ChromePrimitiveKind::line;
    if (!line && primitive.radius <= 0 && primitive.stroke_width <= 0) {
        frame_.fill(primitive.rect, primitive.fill, primitive.background_replace);
        return;
    }

    const float pad = std::clamp(primitive.stroke_width, 1.f, 256.f) + 1;
    PixelRect bounds{
        std::min(primitive.rect.x, primitive.rect.x + primitive.rect.width) - pad,
        std::min(primitive.rect.y, primitive.rect.y + primitive.rect.height) - pad,
        std::abs(primitive.rect.width) + 2 * pad,
        std::abs(primitive.rect.height) + 2 * pad
    };
    const auto width = static_cast<std::uint32_t>(std::ceil(bounds.width));
    const auto height = static_cast<std::uint32_t>(std::ceil(bounds.height));
    if (!width || !height || std::uint64_t(width) * height * 4 > 4u * 1024u * 1024u ||
        std::uint64_t(width) * height * 4 > remaining_upload_bytes) {
        return;
    }

    Bitmap bitmap(width, height);
    auto context = bitmap.context.get();
    CGContextTranslateCTM(context, 0, height);
    CGContextScaleCTM(context, 1, -1);
    auto fill = primitive.fill;
    auto stroke = primitive.stroke;
    CGContextSetRGBFillColor(context, fill.r / 255., fill.g / 255., fill.b / 255., fill.a / 255.);
    if (line) {
        if (primitive.stroke_width <= 0) {
            stroke = fill;
        }
        CGContextMoveToPoint(context, primitive.rect.x - bounds.x, primitive.rect.y - bounds.y);
        CGContextAddLineToPoint(context, primitive.rect.x + primitive.rect.width - bounds.x,
                               primitive.rect.y + primitive.rect.height - bounds.y);
    } else {
        CfOwner<CGPathRef> path(CGPathCreateWithRoundedRect(
            CGRectMake(pad, pad, std::max(0.f, primitive.rect.width), std::max(0.f, primitive.rect.height)),
            std::max(0.f, primitive.radius), std::max(0.f, primitive.radius), nullptr));
        CGContextAddPath(context, path.get());
    }
    CGContextSetRGBStrokeColor(context, stroke.r / 255., stroke.g / 255., stroke.b / 255., stroke.a / 255.);
    CGContextSetLineWidth(context, std::max(1.f, primitive.stroke_width));
    CGContextDrawPath(context, line ? kCGPathStroke : primitive.stroke_width > 0 ? kCGPathFillStroke : kCGPathFill);
    frame_.draw_bgra(width, height, bitmap.bytes, {bounds.x, bounds.y, float(width), float(height)},
                     {255, 255, 255, 255}, primitive.background_replace);
}

void ChromePainter::draw(const ChromeText& label, std::size_t remaining_upload_bytes) {
    if (!frame_.clip(label.clip.value_or(viewport_))) {
        return;
    }
    if (!finite(label.rect) || label.text.empty() || !label.color.a || label.text.size() > 65536 ||
        !std::isfinite(label.size_dip) || label.size_dip < 1 || label.size_dip > 256) {
        return;
    }
    const auto width = static_cast<std::uint32_t>(std::max(0.f, std::ceil(label.rect.width)));
    const auto height = static_cast<std::uint32_t>(std::max(0.f, std::ceil(label.rect.height)));
    if (!width || !height || std::uint64_t(width) * height * 4 > 4u * 1024u * 1024u ||
        std::uint64_t(width) * height * 4 > remaining_upload_bytes) {
        return;
    }

    Bitmap bitmap(width, height);
    const CGFloat size = label.size_dip * dpi_ / 96.;
    NSFont* font = label.family.empty() ? nil : [NSFont fontWithName:string(label.family) size:size];
    if (!font) {
        font = [NSFont systemFontOfSize:size weight:label.semibold ? NSFontWeightSemibold : NSFontWeightRegular];
    } else if (label.semibold) {
        font = [[NSFontManager sharedFontManager] convertFont:font toHaveTrait:NSBoldFontMask];
    }
    auto foreground = label.color;
    NSColor* color = [NSColor colorWithSRGBRed:foreground.r / 255. green:foreground.g / 255.
                                        blue:foreground.b / 255. alpha:1];
    NSAttributedString* text = [[NSAttributedString alloc] initWithString:string(label.text)
        attributes:@{NSFontAttributeName:font, NSForegroundColorAttributeName:color}];
    CfOwner<CTLineRef> line(CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)text));
    const CGFloat flush = label.alignment == ChromeTextAlignment::center ? .5 :
                          label.alignment == ChromeTextAlignment::trailing ? 1 : 0;
    const double pen = std::max(0., CTLineGetPenOffsetForFlush(line.get(), flush, label.rect.width));
    CGContextSetTextPosition(bitmap.context.get(), pen, std::max(0., (height - font.ascender + font.descender) / 2.));
    CTLineDraw(line.get(), bitmap.context.get());
    frame_.draw_bgra(width, height, bitmap.bytes, label.rect, {255, 255, 255, foreground.a});
}

} // namespace stormtty::macos
