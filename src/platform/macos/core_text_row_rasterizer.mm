#include "core_text_row_rasterizer.hpp"
#include "cf_owner.hpp"

#import <AppKit/AppKit.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace stormtty::macos {
namespace {

constexpr std::size_t maximum_row_bitmap_bytes = 64u * 1024u * 1024u;

NSString* string(std::string_view text) {
    NSString* result = [[NSString alloc] initWithBytes:text.data()
                                              length:text.size()
                                            encoding:NSUTF8StringEncoding];
    return result ? result : @"\uFFFD";
}

Color foreground(const Cell& cell) {
    Color color = cell.style.inverse ? cell.background : cell.foreground;
    if (cell.selected) {
        color = {236, 238, 242, 255};
    }
    if (cell.style.faint) {
        color.r = std::uint8_t(color.r * 0.6);
        color.g = std::uint8_t(color.g * 0.6);
        color.b = std::uint8_t(color.b * 0.6);
    }
    color.a = 255;
    return color;
}

void fill_color(CGContextRef context, Color color) {
    CGContextSetRGBFillColor(context, color.r / 255., color.g / 255., color.b / 255., 1);
}

CfOwner<CGContextRef> bitmap_context(RasterizedRow& pixels) {
    CfOwner<CGColorSpaceRef> colors(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    CfOwner<CGContextRef> context(CGBitmapContextCreate(
        pixels.bgra.data(), pixels.width, pixels.height, 8, std::size_t(pixels.width) * 4,
        colors.get(), kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little));
    if (!context) {
        throw std::runtime_error("Cannot allocate CoreText row bitmap");
    }
    CGContextSetShouldAntialias(context.get(), true);
    // Grayscale AA is translucent-safe; do not enable LCD subpixel smoothing.
    CGContextSetShouldSmoothFonts(context.get(), false);
    return context;
}

struct Source {
    CFIndex begin{};
    CFIndex end{};
    std::size_t column{};
    std::size_t span{};
};

struct AttributedRow {
    NSMutableAttributedString* text = [[NSMutableAttributedString alloc] init];
    std::vector<Source> sources;
    std::vector<std::size_t> utf16_source;
};

AttributedRow attributed_row(std::span<const Cell> cells, CTFontRef font,
                             const RowRasterOptions& options) {
    AttributedRow result;
    std::size_t text_bytes = 0;
    // Deliberately after bitmap/context and attributed-string construction,
    // preserving the original allocation and failure ordering.
    if (cells.size() > 4096) {
        throw std::runtime_error("CoreText row exceeds the 4096-cell shaping budget");
    }

    for (std::size_t column = 0; column < cells.size(); ++column) {
        const auto& cell = cells[column];
        if (!cell.span) {
            continue;
        }
        const bool hidden = cell.grapheme.empty() || cell.image_placeholder ||
            cell.style.invisible || (cell.style.blink && !options.text_blink_on);
        // Substitute only rendering work; never modify the original VT text.
        const bool oversized = cell.grapheme.size() > 4096 ||
            cell.grapheme.size() > 65536 - text_bytes - 3 * (cells.size() - column);
        NSString* glyph = hidden ? @" " : oversized ? @"\uFFFD" : string(cell.grapheme);
        text_bytes += hidden ? 1 : oversized ? 3 : cell.grapheme.size();
        const auto start = static_cast<CFIndex>(result.text.length);

        CTFontSymbolicTraits traits = 0;
        if (cell.style.bold) {
            traits |= kCTFontBoldTrait;
        }
        if (cell.style.italic) {
            traits |= kCTFontItalicTrait;
        }
        CfOwner<CTFontRef> variant(traits
            ? CTFontCreateCopyWithSymbolicTraits(font, 0, nullptr, traits, traits)
            : nullptr);
        const auto fg = foreground(cell);
        NSColor* color = [NSColor colorWithSRGBRed:fg.r / 255.
                                          green:fg.g / 255.
                                           blue:fg.b / 255.
                                          alpha:1];
        NSDictionary* attributes = @{
            (__bridge NSString*)kCTFontAttributeName : (__bridge id)(variant ? variant.get() : font),
            (__bridge NSString*)kCTForegroundColorAttributeName : (__bridge id)color.CGColor,
            (__bridge NSString*)kCTLigatureAttributeName : @(options.ligatures ? 1 : 0),
            (__bridge NSString*)kCTLanguageAttributeName : string(options.locale)
        };
        [result.text appendAttributedString:
            [[NSAttributedString alloc] initWithString:glyph attributes:attributes]];
        result.utf16_source.insert(result.utf16_source.end(), glyph.length, result.sources.size());
        result.sources.push_back({start, static_cast<CFIndex>(result.text.length), column, cell.span});
    }
    return result;
}

CfOwner<CTLineRef> draw_glyphs(CGContextRef context, const AttributedRow& text, float cell_width,
                 float baseline, std::uint32_t height) {
    // Shape the entire attributed row, including fallback/contextual runs.
    // CoreText advances never determine terminal geometry.
    CfOwner<CTLineRef> line(CTLineCreateWithAttributedString(
        (__bridge CFAttributedStringRef)text.text));
    if (!line) {
        return line; // Decorations still render when CoreText produces no line.
    }

    const CFArrayRef runs = CTLineGetGlyphRuns(line.get());
    std::vector<CFIndex> boundaries;
    std::vector<double> anchors(text.sources.size(), std::numeric_limits<double>::infinity());
    std::size_t glyph_count = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(runs); ++i) {
        auto run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, i));
        const CFIndex count = CTRunGetGlyphCount(run);
        if (count < 0 || std::size_t(count) > 131072 - glyph_count) {
            throw std::runtime_error("CoreText glyph output exceeds row budget");
        }
        glyph_count += static_cast<std::size_t>(count);
        std::vector<CFIndex> indexes(static_cast<std::size_t>(count));
        std::vector<CGPoint> positions(static_cast<std::size_t>(count));
        CTRunGetStringIndices(run, CFRangeMake(0, 0), indexes.data());
        CTRunGetPositions(run, CFRangeMake(0, 0), positions.data());
        boundaries.insert(boundaries.end(), indexes.begin(), indexes.end());
        for (CFIndex glyph = 0; glyph < count; ++glyph) {
            if (indexes[glyph] >= 0 && std::size_t(indexes[glyph]) < text.utf16_source.size()) {
                const auto source = text.utf16_source[std::size_t(indexes[glyph])];
                anchors[source] = std::min(anchors[source], double(positions[glyph].x));
            }
        }
    }

    boundaries.push_back(static_cast<CFIndex>(text.text.length));
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    for (CFIndex i = 0; i < CFArrayGetCount(runs); ++i) {
        auto run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, i));
        const CFIndex count = CTRunGetGlyphCount(run);
        std::vector<CFIndex> indexes(static_cast<std::size_t>(count));
        std::vector<CGPoint> positions(static_cast<std::size_t>(count));
        CTRunGetStringIndices(run, CFRangeMake(0, 0), indexes.data());
        CTRunGetPositions(run, CFRangeMake(0, 0), positions.data());
        for (CFIndex glyph = 0; glyph < count;) {
            if (indexes[glyph] < 0 || std::size_t(indexes[glyph]) >= text.utf16_source.size()) {
                ++glyph;
                continue;
            }
            const auto source_index = text.utf16_source[std::size_t(indexes[glyph])];
            const auto& source = text.sources[source_index];
            CFIndex end = glyph + 1;
            while (end < count && indexes[end] >= source.begin && indexes[end] < source.end) {
                ++end;
            }
            const auto after = std::upper_bound(boundaries.begin(), boundaries.end(), indexes[glyph]);
            const auto cluster_end = after == boundaries.end()
                ? source.end : std::max(source.end, *after);
            std::size_t last = source.column + source.span;
            if (cluster_end > 0 && std::size_t(cluster_end - 1) < text.utf16_source.size()) {
                const auto& tail = text.sources[text.utf16_source[std::size_t(cluster_end - 1)]];
                last = std::max(last, tail.column + tail.span);
            }
            CGContextSaveGState(context);
            CGContextClipToRect(context, CGRectMake(
                source.column * cell_width, 0, (last - source.column) * cell_width, height));
            CGContextSetTextPosition(context, source.column * cell_width - anchors[source_index],
                                    float(height) - baseline);
            CTRunDraw(run, context, CFRangeMake(glyph, end - glyph));
            CGContextRestoreGState(context);
            glyph = end;
        }
    }
    return line;
}

