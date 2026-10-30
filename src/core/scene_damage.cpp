#include "stormtty/scene_damage.hpp"

#include <algorithm>
#include <cmath>

namespace stormtty {
namespace render_geometry {
PixelRect intersect(PixelRect a, PixelRect b) noexcept {
    const auto x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0f, std::min(a.x + a.width, b.x + b.width) - x),
                  std::max(0.0f, std::min(a.y + a.height, b.y + b.height) - y)};
}

bool nonempty(PixelRect r) noexcept {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
           std::isfinite(r.height) && r.width > 0 && r.height > 0;
}

bool overlaps(PixelRect a, PixelRect b) noexcept {
    return nonempty(intersect(a, b));
}

PixelRect chrome_clip(PixelRect bounds, const std::optional<PixelRect>& viewport) noexcept {
    if (!viewport) {
        return bounds;
    }
    if (!nonempty(*viewport)) {
        return {};
    }
    // Fractional viewports cover whole physical pixels outward; paths and
    // glyph layouts retain their original fractional geometry.
    const auto x = std::floor(viewport->x);
    const auto y = std::floor(viewport->y);
    return intersect(bounds, {x, y, std::ceil(viewport->x + viewport->width) - x,
                                    std::ceil(viewport->y + viewport->height) - y});
}

PixelRect chrome_bounds(const ChromeText& text) noexcept {
    return chrome_clip(text.rect, text.clip);
}

PixelRect chrome_bounds(const ChromePrimitive& primitive) noexcept {
    if (!std::isfinite(primitive.stroke_width) || !std::isfinite(primitive.radius)) {
        return {};
    }
    const auto& rect = primitive.rect;
    const auto pad = std::max(1.0f, primitive.stroke_width * .5f + 1);
    if (primitive.kind == ChromePrimitiveKind::line) {
        return chrome_clip({std::min(rect.x, rect.x + rect.width) - pad,
                            std::min(rect.y, rect.y + rect.height) - pad,
                            std::abs(rect.width) + pad * 2, std::abs(rect.height) + pad * 2},
                           primitive.clip);
    }
    if (!nonempty(rect)) {
        return {};
    }
    return chrome_clip({rect.x - pad, rect.y - pad, rect.width + pad * 2, rect.height + pad * 2},
                       primitive.clip);
}

PixelRect cursor_bounds(const RenderPane& pane, CellMetrics metrics) noexcept {
    if (!pane.frame || !pane.frame->cursor.visible || !pane.cursor_on ||
        pane.frame->cursor.row >= pane.frame->rows || pane.frame->cursor.column >= pane.frame->columns) {
        return {};
    }
    const auto& frame = *pane.frame;
    auto column = frame.cursor.column;
    if (frame.cursor.wide_tail && column) {
        --column;
    }
    std::uint32_t span = 1;
    if (frame.cursor.row < frame.lines.size() && frame.lines[frame.cursor.row] &&
        column < frame.lines[frame.cursor.row]->cells.size()) {
        span = std::max<std::uint32_t>(1, frame.lines[frame.cursor.row]->cells[column].span);
    }
    const auto cw = frame.cell_width ? frame.cell_width : metrics.width;
    const auto ch = frame.cell_height ? frame.cell_height : metrics.height;
    return intersect(pane.content, {pane.content.x + float(column * cw),
                                    pane.content.y + float(frame.cursor.row * ch),
                                    float(span * cw), float(ch)});
}
} // namespace render_geometry

namespace {
bool same_cursor(const FrameCursor& a, const FrameCursor& b) noexcept {
    return a.column == b.column && a.row == b.row && a.visible == b.visible &&
           a.shape == b.shape && a.color == b.color && a.wide_tail == b.wide_tail;
}
bool same_images(const RenderFrame& a, const RenderFrame& b) noexcept {
    if (a.image_placements_generation != b.image_placements_generation || a.images.size() != b.images.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.images.size(); ++i) {
        const auto& x = a.images[i];
        const auto& y = b.images[i];
        if (bool(x.pixels) != bool(y.pixels)) {
            return false;
        }
        if (x.pixels && (x.pixels->image_id != y.pixels->image_id || x.pixels->generation != y.pixels->generation)) {
            return false;
        }
    }
    return true;
}
} // namespace

void SceneDamageTracker::commit(const WindowScene& scene) {
    previous_ = scene;
    previous_.force_redraw = false;
    invalid_ = false;
}

void SceneDamageTracker::invalidate() noexcept {
    invalid_ = true;
}

