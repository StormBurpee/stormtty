#pragma once

#include "stormtty/renderer.hpp"
#include <CoreText/CoreText.h>
#include <string_view>

namespace stormtty::macos {

class MetalFrame;
class MetalTextureResources;

// Synchronous, nonescaping borrow for an already admitted frame. The caller
// keeps the font and locale alive and unchanged through draw(). This painter
// owns neither native resources nor cache, frame lifecycle or scene state.
class TerminalPainter final {
public:
    TerminalPainter(MetalFrame& frame, const MetalTextureResources& textures,
                    CTFontRef font, std::string_view locale, CellMetrics metrics,
                    float dpi, float background_opacity,
                    std::uint64_t font_generation, bool ligatures) noexcept;

    void draw(const RenderPane& pane);

private:
    void draw_backgrounds(const RenderPane& pane, float cell_width, float cell_height);
    void draw_images(const RenderPane& pane, ImageLayer layer,
                     float cell_width, float cell_height);
    void draw_rows(const RenderPane& pane, float cell_width, float cell_height);
    void draw_row(const RenderPane& pane, const FrameRow& row,
                  std::uint32_t width, std::uint32_t height,
                  PixelRect destination, std::uint32_t variant = 0);
    void draw_cursor(const RenderPane& pane, float cell_width, float cell_height);

    MetalFrame& frame_;
    const MetalTextureResources& textures_;
    CTFontRef font_;
    std::string_view locale_;
    CellMetrics metrics_;
    float dpi_;
    float background_opacity_;
    std::uint64_t font_generation_;
    bool ligatures_;
};

} // namespace stormtty::macos
