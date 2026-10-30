#pragma once

#include "stormtty/renderer.hpp"

namespace stormtty {

// Physical-pixel coverage shared by canvas drawing and retained-scene damage.
namespace render_geometry {
PixelRect intersect(PixelRect a, PixelRect b) noexcept;
bool nonempty(PixelRect rect) noexcept;
bool overlaps(PixelRect a, PixelRect b) noexcept;
PixelRect chrome_clip(PixelRect bounds, const std::optional<PixelRect>& viewport) noexcept;
PixelRect chrome_bounds(const ChromeText& text) noexcept;
PixelRect chrome_bounds(const ChromePrimitive& primitive) noexcept;
PixelRect cursor_bounds(const RenderPane& pane, CellMetrics metrics) noexcept;
}

// Renderer-thread-confined owner of the scene represented by the persistent
// canvas. Retains owned immutable frames, never GPU resources or presentation
// state. A busy/occluded Present does not roll back a successful canvas draw.
class SceneDamageTracker final {
public:
    std::vector<PixelRect> damage(const WindowScene& scene, CellMetrics metrics,
                                  PixelRect surface) const;

    // Call after a successful EndDraw, before Present, or for an unchanged scene
    // with no pending presentation. force_redraw is a request, not retained state.
    void commit(const WindowScene& scene);

    // Surface/font/material/device changes and draw-body exceptions require repaint.
    // Keeps the previous owned scene alive until a subsequent successful commit.
    void invalidate() noexcept;

private:
    WindowScene previous_;
    bool invalid_{true};
};

} // namespace stormtty
