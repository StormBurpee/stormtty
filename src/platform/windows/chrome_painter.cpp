#include "chrome_painter.hpp"

#include "paint_surface.hpp"
#include "text_resources.hpp"
#include "stormtty/scene_damage.hpp"

#include <algorithm>
#include <cmath>

namespace stormtty::windows {

void ChromePainter::paint(
    std::span<const ChromePrimitive> primitives,
    std::span<const ChromeText> labels,
    std::span<const ChromeDrawItem> order,
    PixelRect damage_px,
    PixelRect viewport_px,
    bool translucent_material,
    PaintSurface& surface,
    const TextResources& text) const {
    using namespace render_geometry;

    for (const auto& item : order) {
        if (item.kind == ChromeDrawKind::primitive) {
            const auto& primitive = primitives[item.index];
            if (!overlaps(chrome_bounds(primitive), damage_px)) {
                continue;
            }
            auto viewport = surface.clip(chrome_clip(viewport_px, primitive.clip));
            if (primitive.kind == ChromePrimitiveKind::line) {
                surface.line(
                    primitive.rect,
                    primitive.stroke_width > 0 ? primitive.stroke : primitive.fill,
                    std::max(1.0f, primitive.stroke_width));
            } else {
                const auto radius = std::clamp(
                    primitive.radius, 0.0f,
                    std::min(primitive.rect.width, primitive.rect.height) * .5f);
                if (primitive.fill.a || primitive.background_replace) {
                    auto fill_color = primitive.fill;
                    if (primitive.background_replace && !translucent_material) {
                        fill_color.a = 255;
                    }
                    surface.fill_rounded(
                        primitive.rect, radius, fill_color, primitive.background_replace);
                }
                if (primitive.stroke_width > 0 && primitive.stroke.a) {
                    surface.stroke_rounded(
                        primitive.rect, radius, primitive.stroke, primitive.stroke_width);
                }
            }
        } else {
            const auto& label = labels[item.index];
            if (!nonempty(label.rect) || !overlaps(chrome_bounds(label), damage_px) ||
                label.text.empty() || !label.color.a) {
                continue;
            }
            if (label.text.size() > 32768 || label.family.empty() || label.family.size() > 256 ||
                !std::isfinite(label.size_dip) || label.size_dip < 1 || label.size_dip > 256) {
                continue;
            }
            const auto layout = text.chrome_paint_layout(label);
            auto foreground = label.color;
            foreground.a = 255;
            auto clip = surface.clip(chrome_bounds(label));
            // Layer opacity applies to both monochrome and color-font layers.
            // Terminal rows are never faded by deliberate chrome text alpha.
            auto opacity = surface.opacity(
                intersect(label.rect, viewport_px), float(label.color.a) / 255);
            surface.text_layout(label.rect, *layout.get(), foreground);
        }
    }
}

} // namespace stormtty::windows