std::vector<PixelRect> SceneDamageTracker::damage(
    const WindowScene& scene, CellMetrics metrics, PixelRect surface) const {
    using namespace render_geometry;

    if (invalid_ || scene.force_redraw || scene.background != previous_.background ||
        scene.panes.size() != previous_.panes.size()) {
        return {surface};
    }

    std::vector<PixelRect> result;
    auto add = [&](PixelRect rect) {
        if (!nonempty(rect)) {
            return;
        }
        rect = intersect(rect, surface);
        if (nonempty(rect)) {
            result.push_back(rect);
        }
    };

    if (scene.chrome_generation != previous_.chrome_generation) {
        const auto primitives = std::max(scene.primitives.size(), previous_.primitives.size());
        for (std::size_t i = 0; i < primitives; ++i) {
            if (i < scene.primitives.size() && i < previous_.primitives.size() &&
                scene.primitives[i] == previous_.primitives[i]) {
                continue;
            }
            if (i < scene.primitives.size()) {
                add(chrome_bounds(scene.primitives[i]));
            }
            if (i < previous_.primitives.size()) {
                add(chrome_bounds(previous_.primitives[i]));
            }
        }

        const auto texts = std::max(scene.texts.size(), previous_.texts.size());
        for (std::size_t i = 0; i < texts; ++i) {
            if (i < scene.texts.size() && i < previous_.texts.size() &&
                scene.texts[i] == previous_.texts[i]) {
                continue;
            }
            if (i < scene.texts.size()) {
                add(chrome_bounds(scene.texts[i]));
            }
            if (i < previous_.texts.size()) {
                add(chrome_bounds(previous_.texts[i]));
            }
        }
    }

    for (std::size_t i = 0; i < scene.panes.size(); ++i) {
        const auto& now = scene.panes[i];
        const auto& before = previous_.panes[i];
        if (now.id != before.id || now.content != before.content) {
            return {surface};
        }
        if (!now.frame || !before.frame) {
            if (now.frame != before.frame) {
                add(now.content);
            }
            continue;
        }

        const auto& a = *now.frame;
        const auto& b = *before.frame;
        if (a.columns != b.columns || a.rows != b.rows || a.cell_width != b.cell_width ||
            a.cell_height != b.cell_height || a.background != b.background ||
            a.foreground != b.foreground || !same_images(a, b)) {
            add(now.content);
            continue;
        }

        const std::uint32_t row_height = a.cell_height ? a.cell_height : metrics.height;
        const auto row_bounds = [&](std::size_t row) {
            return intersect(now.content, {now.content.x, now.content.y + float(row * row_height),
                                           now.content.width, float(row_height)});
        };
        if (now.frame != before.frame) {
            const auto rows = std::max(a.lines.size(), b.lines.size());
            for (std::size_t row = 0; row < rows; ++row) {
                bool dirty = row >= a.lines.size() || row >= b.lines.size();
                if (!dirty) {
                    const auto& ar = a.lines[row];
                    const auto& br = b.lines[row];
                    dirty = bool(ar) != bool(br) ||
                            (ar && (ar->id != br->id || ar->generation != br->generation));
                }
                if (dirty) {
                    add(row_bounds(row));
                }
            }
        }

        if (now.text_blink_on != before.text_blink_on) {
            for (std::size_t row = 0; row < a.lines.size(); ++row) {
                if (a.lines[row] && std::any_of(a.lines[row]->cells.begin(), a.lines[row]->cells.end(),
                                               [](const Cell& cell) { return cell.style.blink; })) {
                    add(row_bounds(row));
                }
            }
        }

        if (!same_cursor(a.cursor, b.cursor) || now.cursor_on != before.cursor_on ||
            now.focused != before.focused || now.cursor_reveal != before.cursor_reveal) {
            add(cursor_bounds(before, metrics));
            add(cursor_bounds(now, metrics));
        }
    }

    // Preserve the pre-merge threshold and insertion/merge order: fragmentation
    // over this bound repaints the canvas even when bands could later coalesce.
    if (result.size() > 128) {
        return {surface};
    }
    for (std::size_t i = 0; i < result.size(); ++i) {
        for (std::size_t j = i + 1; j < result.size();) {
            auto& a = result[i];
            const auto& b = result[j];
            const bool aligned = a.x == b.x && a.width == b.width &&
                                 a.y <= b.y + b.height && b.y <= a.y + a.height;
            if (aligned || overlaps(a, b)) {
                const auto x = std::min(a.x, b.x);
                const auto y = std::min(a.y, b.y);
                a = {x, y, std::max(a.x + a.width, b.x + b.width) - x,
                           std::max(a.y + a.height, b.y + b.height) - y};
                result.erase(result.begin() + static_cast<std::ptrdiff_t>(j));
            } else {
                ++j;
            }
        }
    }
    return result;
}

} // namespace stormtty
