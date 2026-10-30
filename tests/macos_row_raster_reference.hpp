#pragma once

// Frozen pre-extraction CPU reference from renderer.mm at 3700dd56. Retain the
// original expressions/order here: this is an independent native comparison,
// not a second production rasterizer or a cross-platform pixel golden.
#include "../src/platform/macos/core_text_row_rasterizer.hpp"
#include "../src/platform/macos/cf_owner.hpp"
#import <AppKit/AppKit.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace stormtty::test::original_core_text_row {
using macos::CfOwner;

NSString* string(std::string_view text) {
    return [[NSString alloc] initWithBytes:text.data() length:text.size()
                                encoding:NSUTF8StringEncoding] ?: @"\uFFFD";
}

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Bitmap {
    std::uint32_t width{},height{};
    std::vector<std::uint8_t> bytes;
    CfOwner<CGContextRef> context;
    Bitmap(std::uint32_t w,std::uint32_t h):width(w),height(h),bytes(std::size_t(w)*h*4) {
        CfOwner<CGColorSpaceRef> colors(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
        context=CfOwner<CGContextRef>(CGBitmapContextCreate(bytes.data(),w,h,8,std::size_t(w)*4,colors.get(),
            kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little));
        check(bool(context),"Cannot allocate CoreText row bitmap");
        CGContextSetShouldAntialias(context.get(),true);
        CGContextSetShouldSmoothFonts(context.get(),false); // Grayscale AA: translucent-safe, not LCD subpixels.
    }
};
Color foreground(const Cell& cell) {
    Color c=cell.style.inverse?cell.background:cell.foreground;
    if(cell.selected)c={236,238,242,255};
    if(cell.style.faint){c.r=std::uint8_t(c.r*0.6);c.g=std::uint8_t(c.g*0.6);c.b=std::uint8_t(c.b*0.6);}
    c.a=255;return c;
}
void cg_color(CGContextRef context,Color color) {
    CGContextSetRGBFillColor(context,color.r/255.,color.g/255.,color.b/255.,1);
}
macos::RasterizedRow rasterize(std::span<const Cell> cells, CTFontRef font,
                               const macos::RowRasterOptions& options) {
    const auto w = options.width;
    const auto h = options.height;
    const auto dpi = options.dpi;
        Bitmap bitmap(w,h);auto ctx=bitmap.context.get();
        const auto cell = options.metrics;
        // Borrow only for this synchronous rasterization. Attributed strings and
        // trait copies retain their own references; no borrow enters a GPU callback.

        const float cw=options.cell_width;
        const float base=std::min(float(h)-1,cell.baseline);
        NSMutableAttributedString* text=[[NSMutableAttributedString alloc] init];
        struct Source { CFIndex begin{},end{};std::size_t column{},span{}; };
        std::vector<Source> sources;std::vector<std::size_t> utf16_source;std::size_t text_bytes=0;
        if(cells.size()>4096)throw std::runtime_error("CoreText row exceeds the 4096-cell shaping budget");
        for(std::size_t column=0;column<cells.size();++column) {
            const auto& c=cells[column];if(!c.span)continue;
            const bool hidden=c.grapheme.empty()||c.image_placeholder||c.style.invisible||(c.style.blink&&!options.text_blink_on);
            // The adapter retains the complete VT text. Rendering substitutes an
            // over-budget grapheme rather than allocating unbounded CoreText work.
            const bool oversized=c.grapheme.size()>4096||c.grapheme.size()>65536-text_bytes-3*(cells.size()-column);
            NSString* glyph=hidden?@" ":oversized?@"\uFFFD":string(c.grapheme);
            text_bytes+=hidden?1:oversized?3:c.grapheme.size();
            const auto start=static_cast<CFIndex>(text.length);
            CTFontSymbolicTraits traits=0;if(c.style.bold)traits|=kCTFontBoldTrait;if(c.style.italic)traits|=kCTFontItalicTrait;
            CfOwner<CTFontRef> variant(traits?CTFontCreateCopyWithSymbolicTraits(font,0,nullptr,traits,traits):nullptr);
            const auto fg=foreground(c);
            NSColor* color=[NSColor colorWithSRGBRed:fg.r/255. green:fg.g/255. blue:fg.b/255. alpha:1];
            NSDictionary* attrs=@{(__bridge NSString*)kCTFontAttributeName:(__bridge id)(variant?variant.get():font),
                (__bridge NSString*)kCTForegroundColorAttributeName:(__bridge id)color.CGColor,
                (__bridge NSString*)kCTLigatureAttributeName:@(options.ligatures?1:0),
                (__bridge NSString*)kCTLanguageAttributeName:string(options.locale)};
            [text appendAttributedString:[[NSAttributedString alloc] initWithString:glyph attributes:attrs]];
            utf16_source.insert(utf16_source.end(),glyph.length,sources.size());
            sources.push_back({start,static_cast<CFIndex>(text.length),column,c.span});
        }
        // Shape the entire attributed row, including fallback/contextual runs.
        // Map CoreText UTF-16 clusters back to immutable VT grapheme cells;
        // CoreText advance widths never determine terminal geometry.
        CfOwner<CTLineRef> line(CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)text));
        if(line) {
            const CFArrayRef runs=CTLineGetGlyphRuns(line.get());
            std::vector<CFIndex> boundaries;std::vector<double> anchors(sources.size(),std::numeric_limits<double>::infinity());
            std::size_t glyph_count=0;
            for(CFIndex i=0;i<CFArrayGetCount(runs);++i) {
                auto run=static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs,i));
                const CFIndex count=CTRunGetGlyphCount(run);
                if(count<0||std::size_t(count)>131072-glyph_count)throw std::runtime_error("CoreText glyph output exceeds row budget");
                glyph_count+=static_cast<std::size_t>(count);std::vector<CFIndex> indexes(static_cast<std::size_t>(count));std::vector<CGPoint> positions(static_cast<std::size_t>(count));
                CTRunGetStringIndices(run,CFRangeMake(0,0),indexes.data());CTRunGetPositions(run,CFRangeMake(0,0),positions.data());
                boundaries.insert(boundaries.end(),indexes.begin(),indexes.end());
                for(CFIndex g=0;g<count;++g)if(indexes[g]>=0&&std::size_t(indexes[g])<utf16_source.size()){auto source=utf16_source[std::size_t(indexes[g])];anchors[source]=std::min(anchors[source],double(positions[g].x));}
            }
            boundaries.push_back(static_cast<CFIndex>(text.length));std::sort(boundaries.begin(),boundaries.end());
            boundaries.erase(std::unique(boundaries.begin(),boundaries.end()),boundaries.end());
            for(CFIndex i=0;i<CFArrayGetCount(runs);++i) {
                auto run=static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs,i));
                const CFIndex count=CTRunGetGlyphCount(run);
                std::vector<CFIndex> indexes(static_cast<std::size_t>(count));std::vector<CGPoint> positions(static_cast<std::size_t>(count));
                CTRunGetStringIndices(run,CFRangeMake(0,0),indexes.data());CTRunGetPositions(run,CFRangeMake(0,0),positions.data());
                for(CFIndex g=0;g<count;) {
                    if(indexes[g]<0||std::size_t(indexes[g])>=utf16_source.size()){++g;continue;}
                    const auto source_index=utf16_source[std::size_t(indexes[g])];const auto* found=&sources[source_index];
                    CFIndex end=g+1;
                    while(end<count&&indexes[end]>=found->begin&&indexes[end]<found->end)++end;
                    auto after=std::upper_bound(boundaries.begin(),boundaries.end(),indexes[g]);
                    const auto cluster_end=after==boundaries.end()?found->end:std::max(found->end,*after);
                    std::size_t last=found->column+found->span;
                    if(cluster_end>0&&std::size_t(cluster_end-1)<utf16_source.size()){const auto& tail=sources[utf16_source[std::size_t(cluster_end-1)]];last=std::max(last,tail.column+tail.span);}
                    CGContextSaveGState(ctx);
                    CGContextClipToRect(ctx,CGRectMake(found->column*cw,0,(last-found->column)*cw,h));
                    CGContextSetTextPosition(ctx,found->column*cw-anchors[source_index],float(h)-base);
                    CTRunDraw(run,ctx,CFRangeMake(g,end-g));CGContextRestoreGState(ctx);g=end;
                }
            }
        }
        for(std::size_t col=0;col<cells.size();++col) {
            const auto& c=cells[col];if(!c.span||c.style.invisible||(c.style.blink&&!options.text_blink_on))continue;
            const float x=col*cw,width_cells=c.span*cw;
            cg_color(ctx,foreground(c));
            if(c.style.strikethrough)CGContextFillRect(ctx,CGRectMake(x,h-base+cell.baseline*.34f,width_cells,std::max(1.f,dpi/96.f)));
            if(c.style.overline)CGContextFillRect(ctx,CGRectMake(x,h-1,width_cells,1));
            if(c.style.underline){cg_color(ctx,c.underline_color);const float y=std::max(1.f,h-base-2),thickness=std::max(1.f,dpi/96.f);
                if(c.style.underline<=2){CGContextFillRect(ctx,CGRectMake(x,y,width_cells,thickness));
                    if(c.style.underline==2)CGContextFillRect(ctx,CGRectMake(x,std::max(0.f,y-2*thickness),width_cells,thickness));
                }else{CGContextSaveGState(ctx);const auto color=c.underline_color;
                    CGContextSetRGBStrokeColor(ctx,color.r/255.,color.g/255.,color.b/255.,1);CGContextSetLineWidth(ctx,thickness);
                    if(c.style.underline==3){for(float offset=0;offset<=width_cells;offset+=std::max(1.f,thickness)){
                        const float yy=y+std::sin(offset/(2*thickness))*thickness;
                        if(offset==0)CGContextMoveToPoint(ctx,x,yy);else CGContextAddLineToPoint(ctx,x+offset,yy);}}
                    else{const CGFloat dash[2]={c.style.underline==4?0.:3.*thickness,2.*thickness};
                        CGContextSetLineDash(ctx,0,dash,2);if(c.style.underline==4)CGContextSetLineCap(ctx,kCGLineCapRound);
                        CGContextMoveToPoint(ctx,x,y);CGContextAddLineToPoint(ctx,x+width_cells,y);}
                    CGContextStrokePath(ctx);CGContextRestoreGState(ctx);
                }
            }
        }
    return {w, h, std::move(bitmap.bytes)};
}

} // namespace stormtty::test::original_core_text_row
