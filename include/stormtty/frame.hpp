#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace stormtty {
struct Color {
    std::uint8_t r{}, g{}, b{}, a{255};
    bool operator==(const Color&) const = default;
};
struct CellStyle {
    bool bold{}, italic{}, faint{}, blink{}, inverse{}, invisible{}, strikethrough{}, overline{};
    // 0 none; 1 single; 2 double; 3 curly; 4 dotted; 5 dashed.
    std::uint8_t underline{};
    bool operator==(const CellStyle&) const = default;
};
struct Cell {
    std::string grapheme; // Owned UTF-8, empty for blank/continuation cells.
    Color foreground{}, background{}, underline_color{};
    CellStyle style{};
    std::uint8_t span{1}; // 2 wide lead, 0 continuation/wrap spacer, 1 ordinary.
    bool image_placeholder{}; // Kitty U+10EEEE: reserve cell but do not draw a glyph.
    bool background_is_default{true}; // Explicit RGB/index backgrounds stay opaque.
    bool selected{}, hyperlink{}; // URI opening is not supported by this boundary yet.
    bool operator==(const Cell&) const = default;
};
struct RowId {
    std::array<std::uint64_t, 2> words{};
    bool operator==(const RowId&) const = default;
};
struct SelectionRange {
    std::uint16_t first{}, last{};
    bool operator==(const SelectionRange&) const = default;
}; // Inclusive columns.
struct FrameRow {
    RowId id{};
    std::uint64_t generation{};
    std::optional<SelectionRange> selection;
    std::vector<Cell> cells;
};
enum class CursorShape { block, bar, underline, hollow_block };
struct FrameCursor {
    std::uint16_t column{}, row{};
    bool visible{}, blinking{}, wide_tail{}, password_input{};
    CursorShape shape{CursorShape::block};
    Color color{};
    bool operator==(const FrameCursor&) const = default;
};
enum class PixelFormat { rgb, rgba, gray_alpha, gray };
struct ImagePixels {
    std::uint32_t image_id{}, width{}, height{};
    std::uint64_t generation{};
    PixelFormat format{PixelFormat::rgba};
    std::vector<std::uint8_t> bytes; // Owned, tightly packed, straight alpha.
};
enum class ImageLayer { below_background, below_text, above_text };
enum class ImagePlacementKind { fixed, virtual_placeholder, relative };
struct ImagePlacement {
    std::shared_ptr<const ImagePixels> pixels;
    std::uint32_t placement_id{};
    ImagePlacementKind kind{ImagePlacementKind::fixed};
    ImageLayer layer{ImageLayer::above_text};
    std::int32_t z{}, column{}, row{};
    std::uint32_t columns{}, rows{}, offset_x{}, offset_y{};
    std::uint32_t pixel_width{}, pixel_height{};
    std::uint32_t source_x{}, source_y{}, source_width{}, source_height{};
};
// Frames are published as shared_ptr<const RenderFrame>; no Ghostty pointer escapes.
// Row vector positions are viewport row positions. Rows/pixels shared with prior
// frames stay valid after feed, resize, alternate-buffer switches, or destruction.
struct RenderFrame {
    std::uint64_t generation{}, image_placements_generation{};
    std::uint16_t columns{}, rows{}, cell_width{}, cell_height{};
    Color foreground{}, background{};
    FrameCursor cursor{};
    bool alternate_screen{}, mouse_reporting{};
    std::vector<std::shared_ptr<const FrameRow>> lines;
    std::vector<ImagePlacement> images; // Sorted by layer, then ascending z.
};
using FramePtr = std::shared_ptr<const RenderFrame>;
} // namespace stormtty
