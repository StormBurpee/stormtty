#pragma once

#include "stormtty/context_menu.hpp"
#include "stormtty/renderer.hpp"
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
