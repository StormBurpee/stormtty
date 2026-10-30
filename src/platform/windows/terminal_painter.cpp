#include "terminal_painter.hpp"

#include "graphics_error.hpp"
#include "image_cache.hpp"
#include "paint_surface.hpp"
#include "text_resources.hpp"
#include "stormtty/scene_damage.hpp"

#include <algorithm>
#include <cmath>

namespace stormtty::windows {
namespace {

using render_geometry::intersect;
using render_geometry::nonempty;
using render_geometry::overlaps;

constexpr Color selection_background{58, 79, 91, 255};
constexpr Color selection_foreground{236, 238, 242, 255};

// Exists only for one pane paint. No borrowed row/bitmap or device resource is
// stored here; owners account progress immediately, including exceptional exits.
struct TerminalPaintPass {
    PaintSurface& surface;
    TextResources& text;
    ImageCache& images;
    float dpi;
    float backdrop_opacity;
    std::uint64_t tick;

    bool selected(const FrameRow& row, std::size_t column) const noexcept {
        return (column < row.cells.size() && row.cells[column].selected) ||
               (row.selection && column >= row.selection->first && column <= row.selection->last);
    }

    Color foreground(const FrameRow& row, std::size_t column) const noexcept {
        if (column >= row.cells.size()) {
            return {236, 238, 242, 255};
        }
        const auto& c = row.cells[column];
        if (selected(row, column)) {
            return selection_foreground;
        }
        auto result = c.style.inverse ? c.background : c.foreground;
        if (c.style.faint) {
            result.r /= 2;
            result.g /= 2;
            result.b /= 2;
        }
        return result;
    }

    Color background(const FrameRow& row, std::size_t column) const noexcept {
        const auto& c = row.cells[column];
        return selected(row, column) ? selection_background : (c.style.inverse ? c.foreground : c.background);
    }

    void glyph_run(const GlyphCluster& cluster, D2D1_POINT_2F origin, Color foreground) {
        if (cluster.glyphs.empty()) {
            return;
        }
        DWRITE_GLYPH_RUN run{};
        run.fontFace = cluster.face.get();
        run.fontEmSize = cluster.size;
        run.glyphCount = static_cast<UINT32>(cluster.glyphs.size());
        run.glyphIndices = cluster.glyphs.data();
        run.glyphAdvances = cluster.advances.data();
        run.glyphOffsets = cluster.offsets.data();
        run.isSideways = cluster.sideways;
        run.bidiLevel = cluster.bidi;

        if (text.supports_color_glyphs() && surface.supports_color_glyphs()) {
            Com<IDWriteColorGlyphRunEnumerator1> layers;
            const auto hr = text.color_glyph_layers(run, origin.x, origin.y, layers);
            if (SUCCEEDED(hr)) {
                BOOL available{};
                while (SUCCEEDED(layers->MoveNext(&available)) && available) {
                    const DWRITE_COLOR_GLYPH_RUN1* layer{};
                    if (FAILED(layers->GetCurrentRun(&layer)) || !layer) {
                        break;
                    }
                    // Enumerator pointers are consumed before the next MoveNext.
                    surface.color_glyph_layer(*layer, foreground);
                }
                // Preserve partial-enumerator behavior: successful translation
                // does not repaint monochrome if enumeration later stops early.
                return;
            }
            if (device_lost(hr)) {
                check(hr, "Translate color glyphs");
            }
            // Unsupported color font/API uses explicit monochrome fallback.
        }
        surface.glyph_run(origin, run, foreground);
    }

    void draw_text(
        const RenderPane& pane,
        const FrameRow& row,
        UINT row_index,
        UINT cw,
        UINT ch,
        std::optional<Color> override_color = std::nullopt) {
        ShapedRow temporary;
        const auto& shape = text.shape({pane.id, pane.frame->columns, cw, ch}, row, tick, temporary);
        const auto y = pane.content.y + float(row_index * ch);
        auto row_clip = surface.clip(
            intersect(pane.content, {pane.content.x, y, pane.content.width, float(ch)}));
        for (const auto& cluster : shape.clusters) {
            if (cluster.column >= row.cells.size()) {
                continue;
            }
            const auto left = pane.content.x + float(cluster.column * cw);
            const auto right = pane.content.x + float(cluster.end * cw);
            // At most half a cell of horizontal ink overhang, never outside row
            // or pane. Multi-cell ligatures remain one shaped glyph cluster.
            const PixelRect ink{left - cw * .5f, y, right - left + cw, float(ch)};
            auto ink_clip = surface.clip(ink);
            const D2D1_POINT_2F origin{pane.content.x + cluster.x, y + cluster.baseline};
            // Paint one cluster multiple times only when paint/selection changes
            // inside it. Each pass is clipped to exact cell backgrounds; shaping
            // is unchanged, so selection never tears apart grapheme processing.
            auto start = cluster.column;
            while (start < cluster.end && start < row.cells.size()) {
                const auto fg = override_color.value_or(foreground(row, start));
                const bool invisible = row.cells[start].style.invisible ||
                    (row.cells[start].style.blink && !pane.text_blink_on);
                auto end = start + 1;
                while (end < cluster.end && end < row.cells.size() &&
                       override_color.value_or(foreground(row, end)) == fg &&
                       (row.cells[end].style.invisible || (row.cells[end].style.blink && !pane.text_blink_on)) == invisible) {
                    ++end;
                }
                if (!invisible) {
                    const float x0 = start == cluster.column ? ink.x : pane.content.x + float(start * cw);
                    const float x1 = end == cluster.end ? ink.x + ink.width : pane.content.x + float(end * cw);
                    auto paint_clip = surface.clip({x0, y, x1 - x0, float(ch)});
                    glyph_run(cluster, origin, fg);
                }
                start = end;
            }
        }
    }

