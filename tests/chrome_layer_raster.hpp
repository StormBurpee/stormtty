#pragma once

#include "stormtty/context_menu.hpp"
#include "stormtty/renderer.hpp"
#include "stormtty/settings_panel.hpp"
#include <functional>
#include <limits>
#include <algorithm>
#include <cmath>
#include <stdexcept>

// Shared native raster assertions, run through DirectWrite/D2D/WARP on Windows
// and CoreText/Metal on macOS. These fixtures contain no user or terminal data.
namespace stormtty::test {
inline void chrome_check(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
}
inline Color chrome_pixel(const std::vector<std::uint8_t>& rgba,unsigned width,unsigned x,unsigned y) {
    const auto i=(std::size_t(y)*width+x)*4;
    return {rgba.at(i),rgba.at(i+1),rgba.at(i+2),rgba.at(i+3)};
}
inline bool chrome_same_patch(const std::vector<std::uint8_t>& a,const std::vector<std::uint8_t>& b,
                              unsigned width,PixelRect rect) {
    for (unsigned y=static_cast<unsigned>(std::ceil(rect.y));y<rect.y+rect.height;++y)
        for (unsigned x=static_cast<unsigned>(std::ceil(rect.x));x<rect.x+rect.width;++x)
            if (chrome_pixel(a,width,x,y)!=chrome_pixel(b,width,x,y)) return false;
    return true;
}
inline unsigned chrome_ink(const std::vector<std::uint8_t>& rgba,unsigned width,PixelRect rect,bool red) {
    unsigned count=0;
    for (unsigned y=static_cast<unsigned>(std::ceil(rect.y));y<rect.y+rect.height;++y)
        for (unsigned x=static_cast<unsigned>(std::ceil(rect.x));x<rect.x+rect.width;++x) {
            const auto c=chrome_pixel(rgba,width,x,y);
            if (red?c.r>160 && c.g<100 && c.b<100:c.r>180 && c.g>180 && c.b>180) ++count;
        }
    return count;
}
// Compare clipped pixels with the identical unmodified paint geometry. This
// catches resized rounded corners and re-centered/reflowed partial glyphs, as
// well as a clip leaking into subsequent fixed items or retained dirty regions.
inline void chrome_viewport_raster_tests(RendererOptions options) {
    options.background_opacity=1;
    Renderer renderer(nullptr,options);constexpr unsigned width=384,height=240;
    std::uint64_t generation=0;
    for (const float dpi:{96.f,144.f,192.f}) {
        renderer.resize(width,height,dpi);
        WindowScene base;base.background={11,17,23,255};
        ChromePrimitive fixed;fixed.rect={344,24,24,184};fixed.fill={30,180,100,255};base.primitives.push_back(fixed);
        ChromeText footer;footer.rect={24,180,296,36};footer.text="Fixed footer";footer.family=options.font.family;
        base.texts.push_back(footer);base.chrome_generation=++generation;
        renderer.render(base);const auto background=renderer.read_pixels();
        for (const auto viewport:{PixelRect{32,64,296,80},PixelRect{32.25f,64.5f,295.5f,79.25f}}) {
        for (const float offset:{0.f,22.5f,61.25f,95.5f}) {
            WindowScene full=base;
            ChromePrimitive card;card.rect={16,50-offset,240,96};card.fill={70,90,120,255};card.stroke={240,180,100,255};
            card.radius=14;card.stroke_width=2;full.primitives.insert(full.primitives.begin(),card);
            ChromeText title;title.rect={24,53-offset,220,36};title.text="MMMM clipped glyphs";title.family=options.font.family;title.size_dip=20;
            full.texts.insert(full.texts.begin(),title);
            full.chrome_generation=++generation;renderer.render(full);const auto reference=renderer.read_pixels();
            if (offset<=22.5f) chrome_check(chrome_ink(reference,width,{0,0,328,160},false)>8,"Viewport reference contains no real glyph ink");
            chrome_check(chrome_ink(background,width,footer.rect,false)>8,"Fixed footer reference contains no real glyph ink");
            auto clipped=full;clipped.primitives.front().clip=viewport;clipped.texts.front().clip=viewport;
            clipped.chrome_generation=++generation;renderer.render(clipped);const auto actual=renderer.read_pixels();
            if (offset==0) chrome_check(chrome_ink(actual,width,{33,65,290,78},false)>8,"Clipping dropped the partially visible glyphs");
            if (offset<=22.5f) chrome_check(chrome_pixel(actual,width,100,90)!=chrome_pixel(background,width,100,90),"Clipping dropped the partially visible card");
            for (unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
                const bool inside=x>=std::floor(viewport.x) && x<std::ceil(viewport.x+viewport.width) &&
                    y>=std::floor(viewport.y) && y<std::ceil(viewport.y+viewport.height);
                chrome_check(chrome_pixel(actual,width,x,y)==chrome_pixel(inside?reference:background,width,x,y),
                             "Chrome viewport changed original paint geometry or leaked outside its clip");
            }
            clipped.force_redraw=true;renderer.render(clipped);
            chrome_check(renderer.read_pixels()==actual,"Clipped chrome incremental damage differs from full redraw");
            clipped.force_redraw=false;
            // A clip-only mutation invalidates the previously exposed pixels.
            const auto nan=std::numeric_limits<float>::quiet_NaN(),infinity=std::numeric_limits<float>::infinity();
            for (const auto invalid:{PixelRect{32,64,0,80},PixelRect{32,64,-1,80},PixelRect{32,64,80,-1},
                    PixelRect{nan,64,80,80},PixelRect{32,64,infinity,80},PixelRect{32,64,80,nan}}) {
                clipped.primitives.front().clip=invalid;clipped.texts.front().clip=invalid;
                clipped.chrome_generation=++generation;renderer.render(clipped);
                chrome_check(renderer.read_pixels()==background,"Empty or invalid chrome clip painted or left stale pixels");
            }
        }
        }
    }
}
using SettingsRasterCapture=std::function<void(std::string_view,unsigned,unsigned,const std::vector<std::uint8_t>&)>;
inline void settings_viewport_raster_tests(RendererOptions options,SettingsRasterCapture capture={}) {
    Renderer renderer(nullptr,options);
    for (const bool narrow:{false,true}) for(const float scale:{1.f,1.25f,1.5f}) {
        const unsigned dip_width=narrow?480:1024,dip_height=narrow?640:768;
        const unsigned width=unsigned(dip_width*scale),height=unsigned(dip_height*scale);
        renderer.resize(width,height,96*scale);
        SettingsPanel panel;panel.resize({double(dip_width),double(dip_height)});panel.open(default_config(Platform::windows));
        const auto viewport=panel.layout().content;
        const Point inside{viewport.x+viewport.width/2,viewport.y+viewport.height/2};
        const auto scene_for=[&] {WindowScene scene;scene.chrome_generation=panel.generation();panel.append_scene(scene,scale);return scene;};
        auto initial=scene_for();renderer.render(initial);const auto baseline=renderer.read_pixels();
        double previous_offset=0;
        for (const double offset:{22.5,63.25,108.5,201.75,63.25,22.5}) {
            panel.wheel(inside,(previous_offset-offset)/36);previous_offset=offset;
            auto scene=scene_for();renderer.render(scene);const auto pixels=renderer.read_pixels();
            chrome_check(chrome_same_patch(pixels,baseline,width,{0,0,float(width),float(std::floor(viewport.y*scale))}),"Scrolling painted over fixed Settings headings");
            chrome_check(chrome_same_patch(pixels,baseline,width,{0,float(std::ceil((viewport.y+viewport.height)*scale)),float(width),float(height-std::ceil((viewport.y+viewport.height)*scale))}),"Scrolling painted over fixed Settings footer");
            chrome_check(chrome_same_patch(pixels,baseline,width,{0,float(std::ceil(viewport.y*scale)),float(std::floor(viewport.x*scale)),float(std::floor(viewport.height*scale))}),"Scrolling painted over Settings navigation");
            scene.force_redraw=true;renderer.render(scene);
            chrome_check(renderer.read_pixels()==pixels,"Fractional Settings scroll left stale native pixels");
            if (offset==22.5) chrome_check(chrome_ink(pixels,width,{float((viewport.x+12)*scale),float((viewport.y+15)*scale),100*scale,52*scale},false)>8,
                "Settings dropped the exposed terminal sample in a partially visible first-row card");
            if (capture && offset==22.5) {
                const std::string name=std::string("settings-scroll-")+(narrow?"narrow":"normal")+"-fractional-"+std::to_string(unsigned(scale*100))+"pct.png";
                capture(name,width,height,pixels);
            }
        }
        for(int repeat=0;repeat<4;++repeat) {
            for(int tab=0;tab<24;++tab)panel.key({{"TAB"}});
            auto scene=scene_for();renderer.render(scene);const auto pixels=renderer.read_pixels();
            scene.force_redraw=true;renderer.render(scene);chrome_check(renderer.read_pixels()==pixels,"Settings keyboard reveal left stale pixels");
            panel.cancel();(void)panel.take_action();panel.open(default_config(Platform::windows));
            scene=scene_for();renderer.render(scene);chrome_check(renderer.read_pixels()==baseline,"Settings reopen retained scrolled content");
        }
    }
}
inline void chrome_layer_raster_tests(RendererOptions options) {
    options.background_opacity=.65f;
    Renderer renderer(nullptr,options);
    constexpr unsigned width=384,height=256;
    const PixelRect panel{32,36,304,132},label_rect{48,52,270,32};
    for (const float dpi:{96.f,144.f,192.f}) {
        renderer.resize(width,height,dpi);
        WindowScene base;base.background={11,17,23,255};base.chrome_generation=1;
        auto frame=std::make_shared<RenderFrame>();frame->generation=1;frame->columns=25;frame->rows=6;
        frame->cell_width=12;frame->cell_height=24;frame->background=base.background;frame->foreground={255,20,20,255};
        for (unsigned row_index=0;row_index<frame->rows;++row_index) {
            auto row=std::make_shared<FrameRow>();row->id.words={91,row_index+1};row->generation=1;
            row->cells.resize(frame->columns);
            for (auto& cell:row->cells) {cell.grapheme="M";cell.foreground=frame->foreground;cell.background=frame->background;}
            frame->lines.push_back(row);
        }
        base.panes.push_back({91,panel,frame,false,false});
        ChromeText base_text;base_text.rect=label_rect;base_text.text="Settings workspace";base_text.color={255,20,20,255};
        base_text.family=options.font.family;base_text.size_dip=13;base.texts.push_back(base_text);
        ChromeText partial=base_text;partial.rect={8,176,352,36};partial.text="MMMMMMMMMMMMMMMMMMMMMMMM";base.texts.push_back(partial);
        renderer.render(base);const auto baseline=renderer.read_pixels();
        chrome_check(chrome_ink(baseline,width,label_rect,true)>20,"Layer fixture has no underlying label ink");
        for (const auto layer:{ChromeLayer::transient,ChromeLayer::toast,ChromeLayer::palette,ChromeLayer::settings,ChromeLayer::modal,ChromeLayer::context_menu}) {
            WindowScene scene=base;++scene.chrome_generation;
            ChromePrimitive cover;cover.rect=panel;cover.fill={24,29,36,255};cover.z={layer};scene.primitives.push_back(cover);
            cover.rect={96,172,176,48};scene.primitives.push_back(cover);
            ChromeText overlay=base_text;overlay.rect={48,104,270,36};overlay.text="Overlay label";
            overlay.color={255,255,255,255};overlay.z={layer};scene.texts.push_back(overlay);
            // Deliberately append base text last: stacking belongs to the shared
            // surface key, not the order in which independent components append.
            std::rotate(scene.texts.begin(),scene.texts.begin()+2,scene.texts.end());
            renderer.render(scene);const auto covered=renderer.read_pixels();
            WindowScene reference=scene;
            std::erase_if(reference.texts,[](const auto& text){return text.z.layer==ChromeLayer::base;});
            ++reference.chrome_generation;renderer.render(reference);const auto without_base=renderer.read_pixels();
            chrome_check(chrome_same_patch(covered,without_base,width,panel),"Base label overprinted an opaque later surface");
            chrome_check(chrome_ink(covered,width,panel,true)==0,"Terminal or base chrome text leaked through an opaque overlay");
            chrome_check(chrome_same_patch(covered,without_base,width,{96,172,176,48}),"Partially covered base label overprinted the overlay");
            chrome_check(chrome_ink(covered,width,{8,176,80,36},true)>10,"Layer composition erased the uncovered part of a base label");
            chrome_check(chrome_ink(covered,width,overlay.rect,false)>20,"Overlay label disappeared behind its own surface");
            chrome_check(chrome_pixel(covered,width,60,44).a==255,"Opaque chrome inherited terminal background translucency");
            // A z-only change must participate in Windows damage computation.
            scene.texts[1].z={ChromeLayer::context_menu,1};scene.chrome_generation=reference.chrome_generation+1;
            renderer.render(scene);const auto raised=renderer.read_pixels();
            chrome_check(chrome_ink(raised,width,label_rect,true)>20,"Changing only a label's z-order did not invalidate its pixels");
            base.chrome_generation=scene.chrome_generation+1;renderer.render(base);
            chrome_check(renderer.read_pixels()==baseline,"Dismissing an overlay failed to restore lower chrome");
        }
    }

    // Real overlapping submenu geometry on a narrow viewport. Parent labels
    // remain intact outside the child panel; child labels remain visible.
    renderer.resize(420,260);ContextMenu menu;menu.resize({420,260});
    ContextMenuItem parent;parent.id="parent";parent.label="Parent command";
    ContextMenuItem nested;nested.id="nested";nested.label="MMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";
    ContextMenuItem child;child.id="child";child.label="Child command";
    nested.children.push_back(child);
    chrome_check(menu.open(std::vector{parent,nested},{80,28},true),"Cannot open native menu layer fixture");
    menu.key({{"DOWN"}});
    WindowScene parent_scene;parent_scene.chrome_generation=menu.generation();menu.append_scene(parent_scene);
    renderer.render(parent_scene);const auto parent_pixels=renderer.read_pixels();
    menu.key({{"RIGHT"}});chrome_check(menu.layout().panels.size()==2,"Submenu fixture did not open");
    WindowScene nested_scene;nested_scene.chrome_generation=menu.generation();menu.append_scene(nested_scene);
    const auto child_bounds=menu.layout().panels[1].bounds;
    const PixelRect child_interior{float(child_bounds.x+12),float(child_bounds.y+12),float(child_bounds.width-24),float(child_bounds.height-24)};
    renderer.render(nested_scene);const auto nested_pixels=renderer.read_pixels();
    WindowScene without_parent=nested_scene;
    std::erase_if(without_parent.texts,[](const auto& text){return text.z.depth==0;});
    ++without_parent.chrome_generation;renderer.render(without_parent);const auto clean_child=renderer.read_pixels();
    chrome_check(chrome_same_patch(nested_pixels,clean_child,420,child_interior),"Parent menu text overprinted its child panel");
    const auto& parent_row=menu.layout().panels[0].rows[1].bounds;
    const auto visible_x=float(child_bounds.x+child_bounds.width+8);
    const PixelRect exposed_parent{visible_x,float(parent_row.y),float(parent_row.x+parent_row.width-8-visible_x),float(parent_row.height)};
    chrome_check(exposed_parent.width>12 && !chrome_same_patch(nested_pixels,clean_child,420,exposed_parent),
                 "Submenu erased the exposed part of a partially covered parent label");
    const auto child_label=std::find_if(nested_scene.texts.begin(),nested_scene.texts.end(),[](const auto& text){return text.text=="Child command";});
    chrome_check(child_label!=nested_scene.texts.end() && chrome_ink(nested_pixels,420,child_label->rect,false)>20,"Nested menu label is not visible");
    menu.key({{"LEFT"}});WindowScene closed_child;closed_child.chrome_generation=without_parent.chrome_generation+1;menu.append_scene(closed_child);
    renderer.render(closed_child);chrome_check(renderer.read_pixels()==parent_pixels,"Closing a submenu left damaged parent text");

    // Cached palette closing frames use the same layer, including intentional
    // chrome-only alpha. Never make an underlying terminal row translucent.
    renderer.set_background_opacity(0);renderer.resize(384,96);
    WindowScene fade;fade.chrome_generation=closed_child.chrome_generation+1;
    ChromeText label;label.rect={16,12,320,48};label.text="Closing palette";label.family=options.font.family;
    label.size_dip=20;label.color={255,255,255,128};label.z={ChromeLayer::palette};fade.texts.push_back(label);
    renderer.render(fade);auto faded=renderer.read_pixels();std::uint8_t maximum=0;
    for (std::size_t i=3;i<faded.size();i+=4) maximum=std::max(maximum,faded[i]);
    chrome_check(maximum>60 && maximum<=129,"Chrome transition alpha did not cover all native glyph layers");
    fade.texts[0].color.a=0;++fade.chrome_generation;renderer.render(fade);faded=renderer.read_pixels();
    for (std::size_t i=3;i<faded.size();i+=4) chrome_check(faded[i]==0,"Dismissed palette left chrome pixels behind");
    chrome_check(renderer.render(fade)==RenderResult::unchanged,"Settled palette kept repainting");
}
} // namespace stormtty::test
