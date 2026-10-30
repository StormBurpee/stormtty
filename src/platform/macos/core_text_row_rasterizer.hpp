#pragma once

#include "stormtty/frame.hpp"
#include "stormtty/renderer.hpp"
#include <CoreText/CoreText.h>
#include <cstddef>
#include <span>
#include <string_view>

namespace stormtty::macos {

struct RowRasterOptions {
    std::uint32_t width{};
    std::uint32_t height{};
    float cell_width{};
    CellMetrics metrics;
    float dpi{};
    bool text_blink_on{};
    bool ligatures{};
    std::string_view locale;
    std::size_t maximum_output_bytes{};
};

struct RasterizedRow {
    std::uint32_t width{};
    std::uint32_t height{};
    // Owned, tightly packed sRGB BGRA, premultiplied alpha, transparent canvas.
    std::vector<std::uint8_t> bgra;
};

// CPU-only synchronous rasterization. Owns no cache, font or platform host.
class CoreTextRowRasterizer final {
public:
    // Cells, font and locale are borrowed only for this call. The caller keeps
    // the font alive without reset and supplies validated renderer geometry/DPI.
    // maximum_output_bytes is an allowance snapshot, not a consumed reservation;
    // the renderer alone accounts for the eventual texture upload.
    RasterizedRow rasterize(std::span<const Cell> cells, CTFontRef borrowed_font,
                            const RowRasterOptions& options) const;
};

} // namespace stormtty::macos
