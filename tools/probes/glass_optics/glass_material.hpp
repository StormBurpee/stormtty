#pragma once
#include <array>
#include <cstddef>
#include <vector>

namespace stormtty::glass_optics {
struct Vec2 { float x{}, y{}; };
// All geometry is in physical pixels. Convert DIP geometry once, before creation.
struct MaterialConfig {
    Vec2 center{64,48};
    Vec2 size{96,64};
    float corner_radius{16};
    float rim_width{12};
    float thickness{8};
    float refractive_index{1.5f};
    float maximum_displacement{12};
    float blur_radius{0}; // Nine bounded taps, not a Gaussian-quality claim.
    float highlight_strength{0.12f};
    bool enabled{true};
};
struct alignas(16) ShaderParameters {
    std::array<float,4> geometry; // center.xy, size.xy
    std::array<float,4> shape; // radius, rim, thickness, index
    std::array<float,4> effects; // displacement cap, blur, highlight, enabled
    std::array<float,4> source; // physical source width/height, reserved zeros
};
static_assert(sizeof(ShaderParameters)==64);
struct OpticalSample {
    Vec2 source_pixel{};
    std::array<float,3> normal{0,0,1};
    float height{};
    float highlight{};
    float coverage{}; // One physical pixel of smooth silhouette antialiasing.
    bool inside{};
};
// Immutable parameters. No clock, randomness, history, capture, or desktop access.
class GlassMaterial final {
public:
    explicit GlassMaterial(MaterialConfig config);
    const MaterialConfig& config() const noexcept { return config_; }
    OpticalSample sample(Vec2 pixel) const;
    ShaderParameters shader_parameters(unsigned width,unsigned height) const;
    static MaterialConfig from_dip(MaterialConfig config, float scale);
private:
    MaterialConfig config_;
};
// Linear-light premultiplied RGBA. The reference uses only explicitly supplied data.
using Color = std::array<float,4>;
class Image final {
public:
    Image(unsigned width, unsigned height, Color fill = {0,0,0,1});
    unsigned width() const noexcept { return width_; }
    unsigned height() const noexcept { return height_; }
    const std::vector<Color>& pixels() const noexcept { return pixels_; }
    Color& at(unsigned x,unsigned y);
    const Color& at(unsigned x,unsigned y) const;
private:
    unsigned width_,height_;
    std::vector<Color> pixels_;
};
class GlassReferenceRenderer final {
public:
    // Returns a distinct destination. Source is never altered or sampled from output.
    static Image render(const Image& source, const GlassMaterial& material);
    // Labels/glyph masks are applied AFTER optics; this material never distorts them.
    static void overlay(Image& destination, const Image& foreground);
};
}