    void decorations(
        const RenderPane& pane,
        const FrameRow& row,
        UINT row_index,
        UINT cw,
        UINT ch) {
        const auto y = pane.content.y + float(row_index * ch);
        const auto thickness = std::max(1.0f, std::floor(dpi / 96));
        const auto count = std::min<std::size_t>(row.cells.size(), pane.frame->columns);
        for (std::size_t column = 0; column < count; ++column) {
            const auto& c = row.cells[column];
            if (!c.span || c.style.invisible || (c.style.blink && !pane.text_blink_on)) {
                continue;
            }
            const auto x = pane.content.x + float(column * cw);
            const auto length = float(std::min<std::size_t>(c.span, count - column) * cw);
            auto underline_color = selected(row, column) ? selection_foreground : c.underline_color;
            if (c.style.faint) {
                underline_color.r /= 2;
                underline_color.g /= 2;
                underline_color.b /= 2;
            }
            const float uy = y + float(ch) - thickness * 2;
            if (c.style.underline == 1 || c.style.underline == 2) {
                surface.fill({x, uy, length, thickness}, underline_color);
                if (c.style.underline == 2) {
                    surface.fill({x, uy - thickness * 2, length, thickness}, underline_color);
                }
            } else if (c.style.underline >= 3) {
                const float step = c.style.underline == 5 ? thickness * 4 : thickness * 2;
                for (float p = 0; p < length; p += step) {
                    if (c.style.underline == 3) {
                        const auto end = std::min(length, p + step);
                        surface.line(
                            {x + p, uy}, {x + (p + end) / 2, uy - thickness},
                            underline_color, thickness);
                        surface.line(
                            {x + (p + end) / 2, uy - thickness}, {x + end, uy},
                            underline_color, thickness);
                    } else {
                        surface.fill(
                            {x + p, uy,
                             std::min(length - p, c.style.underline == 5 ? step * .65f : thickness),
                             thickness},
                            underline_color);
                    }
                }
            }
            const auto fg = foreground(row, column);
            if (c.style.strikethrough) {
                surface.fill({x, y + ch * .52f, length, thickness}, fg);
            }
            if (c.style.overline) {
                surface.fill({x, y + thickness, length, thickness}, fg);
            }
        }
    }

    void draw_images(
        const RenderPane& pane,
        ImageLayer layer,
        UINT cw,
        UINT ch,
        PixelRect damage) {
        for (const auto& placement : pane.frame->images) {
            if (placement.layer != layer || !placement.pixels) {
                continue;
            }
            const auto& pixels = *placement.pixels;
            if (placement.source_x >= pixels.width || placement.source_y >= pixels.height) {
                continue;
            }
            const auto sw = std::min(
                placement.source_width ? placement.source_width : pixels.width - placement.source_x,
                pixels.width - placement.source_x);
            const auto sh = std::min(
                placement.source_height ? placement.source_height : pixels.height - placement.source_y,
                pixels.height - placement.source_y);
            const auto dw = placement.pixel_width ? float(placement.pixel_width)
                : placement.columns ? float(placement.columns) * float(cw) : float(sw);
            const auto dh = placement.pixel_height ? float(placement.pixel_height)
                : placement.rows ? float(placement.rows) * float(ch) : float(sh);
            const PixelRect target{
                pane.content.x + float(placement.column) * cw + placement.offset_x,
                pane.content.y + float(placement.row) * ch + placement.offset_y,
                dw, dh};
            if (!sw || !sh || !overlaps(target, damage)) {
                continue;
            }
            surface.image(
                images, pane.id, pixels, tick, target,
                {float(placement.source_x), float(placement.source_y),
                 float(placement.source_x + sw), float(placement.source_y + sh)});
        }
    }

