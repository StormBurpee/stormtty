// Native Windows runtime test. Cross-compiling this file is NOT a passing test.
#include "stormtty/renderer.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace stormtty;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct HostWindow {
    HWND hwnd{};
    bool reject_no_redirection{}, reject_redirection{};
    unsigned no_redirection_requests{};
    static constexpr const wchar_t* class_name = L"StormTTYRendererHostSmoke";
    static LRESULT CALLBACK dispatch(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<HostWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && message == WM_STYLECHANGING && static_cast<int>(wparam) == GWL_EXSTYLE) {
            auto* style = reinterpret_cast<STYLESTRUCT*>(lparam);
            if (style->styleNew & WS_EX_NOREDIRECTIONBITMAP) {
                ++self->no_redirection_requests;
                if (self->reject_no_redirection) style->styleNew &= ~DWORD(WS_EX_NOREDIRECTIONBITMAP);
            } else if (self->reject_redirection) style->styleNew |= WS_EX_NOREDIRECTIONBITMAP;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    explicit HostWindow(bool no_redirection, DWORD base_style=WS_EX_TOOLWINDOW) {
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=class_name;wc.lpfnWndProc=dispatch;
        require(RegisterClassW(&wc)!=0,"Register renderer host class failed");
        hwnd=CreateWindowExW(base_style | (no_redirection ? WS_EX_NOREDIRECTIONBITMAP : 0),
            class_name,L"Renderer host smoke",WS_POPUP,0,0,96,48,nullptr,nullptr,wc.hInstance,this);
        if (!hwnd) {
            UnregisterClassW(class_name,wc.hInstance);
            throw std::runtime_error("Create hidden renderer host failed");
        }
    }
    ~HostWindow() { if(hwnd)DestroyWindow(hwnd);UnregisterClassW(class_name,GetModuleHandleW(nullptr)); }
    HostWindow(const HostWindow&)=delete;
    HostWindow& operator=(const HostWindow&)=delete;
    LONG_PTR style() const { return GetWindowLongPtrW(hwnd,GWL_EXSTYLE); }
    void add_unrelated_style() {
        SetLastError(ERROR_SUCCESS);
        const auto previous=SetWindowLongPtrW(hwnd,GWL_EXSTYLE,style() | WS_EX_ACCEPTFILES);
        require(previous!=0 || GetLastError()==ERROR_SUCCESS,"Add unrelated host style failed");
    }
};
void native_host_surface_test() {
    // A real HWND, not offscreen alpha readback: both initial bit states, both
    // presentation paths, recovery, and mutations owned by the host itself.
    for (const bool original_no_redirection : {false,true}) {
        for (const bool composition : {false,true}) {
            HostWindow host(original_no_redirection,composition ? 0 : WS_EX_TOOLWINDOW);
            const auto original=host.style();
            RendererOptions options;options.force_warp=true;options.composition=composition;
            {
                Renderer renderer(host.hwnd,options);
                require(host.style()==original,"Renderer changed host style before creating a surface");
                renderer.resize(96,48);
                require(renderer.stats().composition==composition,"Requested native composition/opaque test path unavailable");
                require(((host.style() & WS_EX_NOREDIRECTIONBITMAP)!=0)==composition,
                        "Host redirection style does not match active swapchain path");
                require((host.style() & ~LONG_PTR(WS_EX_NOREDIRECTIONBITMAP))==
                        (original & ~LONG_PTR(WS_EX_NOREDIRECTIONBITMAP)),"Renderer clobbered unrelated host styles");
                host.add_unrelated_style();
                WindowScene scene;renderer.render(scene);
                require(renderer.read_pixels().size()==96*48*4,"Native host surface did not render");
                renderer.recreate_device();renderer.render(scene);
                require(renderer.stats().composition==composition && renderer.stats().device_recoveries==1,
                        "Native host did not reconstruct its selected presentation path");
                require(((host.style() & WS_EX_NOREDIRECTIONBITMAP)!=0)==composition,
                        "Recovery left the wrong host redirection style");
                require((host.style() & WS_EX_ACCEPTFILES)!=0,"Recovery lost host-owned extended style change");
                if (composition && !original_no_redirection) {
                    host.reject_no_redirection=true;renderer.recreate_device();
                    require(!renderer.stats().composition && (host.style() & WS_EX_NOREDIRECTIONBITMAP)==0,
                            "Composition-to-opaque recovery retained the no-redirection bit");
                    require((host.style() & WS_EX_ACCEPTFILES)!=0,"Fallback recovery lost unrelated host style");
                    host.reject_no_redirection=false;renderer.recreate_device();
                    require(renderer.stats().composition && (host.style() & WS_EX_NOREDIRECTIONBITMAP)!=0,
                            "Opaque-to-composition recovery did not restore the required host style");
                }
            }
            require(host.style()==(original | WS_EX_ACCEPTFILES),"Renderer destruction did not restore only its owned style bit");
        }
    }
    {
        // WM_STYLECHANGING can reject a successfully requested SetWindowLongPtr.
        // That must fall back, not leave a translucent visual above HWND pixels.
        HostWindow host(false);host.reject_no_redirection=true;
        const auto original=host.style();
        RendererOptions options;options.force_warp=true;
        {
            Renderer renderer(host.hwnd,options);renderer.resize(96,48);
            require(host.no_redirection_requests>0,"Rejected-style fixture never reached composition host setup");
            require(!renderer.stats().composition,"Rejected DComp host style did not select opaque fallback");
            require(host.style()==original,"Composition failure left a stale no-redirection bit");
            host.reject_no_redirection=false;
            renderer.recreate_device();
            require(renderer.stats().composition && (host.style() & WS_EX_NOREDIRECTIONBITMAP)!=0,
                    "Recovery from opaque fallback failed to enable composition hosting");
        }
        require(host.style()==original,"Fallback-to-composition destruction lost original host style");
    }
    {
        // Conversely an HWND path may not proceed when redirection is refused.
        // Its thrown setup failure must roll back and permit a later clean retry.
        HostWindow host(true);host.reject_redirection=true;
        const auto original=host.style();
        RendererOptions options;options.force_warp=true;options.composition=false;
        {
            Renderer renderer(host.hwnd,options);
            bool rejected=false;
            try { renderer.resize(96,48); } catch(const std::exception&) { rejected=true; }
            require(rejected,"HWND swapchain creation proceeded without a redirection surface");
            require(host.style()==original && !renderer.stats().composition,
                    "Failed surface construction did not restore original host style");
            host.reject_redirection=false;renderer.resize(96,48);
            require((host.style() & WS_EX_NOREDIRECTIONBITMAP)==0,"Retry did not enable opaque HWND redirection");
        }
        require(host.style()==original,"Opaque renderer did not restore caller's original no-redirection bit");
    }
}
Color pixel(const std::vector<std::uint8_t>& rgba, std::size_t width, std::size_t x, std::size_t y) {
    const auto offset=(y*width+x)*4;
    require(offset+3<rgba.size(),"pixel coordinate outside readback");
    return {rgba[offset],rgba[offset+1],rgba[offset+2],rgba[offset+3]};
}
bool colors_near(Color a, Color b, unsigned tolerance=2) {
    auto close=[tolerance](unsigned x,unsigned y) { return x>y?x-y<=tolerance:y-x<=tolerance; };
    return close(a.r,b.r)&&close(a.g,b.g)&&close(a.b,b.b)&&close(a.a,b.a);
}
std::shared_ptr<RenderFrame> frame() {
    auto result=std::make_shared<RenderFrame>();
    result->generation=1;result->columns=12;result->rows=4;result->cell_width=16;result->cell_height=24;
    result->foreground={236,238,242,255};result->background={23,25,29,255};
    for(unsigned r=0;r<4;++r) {
        auto row=std::make_shared<FrameRow>();row->id.words={42,r+1};row->generation=1;row->cells.resize(12);
        for(auto& c:row->cells) {c.foreground=result->foreground;c.background=result->background;c.underline_color=result->foreground;}
        result->lines.push_back(row);
    }
    return result;
}
void native_render_test() {
    RendererOptions options;options.force_warp=true;options.image_cache_bytes=4096;
    Renderer renderer(nullptr,options);renderer.resize(256,160);
    require(renderer.stats().bundled_font,"Packaged Cascadia Mono upright/italic private collection was not selected");
    auto source=frame();
    auto first=std::make_shared<FrameRow>(*source->lines[0]);
    first->cells[0].grapheme="A";
    first->cells[4].background={200,30,40,255};first->cells[4].background_is_default=false;
    first->cells[5].foreground={20,190,50,255};first->cells[5].style.inverse=true;
    source->lines[0]=first;
    auto unicode=std::make_shared<FrameRow>(*source->lines[1]);
    unicode->cells[0].grapheme="e\xCC\x81"; // combining sequence, one terminal cell
    unicode->cells[1].grapheme="\xE7\x95\x8C";unicode->cells[1].span=2;unicode->cells[2].span=0;
    unicode->cells[3].grapheme="\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB";
    unicode->cells[3].span=2;unicode->cells[4].span=0;
    // Contextual Latin shaping and Arabic exercise fallback/cluster capture; this
    // test deliberately does not assert font-specific glyph counts or full bidi.
    for(unsigned i=0;i<6;++i) unicode->cells[6+i].grapheme=std::string(1,"office"[i]);
    source->lines[1]=unicode;
    auto selection=std::make_shared<FrameRow>(*source->lines[2]);selection->cells[0].selected=true;
    source->lines[2]=selection;
    auto pixels=std::make_shared<ImagePixels>();pixels->image_id=7;pixels->generation=1;pixels->width=2;pixels->height=1;
    pixels->bytes={255,0,0,255,0,80,240,255};
    ImagePlacement placement;placement.pixels=pixels;placement.column=8;placement.row=2;
    placement.pixel_width=16;placement.pixel_height=24;placement.source_x=1;placement.source_width=1;placement.source_height=1;
    placement.kind=ImagePlacementKind::virtual_placeholder; // Geometry is already resolved by the adapter.
    source->images.push_back(placement);source->image_placements_generation=1;
    WindowScene scene;scene.panes.push_back({100,{16,24,192,96},source,true,true});
    require(renderer.render(scene)==RenderResult::presented,"first offscreen frame not presented");
    auto stats=renderer.stats();require(stats.warp,"smoke test did not use WARP");require(stats.shaped_rows==4,"expected four shaped rows");
    auto rgba=renderer.read_pixels();require(rgba.size()==256*160*4,"readback size wrong");
    require(colors_near(pixel(rgba,256,16+4*16+8,24+12),{200,30,40,255}),"explicit background paint wrong");
    require(colors_near(pixel(rgba,256,16+5*16+8,24+12),{20,190,50,255}),"inverse background paint wrong");
    require(colors_near(pixel(rgba,256,24,24+2*24+12),{58,79,91,255}),"selection background paint wrong");
    require(colors_near(pixel(rgba,256,16+8*16+8,24+2*24+12),{0,80,240,255}),"cropped image placement not drawn");
    unsigned ink=0;
    for(unsigned y=24;y<48;++y) for(unsigned x=16;x<32;++x)
        if(!colors_near(pixel(rgba,256,x,y),source->background,8)) ++ink;
    require(ink>4,"DirectWrite did not rasterize visible text");
    require(renderer.render(scene)==RenderResult::unchanged,"identical scene was repainted");
    require(renderer.stats().shaped_rows==stats.shaped_rows,"identical frame reshaped text");

    auto changed=std::make_shared<RenderFrame>(*source);changed->generation++;
    auto row=std::make_shared<FrameRow>(*changed->lines[1]);row->generation++;row->cells[6].grapheme="O";
    changed->lines[1]=row;scene.panes[0].frame=changed;
    const auto before_damage=renderer.stats().dirty_rectangles;
    renderer.render(scene);
    require(renderer.stats().shaped_rows==stats.shaped_rows+1,"single-row edit reshaped unchanged rows");
    require(renderer.stats().dirty_rectangles==before_damage+1,"single-row edit did not produce one damage band");

    auto cursor_frame=std::make_shared<RenderFrame>(*changed);cursor_frame->generation++;
    cursor_frame->cursor={1,3,true,false,false,false,CursorShape::block,{220,230,240,255}};
    scene.panes[0].frame=cursor_frame;renderer.render(scene);
    const auto cursor_shapes=renderer.stats().shaped_rows;
    scene.panes[0].cursor_on=false;renderer.render(scene);
    require(renderer.stats().shaped_rows==cursor_shapes,"cursor blink reshaped unchanged rows");
    rgba=renderer.read_pixels();
    require(colors_near(pixel(rgba,256,16+24,24+3*24+12),source->background),"old cursor was not erased");

    auto animation=std::make_shared<RenderFrame>(*cursor_frame);animation->generation++;
    auto next_pixels=std::make_shared<ImagePixels>(*pixels);next_pixels->generation++;next_pixels->bytes={255,0,0,255,30,210,60,255};
    animation->images[0].pixels=next_pixels;scene.panes[0].frame=animation;renderer.render(scene);
    rgba=renderer.read_pixels();
    require(colors_near(pixel(rgba,256,16+8*16+8,24+2*24+12),{30,210,60,255}),"image animation generation used stale pixels");
    require(renderer.stats().image_uploads==2,"unchanged image was reuploaded or new frame was missed");
    require(renderer.stats().image_cache_bytes<=options.image_cache_bytes,"image cache exceeded budget");

    const auto old_metrics=renderer.metrics();renderer.resize(256,160,144);
    require(renderer.metrics().width>old_metrics.width,"DPI did not remeasure font");
    renderer.render(scene);renderer.recreate_device();renderer.render(scene);
    require(renderer.stats().device_recoveries==1,"device reconstruction not exercised");
    rgba=renderer.read_pixels();
    require(colors_near(pixel(rgba,256,16+8*16+8,24+2*24+12),{30,210,60,255}),"owned image lost after device reconstruction");
    renderer.resize(0,0);require(renderer.render(scene)==RenderResult::suspended,"zero-size surface did not suspend");
    renderer.resize(256,160,96);require(renderer.render(scene)==RenderResult::presented,"restore from zero size failed");
}
void font_override_test() {
    RendererOptions options;options.force_warp=true;options.font.family="Consolas";
    Renderer renderer(nullptr,options);renderer.resize(96,48);
    require(!renderer.stats().bundled_font,"Explicit system font override was replaced with bundled Cascadia");
    auto source=frame();WindowScene scene;scene.panes.push_back({104,{0,0,96,48},source});
    require(renderer.render(scene)==RenderResult::presented,"System font override did not render");
    options.font.family="Cascadia Mono";renderer.set_font(options.font);
    require(renderer.stats().bundled_font,"Switching back to bundled font failed");
    require(renderer.render(scene)==RenderResult::presented,"Bundled font switch did not invalidate layout");
    options.font.family="StormTTY missing font fixture";renderer.set_font(options.font);
    require(!renderer.stats().bundled_font,"Unknown user font must preserve system fallback policy");
    require(renderer.render(scene)==RenderResult::presented,"Missing user font fallback did not render");
}
void transparency_test() {
    RendererOptions options;options.force_warp=true;options.background_opacity=.9f;
    Renderer renderer(nullptr,options);renderer.resize(96,48);
    auto source=frame();source->columns=4;source->rows=1;
    auto row=std::make_shared<FrameRow>(*source->lines[0]);
    row->cells[1].background_is_default=false; // Same RGB but explicit TUI background must stay opaque.
    source->lines={row};
    WindowScene scene;scene.panes.push_back({101,{0,0,64,24},source});renderer.render(scene);
    auto rgba=renderer.read_pixels();
    require(std::abs(int(pixel(rgba,96,8,12).a)-230)<=1,"default background opacity not preserved");
    require(pixel(rgba,96,24,12).a==255,"explicit background was made translucent");
    auto changed=std::make_shared<RenderFrame>(*source);changed->generation++;
    auto changed_row=std::make_shared<FrameRow>(*row);changed_row->generation++;changed_row->cells[3].grapheme="A";
    changed->lines={changed_row};scene.panes[0].frame=changed;renderer.render(scene);
    require(std::abs(int(pixel(renderer.read_pixels(),96,8,12).a)-230)<=1,"damage repaint accumulated background alpha");
    renderer.set_background_opacity(1);renderer.render(scene);
    require(pixel(renderer.read_pixels(),96,8,12).a==255,"opaque fallback not applied");
}
void chrome_material_test() {
    RendererOptions options;options.force_warp=true;options.background_opacity=.9f;
    Renderer renderer(nullptr,options);renderer.resize(128,96);
    auto source=frame();source->rows=1;source->columns=8;
    auto row=std::make_shared<FrameRow>(*source->lines[0]);row->cells[0].grapheme="M";source->lines={row};
    WindowScene scene;scene.chrome_generation=1;scene.panes.push_back({105,{0,48,128,24},source});
    ChromePrimitive band;band.rect={0,0,128,32};band.fill={27,38,48,204};band.background_replace=true;
    scene.primitives.push_back(band);
    ChromePrimitive tab;tab.rect={80,4,36,24};tab.fill={52,65,74,255};tab.radius=7;scene.primitives.push_back(tab);
    renderer.render(scene);const auto first=renderer.read_pixels();
    require(colors_near(pixel(first,128,20,16),{27,38,48,204}),"chrome material alpha compounded over canvas");
    require(colors_near(pixel(first,128,96,16),{52,65,74,255}),"active tab must stay opaque");
    require(std::abs(int(pixel(first,128,20,80).a)-230)<=1,"terminal canvas opacity changed under chrome");
    require(colors_near(pixel(first,128,80,4),{27,38,48,204}),"rounded tab corner was filled as a square");
    const auto before=renderer.stats();scene.primitives[0].fill.a=180;++scene.chrome_generation;
    renderer.render(scene);const auto second=renderer.read_pixels();
    require(std::abs(int(pixel(second,128,20,16).a)-180)<=1,"material alpha did not replace old frame alpha");
    require(renderer.stats().shaped_rows==before.shaped_rows && renderer.stats().shape_cache_hits==before.shape_cache_hits,
            "chrome-only animation touched terminal rows");
    for(unsigned y=48;y<72;++y)for(unsigned x=0;x<128;++x)
        require(pixel(first,128,x,y)==pixel(second,128,x,y),"chrome transition altered terminal pixels");
    options.composition=false;Renderer fallback(nullptr,options);fallback.resize(128,96);fallback.render(scene);
    require(pixel(fallback.read_pixels(),128,20,16).a==255,"opaque fallback left material band translucent");
    require(pixel(fallback.read_pixels(),128,20,80).a==255,"opaque fallback left canvas translucent");
}
void chrome_ellipsis_and_opacity_test() {
    RendererOptions options;options.force_warp=true;options.font.family="Consolas";options.font.size_dip=20;
    options.background_opacity=0;
    Renderer renderer(nullptr,options);renderer.resize(192,48);
    WindowScene scene;scene.chrome_generation=1;
    ChromeText label;label.rect={8,4,float(renderer.metrics().width)*6,36};label.family="Consolas";label.size_dip=20;
    label.text="MMMMMMMMMMMMMMMMMMMM";label.color={255,255,255,255};scene.texts.push_back(label);
    renderer.render(scene);const auto truncated=renderer.read_pixels();
    scene.texts[0].text="MMMMM\xE2\x80\xA6";++scene.chrome_generation;
    renderer.render(scene);const auto reference=renderer.read_pixels();
    unsigned differences=0;
    for(std::size_t i=0;i<reference.size();++i)if(std::abs(int(reference[i])-int(truncated[i]))>2)++differences;
    require(differences<=16,"long chrome label did not match native end-ellipsis layout");
    std::uint8_t opaque=0;for(std::size_t i=3;i<reference.size();i+=4)opaque=std::max(opaque,reference[i]);
    require(opaque>200,"ellipsis fixture had no visible text ink");
    scene.texts[0].color.a=128;++scene.chrome_generation;renderer.render(scene);auto faded=renderer.read_pixels();
    std::uint8_t maximum=0;for(std::size_t i=3;i<faded.size();i+=4)maximum=std::max(maximum,faded[i]);
    require(maximum<=129 && maximum>60,"finite text opacity did not scale all glyph coverage");
    scene.texts[0].color.a=0;++scene.chrome_generation;renderer.render(scene);faded=renderer.read_pixels();
    for(std::size_t i=3;i<faded.size();i+=4)require(faded[i]==0,"completed hidden palette left text pixels behind");
    const auto presents=renderer.stats().presents;
    require(renderer.render(scene)==RenderResult::unchanged && renderer.stats().presents==presents,
            "settled palette continued rendering unchanged frames");
}
void cache_pressure_test() {
    RendererOptions options;options.force_warp=true;options.image_cache_bytes=8;options.shape_cache_bytes=512;
    Renderer renderer(nullptr,options);renderer.resize(96,64);
    auto source=frame();source->rows=1;source->columns=4;
    auto red=std::make_shared<ImagePixels>();red->image_id=1;red->generation=1;red->width=2;red->height=1;
    red->bytes={240,20,30,255,240,20,30,255};
    auto blue=std::make_shared<ImagePixels>(*red);blue->image_id=2;blue->bytes={20,30,240,255,20,30,240,255};
    ImagePlacement a;a.pixels=red;a.column=-1;a.pixel_width=32;a.pixel_height=24;
    ImagePlacement b;b.pixels=blue;b.column=1;b.pixel_width=16;b.pixel_height=24;
    source->images={a,b};source->image_placements_generation=1;
    WindowScene scene;scene.background={100,110,120,255};scene.panes.push_back({103,{16,16,64,24},source});
    renderer.render(scene);const auto rgba=renderer.read_pixels();
    require(colors_near(pixel(rgba,96,8,24),scene.background),"negative image placement escaped pane clip");
    require(colors_near(pixel(rgba,96,24,24),{240,20,30,255}),"evicted texture lost in-flight drawing reference");
    require(colors_near(pixel(rgba,96,40,24),{20,30,240,255}),"second cache-pressure image not drawn");
    require(renderer.stats().image_uploads==2 && renderer.stats().image_cache_bytes<=8,"image eviction budget failed");
    require(renderer.stats().shape_cache_bytes<=512,"shape cache budget failed");
}
void rejected_image_test() {
    RendererOptions options;options.force_warp=true;options.image_cache_bytes=4;
    Renderer renderer(nullptr,options);renderer.resize(64,48);
    auto source=frame();auto pixels=std::make_shared<ImagePixels>();pixels->width=8193;pixels->height=8193;
    ImagePlacement image;image.pixels=pixels;image.pixel_width=16;image.pixel_height=16;
    source->images.push_back(image);
    WindowScene scene;scene.panes.push_back({102,{0,0,64,48},source});renderer.render(scene);
    require(renderer.stats().image_uploads==0,"oversized malformed image was uploaded");
    require(renderer.stats().image_cache_bytes==0,"rejected image consumed cache budget");
}
} // namespace
int main() {
    try {
        // Asset resolution must follow the executable, not the process/shell CWD.
        const auto previous_directory=std::filesystem::current_path();
        struct RestoreDirectory {std::filesystem::path path;~RestoreDirectory(){std::error_code ignored;std::filesystem::current_path(path,ignored);}} restore{previous_directory};
        std::filesystem::current_path(std::filesystem::temp_directory_path());
        native_host_surface_test();native_render_test();font_override_test();transparency_test();chrome_material_test();chrome_ellipsis_and_opacity_test();cache_pressure_test();rejected_image_test();
        std::cout<<"PASS Windows WARP renderer: HWND/DComp style lifecycle and fallback, private Cascadia/overrides, pixels, Unicode path, damage/cache, images, alpha, DPI, recovery\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL renderer smoke: "<<error.what()<<'\n';return 1; }
}
