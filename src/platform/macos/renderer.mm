#include "stormtty/renderer.hpp"
#include "chrome_painter.hpp"
#include "core_text_font_resources.hpp"
#include "metal_texture_resources.hpp"
#include "metal_compositor.hpp"
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace stormtty {
namespace {
struct PaneStamp {
    std::uint64_t id{},generation{},images{};PixelRect content;bool focused{},cursor{},blink{};
    bool operator==(const PaneStamp&) const = default;
};

}
struct Renderer::Impl {
    // Early storage keeps native resources alive until texture/font destruction.
    // Deferred emplacement preserves the original shared-state allocation phase.
    std::optional<macos::MetalCompositor> compositor;
    RendererOptions options;
    RendererStats statistics;
    macos::CoreTextFontResources font_resources;
    std::uint32_t width{},height{};float dpi{96};
    std::uint64_t font_generation{1},chrome_generation{};
    std::vector<PaneStamp> previous;
    Color previous_background{};bool have_previous{};
    macos::MetalTextureResources textures;
    explicit Impl(void* handle, RendererOptions settings)
        : options(std::move(settings)),
          textures({options.shape_cache_bytes, options.image_cache_bytes,
                    options.maximum_image_dimension}) {
        compositor.emplace(handle);
        try {
            if (options.force_warp) {
                throw std::invalid_argument("WARP is Windows-only; macOS native tests require a Metal device");
            }
            if (!std::isfinite(options.background_opacity) ||
                options.background_opacity < 0.f || options.background_opacity > 1.f) {
                throw std::invalid_argument("Invalid default background opacity");
            }
            create_device();
            measure();
        } catch (...) {
            compositor->release_generation_references_for_unwind();
            throw;
        }
    }

    ~Impl() {
        compositor->release_generation_references_for_unwind();
    }

    void create_device() {
        compositor->create_device(textures);
        statistics.composition = true;
        statistics.color_fonts = true;
        have_previous = false;
    }
    void measure() {
        font_resources.reset(options.font, dpi);
        ++font_generation;
        textures.clear_rows();
        have_previous = false;
    }
    void resize(std::uint32_t w, std::uint32_t h, float new_dpi) {
        if (!std::isfinite(new_dpi) || new_dpi < 48 || new_dpi > 768) {
            throw std::invalid_argument("Invalid renderer DPI");
        }
        macos::MetalCompositor::validate_surface_size(w, h);
        if (new_dpi != dpi) {
            dpi = new_dpi;
            measure();
        }
        compositor->update_layer_geometry(dpi);
        if (compositor->surface_matches(w, h)) {
            return;
        }
        width = w;
        height = h;
        have_previous = false;
        compositor->replace_surface(w, h, dpi);
    }

    void draw_row(macos::MetalFrame& paint, const RenderPane& pane, const FrameRow& row,
                  std::uint32_t width, std::uint32_t height, PixelRect destination,
                  std::uint32_t variant = 0) {
        const auto cell = font_resources.metrics();
        const macos::RowRasterOptions raster_options{
            width, height, pane.frame->cell_width ? pane.frame->cell_width : float(cell.width),
            cell, dpi, pane.text_blink_on, options.font.ligatures, options.font.locale,
            textures.remaining_upload_bytes()
        };
        const macos::RowTextureIdentity identity{pane.id, font_generation, variant};
        paint.draw_row(identity, row, font_resources.borrow_font(), raster_options, destination);
    }

    void draw_images(macos::MetalFrame& paint,const RenderPane& pane,ImageLayer which) {
        const auto cell = font_resources.metrics();
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
            paint.draw_image(pane.id, image, destination,
                {float(placement.source_x)/image.width, float(placement.source_y)/image.height,
                 float(sw)/image.width, float(sh)/image.height});
        }
    }
    RenderResult render(const WindowScene& scene) {
        const auto cell = font_resources.metrics();
        if(!width||!height||!compositor->has_surface())return RenderResult::suspended;
        if(scene.panes.size()>4096||scene.primitives.size()>16384||scene.texts.size()>4096)throw std::runtime_error("Metal scene exceeds bounded primitive counts");
        compositor->check_gpu_health();
        if (compositor->is_occluded()) return RenderResult::occluded;
        std::vector<PaneStamp> stamps;
        for(const auto& p:scene.panes)if(p.frame)stamps.push_back({p.id,p.frame->generation,p.frame->image_placements_generation,p.content,p.focused,p.cursor_on,p.text_blink_on});
        if(have_previous&&!scene.force_redraw&&chrome_generation==scene.chrome_generation&&stamps==previous&&scene.background==previous_background)return RenderResult::unchanged;
        if(compositor->submission_busy())return RenderResult::busy;
        @autoreleasepool {
            // Do not retain CAMetalDrawable across frames. Apple may briefly wait
            // inside nextDrawable; two in-flight buffers plus a three-drawable pool
            // avoid saturating it. This is not a proven nonblocking present path.
            auto paint = compositor->begin_frame(textures);
            if (!paint) return RenderResult::busy;
            Color canvas=scene.background;canvas.a=static_cast<std::uint8_t>(std::lround(options.background_opacity*255));
            paint.fill({0,0,float(width),float(height)},canvas);
            for(const auto& pane:scene.panes) {
                if(!pane.frame||!paint.clip(pane.content))continue;
                const auto& frame=*pane.frame;
                if(frame.lines.size()>4096||frame.images.size()>1024)throw std::runtime_error("Terminal frame exceeds renderer row/placement limits");
                const float cw=frame.cell_width?frame.cell_width:cell.width,ch=frame.cell_height?frame.cell_height:cell.height;
                // Reset the pane first; source-overing translucent backgrounds on
                // a previous/global background would accumulate opacity.
                paint.fill(pane.content,{0,0,0,0},true);
                Color background=frame.background;background.a=static_cast<std::uint8_t>(std::lround(options.background_opacity*255));
                paint.fill(pane.content,background);
                draw_images(paint,pane,ImageLayer::below_background);
                for(std::size_t r=0;r<frame.lines.size();++r)if(frame.lines[r]) {
                    const auto& row=*frame.lines[r];
                    for(std::size_t c=0;c<row.cells.size();++c) {
                        const auto& cell_value=row.cells[c];Color bg=cell_value.style.inverse?cell_value.foreground:cell_value.background;
                        if(cell_value.selected)bg={58,79,91,255};
                        if(!cell_value.background_is_default||cell_value.style.inverse||cell_value.selected){bg.a=255;paint.fill({pane.content.x+c*cw,pane.content.y+r*ch,cw,ch},bg);}
                    }
                }
                draw_images(paint,pane,ImageLayer::below_text);
                for(std::size_t r=0;r<frame.lines.size();++r)if(frame.lines[r]&&pane.content.y+r*ch<pane.content.y+pane.content.height) {
                    const auto rw=static_cast<std::uint32_t>(std::ceil(frame.columns*cw));const auto rh=static_cast<std::uint32_t>(std::ceil(ch));
                    if(rw&&rh)draw_row(paint,pane,*frame.lines[r],rw,rh,{pane.content.x,pane.content.y+r*ch,float(rw),float(rh)});
                }
                draw_images(paint,pane,ImageLayer::above_text);
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
                        paint.fill({rect.x,rect.y,rect.width,line},color);paint.fill({rect.x,rect.y+rect.height-line,rect.width,line},color);
                        paint.fill({rect.x,rect.y,line,rect.height},color);paint.fill({rect.x+rect.width-line,rect.y,line,rect.height},color);
                    }else{
                        paint.fill(rect,color);
                        if(cursor.shape==CursorShape::block&&cursor_row&&column<cursor_row->cells.size()){
                            FrameRow inverted=*cursor_row;auto& c=inverted.cells[column];
                            c.foreground=c.style.inverse?c.foreground:c.background;c.style.inverse=false;c.selected=false;c.style.blink=false;
                            RenderPane cursor_pane=pane;cursor_pane.text_blink_on=true;
                            const auto right=std::min(rect.x+rect.width,pane.content.x+pane.content.width),bottom=std::min(rect.y+rect.height,pane.content.y+pane.content.height);
                            PixelRect clipped{std::max(rect.x,pane.content.x),std::max(rect.y,pane.content.y),0,0};clipped.width=right-clipped.x;clipped.height=bottom-clipped.y;
                            if(paint.clip(clipped))draw_row(paint,cursor_pane,inverted,static_cast<std::uint32_t>(std::ceil(frame.columns*cw)),static_cast<std::uint32_t>(std::ceil(ch)),
                                {pane.content.x,pane.content.y+cursor.row*ch,frame.columns*cw,ch},column+1);
                        }
                    }
                }
            }
            paint.clip({0,0,float(width),float(height)});
            macos::ChromePainter chrome(paint, {0, 0, float(width), float(height)}, dpi);
            for (const auto& item : chrome_paint_order(scene)) {
                if (item.kind == ChromeDrawKind::primitive) {
                    chrome.draw(scene.primitives[item.index], textures.remaining_upload_bytes());
                } else {
                    chrome.draw(scene.texts[item.index], textures.remaining_upload_bytes());
                }
            }
            paint.submit();
            ++statistics.presents;
            previous = std::move(stamps);
            chrome_generation = scene.chrome_generation;
            previous_background = scene.background;
            have_previous = true;
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
    if(!std::isfinite(opacity)||opacity<0.f||opacity>1.f)throw std::invalid_argument("Background opacity must be between 0 and 1");
    impl_->options.background_opacity=opacity;impl_->have_previous=false;
}
CellMetrics Renderer::metrics()const noexcept{return impl_->font_resources.metrics();}
RenderResult Renderer::render(const WindowScene& scene){return impl_->render(scene);}
void* Renderer::frame_latency_wait_handle()const noexcept{return nullptr;}
RendererStats Renderer::stats() const noexcept {
    auto result = impl_->statistics;
    const auto textures = impl_->textures.stats();
    result.shaped_rows = textures.shaped_rows;
    result.shape_cache_hits = textures.row_cache_hits;
    result.image_uploads = textures.image_uploads;
    result.dirty_rectangles = textures.dirty_rectangles;
    result.shape_cache_bytes = textures.row_bytes;
    result.image_cache_bytes = textures.image_bytes;
    return result;
}
// The alpha diagnostic currently targets the Windows DComp investigation.
// Explicitly unavailable rather than blocking AppKit or pretending a sample.
bool Renderer::begin_alpha_probe(PixelRect) { return false; }
std::optional<RendererAlphaProbe> Renderer::poll_alpha_probe() { return std::nullopt; }
void Renderer::cancel_alpha_probe() noexcept {}
std::vector<std::uint8_t> Renderer::read_pixels() {
    return impl_->compositor->read_pixels();
}
void Renderer::recreate_device(){const auto w=impl_->width,h=impl_->height;impl_->create_device();impl_->resize(w,h,impl_->dpi);++impl_->statistics.device_recoveries;}
}
