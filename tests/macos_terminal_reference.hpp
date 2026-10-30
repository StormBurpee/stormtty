#pragma once

// Frozen from renderer.mm at 31c01ec895ba931f42af90c5ab0ed85b68a3f61d,
// before TerminalPainter extraction. Helpers and pane loop below are verbatim.
// Keep this test-only oracle independent of the extracted painter.
#include "../src/platform/macos/core_text_font_resources.hpp"
#include "../src/platform/macos/metal_compositor.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace stormtty::test::terminal_reference {
struct Original {
    const macos::CoreTextFontResources& font_resources;
    const RendererOptions& options;
    const macos::MetalTextureResources& textures;
    float dpi;
    std::uint64_t font_generation;

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
    void draw(const WindowScene& scene, macos::MetalFrame& paint) {
        const auto cell = font_resources.metrics();
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
    }
};
} // namespace stormtty::test::terminal_reference
