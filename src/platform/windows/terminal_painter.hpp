#pragma once

#include "stormtty/renderer.hpp"

namespace stormtty::windows {

class ImageCache;
class PaintSurface;
class TextResources;

// Terminal paint policy only. All inputs are synchronous borrows; resource
// owners, damage/presentation transactions and cumulative counters stay outside.
class TerminalPainter final {
public:
    void paint(
        const RenderPane& pane,
        PixelRect damage_px,
        float dpi,
        float backdrop_opacity,
        std::uint64_t render_tick,
        PaintSurface& surface,
        TextResources& text,
        ImageCache& images) const;
};

} // namespace stormtty::windows
