#pragma once

#include "stormtty/renderer.hpp"

namespace stormtty::windows {

class PaintSurface;
class TextResources;

// Rasterizes already ordered chrome; owns no scene, cache or native resources.
class ChromePainter final {
public:
    void paint(
        std::span<const ChromePrimitive> primitives,
        std::span<const ChromeText> labels,
        std::span<const ChromeDrawItem> order,
        PixelRect damage_px,
        PixelRect viewport_px,
        bool translucent_material,
        PaintSurface& surface,
        const TextResources& text) const;
};

} // namespace stormtty::windows
