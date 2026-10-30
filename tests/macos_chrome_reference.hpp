#pragma once

// Frozen pre-extraction chrome algorithm from 1378612d. Test-only baseline;
// keep independent of ChromePainter so pixel comparisons can detect divergence.
#include "../src/platform/macos/cf_owner.hpp"
#include "../src/platform/macos/metal_compositor.hpp"
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace stormtty::test::chrome_reference {
using macos::CfOwner;
NSString* string(std::string_view text) {
    return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding] ?: @"\uFFFD";
}
void check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
bool finite(PixelRect r){return std::isfinite(r.x)&&std::isfinite(r.y)&&std::isfinite(r.width)&&std::isfinite(r.height)&&std::abs(r.x)<=1e7f&&std::abs(r.y)<=1e7f&&std::abs(r.width)<=1e7f&&std::abs(r.height)<=1e7f;}
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

inline void draw(const WindowScene& scene, macos::MetalFrame& paint,
                 const macos::MetalTextureResources& textures,
                 std::uint32_t width, std::uint32_t height, float dpi) {
            for(const auto& item:chrome_paint_order(scene)) {
                if(item.kind==ChromeDrawKind::primitive) {
                    const auto& primitive=scene.primitives[item.index];
                    // Reset for every item, including the first fixed item after
                    // scroll content. Raster textures keep their original bounds.
                    if(!paint.clip(primitive.clip.value_or(PixelRect{0,0,float(width),float(height)})))continue;
                    if(!finite(primitive.rect)||!std::isfinite(primitive.stroke_width)||!std::isfinite(primitive.radius))continue;
                    const bool line=primitive.kind==ChromePrimitiveKind::line;
                    if(!line&&primitive.radius<=0&&primitive.stroke_width<=0){paint.fill(primitive.rect,primitive.fill,primitive.background_replace);continue;}
                    const float pad=std::clamp(primitive.stroke_width,1.f,256.f)+1;
                    PixelRect bounds{std::min(primitive.rect.x,primitive.rect.x+primitive.rect.width)-pad,
                        std::min(primitive.rect.y,primitive.rect.y+primitive.rect.height)-pad,std::abs(primitive.rect.width)+2*pad,std::abs(primitive.rect.height)+2*pad};
                    const auto w=static_cast<std::uint32_t>(std::ceil(bounds.width)),h=static_cast<std::uint32_t>(std::ceil(bounds.height));
                    if(!w||!h||std::uint64_t(w)*h*4>4u*1024u*1024u||std::uint64_t(w)*h*4>textures.remaining_upload_bytes())continue;
                    Bitmap bitmap(w,h);auto ctx=bitmap.context.get();CGContextTranslateCTM(ctx,0,h);CGContextScaleCTM(ctx,1,-1);
                    auto fill=primitive.fill,stroke=primitive.stroke;
                    CGContextSetRGBFillColor(ctx,fill.r/255.,fill.g/255.,fill.b/255.,fill.a/255.);
                    if(line){if(primitive.stroke_width<=0)stroke=fill;CGContextMoveToPoint(ctx,primitive.rect.x-bounds.x,primitive.rect.y-bounds.y);
                        CGContextAddLineToPoint(ctx,primitive.rect.x+primitive.rect.width-bounds.x,primitive.rect.y+primitive.rect.height-bounds.y);
                    }else{CfOwner<CGPathRef> path(CGPathCreateWithRoundedRect(CGRectMake(pad,pad,std::max(0.f,primitive.rect.width),std::max(0.f,primitive.rect.height)),
                        std::max(0.f,primitive.radius),std::max(0.f,primitive.radius),nullptr));CGContextAddPath(ctx,path.get());}
                    CGContextSetRGBStrokeColor(ctx,stroke.r/255.,stroke.g/255.,stroke.b/255.,stroke.a/255.);CGContextSetLineWidth(ctx,std::max(1.f,primitive.stroke_width));
                    CGContextDrawPath(ctx,line?kCGPathStroke:primitive.stroke_width>0?kCGPathFillStroke:kCGPathFill);
                    paint.draw_bgra(w,h,bitmap.bytes,{bounds.x,bounds.y,float(w),float(h)},
                        {255,255,255,255},primitive.background_replace);
                } else {
                    // Chrome text is small and infrequent; row text is the bounded cache.
                    const auto& label=scene.texts[item.index];
                    if(!paint.clip(label.clip.value_or(PixelRect{0,0,float(width),float(height)})))continue;
                    if(!finite(label.rect)||label.text.empty()||!label.color.a||label.text.size()>65536||!std::isfinite(label.size_dip)||label.size_dip<1||label.size_dip>256)continue;
                    const auto w=static_cast<std::uint32_t>(std::max(0.f,std::ceil(label.rect.width))),h=static_cast<std::uint32_t>(std::max(0.f,std::ceil(label.rect.height)));
                    if(!w||!h||std::uint64_t(w)*h*4>4u*1024u*1024u||std::uint64_t(w)*h*4>textures.remaining_upload_bytes())continue;
                    Bitmap bitmap(w,h);const CGFloat size=label.size_dip*dpi/96.;
                    NSFont* label_font=label.family.empty()?nil:[NSFont fontWithName:string(label.family) size:size];
                    if(!label_font)label_font=[NSFont systemFontOfSize:size weight:label.semibold?NSFontWeightSemibold:NSFontWeightRegular];
                    else if(label.semibold)label_font=[[NSFontManager sharedFontManager] convertFont:label_font toHaveTrait:NSBoldFontMask];
                    auto fg=label.color;NSColor* color=[NSColor colorWithSRGBRed:fg.r/255. green:fg.g/255. blue:fg.b/255. alpha:1];
                    NSAttributedString* text=[[NSAttributedString alloc] initWithString:string(label.text) attributes:@{NSFontAttributeName:label_font,NSForegroundColorAttributeName:color}];
                    CfOwner<CTLineRef> line(CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)text));
                    const CGFloat flush=label.alignment==ChromeTextAlignment::center?.5:label.alignment==ChromeTextAlignment::trailing?1:0;
                    const double pen=std::max(0.,CTLineGetPenOffsetForFlush(line.get(),flush,label.rect.width));
                    CGContextSetTextPosition(bitmap.context.get(),pen,std::max(0.,(h-label_font.ascender+label_font.descender)/2.));
                    CTLineDraw(line.get(),bitmap.context.get());paint.draw_bgra(w,h,bitmap.bytes,label.rect,{255,255,255,fg.a});
                }
            }
}
} // namespace stormtty::test::chrome_reference
