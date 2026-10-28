#include "stormtty/renderer.hpp"
#include "cf_owner.hpp"
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <mutex>
#include <map>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <tuple>
#include <utility>

namespace stormtty {
namespace {
using macos::CfOwner;
constexpr std::size_t max_surface_bytes=128u*1024u*1024u;
constexpr std::size_t max_transient_bytes=64u*1024u*1024u;
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
struct alignas(16) Quad { float destination[4],uv[4],viewport[2]; };
static_assert(sizeof(Quad)==48,"Metal constant-buffer ABI requires 16-byte rounded alignment");
struct Tint { float value[4]; };
constexpr const char* shader_source=R"metal(
#include <metal_stdlib>
using namespace metal;
struct Quad { float4 destination; float4 uv; float2 viewport; };
struct Vertex { float4 position [[position]]; float2 uv; };
vertex Vertex quad_vertex(uint n [[vertex_id]],constant Quad& q [[buffer(0)]]) {
    const float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    float2 p=q.destination.xy+corners[n]*q.destination.zw;
    Vertex v;v.position=float4(p.x/q.viewport.x*2-1,1-p.y/q.viewport.y*2,0,1);
    v.uv=q.uv.xy+corners[n]*q.uv.zw;return v;
}
fragment float4 quad_fragment(Vertex v [[stage_in]],texture2d<float> t [[texture(0)]],
    constant float4& tint [[buffer(0)]]) {
    constexpr sampler s(coord::normalized,filter::nearest,address::clamp_to_edge);
    return t.sample(s,v.uv)*tint;
}
fragment float4 quad_replace_fragment(Vertex v [[stage_in]],texture2d<float> t [[texture(0)]],
    constant float4& tint [[buffer(0)]]) {
    constexpr sampler s(coord::normalized,filter::nearest,address::clamp_to_edge);
    const float4 pixel=t.sample(s,v.uv);
    // Bounded CoreGraphics overlays include transparent padding. COPY applies
    // to their covered shape, not its otherwise-transparent bounding rectangle.
    if(pixel.a==0)discard_fragment();
    return pixel*tint;
}
)metal";
struct RowKey {
    std::uint64_t pane{},id0{},id1{},generation{},font{};
    std::uint32_t width{},height{},variant{};bool blink{};
    auto operator<=>(const RowKey&) const = default;
};
struct CachedRow { id<MTLTexture> texture;std::size_t bytes{};std::uint64_t used{}; };
struct ImageKey {
    std::uint64_t pane{},generation{};std::uint32_t id{},width{},height{};
    auto operator<=>(const ImageKey&) const = default;
};
struct CachedImage { id<MTLTexture> texture;std::size_t bytes{};std::uint64_t used{}; };
struct Presentation {
    std::mutex mutex;std::condition_variable changed;
    std::uint64_t serial{},submitted{},scheduled{},completed{},callbacks{},zero_time_callbacks{};
    double last_time{};bool gpu_error{};std::string surface;
};
struct PaneStamp {
    std::uint64_t id{},generation{},images{};PixelRect content;bool focused{},cursor{},blink{};
    bool operator==(const PaneStamp&) const = default;
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
}
struct Renderer::Impl {
    __weak NSView* view;
    CAMetalLayer* layer;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> pipeline,replace_pipeline;
    id<MTLTexture> target,white;
    RendererOptions options;
    RendererStats statistics;
    CellMetrics cell;
    CfOwner<CTFontRef> font;
    std::uint32_t width{},height{};float dpi{96};
    std::uint64_t font_generation{1},serial{},chrome_generation{};
    std::vector<PaneStamp> previous;
    Color previous_background{};bool have_previous{};
    std::map<RowKey,CachedRow> rows;
    std::map<ImageKey,CachedImage> images;
    std::shared_ptr<std::atomic<unsigned>> in_flight=std::make_shared<std::atomic<unsigned>>(0);
    std::shared_ptr<std::atomic<bool>> gpu_failed=std::make_shared<std::atomic<bool>>(false);
    std::size_t transient{};std::shared_ptr<Presentation> presented=std::make_shared<Presentation>();
    explicit Impl(void* handle,RendererOptions settings):view((__bridge NSView*)handle),options(std::move(settings)) {
        if(options.force_warp) throw std::invalid_argument("WARP is Windows-only; macOS native tests require a Metal device");
        options.maximum_image_dimension=std::min(options.maximum_image_dimension,8192u);
        if(!std::isfinite(options.background_opacity)||options.background_opacity<.7f||options.background_opacity>1.f)throw std::invalid_argument("Invalid default background opacity");
        options.shape_cache_bytes=std::min(options.shape_cache_bytes,std::size_t(64u*1024u*1024u));
        options.image_cache_bytes=std::min(options.image_cache_bytes,std::size_t(128u*1024u*1024u));
        create_device();measure();
    }
    void create_device() {
        transient=0;in_flight=std::make_shared<std::atomic<unsigned>>(0);gpu_failed=std::make_shared<std::atomic<bool>>(false);presented=std::make_shared<Presentation>();device=MTLCreateSystemDefaultDevice();check(device!=nil,"A supported Metal device is required");
        queue=[device newCommandQueue];check(queue!=nil,"Cannot create Metal command queue");
        NSError* error=nil;
        id<MTLLibrary> library=[device newLibraryWithSource:string(shader_source) options:nil error:&error];
        if(!library)throw std::runtime_error("Metal shader compile: "+std::string(error.localizedDescription.UTF8String ?: "unknown error"));
        MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction=[library newFunctionWithName:@"quad_vertex"];
        descriptor.fragmentFunction=[library newFunctionWithName:@"quad_fragment"];
        descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
        auto attachment=descriptor.colorAttachments[0];attachment.blendingEnabled=YES;
        attachment.sourceRGBBlendFactor=MTLBlendFactorOne;attachment.destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        attachment.sourceAlphaBlendFactor=MTLBlendFactorOne;attachment.destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        check(pipeline!=nil,"Cannot create Metal composition pipeline");
        attachment.blendingEnabled=NO;
        descriptor.fragmentFunction=[library newFunctionWithName:@"quad_replace_fragment"];
        replace_pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        check(replace_pipeline!=nil,"Cannot create Metal replacement pipeline");
        const std::uint8_t pixel[4]={255,255,255,255};white=texture(1,1,pixel);
        if(view) {
            layer=[CAMetalLayer layer];layer.device=device;layer.pixelFormat=MTLPixelFormatBGRA8Unorm;
            layer.framebufferOnly=NO;layer.opaque=NO;layer.maximumDrawableCount=3;
            layer.allowsNextDrawableTimeout=YES;layer.displaySyncEnabled=YES;
            CfOwner<CGColorSpaceRef> color(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));layer.colorspace=color.get();
            // Layer-hosting NSViews do not promise AppKit-managed layer geometry.
            // A valid drawable with a zero-size/unattached layer can execute on
            // the GPU but never acquire an onscreen presentedTime.
            layer.frame=view.bounds;
            view.layer=layer;view.wantsLayer=YES;
        }
        statistics.composition=true;statistics.color_fonts=true;
        rows.clear();images.clear();statistics.shape_cache_bytes=statistics.image_cache_bytes=0;
        target=nil;have_previous=false;gpu_failed->store(false);
    }
    id<MTLTexture> texture(std::uint32_t w,std::uint32_t h,const void* bytes) {
        const auto allocation=std::uint64_t(w)*h*4;
        if(allocation>max_transient_bytes-transient)throw std::runtime_error("Metal per-frame upload budget exceeded");
        transient+=static_cast<std::size_t>(allocation);
        check(w&&h&&w<=16384&&h<=16384,"Metal texture dimensions exceed supported limits");
        MTLTextureDescriptor* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        desc.storageMode=[device supportsFamily:MTLGPUFamilyApple1]?MTLStorageModeShared:MTLStorageModeManaged;desc.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> result=[device newTextureWithDescriptor:desc];check(result!=nil,"Cannot allocate Metal texture");
        [result replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:bytes bytesPerRow:std::size_t(w)*4];return result;
    }
    void measure() {
        if(options.font.family.empty()||options.font.family.size()>256||options.font.locale.size()>128||!std::isfinite(options.font.size_dip)||options.font.size_dip<6||options.font.size_dip>72)throw std::invalid_argument("Invalid CoreText font options");
        const float px=options.font.size_dip*dpi/96.f;
        font=CfOwner<CTFontRef>(CTFontCreateWithName((__bridge CFStringRef)string(options.font.family),px,nullptr));
        check(bool(font),"Cannot create terminal CoreText font");
        UniChar letter='M';CGGlyph glyph{};CGSize advance{};
        CTFontGetGlyphsForCharacters(font.get(),&letter,&glyph,1);CTFontGetAdvancesForGlyphs(font.get(),kCTFontOrientationHorizontal,&glyph,&advance,1);
        cell.width=static_cast<std::uint16_t>(std::clamp(std::ceil(advance.width),1.,512.));
        cell.height=static_cast<std::uint16_t>(std::clamp(std::ceil(CTFontGetAscent(font.get())+CTFontGetDescent(font.get())+CTFontGetLeading(font.get())+2*dpi/96.),1.,1024.));
        cell.baseline=static_cast<float>(CTFontGetAscent(font.get())+dpi/96.);
        ++font_generation;rows.clear();statistics.shape_cache_bytes=0;have_previous=false;
    }
    void resize(std::uint32_t w,std::uint32_t h,float new_dpi) {
        if(!std::isfinite(new_dpi)||new_dpi<48||new_dpi>768)throw std::invalid_argument("Invalid renderer DPI");
        if(w>16384||h>16384||std::uint64_t(w)*h*4>max_surface_bytes)throw std::invalid_argument("Metal window surface exceeds 128 MiB budget");
        if(new_dpi!=dpi){dpi=new_dpi;measure();}
        if(layer){layer.contentsScale=dpi/96.;if(view)layer.frame=view.bounds;}
        if(width==w&&height==h&&target)return;
        width=w;height=h;target=nil;have_previous=false;
        if(layer){layer.contentsScale=dpi/96.;layer.drawableSize=CGSizeMake(w,h);}
        if(!w||!h)return;
        auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        desc.storageMode=MTLStorageModePrivate;desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        target=[device newTextureWithDescriptor:desc];check(target!=nil,"Cannot allocate Metal composition surface");
    }
    void draw(id<MTLRenderCommandEncoder> encoder,id<MTLTexture> image,PixelRect rect,
              Color tint={255,255,255,255},bool replace=false,PixelRect uv={0,0,1,1}) {
        if(!finite(rect)||!finite(uv)||rect.width<=0||rect.height<=0)return;
        Quad quad{{rect.x,rect.y,rect.width,rect.height},{uv.x,uv.y,uv.width,uv.height},{float(width),float(height)}};
        const float a=tint.a/255.f;Tint color{{tint.r/255.f*a,tint.g/255.f*a,tint.b/255.f*a,a}};
        [encoder setRenderPipelineState:replace?replace_pipeline:pipeline];
        [encoder setVertexBytes:&quad length:sizeof(quad) atIndex:0];
        [encoder setFragmentBytes:&color length:sizeof(color) atIndex:0];
        [encoder setFragmentTexture:image atIndex:0];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    }
    bool clip(id<MTLRenderCommandEncoder> encoder,PixelRect r) {
        if(!finite(r))return false;
        const auto x=std::clamp(std::floor(r.x),0.f,float(width));
        const auto y=std::clamp(std::floor(r.y),0.f,float(height));
        const auto right=std::clamp(std::ceil(r.x+r.width),0.f,float(width));
        const auto bottom=std::clamp(std::ceil(r.y+r.height),0.f,float(height));
        if(right<=x||bottom<=y)return false;
        [encoder setScissorRect:MTLScissorRect{std::size_t(x),std::size_t(y),std::size_t(right-x),std::size_t(bottom-y)}];return true;
    }
    void trim_rows(std::size_t incoming) {
        while(!rows.empty()&&(statistics.shape_cache_bytes+incoming>options.shape_cache_bytes||rows.size()>=16384)) {
            auto oldest=std::min_element(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
            statistics.shape_cache_bytes-=oldest->second.bytes;rows.erase(oldest);
        }
    }
    id<MTLTexture> raster_row(const RenderPane& pane,const FrameRow& row,std::uint32_t w,std::uint32_t h,std::uint32_t variant=0) {
        const bool blink=std::any_of(row.cells.begin(),row.cells.end(),[](const Cell& c){return c.style.blink;});
        const RowKey key{pane.id,row.id.words[0],row.id.words[1],row.generation,font_generation,w,h,variant,!blink||pane.text_blink_on};
        if(auto found=rows.find(key);found!=rows.end()){found->second.used=serial;++statistics.shape_cache_hits;return found->second.texture;}
        const auto bytes=std::size_t(w)*h*4;
        if(bytes>max_transient_bytes-transient)throw std::runtime_error("Visible CoreText rows exceed transient texture budget");
        Bitmap bitmap(w,h);auto ctx=bitmap.context.get();
        const float cw=pane.frame->cell_width?pane.frame->cell_width:cell.width;
        const float base=std::min(float(h)-1,cell.baseline);
        NSMutableAttributedString* text=[[NSMutableAttributedString alloc] init];
        struct Source { CFIndex begin{},end{};std::size_t column{},span{}; };
        std::vector<Source> sources;std::vector<std::size_t> utf16_source;std::size_t text_bytes=0;
        if(row.cells.size()>4096)throw std::runtime_error("CoreText row exceeds the 4096-cell shaping budget");
        for(std::size_t column=0;column<row.cells.size();++column) {
            const auto& c=row.cells[column];if(!c.span)continue;
            const bool hidden=c.grapheme.empty()||c.image_placeholder||c.style.invisible||(c.style.blink&&!pane.text_blink_on);
            // The adapter retains the complete VT text. Rendering substitutes an
            // over-budget grapheme rather than allocating unbounded CoreText work.
            const bool oversized=c.grapheme.size()>4096||c.grapheme.size()>65536-text_bytes-3*(row.cells.size()-column);
            NSString* glyph=hidden?@" ":oversized?@"\uFFFD":string(c.grapheme);
            text_bytes+=hidden?1:oversized?3:c.grapheme.size();
            const auto start=static_cast<CFIndex>(text.length);
            CTFontSymbolicTraits traits=0;if(c.style.bold)traits|=kCTFontBoldTrait;if(c.style.italic)traits|=kCTFontItalicTrait;
            CfOwner<CTFontRef> variant(traits?CTFontCreateCopyWithSymbolicTraits(font.get(),0,nullptr,traits,traits):nullptr);
            const auto fg=foreground(c);
            NSColor* color=[NSColor colorWithSRGBRed:fg.r/255. green:fg.g/255. blue:fg.b/255. alpha:1];
            NSDictionary* attrs=@{(__bridge NSString*)kCTFontAttributeName:(__bridge id)(variant?variant.get():font.get()),
                (__bridge NSString*)kCTForegroundColorAttributeName:(__bridge id)color.CGColor,
                (__bridge NSString*)kCTLigatureAttributeName:@(options.font.ligatures?1:0),
                (__bridge NSString*)kCTLanguageAttributeName:string(options.font.locale)};
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
        for(std::size_t col=0;col<row.cells.size();++col) {
            const auto& c=row.cells[col];if(!c.span||c.style.invisible||(c.style.blink&&!pane.text_blink_on))continue;
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
        id<MTLTexture> result=texture(w,h,bitmap.bytes.data());++statistics.shaped_rows;++statistics.dirty_rectangles;
        trim_rows(bytes);
        if(bytes<=options.shape_cache_bytes){rows.emplace(key,CachedRow{result,bytes,serial});statistics.shape_cache_bytes+=bytes;}
        
        return result;
    }
    id<MTLTexture> image_texture(std::uint64_t pane,const ImagePixels& image) {
        const auto bytes=std::size_t(image.width)*image.height*4;
        if(!image.width||!image.height||image.width>options.maximum_image_dimension||image.height>options.maximum_image_dimension||
            bytes>options.image_cache_bytes||bytes>max_transient_bytes)return nil;
        ImageKey key{pane,image.generation,image.image_id,image.width,image.height};
        if(auto found=images.find(key);found!=images.end()){found->second.used=serial;return found->second.texture;}
        const unsigned channels=image.format==PixelFormat::rgba?4:image.format==PixelFormat::rgb?3:image.format==PixelFormat::gray_alpha?2:1;
        if(std::uint64_t(image.width)*image.height*channels!=image.bytes.size())return nil;
        if(bytes>max_transient_bytes-transient)throw std::runtime_error("Image upload exceeds the remaining frame budget");
        std::vector<std::uint8_t> bgra(bytes);
        for(std::size_t p=0;p<std::size_t(image.width)*image.height;++p) {
            const auto* source=image.bytes.data()+p*channels;const unsigned a=channels==4?source[3]:channels==2?source[1]:255;
            const unsigned r=source[0],g=channels>=3?source[1]:r,b=channels>=3?source[2]:r;
            bgra[p*4]=static_cast<std::uint8_t>((b*a+127)/255);bgra[p*4+1]=static_cast<std::uint8_t>((g*a+127)/255);
            bgra[p*4+2]=static_cast<std::uint8_t>((r*a+127)/255);bgra[p*4+3]=static_cast<std::uint8_t>(a);
        }
        while(!images.empty()&&(statistics.image_cache_bytes+bytes>options.image_cache_bytes||images.size()>=1024)){
            auto oldest=std::min_element(images.begin(),images.end(),[](const auto&a,const auto&b){return a.second.used<b.second.used;});
            statistics.image_cache_bytes-=oldest->second.bytes;images.erase(oldest);
        }
        auto result=texture(image.width,image.height,bgra.data());images.emplace(key,CachedImage{result,bytes,serial});
        statistics.image_cache_bytes+=bytes;++statistics.image_uploads;return result;
    }
    void draw_images(id<MTLRenderCommandEncoder> encoder,const RenderPane& pane,ImageLayer which) {
        const auto& f=*pane.frame;const float cw=f.cell_width?f.cell_width:cell.width,ch=f.cell_height?f.cell_height:cell.height;
        for(const auto& placement:f.images) {
            if(placement.layer!=which||!placement.pixels)continue;
            const auto& image=*placement.pixels;
            const auto sw=placement.source_width?placement.source_width:image.width;
            const auto sh=placement.source_height?placement.source_height:image.height;
            if(placement.source_x>=image.width||placement.source_y>=image.height||sw>image.width-placement.source_x||sh>image.height-placement.source_y)continue;
            const float w=placement.pixel_width?placement.pixel_width:placement.columns?placement.columns*cw:sw;
            const float h=placement.pixel_height?placement.pixel_height:placement.rows?placement.rows*ch:sh;
            const PixelRect destination{pane.content.x+placement.column*cw+placement.offset_x,pane.content.y+placement.row*ch+placement.offset_y,w,h};
            if(destination.x+destination.width<=pane.content.x||destination.y+destination.height<=pane.content.y||destination.x>=pane.content.x+pane.content.width||destination.y>=pane.content.y+pane.content.height)continue;
            auto image_tex=image_texture(pane.id,image);if(!image_tex)continue;
            draw(encoder,image_tex,destination,
                {255,255,255,255},false,{float(placement.source_x)/image.width,float(placement.source_y)/image.height,float(sw)/image.width,float(sh)/image.height});
        }
    }
    RenderResult render(const WindowScene& scene) {
        if(!width||!height||!target)return RenderResult::suspended;
        if(scene.panes.size()>4096||scene.primitives.size()>16384||scene.texts.size()>4096)throw std::runtime_error("Metal scene exceeds bounded primitive counts");
        if(gpu_failed->load())throw std::runtime_error("Metal command buffer failed; renderer reconstruction is required");
        if(view&&(!view.window.visible||(view.window.occlusionState&NSWindowOcclusionStateVisible)==0))return RenderResult::occluded;
        std::vector<PaneStamp> stamps;
        for(const auto& p:scene.panes)if(p.frame)stamps.push_back({p.id,p.frame->generation,p.frame->image_placements_generation,p.content,p.focused,p.cursor_on,p.text_blink_on});
        if(have_previous&&!scene.force_redraw&&chrome_generation==scene.chrome_generation&&stamps==previous&&scene.background==previous_background)return RenderResult::unchanged;
        if(in_flight->load()>=2)return RenderResult::busy;
        @autoreleasepool {
            // Do not retain CAMetalDrawable across frames. Apple may briefly wait
            // inside nextDrawable; two in-flight buffers plus a three-drawable pool
            // avoid saturating it. This is not a proven nonblocking present path.
            id<CAMetalDrawable> drawable=layer?[layer nextDrawable]:nil;
            if(layer&&!drawable)return RenderResult::busy;
            ++serial;transient=0;
            if(layer){std::ostringstream details;const auto frame=layer.frame,bounds=layer.bounds;
                details<<"attached="<<(layer.superlayer!=nil)<<" hidden="<<bool(layer.hidden)
                    <<" opacity="<<layer.opacity<<" frame="<<frame.origin.x<<','<<frame.origin.y<<','<<frame.size.width<<','<<frame.size.height
                    <<" bounds="<<bounds.origin.x<<','<<bounds.origin.y<<','<<bounds.size.width<<','<<bounds.size.height
                    <<" drawable="<<layer.drawableSize.width<<'x'<<layer.drawableSize.height<<" scale="<<layer.contentsScale;
                std::lock_guard lock(presented->mutex);presented->submitted=serial;presented->surface=details.str();}
            id<MTLCommandBuffer> command=[queue commandBuffer];check(command!=nil,"Cannot create Metal command buffer");
            auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=target;
            pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
            pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);
            id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];check(encoder!=nil,"Cannot create Metal render encoder");
            Color canvas=scene.background;canvas.a=static_cast<std::uint8_t>(std::lround(options.background_opacity*255));
            draw(encoder,white,{0,0,float(width),float(height)},canvas);
            for(const auto& pane:scene.panes) {
                if(!pane.frame||!clip(encoder,pane.content))continue;
                const auto& frame=*pane.frame;
                if(frame.lines.size()>4096||frame.images.size()>1024)throw std::runtime_error("Terminal frame exceeds renderer row/placement limits");
                const float cw=frame.cell_width?frame.cell_width:cell.width,ch=frame.cell_height?frame.cell_height:cell.height;
                // Reset the pane first; source-overing translucent backgrounds on
                // a previous/global background would accumulate opacity.
                draw(encoder,white,pane.content,{0,0,0,0},true);
                Color background=frame.background;background.a=static_cast<std::uint8_t>(std::lround(options.background_opacity*255));
                draw(encoder,white,pane.content,background);
                draw_images(encoder,pane,ImageLayer::below_background);
                for(std::size_t r=0;r<frame.lines.size();++r)if(frame.lines[r]) {
                    const auto& row=*frame.lines[r];
                    for(std::size_t c=0;c<row.cells.size();++c) {
                        const auto& cell_value=row.cells[c];Color bg=cell_value.style.inverse?cell_value.foreground:cell_value.background;
                        if(cell_value.selected)bg={58,79,91,255};
                        if(!cell_value.background_is_default||cell_value.style.inverse||cell_value.selected){bg.a=255;draw(encoder,white,{pane.content.x+c*cw,pane.content.y+r*ch,cw,ch},bg);}
                    }
                }
                draw_images(encoder,pane,ImageLayer::below_text);
                for(std::size_t r=0;r<frame.lines.size();++r)if(frame.lines[r]&&pane.content.y+r*ch<pane.content.y+pane.content.height) {
                    const auto rw=static_cast<std::uint32_t>(std::ceil(frame.columns*cw));const auto rh=static_cast<std::uint32_t>(std::ceil(ch));
                    if(rw&&rh)draw(encoder,raster_row(pane,*frame.lines[r],rw,rh),{pane.content.x,pane.content.y+r*ch,float(rw),float(rh)});
                }
                draw_images(encoder,pane,ImageLayer::above_text);
                const auto& cursor=frame.cursor;
                if(cursor.visible&&pane.cursor_on&&cursor.column<frame.columns&&cursor.row<frame.rows) {
                    auto column=cursor.column;
                    const FrameRow* cursor_row=cursor.row<frame.lines.size()&&frame.lines[cursor.row]?frame.lines[cursor.row].get():nullptr;
                    if(column&&cursor_row&&column<cursor_row->cells.size()&&(!cursor_row->cells[column].span||cursor.wide_tail))--column;
                    const unsigned span=cursor_row&&column<cursor_row->cells.size()?std::max(1u,unsigned(cursor_row->cells[column].span)):1;
                    PixelRect rect{pane.content.x+column*cw,pane.content.y+cursor.row*ch,cw*span,ch};Color color=cursor.color;color.a=255;
                    const float line=std::max(1.f,dpi/96.f);
                    if(cursor.shape==CursorShape::bar)rect.width=line;
                    else if(cursor.shape==CursorShape::underline){rect.y+=rect.height-line;rect.height=line;}
                    if(!pane.focused||cursor.shape==CursorShape::hollow_block){
                        draw(encoder,white,{rect.x,rect.y,rect.width,line},color);draw(encoder,white,{rect.x,rect.y+rect.height-line,rect.width,line},color);
                        draw(encoder,white,{rect.x,rect.y,line,rect.height},color);draw(encoder,white,{rect.x+rect.width-line,rect.y,line,rect.height},color);
                    }else{
                        draw(encoder,white,rect,color);
                        if(cursor.shape==CursorShape::block&&cursor_row&&column<cursor_row->cells.size()){
                            FrameRow inverted=*cursor_row;auto& c=inverted.cells[column];
                            c.foreground=c.style.inverse?c.foreground:c.background;c.style.inverse=false;c.selected=false;c.style.blink=false;
                            RenderPane cursor_pane=pane;cursor_pane.text_blink_on=true;
                            const auto right=std::min(rect.x+rect.width,pane.content.x+pane.content.width),bottom=std::min(rect.y+rect.height,pane.content.y+pane.content.height);
                            PixelRect clipped{std::max(rect.x,pane.content.x),std::max(rect.y,pane.content.y),0,0};clipped.width=right-clipped.x;clipped.height=bottom-clipped.y;
                            if(clip(encoder,clipped))draw(encoder,raster_row(cursor_pane,inverted,static_cast<std::uint32_t>(std::ceil(frame.columns*cw)),static_cast<std::uint32_t>(std::ceil(ch)),column+1),
                                {pane.content.x,pane.content.y+cursor.row*ch,frame.columns*cw,ch});
                        }
                    }
                }
            }
            clip(encoder,{0,0,float(width),float(height)});
            for(const auto& primitive:scene.primitives) {
                if(!finite(primitive.rect)||!std::isfinite(primitive.stroke_width)||!std::isfinite(primitive.radius))continue;
                const bool line=primitive.kind==ChromePrimitiveKind::line;
                if(!line&&primitive.radius<=0&&primitive.stroke_width<=0){draw(encoder,white,primitive.rect,primitive.fill,primitive.background_replace);continue;}
                const float pad=std::clamp(primitive.stroke_width,1.f,256.f)+1;
                PixelRect bounds{std::min(primitive.rect.x,primitive.rect.x+primitive.rect.width)-pad,
                    std::min(primitive.rect.y,primitive.rect.y+primitive.rect.height)-pad,std::abs(primitive.rect.width)+2*pad,std::abs(primitive.rect.height)+2*pad};
                const auto w=static_cast<std::uint32_t>(std::ceil(bounds.width)),h=static_cast<std::uint32_t>(std::ceil(bounds.height));
                if(!w||!h||std::uint64_t(w)*h*4>4u*1024u*1024u||std::uint64_t(w)*h*4>max_transient_bytes-transient)continue;
                Bitmap bitmap(w,h);auto ctx=bitmap.context.get();CGContextTranslateCTM(ctx,0,h);CGContextScaleCTM(ctx,1,-1);
                auto fill=primitive.fill,stroke=primitive.stroke;
                CGContextSetRGBFillColor(ctx,fill.r/255.,fill.g/255.,fill.b/255.,fill.a/255.);
                if(line){if(primitive.stroke_width<=0)stroke=fill;CGContextMoveToPoint(ctx,primitive.rect.x-bounds.x,primitive.rect.y-bounds.y);
                    CGContextAddLineToPoint(ctx,primitive.rect.x+primitive.rect.width-bounds.x,primitive.rect.y+primitive.rect.height-bounds.y);
                }else{CfOwner<CGPathRef> path(CGPathCreateWithRoundedRect(CGRectMake(pad,pad,std::max(0.f,primitive.rect.width),std::max(0.f,primitive.rect.height)),
                    std::max(0.f,primitive.radius),std::max(0.f,primitive.radius),nullptr));CGContextAddPath(ctx,path.get());}
                CGContextSetRGBStrokeColor(ctx,stroke.r/255.,stroke.g/255.,stroke.b/255.,stroke.a/255.);CGContextSetLineWidth(ctx,std::max(1.f,primitive.stroke_width));
                CGContextDrawPath(ctx,line?kCGPathStroke:primitive.stroke_width>0?kCGPathFillStroke:kCGPathFill);
                draw(encoder,texture(w,h,bitmap.bytes.data()),{bounds.x,bounds.y,float(w),float(h)},
                    {255,255,255,255},primitive.background_replace);
            }
            // Chrome text is small and infrequent; row text is the bounded cache.
            for(const auto& label:scene.texts) {
                if(!finite(label.rect)||label.text.size()>65536||!std::isfinite(label.size_dip)||label.size_dip<1||label.size_dip>256)continue;
                const auto w=static_cast<std::uint32_t>(std::max(0.f,std::ceil(label.rect.width))),h=static_cast<std::uint32_t>(std::max(0.f,std::ceil(label.rect.height)));
                if(!w||!h||std::uint64_t(w)*h*4>4u*1024u*1024u||std::uint64_t(w)*h*4>max_transient_bytes-transient)continue;
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
                CTLineDraw(line.get(),bitmap.context.get());draw(encoder,texture(w,h,bitmap.bytes.data()),label.rect);
            }
            [encoder endEncoding];
            if(drawable){auto presentation=presented;const auto frame_serial=serial;
                [drawable addPresentedHandler:^(id<MTLDrawable> shown){
                    {std::lock_guard lock(presentation->mutex);++presentation->callbacks;presentation->last_time=shown.presentedTime;
                        if(shown.presentedTime>0)presentation->serial=std::max(presentation->serial,frame_serial);
                        else ++presentation->zero_time_callbacks;}
                    presentation->changed.notify_all();
                }];
                [command addScheduledHandler:^(id<MTLCommandBuffer> scheduled){(void)scheduled;std::lock_guard lock(presentation->mutex);presentation->scheduled=std::max(presentation->scheduled,frame_serial);}];
                id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
                [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1)
                    toTexture:drawable.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];[command presentDrawable:drawable];}
            auto count=in_flight;auto failure=gpu_failed;auto presentation=presented;const auto frame_serial=serial;count->fetch_add(1);
            [command addCompletedHandler:^(id<MTLCommandBuffer> buffer){
                const bool failed=buffer.status==MTLCommandBufferStatusError;if(failed)failure->store(true);count->fetch_sub(1);
                {std::lock_guard lock(presentation->mutex);presentation->completed=std::max(presentation->completed,frame_serial);presentation->gpu_error|=failed;}
                presentation->changed.notify_all();
            }];
            [command commit];++statistics.presents;previous=std::move(stamps);chrome_generation=scene.chrome_generation;previous_background=scene.background;have_previous=true;
            return RenderResult::presented;
        }
    }
};
Renderer::Renderer(void* handle,RendererOptions options):impl_(std::make_unique<Impl>(handle,std::move(options))) {}
Renderer::~Renderer()=default;
Renderer::Renderer(Renderer&&) noexcept=default;
Renderer& Renderer::operator=(Renderer&&) noexcept=default;
void Renderer::resize(std::uint32_t w,std::uint32_t h,float dpi){impl_->resize(w,h,dpi);}
void Renderer::set_font(FontOptions font){impl_->options.font=std::move(font);impl_->measure();}
void Renderer::set_background_opacity(float opacity){
    if(!std::isfinite(opacity)||opacity<0.7f||opacity>1.f)throw std::invalid_argument("Background opacity must be between 0.7 and 1");
    impl_->options.background_opacity=opacity;impl_->have_previous=false;
}
CellMetrics Renderer::metrics()const noexcept{return impl_->cell;}
RenderResult Renderer::render(const WindowScene& scene){return impl_->render(scene);}
void* Renderer::frame_latency_wait_handle()const noexcept{return nullptr;}
RendererStats Renderer::stats()const noexcept{return impl_->statistics;}
// The alpha diagnostic currently targets the Windows DComp investigation.
// Explicitly unavailable rather than blocking AppKit or pretending a sample.
bool Renderer::begin_alpha_probe(PixelRect) { return false; }
std::optional<RendererAlphaProbe> Renderer::poll_alpha_probe() { return std::nullopt; }
void Renderer::cancel_alpha_probe() noexcept {}
std::vector<std::uint8_t> Renderer::read_pixels(){
    if(!impl_->target)return {};
    const auto stride=(std::size_t(impl_->width)*4+255)&~std::size_t(255);
    const bool shared=[impl_->device supportsFamily:MTLGPUFamilyApple1];
    auto buffer=[impl_->device newBufferWithLength:stride*impl_->height options:shared?MTLResourceStorageModeShared:MTLResourceStorageModeManaged];check(buffer!=nil,"Cannot allocate Metal readback buffer");
    auto command=[impl_->queue commandBuffer];auto blit=[command blitCommandEncoder];
    [blit copyFromTexture:impl_->target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(impl_->width,impl_->height,1)
        toBuffer:buffer destinationOffset:0 destinationBytesPerRow:stride destinationBytesPerImage:stride*impl_->height];if(!shared)[blit synchronizeResource:buffer];[blit endEncoding];[command commit];[command waitUntilCompleted];
    check(command.status==MTLCommandBufferStatusCompleted,"Metal readback failed");
    if(impl_->layer){std::unique_lock lock(impl_->presented->mutex);auto& observed=*impl_->presented;
        const bool signalled=observed.changed.wait_for(lock,std::chrono::seconds(3),[&]{return observed.serial>=impl_->serial||observed.gpu_error;});
        if(!signalled||observed.gpu_error||observed.serial<impl_->serial){std::ostringstream error;
            error<<"Metal drawable was not observed presented onscreen: requested="<<impl_->serial<<" submitted="<<observed.submitted
                <<" scheduled="<<observed.scheduled<<" completed="<<observed.completed<<" presented="<<observed.serial
                <<" callbacks="<<observed.callbacks<<" zeroTimeCallbacks="<<observed.zero_time_callbacks<<" lastPresentedTime="<<observed.last_time
                <<" gpuError="<<observed.gpu_error<<" layer{"<<observed.surface<<'}';
            throw std::runtime_error(error.str());}
    }
    std::vector<std::uint8_t> rgba(std::size_t(impl_->width)*impl_->height*4);auto source=static_cast<const std::uint8_t*>(buffer.contents);
    for(std::uint32_t y=0;y<impl_->height;++y)for(std::uint32_t x=0;x<impl_->width;++x){const auto s=std::size_t(y)*stride+x*4,d=(std::size_t(y)*impl_->width+x)*4;
        const unsigned a=source[s+3];rgba[d+3]=static_cast<std::uint8_t>(a);
        const unsigned components[3]={source[s+2],source[s+1],source[s]};
        for(unsigned c=0;c<3;++c)rgba[d+c]=a?static_cast<std::uint8_t>(std::min(255u,(components[c]*255+a/2)/a)):0;}return rgba;
}
void Renderer::recreate_device(){const auto w=impl_->width,h=impl_->height;impl_->create_device();impl_->resize(w,h,impl_->dpi);++impl_->statistics.device_recoveries;}
}