    void draw_cursor(const RenderPane& pane, UINT cw, UINT ch) {
        auto bounds = render_geometry::cursor_bounds(pane, text.metrics());
        if (!nonempty(bounds)) {
            return;
        }
        const auto& cursor = pane.frame->cursor;
        const auto thickness = std::max(1.0f, std::round(dpi / 96));
        const auto shape = pane.focused ? cursor.shape : CursorShape::hollow_block;
        if (shape == CursorShape::bar) {
            bounds.width = thickness * 2;
        }
        if (shape == CursorShape::underline) {
            bounds.y += bounds.height - thickness * 2;
            bounds.height = thickness * 2;
        }
        const auto reveal = std::clamp(pane.cursor_reveal, 0.0f, 1.0f);
        if (pane.focused && shape != CursorShape::hollow_block) {
            if (shape == CursorShape::bar) {
                const auto inset = bounds.height * (1 - reveal);
                bounds.y += inset;
                bounds.height -= inset;
            } else {
                bounds.width *= reveal;
            }
        }
        if (shape == CursorShape::hollow_block) {
            const D2D1_RECT_F outline{
                bounds.x + thickness * .5f,
                bounds.y + thickness * .5f,
                bounds.x + bounds.width - thickness * .5f,
                bounds.y + bounds.height - thickness * .5f};
            surface.stroke_rectangle(outline, cursor.color, thickness);
        } else {
            surface.fill(bounds, cursor.color);
            if (shape == CursorShape::block && cursor.row < pane.frame->lines.size() && pane.frame->lines[cursor.row]) {
                auto clip = surface.clip(bounds);
                const auto& row = *pane.frame->lines[cursor.row];
                const std::size_t column = cursor.wide_tail && cursor.column
                    ? cursor.column - 1 : cursor.column;
                const auto text_color = column < row.cells.size()
                    ? background(row, column) : pane.frame->background;
                draw_text(pane, row, cursor.row, cw, ch, text_color);
            }
        }
    }

    void draw_pane(const RenderPane& pane, PixelRect damage) {
        if (!pane.frame || !nonempty(pane.content) || !overlaps(pane.content, damage)) {
            return;
        }
        const auto& frame = *pane.frame;
        const auto cell = text.metrics();
        const UINT cw = frame.cell_width ? frame.cell_width : cell.width;
        const UINT ch = frame.cell_height ? frame.cell_height : cell.height;
        auto clip = surface.clip(pane.content);
        surface.fill(pane.content, frame.background, backdrop_opacity, true);
        draw_images(pane, ImageLayer::below_background, cw, ch, damage);
        const auto rows = std::min<std::size_t>(frame.rows, frame.lines.size());
        for (std::size_t r = 0; r < rows; ++r) {
            if (!frame.lines[r] || !overlaps(
                    {pane.content.x, pane.content.y + float(r * ch), pane.content.width, float(ch)},
                    damage)) {
                continue;
            }
            const auto& row = *frame.lines[r];
            const auto columns = std::min<std::size_t>(frame.columns, row.cells.size());
            for (std::size_t column = 0; column < columns;) {
                const auto& c = row.cells[column];
                const auto bg = background(row, column);
                const bool default_bg = c.background_is_default && !c.style.inverse && !selected(row, column);
                auto end = column + 1;
                while (end < columns && background(row, end) == bg &&
                    (row.cells[end].background_is_default && !row.cells[end].style.inverse && !selected(row, end)) == default_bg) {
                    ++end;
                }
                // Default backgrounds were painted once; skipping them also lets
                // Kitty's below-background layer show through. Explicit cells,
                // inverse and selection remain opaque even at matching RGB.
                if (!default_bg) {
                    surface.fill(
                        {pane.content.x + float(column * cw), pane.content.y + float(r * ch),
                         float((end - column) * cw), float(ch)},
                        bg);
                }
                column = end;
            }
        }
        draw_images(pane, ImageLayer::below_text, cw, ch, damage);
        for (std::size_t r = 0; r < rows; ++r) {
            if (!frame.lines[r] || !overlaps(
                    {pane.content.x, pane.content.y + float(r * ch), pane.content.width, float(ch)},
                    damage)) {
                continue;
            }
            draw_text(pane, *frame.lines[r], static_cast<UINT>(r), cw, ch);
            decorations(pane, *frame.lines[r], static_cast<UINT>(r), cw, ch);
        }
        draw_images(pane, ImageLayer::above_text, cw, ch, damage);
        draw_cursor(pane, cw, ch);
    }
};

} // namespace

void TerminalPainter::paint(
    const RenderPane& pane,
    PixelRect damage_px,
    float dpi,
    float backdrop_opacity,
    std::uint64_t render_tick,
    PaintSurface& surface,
    TextResources& text,
    ImageCache& images) const {
    TerminalPaintPass pass{surface, text, images, dpi, backdrop_opacity, render_tick};
    pass.draw_pane(pane, damage_px);
}

} // namespace stormtty::windows
