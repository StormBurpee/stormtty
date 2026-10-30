#include "glass_material.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace stormtty::glass_optics {
namespace {
constexpr float pi=3.14159265358979323846f;
void require(bool valid,const char* message) { if(!valid) throw std::invalid_argument(message); }
bool finite(float n) { return std::isfinite(n); }
float sign(float n) { return n<0 ? -1.f : 1.f; }
float length(Vec2 n) { return std::hypot(n.x,n.y); }
float clamp(float n,float lo,float hi) { return std::clamp(n,lo,hi); }
void valid_color(const Color& color) {
    require(finite(color[3])&&color[3]>=0&&color[3]<=1,"Invalid source alpha");
    for(unsigned c=0;c<3;++c)require(finite(color[c])&&color[c]>=0&&color[c]<=color[3],"Invalid premultiplied source color");
}
void valid_image(const Image& image) { for(const auto& pixel:image.pixels()) valid_color(pixel); }
Color sample_image(const Image& image,Vec2 pixel) {
    // Texel-center convention: first pixel center is (0.5,0.5).
    const float x=clamp(pixel.x-.5f,0.f,float(image.width()-1));
    const float y=clamp(pixel.y-.5f,0.f,float(image.height()-1));
    const auto x0=unsigned(std::floor(x)), y0=unsigned(std::floor(y));
    const auto x1=std::min(x0+1,image.width()-1),y1=std::min(y0+1,image.height()-1);
    const float tx=x-float(x0),ty=y-float(y0);
    Color result{};
    for(unsigned c=0;c<4;++c) {
        const float a=std::lerp(image.at(x0,y0)[c],image.at(x1,y0)[c],tx);
        const float b=std::lerp(image.at(x0,y1)[c],image.at(x1,y1)[c],tx);
        result[c]=std::lerp(a,b,ty);
    }
    return result;
}
Color blurred(const Image& image,Vec2 position,float radius) {
    if(radius==0) return sample_image(image,position);
    // Separable tent weights in a 3x3 footprint (exactly nine taps).
    constexpr float weights[3]={.25f,.5f,.25f};
    Color result{};
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        const auto tap=sample_image(image,{position.x+float(x)*radius,position.y+float(y)*radius});
        const float weight=weights[x+1]*weights[y+1];
        for(unsigned c=0;c<4;++c) result[c]+=tap[c]*weight;
    }
    return result;
}
}
GlassMaterial::GlassMaterial(MaterialConfig c):config_(c) {
    require(finite(c.center.x)&&finite(c.center.y)&&std::abs(c.center.x)<=65536&&std::abs(c.center.y)<=65536,"Invalid material center");
    require(finite(c.size.x)&&finite(c.size.y)&&c.size.x>=2&&c.size.y>=2&&c.size.x<=16384&&c.size.y<=16384,"Invalid material size");
    require(finite(c.corner_radius)&&c.corner_radius>=.25f&&c.corner_radius<=std::min(c.size.x,c.size.y)*.5f,"Invalid corner radius");
    require(finite(c.rim_width)&&c.rim_width>=.25f&&c.rim_width<=c.corner_radius,"Invalid rim width");
    require(finite(c.thickness)&&c.thickness>=0&&c.thickness<=64,"Invalid optical thickness");
    require(finite(c.refractive_index)&&c.refractive_index>=1&&c.refractive_index<=2,"Invalid refractive index");
    require(finite(c.maximum_displacement)&&c.maximum_displacement>=0&&c.maximum_displacement<=64,"Invalid displacement cap");
    require(finite(c.blur_radius)&&c.blur_radius>=0&&c.blur_radius<=16,"Invalid blur radius");
    require(finite(c.highlight_strength)&&c.highlight_strength>=0&&c.highlight_strength<=.3f,"Invalid highlight strength");
}
MaterialConfig GlassMaterial::from_dip(MaterialConfig c,float scale) {
    require(finite(scale)&&scale>=.5f&&scale<=4,"Invalid DPI scale");
    c.center={c.center.x*scale,c.center.y*scale}; c.size={c.size.x*scale,c.size.y*scale};
    c.corner_radius*=scale;c.rim_width*=scale;c.thickness*=scale;
    c.maximum_displacement*=scale;c.blur_radius*=scale;
    return GlassMaterial(c).config(); // Revalidate scaled bounds.
}
ShaderParameters GlassMaterial::shader_parameters(unsigned width,unsigned height) const {
    require(width&&height&&width<=4096&&height<=4096&&std::uint64_t(width)*height<=4194304,"Shader source exceeds budget");
    const auto& c=config_;
    return {{c.center.x,c.center.y,c.size.x,c.size.y},
            {c.corner_radius,c.rim_width,c.thickness,c.refractive_index},
            {c.maximum_displacement,c.blur_radius,c.highlight_strength,c.enabled?1.f:0.f},
            {float(width),float(height),0,0}};
}
OpticalSample GlassMaterial::sample(Vec2 pixel) const {
    require(finite(pixel.x)&&finite(pixel.y)&&std::abs(pixel.x)<=65536&&std::abs(pixel.y)<=65536,"Invalid sample position");
    OpticalSample result; result.source_pixel=pixel;
    const auto& c=config_;
    if(!c.enabled) return result;
    const Vec2 local{pixel.x-c.center.x,pixel.y-c.center.y};
    const Vec2 q{std::abs(local.x)-(c.size.x*.5f-c.corner_radius),std::abs(local.y)-(c.size.y*.5f-c.corner_radius)};
    const Vec2 outside{std::max(q.x,0.f),std::max(q.y,0.f)};
    const float outer_length=length(outside);
    const float distance=outer_length+std::min(std::max(q.x,q.y),0.f)-c.corner_radius;
    if(distance>=0) return result;
    result.inside=true;
    const float coverage=clamp(-distance,0.f,1.f);
    result.coverage=coverage*coverage*(3-2*coverage);
    Vec2 outward{};
    if(outer_length>1e-6f) outward={outside.x/outer_length*sign(local.x),outside.y/outer_length*sign(local.y)};
    else if(q.x>q.y) outward={sign(local.x),0};
    else outward={0,sign(local.y)};
    const float t=clamp(-distance/c.rim_width,0.f,1.f);
    result.height=c.thickness*std::sin(t*pi*.5f);
    // A flat interior meets a rounded height ramp with zero slope at the seam.
    const float slope=t>=1 ? 0.f : c.thickness*pi/(2*c.rim_width)*std::cos(t*pi*.5f);
    const float inv=1/std::sqrt(1+slope*slope);
    result.normal={outward.x*slope*inv,outward.y*slope*inv,inv};
    // Snell refraction of an orthographic incident ray (0,0,-1) into glass.
    // Single-interface height-field approximation; not Apple's proprietary optics.
    const float eta=1/c.refractive_index,nz=result.normal[2];
    const float k=std::max(0.f,1-eta*eta*(1-nz*nz));
    const float coefficient=eta*nz-std::sqrt(k);
    const float rz=-eta+coefficient*nz;
    Vec2 shift{result.normal[0]*coefficient*result.height/std::max(-rz,.01f),
               result.normal[1]*coefficient*result.height/std::max(-rz,.01f)};
    const float magnitude=length(shift);
    if(magnitude>c.maximum_displacement && magnitude>0) {
        shift.x*=c.maximum_displacement/magnitude;shift.y*=c.maximum_displacement/magnitude;
    }
    result.source_pixel={pixel.x+shift.x,pixel.y+shift.y};
    // Fixed top-left light; restrained Fresnel-like rim response, no perpetual motion.
    const float facing=std::max(0.f,-.6f*result.normal[0]-.8f*result.normal[1]);
    result.highlight=c.highlight_strength*(1-nz)*facing;
    return result;
}
Image::Image(unsigned w,unsigned h,Color fill):width_(w),height_(h) {
    require(w&&h&&w<=4096&&h<=4096&&std::uint64_t(w)*h<=4194304,"Image exceeds reference budget");
    valid_color(fill);
    pixels_.assign(std::size_t(w)*h,fill);
}
Color& Image::at(unsigned x,unsigned y) { if(x>=width_||y>=height_) throw std::out_of_range("Image pixel"); return pixels_[std::size_t(y)*width_+x]; }
const Color& Image::at(unsigned x,unsigned y)const { if(x>=width_||y>=height_) throw std::out_of_range("Image pixel"); return pixels_[std::size_t(y)*width_+x]; }
Image GlassReferenceRenderer::render(const Image& source,const GlassMaterial& material) {
    valid_image(source);
    if(!material.config().enabled) return source; // Exact byte-for-byte pixel identity; no sampling.
    Image output=source;
    for(unsigned y=0;y<source.height();++y) for(unsigned x=0;x<source.width();++x) {
        const auto optics=material.sample({float(x)+.5f,float(y)+.5f});
        if(!optics.inside) continue;
        auto value=blurred(source,optics.source_pixel,material.config().blur_radius);
        for(unsigned c=0;c<3;++c) value[c]=std::lerp(value[c],value[3],optics.highlight);
        for(unsigned channel=0;channel<4;++channel)
            value[channel]=std::lerp(source.at(x,y)[channel],value[channel],optics.coverage);
        output.at(x,y)=value;
    }
    return output;
}
void GlassReferenceRenderer::overlay(Image& destination,const Image& foreground) {
    require(destination.width()==foreground.width()&&destination.height()==foreground.height(),"Foreground extent mismatch");
    valid_image(destination);valid_image(foreground);
    for(unsigned y=0;y<destination.height();++y) for(unsigned x=0;x<destination.width();++x) {
        const auto top=foreground.at(x,y);auto& bottom=destination.at(x,y);
        for(unsigned c=0;c<4;++c) bottom[c]=top[c]+bottom[c]*(1-top[3]);
    }
}
}
