#include "terminal_painter.hpp"
#include "metal_compositor.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace stormtty::macos {

TerminalPainter::TerminalPainter(MetalFrame& frame, const MetalTextureResources& textures,
                                 CTFontRef font, std::string_view locale, CellMetrics metrics,
                                 float dpi, float background_opacity,
                                 std::uint64_t font_generation, bool ligatures) noexcept
    : frame_(frame), textures_(textures), font_(font), locale_(locale), metrics_(metrics),
      dpi_(dpi), background_opacity_(background_opacity),
      font_generation_(font_generation), ligatures_(ligatures) {}

void TerminalPainter::draw(const RenderPane& pane) {
    if (!pane.frame || !frame_.clip(pane.content)) return;

    const auto& terminal = *pane.frame;
    if (terminal.lines.size() > 4096 || terminal.images.size() > 1024) {
        throw std::runtime_error("Terminal frame exceeds renderer row/placement limits");
    }
    const float cell_width = terminal.cell_width ? terminal.cell_width : metrics_.width;
    const float cell_height = terminal.cell_height ? terminal.cell_height : metrics_.height;

    // Replacement prevents accumulated opacity over the scene or previous pane.
    frame_.fill(pane.content, {0, 0, 0, 0}, true);
    Color background = terminal.background;
    background.a = static_cast<std::uint8_t>(std::lround(background_opacity_ * 255));
    frame_.fill(pane.content, background);
    draw_images(pane, ImageLayer::below_background, cell_width, cell_height);
    draw_backgrounds(pane, cell_width, cell_height);
    draw_images(pane, ImageLayer::below_text, cell_width, cell_height);
    draw_rows(pane, cell_width, cell_height);
    draw_images(pane, ImageLayer::above_text, cell_width, cell_height);
    draw_cursor(pane, cell_width, cell_height);
}

void TerminalPainter::draw_backgrounds(const RenderPane& pane,
                                       float cell_width, float cell_height) {
    const auto& lines = pane.frame->lines;
    for (std::size_t row_index = 0; row_index < lines.size(); ++row_index) {
        if (!lines[row_index]) continue;
        const auto& row = *lines[row_index];
        for (std::size_t column = 0; column < row.cells.size(); ++column) {
            const auto& cell = row.cells[column];
            Color background = cell.style.inverse ? cell.foreground : cell.background;
            if (cell.selected) background = {58, 79, 91, 255};
            if (!cell.background_is_default || cell.style.inverse || cell.selected) {
                background.a = 255;
                frame_.fill({pane.content.x + column * cell_width,
                             pane.content.y + row_index * cell_height,
                             cell_width, cell_height}, background);
            }
        }
    }
}

void TerminalPainter::draw_images(const RenderPane& pane, ImageLayer layer,
                                  float cell_width, float cell_height) {
    for (const auto& placement : pane.frame->images) {
        if (placement.layer != layer || !placement.pixels) continue;
        const auto& image = *placement.pixels;
        const auto source_width = placement.source_width ? placement.source_width : image.width;
        const auto source_height = placement.source_height ? placement.source_height : image.height;
        if (placement.source_x >= image.width || placement.source_y >= image.height ||
            source_width > image.width - placement.source_x ||
            source_height > image.height - placement.source_y) continue;

        const float width = placement.pixel_width ? placement.pixel_width :
                            placement.columns ? placement.columns * cell_width : source_width;
        const float height = placement.pixel_height ? placement.pixel_height :
                             placement.rows ? placement.rows * cell_height : source_height;
        const PixelRect destination{
            pane.content.x + placement.column * cell_width + placement.offset_x,
            pane.content.y + placement.row * cell_height + placement.offset_y, width, height};
        if (destination.x + destination.width <= pane.content.x ||
            destination.y + destination.height <= pane.content.y ||
            destination.x >= pane.content.x + pane.content.width ||
            destination.y >= pane.content.y + pane.content.height) continue;

        frame_.draw_image(pane.id, image, destination,
                          {float(placement.source_x) / image.width,
                           float(placement.source_y) / image.height,
                           float(source_width) / image.width,
                           float(source_height) / image.height});
    }
}