void draw_decorations(CGContextRef context, std::span<const Cell> cells,
                      const RowRasterOptions& options, float baseline) {
    const auto height = options.height;
    for (std::size_t column = 0; column < cells.size(); ++column) {
        const auto& cell = cells[column];
        // Unlike text hiding, empty/image-placeholder cells can have decorations.
        if (!cell.span || cell.style.invisible || (cell.style.blink && !options.text_blink_on)) {
            continue;
        }
        const float x = column * options.cell_width;
        const float width = cell.span * options.cell_width;
        fill_color(context, foreground(cell));
        if (cell.style.strikethrough) {
            CGContextFillRect(context, CGRectMake(x, height - baseline + options.metrics.baseline * .34f,
                                                 width, std::max(1.f, options.dpi / 96.f)));
        }
        if (cell.style.overline) {
            CGContextFillRect(context, CGRectMake(x, height - 1, width, 1));
        }
        if (!cell.style.underline) {
            continue;
        }
        fill_color(context, cell.underline_color);
        const float y = std::max(1.f, height - baseline - 2);
        const float thickness = std::max(1.f, options.dpi / 96.f);
        if (cell.style.underline <= 2) {
            CGContextFillRect(context, CGRectMake(x, y, width, thickness));
            if (cell.style.underline == 2) {
                CGContextFillRect(context, CGRectMake(x, std::max(0.f, y - 2 * thickness), width, thickness));
            }
            continue;
        }

        CGContextSaveGState(context);
        const auto color = cell.underline_color;
        CGContextSetRGBStrokeColor(context, color.r / 255., color.g / 255., color.b / 255., 1);
        CGContextSetLineWidth(context, thickness);
        if (cell.style.underline == 3) {
            for (float offset = 0; offset <= width; offset += std::max(1.f, thickness)) {
                const float yy = y + std::sin(offset / (2 * thickness)) * thickness;
                if (offset == 0) {
                    CGContextMoveToPoint(context, x, yy);
                } else {
                    CGContextAddLineToPoint(context, x + offset, yy);
                }
            }
        } else {
            const CGFloat dash[2] = {cell.style.underline == 4 ? 0. : 3. * thickness, 2. * thickness};
            CGContextSetLineDash(context, 0, dash, 2);
            if (cell.style.underline == 4) {
                CGContextSetLineCap(context, kCGLineCapRound);
            }
            CGContextMoveToPoint(context, x, y);
            CGContextAddLineToPoint(context, x + width, y);
        }
        CGContextStrokePath(context);
        CGContextRestoreGState(context);
    }
}

} // namespace

RasterizedRow CoreTextRowRasterizer::rasterize(std::span<const Cell> cells, CTFontRef borrowed_font,
                                              const RowRasterOptions& options) const {
    const auto bytes = std::size_t(options.width) * options.height * 4;
    if (bytes > std::min(options.maximum_output_bytes, maximum_row_bitmap_bytes)) {
        throw std::runtime_error("Visible CoreText rows exceed transient texture budget");
    }

    RasterizedRow pixels{options.width, options.height, std::vector<std::uint8_t>(bytes)};
    {
        auto context = bitmap_context(pixels);
        const float baseline = std::min(float(options.height) - 1, options.metrics.baseline);
        const auto text = attributed_row(cells, borrowed_font, options);
        const auto line = draw_glyphs(context.get(), text, options.cell_width, baseline, options.height);
        draw_decorations(context.get(), cells, options, baseline);
    }
    // Destroy CoreGraphics' borrowed backing-store reference before returning
    // owned pixels. No second pixel allocation or native object escapes.
    return pixels;
}

} // namespace stormtty::macos
