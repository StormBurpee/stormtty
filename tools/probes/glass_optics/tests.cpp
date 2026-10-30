#include "glass_material.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace stormtty::glass_optics;
namespace {
void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
bool near(float a,float b,float eps=1e-5f) { return std::abs(a-b)<=eps; }
template<class F>void rejects(F test) { bool rejected=false;try {test();}catch(const std::invalid_argument&){rejected=true;}check(rejected,"Invalid input accepted"); }
Image grid(unsigned w,unsigned h) {
    Image image(w,h);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)
        image.at(x,y)={float((x/3+y/5)%2),float(x)/float(w-1),float(y)/float(h-1),1};
    return image;
}
void geometry() {
    GlassMaterial glass({});
    const auto center=glass.sample({64,48});
    check(center.inside&&center.source_pixel.x==64&&center.source_pixel.y==48,"Center displaced");
    check(center.normal==std::array<float,3>{0,0,1}&&center.highlight==0,"Center is not flat");
    const auto outside=glass.sample({1,1});check(!outside.inside&&outside.source_pixel.x==1,"Outside altered");
    const auto boundary=glass.sample({16,48});check(!boundary.inside,"Boundary classification");
    const auto rim=glass.sample({20,48});
    check(rim.inside&&rim.height>0&&rim.source_pixel.x>20&&near(rim.source_pixel.y,48),"Rim has no inward lensing");
    const auto opposite=glass.sample({108,48});
    check(near(rim.source_pixel.x-20,108-opposite.source_pixel.x),"Lens symmetry");
    check(rim.highlight>opposite.highlight,"Directional highlight is not geometry-aware");
    for(int y=-4;y<=100;++y)for(int x=-4;x<=132;++x) {
        const auto sample=glass.sample({float(x)+.25f,float(y)+.25f});
        check(std::isfinite(sample.source_pixel.x)&&std::isfinite(sample.source_pixel.y)&&std::isfinite(sample.height),"Nonfinite optics");
        check(std::hypot(sample.source_pixel.x-(float(x)+.25f),sample.source_pixel.y-(float(y)+.25f))<=12.0001f,"Displacement exceeds cap");
        check(near(sample.normal[0]*sample.normal[0]+sample.normal[1]*sample.normal[1]+sample.normal[2]*sample.normal[2],1),"Normal length");
        check(sample.highlight>=0&&sample.highlight<=.12f,"Highlight bound");
    }
    auto c=glass.config();c.refractive_index=1;
    const auto air=GlassMaterial(c).sample({20,48});check(near(air.source_pixel.x,20),"Index one refracts");
    c.refractive_index=1.5f;c.maximum_displacement=.1f;
    const auto capped=GlassMaterial(c).sample({20,48});check(capped.source_pixel.x-20<=.10001f,"Small displacement cap ignored");
    for(float d:{.0001f,.001f,.01f}) {
        const auto edge=glass.sample({16+d,48});check(std::abs(edge.source_pixel.x-(16+d))<d*2,"Edge displacement discontinuity");
        check(edge.highlight*edge.coverage<d*.1f,"Edge highlight discontinuity");
    }
}
void identity_and_separation() {
    auto source=grid(128,96);source.at(0,0)[0]=-0.f;const auto before=source.pixels();
    MaterialConfig c;c.enabled=false;c.blur_radius=16;
    const auto off=GlassReferenceRenderer::render(source,GlassMaterial(c));
    check(std::memcmp(off.pixels().data(),before.data(),before.size()*sizeof(Color))==0,"Off must be bit-exact source, including signed zero, even if blur is configured");
    c.enabled=true;c.blur_radius=0;c.highlight_strength=0;
    const auto displaced=GlassReferenceRenderer::render(source,GlassMaterial(c));
    check(displaced.pixels()!=before,"Displacement-only pass did nothing");
    c.maximum_displacement=0;c.blur_radius=2;
    const auto blur=GlassReferenceRenderer::render(source,GlassMaterial(c));
    check(displaced.pixels()!=blur.pixels(),"Lens is indistinguishable from blur-only");
    c.blur_radius=0;
    check(GlassReferenceRenderer::render(source,GlassMaterial(c)).pixels()==before,"Zero optics must preserve texels");
    c.maximum_displacement=12;c.highlight_strength=.12f;
    const auto first=GlassReferenceRenderer::render(source,GlassMaterial(c));
    const auto second=GlassReferenceRenderer::render(source,GlassMaterial(c));
    check(first.pixels()==second.pixels()&&source.pixels()==before,"Source changed or temporal feedback occurred");
    check(first.pixels().data()!=source.pixels().data(),"Source/output alias");
    check(first.at(1,1)==source.at(1,1),"Outside was filtered");
}
void clamp_and_labels() {
    Image source(32,24,{.1f,.2f,.3f,1});
    MaterialConfig c;c.center={1,12};c.size={32,24};c.corner_radius=8;c.rim_width=8;
    c.thickness=64;c.maximum_displacement=64;c.blur_radius=16;c.highlight_strength=0;
    auto out=GlassReferenceRenderer::render(source,GlassMaterial(c));
    for(const auto& pixel:out.pixels())for(unsigned k=0;k<4;++k)
        check(near(pixel[k],source.at(0,0)[k]),"Clamped edge sampling introduced undefined/black pixels");
    Image foreground(32,24,{0,0,0,0});
    // Authored crisp F glyph crossing the lens rim, not a raster/font dependency.
    for(unsigned y=3;y<20;++y) foreground.at(3,y)={1,1,1,1};
    for(unsigned x=3;x<18;++x) {foreground.at(x,3)={1,1,1,1};foreground.at(x,10)={1,1,1,1};}
    const auto before_overlay=out;
    GlassReferenceRenderer::overlay(out,foreground);
    for(unsigned y=0;y<24;++y)for(unsigned x=0;x<32;++x)
        if(foreground.at(x,y)[3]==1)check(out.at(x,y)==foreground.at(x,y),"Foreground glyph distorted");
    check(out.at(4,4)==before_overlay.at(4,4),"Glyph contaminated neighboring pixels");
    c.center={-20,12};c.size={64,64};c.corner_radius=24;c.rim_width=24;c.blur_radius=0;
    const auto edge_sample=GlassMaterial(c).sample({.5f,12.5f});
    check(edge_sample.source_pixel.x<.5f,"UV clamp fixture does not cross source edge");
    const auto ramp=grid(32,24);
    auto clamped=GlassReferenceRenderer::render(ramp,GlassMaterial(c));
    check(near(clamped.at(0,12)[1],0),"Source coordinate was not clamped to edge texel");
}
void alpha_and_boundary() {
    Image source(128,96,{.1f,.2f,.3f,.5f});
    MaterialConfig c;c.blur_radius=2;
    auto output=GlassReferenceRenderer::render(source,GlassMaterial(c));
    for(const auto& color:output.pixels()) {
        check(near(color[3],.5f),"Material changed alpha on constant source");
        for(unsigned k=0;k<3;++k)check(color[k]>=0&&color[k]<=color[3],"Premultiplied color escaped alpha");
    }
    Image top(128,96,{.25f,0,0,.25f});
    const auto before=output.at(20,48);
    GlassReferenceRenderer::overlay(output,top);
    check(near(output.at(20,48)[0],.25f+before[0]*.75f)&&near(output.at(20,48)[3],.625f),"Fractional foreground blend");
    Image transparent(128,96,{0,0,0,0});
    const auto zero=GlassReferenceRenderer::render(transparent,GlassMaterial(c));
    check(zero.pixels()==transparent.pixels(),"Highlight created opaque color from transparent source");
    // Vary silhouette continuously across the same texel with blur + light active.
    const auto texture=grid(128,96);
    c.center.x=64.5f;c.highlight_strength=.3f;
    const auto original=texture.at(16,48);
    for(float epsilon:{.0001f,.001f,.01f}) {
        auto near_edge=c;near_edge.center.x-=epsilon;
        const auto result=GlassReferenceRenderer::render(texture,GlassMaterial(near_edge));
        for(unsigned k=0;k<4;++k)check(std::abs(result.at(16,48)[k]-original[k])<epsilon*.05f,"Rendered boundary discontinuity");
    }
    // Medial-axis corners lie in flat interior: no refraction-direction seam.
    c.center={64,48};c.corner_radius=16;c.rim_width=16;
    for(Vec2 p: {Vec2{32-.0001f,32+.0001f},Vec2{32+.0001f,32-.0001f}}) {
        const auto sample=GlassMaterial(c).sample(p);
        check(std::hypot(sample.source_pixel.x-p.x,sample.source_pixel.y-p.y)<.001f,"Inner corner normal seam");
    }
    auto invalid=c;invalid.rim_width=17;
    rejects([&]{GlassMaterial material(invalid);});
}
void dpi_and_validation() {
    MaterialConfig c;
    for(float scale:{.5f,1.f,1.25f,1.5f,2.f,4.f}) {
        GlassMaterial scaled(GlassMaterial::from_dip(c,scale));
        const auto a=GlassMaterial(c).sample({20,48}), b=scaled.sample({20*scale,48*scale});
        check(near(b.source_pixel.x,a.source_pixel.x*scale,.0002f)&&near(b.height,a.height*scale,.0002f),"DPI optics changed shape");
        check(near(a.highlight,b.highlight),"DPI highlight changed");
    }
    c.center={100,60};c.size={160,100};
    check(GlassMaterial(c).sample(c.center).source_pixel.x==100,"Resize center stale");
    for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-1.f}) {
        auto invalid=c;invalid.thickness=bad;rejects([&]{GlassMaterial material(invalid);});
        invalid=c;invalid.blur_radius=bad;rejects([&]{GlassMaterial material(invalid);});
    }
    rejects([&]{GlassMaterial::from_dip(c,0);});
    rejects([&]{GlassMaterial(c).sample({std::numeric_limits<float>::infinity(),0});});
    rejects([]{Image too_big(4096,4096);});
    rejects([]{Image empty(0,1);});
    const auto params=GlassMaterial(c).shader_parameters(128,96);
    check(params.geometry[0]==100&&params.source[0]==128&&params.source[1]==96&&params.effects[3]==1,"Shader ABI values");
    rejects([&]{GlassMaterial(c).shader_parameters(0,1);});
    Image invalid(4,4);invalid.at(1,1)[0]=std::numeric_limits<float>::quiet_NaN();
    rejects([&]{GlassReferenceRenderer::render(invalid,GlassMaterial(c));});
    Image a(4,4),b(3,3);rejects([&]{GlassReferenceRenderer::overlay(a,b);});
}
}
int main() {
    try {geometry();identity_and_separation();clamp_and_labels();alpha_and_boundary();dpi_and_validation();
        std::cout<<"PASS deterministic lens geometry, bounds, displacement != blur, Off identity, immutable source, labels, DPI, validation\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