void TerminalPainter::draw_rows(const RenderPane& pane, float cell_width, float cell_height) {
    const auto& terminal = *pane.frame;
    for (std::size_t row_index = 0; row_index < terminal.lines.size(); ++row_index) {
        if (!terminal.lines[row_index] ||
            !(pane.content.y + row_index * cell_height < pane.content.y + pane.content.height)) continue;
        const auto width = static_cast<std::uint32_t>(std::ceil(terminal.columns * cell_width));
        const auto height = static_cast<std::uint32_t>(std::ceil(cell_height));
        if (width && height) {
            draw_row(pane, *terminal.lines[row_index], width, height,
                     {pane.content.x, pane.content.y + row_index * cell_height,
                      float(width), float(height)});
        }
    }
}

void TerminalPainter::draw_row(const RenderPane& pane, const FrameRow& row,
                               std::uint32_t width, std::uint32_t height,
                               PixelRect destination, std::uint32_t variant) {
    // Read after prior rows/images: the texture owner, not this painter, charges
    // uploads (including failures) and owns the aggregate frame allowance.
    const RowRasterOptions options{
        width, height, pane.frame->cell_width ? pane.frame->cell_width : float(metrics_.width),
        metrics_, dpi_, pane.text_blink_on, ligatures_, locale_, textures_.remaining_upload_bytes()};
    const RowTextureIdentity identity{pane.id, font_generation_, variant};
    frame_.draw_row(identity, row, font_, options, destination);
}

void TerminalPainter::draw_cursor(const RenderPane& pane, float cell_width, float cell_height) {
    const auto& terminal = *pane.frame;
    const auto& cursor = terminal.cursor;
    if (!cursor.visible || !pane.cursor_on ||
        cursor.column >= terminal.columns || cursor.row >= terminal.rows) return;

    auto column = cursor.column;
    const FrameRow* row = cursor.row < terminal.lines.size() && terminal.lines[cursor.row]
                             ? terminal.lines[cursor.row].get() : nullptr;
    if (column && row && column < row->cells.size() &&
        (!row->cells[column].span || cursor.wide_tail)) --column;
    const unsigned span = row && column < row->cells.size()
                              ? std::max(1u, unsigned(row->cells[column].span)) : 1;
    PixelRect rect{pane.content.x + column * cell_width,
                   pane.content.y + cursor.row * cell_height, cell_width * span, cell_height};
    Color color = cursor.color;
    color.a = 255;
    const float line = std::max(1.f, dpi_ / 96.f);
    if (cursor.shape == CursorShape::bar) {
        rect.width = line;
    } else if (cursor.shape == CursorShape::underline) {
        rect.y += rect.height - line;
        rect.height = line;
    }
    if (!pane.focused || cursor.shape == CursorShape::hollow_block) {
        frame_.fill({rect.x, rect.y, rect.width, line}, color);
        frame_.fill({rect.x, rect.y + rect.height - line, rect.width, line}, color);
        frame_.fill({rect.x, rect.y, line, rect.height}, color);
        frame_.fill({rect.x + rect.width - line, rect.y, line, rect.height}, color);
        return;
    }

    frame_.fill(rect, color);
    if (cursor.shape != CursorShape::block || !row || column >= row->cells.size()) return;

    FrameRow inverted = *row;
    auto& cell = inverted.cells[column];
    cell.foreground = cell.style.inverse ? cell.foreground : cell.background;
    cell.style.inverse = false;
    cell.selected = false;
    cell.style.blink = false;
    RenderPane cursor_pane = pane;
    cursor_pane.text_blink_on = true;
    const auto right = std::min(rect.x + rect.width, pane.content.x + pane.content.width);
    const auto bottom = std::min(rect.y + rect.height, pane.content.y + pane.content.height);
    PixelRect clipped{std::max(rect.x, pane.content.x), std::max(rect.y, pane.content.y), 0, 0};
    clipped.width = right - clipped.x;
    clipped.height = bottom - clipped.y;
    // Deliberately leave cursor clip installed; the next pane or renderer's
    // viewport reset owns restoring it. Cursor destination remains unrounded.
    if (frame_.clip(clipped)) {
        draw_row(cursor_pane, inverted,
                 static_cast<std::uint32_t>(std::ceil(terminal.columns * cell_width)),
                 static_cast<std::uint32_t>(std::ceil(cell_height)),
                 {pane.content.x, pane.content.y + cursor.row * cell_height,
                  terminal.columns * cell_width, cell_height}, column + 1);
    }
}

} // namespace stormtty::macos
