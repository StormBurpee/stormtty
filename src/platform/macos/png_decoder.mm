#include "platform.hpp"
#include "cf_owner.hpp"
#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace stormtty {
namespace {
using macos::CfOwner;
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
// Validate the complete container without asking ImageIO to sniff another image
// format. Restrict metadata and reject APNG: terminal animation uses independent
// bounded frames, not an additional unaccounted ImageIO animation cache.
bool png_dimensions(std::span<const std::uint8_t> data, std::uint32_t& w, std::uint32_t& h) {
    constexpr std::array<std::uint8_t, 8> signature{137,80,78,71,13,10,26,10};
    if (data.size() < 45 || data.size() > 16u * 1024u * 1024u ||
        !std::equal(signature.begin(), signature.end(), data.begin()) ||
        be32(data.data() + 8) != 13 || std::memcmp(data.data() + 12, "IHDR", 4)) return false;
    w = be32(data.data() + 16); h = be32(data.data() + 20);
    if(data[24]!=1&&data[24]!=2&&data[24]!=4&&data[24]!=8)return false; // No 16-bit expansion.
    if (!w || !h || w > 8192 || h > 8192 || std::uint64_t(w) * h > 16u * 1024u * 1024u) return false;
    bool idat = false;
    for (std::size_t offset = 8; offset < data.size();) {
        if (data.size() - offset < 12) return false;
        const std::size_t length = be32(data.data() + offset);
        if (length > data.size() - offset - 12) return false;
        const auto* kind = data.data() + offset + 4;
        if(offset!=8&&!std::memcmp(kind,"IHDR",4))return false;
        if (!std::memcmp(kind,"acTL",4) || !std::memcmp(kind,"fcTL",4) || !std::memcmp(kind,"fdAT",4)) return false;
        // Compressed text/color profiles can expand independently of pixels.
        if (!std::memcmp(kind,"iCCP",4) || !std::memcmp(kind,"zTXt",4) || !std::memcmp(kind,"iTXt",4)) return false;
        if (!std::memcmp(kind,"IDAT",4)) idat = true;
        if (!std::memcmp(kind,"IEND",4)) return idat && !length && offset + 12 == data.size();
        offset += length + 12;
    }
    return false;
}
}
std::optional<DecodedPng> decode_macos_png(std::span<const std::uint8_t> bytes, std::size_t budget) {
    @autoreleasepool {
        try {
            std::uint32_t w{}, h{};
            if (!png_dimensions(bytes,w,h) || std::uint64_t(w)*h*4 > budget) return {};
            const auto count = std::size_t(w)*h*4;
            // Bound the RGBA destination to 64 MiB. ImageIO internal scratch is
            // not allocator-controlled; reject 16-bit/APNG/compressed metadata
            // expansion, and retain no decoded ImageIO cache. TerminalCore
            // independently caps retained output. This is not a hard RSS cap.
            if (count > 64u*1024u*1024u) return {};
            CfOwner<CFDataRef> data(CFDataCreate(kCFAllocatorDefault,bytes.data(),static_cast<CFIndex>(bytes.size())));
            NSDictionary* options = @{(__bridge NSString*)kCGImageSourceShouldCache:@NO,
                                      (__bridge NSString*)kCGImageSourceShouldAllowFloat:@NO};
            if(!data)return {};
            CfOwner<CGImageSourceRef> source(CGImageSourceCreateWithData(data.get(),(__bridge CFDictionaryRef)options));
            if (!source || CGImageSourceGetCount(source.get()) != 1 ||
                !CGImageSourceGetType(source.get()) || !CFEqual(CGImageSourceGetType(source.get()),CFSTR("public.png"))) return {};
            CfOwner<CFDictionaryRef> properties(CGImageSourceCopyPropertiesAtIndex(source.get(),0,(__bridge CFDictionaryRef)options));
            if(!properties)return {};
            auto properties_dict=(__bridge NSDictionary*)properties.get();
            NSNumber* reported_width=properties_dict[(__bridge NSString*)kCGImagePropertyPixelWidth];
            NSNumber* reported_height=properties_dict[(__bridge NSString*)kCGImagePropertyPixelHeight];
            if(![reported_width isKindOfClass:NSNumber.class]||![reported_height isKindOfClass:NSNumber.class]||reported_width.unsignedLongLongValue!=w||reported_height.unsignedLongLongValue!=h)return {};
            CfOwner<CGImageRef> image(CGImageSourceCreateImageAtIndex(source.get(),0,(__bridge CFDictionaryRef)options));
            if (!image || CGImageGetWidth(image.get()) != w || CGImageGetHeight(image.get()) != h) return {};
            DecodedPng result{w,h,std::vector<std::uint8_t>(count)};
            CfOwner<CGColorSpaceRef> colors(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
            CfOwner<CGContextRef> context(CGBitmapContextCreate(result.rgba.data(),w,h,8,std::size_t(w)*4,colors.get(),
                kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big));
            if (!context) return {};
            CGContextSetBlendMode(context.get(),kCGBlendModeCopy);
            CGContextDrawImage(context.get(),CGRectMake(0,0,w,h),image.get());
            // The adapter contract is straight alpha; Core Graphics rasterizes
            // premultiplied bytes. Transparent RGB is deliberately normalized.
            for (std::size_t i=0;i<count;i+=4) {
                const unsigned alpha=result.rgba[i+3];
                for (unsigned c=0;c<3;++c) result.rgba[i+c]=alpha ?
                    static_cast<std::uint8_t>(std::min(255u,(unsigned(result.rgba[i+c])*255u+alpha/2)/alpha)) : 0;
            }
            return result;
        } catch (...) { return {}; } // Callback boundary must not throw.
    }
}
void install_macos_png_decoder() { install_png_decoder(decode_macos_png,64u*1024u*1024u,8192); }
void write_macos_capture(const std::filesystem::path& path, std::span<const std::uint8_t> rgba,
                         std::uint32_t width, std::uint32_t height) {
    if (!width || !height || std::uint64_t(width)*height*4 != rgba.size())
        throw std::invalid_argument("Invalid capture dimensions");
    // Caller explicitly requested this path in --smoke mode. No user desktop,
    // screen recording or unrelated window contents are captured.
    const auto filename=path.string();
    CfOwner<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(filename.data()),static_cast<CFIndex>(filename.size()),false));
    CfOwner<CGDataProviderRef> provider(CGDataProviderCreateWithData(nullptr,rgba.data(),rgba.size(),nullptr));
    CfOwner<CGColorSpaceRef> colors(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    CfOwner<CGImageRef> image(CGImageCreate(width,height,8,32,std::size_t(width)*4,colors.get(),
        kCGImageAlphaLast | kCGBitmapByteOrder32Big,provider.get(),nullptr,false,kCGRenderingIntentDefault));
    CfOwner<CGImageDestinationRef> destination(CGImageDestinationCreateWithURL(url.get(),CFSTR("public.png"),1,nullptr));
    if (!image || !destination) throw std::runtime_error("Cannot create smoke PNG capture");
    CGImageDestinationAddImage(destination.get(),image.get(),nullptr);
    if (!CGImageDestinationFinalize(destination.get())) throw std::runtime_error("Cannot write smoke PNG capture");
}
}
