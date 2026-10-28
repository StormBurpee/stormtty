#import <AppKit/AppKit.h>
#include "stormtty/renderer.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
using namespace stormtty;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
Color pixel(const std::vector<std::uint8_t>& bytes,unsigned x,unsigned y){auto i=(std::size_t(y)*256+x)*4;return {bytes.at(i),bytes.at(i+1),bytes.at(i+2),bytes.at(i+3)};}
bool near(Color a,Color b){return std::abs(int(a.r)-b.r)<=3&&std::abs(int(a.g)-b.g)<=3&&std::abs(int(a.b)-b.b)<=3&&std::abs(int(a.a)-b.a)<=3;}
void test(){
    RendererOptions options;options.font.family="Menlo";options.background_opacity=.9f;options.image_cache_bytes=4096;
    Renderer renderer(nullptr,options);renderer.resize(256,160);
    auto frame=std::make_shared<RenderFrame>();frame->generation=1;frame->columns=12;frame->rows=4;frame->cell_width=16;frame->cell_height=24;
    frame->background={23,25,29,255};frame->foreground={236,238,242,255};
    for(unsigned r=0;r<4;++r){auto row=std::make_shared<FrameRow>();row->id.words={42,r+1};row->generation=1;row->cells.resize(12);
        for(auto& c:row->cells){c.background=frame->background;c.foreground=frame->foreground;c.underline_color=frame->foreground;}frame->lines.push_back(row);}
    auto row=std::make_shared<FrameRow>(*frame->lines[0]);row->cells[0].grapheme="A";row->cells[1].grapheme="e\xCC\x81";
    row->cells[2].grapheme="界";row->cells[2].span=2;row->cells[3].span=0;
    row->cells[4].background={210,30,40,255};row->cells[4].background_is_default=false;frame->lines[0]=row;
    auto unicode=std::make_shared<FrameRow>(*frame->lines[1]);unicode->cells[0].grapheme="👩‍💻";unicode->cells[0].span=2;unicode->cells[1].span=0;frame->lines[1]=unicode;
    auto image=std::make_shared<ImagePixels>();image->image_id=1;image->generation=1;image->width=2;image->height=1;image->bytes={255,0,0,255,0,80,240,255};
    ImagePlacement placement;placement.pixels=image;placement.column=8;placement.row=2;placement.pixel_width=16;placement.pixel_height=24;placement.source_x=1;placement.source_width=1;placement.source_height=1;
    frame->images.push_back(placement);
    auto below=placement;below.column=6;below.row=3;below.layer=ImageLayer::below_background;frame->images.push_back(below);frame->image_placements_generation=1;
    WindowScene scene;scene.panes.push_back({7,{16,24,192,96},frame});
    require(renderer.render(scene)==RenderResult::presented,"First offscreen render did not submit");auto rgba=renderer.read_pixels();
    require(rgba.size()==256u*160u*4u,"Metal readback size");require(near(pixel(rgba,88,36),{210,30,40,255}),"Opaque explicit ANSI background");
    require(std::abs(int(pixel(rgba,24,108).a)-230)<=2,"Default background translucency");
    require(near(pixel(rgba,152,84),{0,80,240,255}),"Cropped image placement");
    require(near(pixel(rgba,120,108),{0,80,240,255}),"Below-background image must remain visible over default background");
    require(std::abs(int(pixel(rgba,24,108).r)-23)<=3,"Readback must be straight, not premultiplied RGBA");
    unsigned ink=0;for(unsigned y=24;y<48;++y)for(unsigned x=16;x<32;++x)if(pixel(rgba,x,y).a>240)++ink;require(ink>4,"CoreText produced no opaque ink");
    const auto stats=renderer.stats();require(stats.shaped_rows==4,"Initial dirty-row count");
    require(renderer.render(scene)==RenderResult::unchanged,"Unchanged scene redrawn");
    auto changed=std::make_shared<RenderFrame>(*frame);++changed->generation;auto changed_row=std::make_shared<FrameRow>(*frame->lines[1]);++changed_row->generation;changed_row->cells[3].grapheme="B";changed->lines[1]=changed_row;
    scene.panes[0].frame=changed;renderer.render(scene);renderer.read_pixels();require(renderer.stats().shaped_rows==stats.shaped_rows+1,"One-row mutation invalidated unrelated rows");
    scene.panes[0].cursor_on=false;renderer.render(scene);renderer.read_pixels();require(renderer.stats().shaped_rows==stats.shaped_rows+1,"Cursor toggle reshaped row text");
    renderer.set_background_opacity(1);renderer.render(scene);require(pixel(renderer.read_pixels(),24,108).a==255,"Opaque fallback");
    renderer.set_background_opacity(.9f);ChromePrimitive material;
    material.rect={0,0,256,16};material.fill={23,25,29,204};material.background_replace=true;
    scene.primitives={material};++scene.chrome_generation;renderer.render(scene);
    require(std::abs(int(pixel(renderer.read_pixels(),100,8).a)-204)<=1,"Chrome background replacement accumulated canvas alpha");
    scene.primitives[0].background_replace=false;++scene.chrome_generation;renderer.render(scene);
    require(pixel(renderer.read_pixels(),100,8).a>=248,"Ordinary chrome overlay did not source-over");
    scene.primitives.clear();++scene.chrome_generation;
    const auto old=renderer.metrics();renderer.resize(256,160,192);require(renderer.metrics().width>old.width,"Retina font metrics");
    renderer.recreate_device();renderer.render(scene);renderer.read_pixels();require(renderer.stats().device_recoveries==1,"Metal resource recreation");
    renderer.resize(0,0);require(renderer.render(scene)==RenderResult::suspended,"Minimized renderer did not suspend");
    require(renderer.stats().image_cache_bytes<=options.image_cache_bytes,"Image cache exceeded budget");
}
}
int main(){@autoreleasepool{try{test();std::cout<<"PASS native macOS CoreText/Metal offscreen raster/composition/readback\n";return 0;}
catch(const std::exception& e){std::cerr<<"FAIL macOS renderer: "<<e.what()<<'\n';return 1;}}}
