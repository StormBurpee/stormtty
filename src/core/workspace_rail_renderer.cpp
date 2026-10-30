#include "stormtty/workspace_rail_renderer.hpp"
#include "stormtty/chrome_paint.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace stormtty {
namespace {

Color opacity(Color color, double amount) {
    color.a = static_cast<std::uint8_t>(std::clamp(std::lround(color.a * amount), 0L, 255L));
    return color;
}

Color blend(Color first, Color second, double amount) {
    const auto channel = [amount](int left, int right) {
        return static_cast<std::uint8_t>(std::lround(left + (right - left) * amount));
    };
    return {
        channel(first.r, second.r), channel(first.g, second.g),
        channel(first.b, second.b), channel(first.a, second.a)};
}

// Local emission mechanics only; no host/service references or native resources.
class RailPaint final {
public:
    RailPaint(WindowScene& scene, double scale) : scene_(scene), scale_(scale) {}

    void rectangle(
        Rect bounds,
        Color fill,
        double radius = 0,
        Color stroke = {},
        double stroke_width = 0,
        bool replace = false) {
        scene_.primitives.push_back({
            pixels(bounds), fill, stroke,
            static_cast<float>(radius * scale_), static_cast<float>(stroke_width * scale_),
            ChromePrimitiveKind::rectangle, replace, {ChromeLayer::base}});
    }

    void line(double x1, double y1, double x2, double y2, Color color, double width = 1) {
        scene_.primitives.push_back({
            pixels({x1, y1, x2 - x1, y2 - y1}), {}, color, 0,
            static_cast<float>(width * scale_), ChromePrimitiveKind::line, false,
            {ChromeLayer::base}});
    }

    void label(
        Rect bounds,
        std::string value,
        Color color,
        float size = 13,
        bool bold = false,
        ChromeTextAlignment alignment = ChromeTextAlignment::leading) {
        scene_.texts.push_back({
            pixels(bounds), std::move(value), color, size, bold,
            "Segoe UI Variable Text", alignment, {ChromeLayer::base}});
    }

private:
    PixelRect pixels(Rect bounds) const {
        return {
            static_cast<float>(bounds.x * scale_), static_cast<float>(bounds.y * scale_),
            static_cast<float>(bounds.width * scale_), static_cast<float>(bounds.height * scale_)};
    }

    WindowScene& scene_;
    double scale_;
};

void append_workspace_icon(RailPaint& paint, std::string_view name,
                           double x, double y, Color icon) {
    if (name == "folder") {
        paint.rectangle({x, y + 3, 16, 10}, {0, 0, 0, 0}, 1, icon, 1);
        paint.line(x, y + 3, x, y, icon);
        paint.line(x, y, x + 6, y, icon);
        paint.line(x + 6, y, x + 9, y + 3, icon);
    } else if (name == "code") {
        paint.line(x + 5, y + 2, x, y + 7, icon);
        paint.line(x, y + 7, x + 5, y + 12, icon);
        paint.line(x + 11, y + 2, x + 16, y + 7, icon);
        paint.line(x + 16, y + 7, x + 11, y + 12, icon);
    } else if (name == "terminal") {
        paint.rectangle({x, y, 16, 14}, {0, 0, 0, 0}, 2, icon, 1);
        paint.line(x + 3, y + 4, x + 6, y + 7, icon);
        paint.line(x + 6, y + 7, x + 3, y + 10, icon);
        paint.line(x + 9, y + 10, x + 13, y + 10, icon);
    } else if (name == "bolt") {
        paint.line(x + 10, y, x + 3, y + 8, icon);
        paint.line(x + 3, y + 8, x + 9, y + 8, icon);
        paint.line(x + 9, y + 8, x + 6, y + 15, icon);
        paint.line(x + 6, y + 15, x + 14, y + 5, icon);
        paint.line(x + 14, y + 5, x + 8, y + 5, icon);
    } else if (name == "globe") {
        paint.rectangle({x, y, 16, 16}, {0, 0, 0, 0}, 8, icon, 1);
        paint.rectangle({x + 5, y, 6, 16}, {0, 0, 0, 0}, 3, icon, 1);
        paint.line(x, y + 8, x + 16, y + 8, icon);
    } else if (name == "git") {
        paint.line(x + 4, y + 3, x + 4, y + 14, icon);
        paint.line(x + 4, y + 10, x + 12, y + 6, icon);
        paint.line(x + 12, y + 6, x + 12, y + 3, icon);
        paint.rectangle({x + 2, y, 4, 4}, {0, 0, 0, 0}, 2, icon, 1);
        paint.rectangle({x + 10, y, 4, 4}, {0, 0, 0, 0}, 2, icon, 1);
        paint.rectangle({x + 2, y + 12, 4, 4}, {0, 0, 0, 0}, 2, icon, 1);
    } else if (name == "flask") {
        paint.line(x + 5, y, x + 11, y, icon);
        paint.line(x + 6, y, x + 6, y + 5, icon);
        paint.line(x + 10, y, x + 10, y + 5, icon);
        paint.line(x + 6, y + 5, x + 1, y + 14, icon);
        paint.line(x + 10, y + 5, x + 15, y + 14, icon);
        paint.line(x + 1, y + 14, x + 15, y + 14, icon);
        paint.line(x + 4, y + 9, x + 12, y + 9, icon);
    } else if (name == "cloud") {
        paint.rectangle({x, y + 6, 16, 9}, {0, 0, 0, 0}, 4, icon, 1);
        paint.rectangle({x + 4, y + 1, 9, 11}, {0, 0, 0, 0}, 5, icon, 1);
    } else {
        paint.rectangle({x, y, 16, 15}, {0, 0, 0, 0}, 2, icon, 1);
        if (name == "book") {
            paint.line(x + 8, y, x + 8, y + 15, icon);
            paint.line(x + 3, y + 4, x + 6, y + 4, icon);
            paint.line(x + 10, y + 4, x + 13, y + 4, icon);
        } else {
            paint.line(x, y + 5, x + 16, y + 5, icon);
            paint.line(x, y + 10, x + 16, y + 10, icon);
        }
    }
}

void append_workspace_row(RailPaint& paint, const WorkspaceRailRow& row,
                          bool expanded, const WorkspaceRailStyle& style) {
    const auto& bounds = row.bounds;
    const double focus = row.focus_amount;
    const double hovered = row.hover_amount;
    if (focus > 0 || hovered > 0) {
        const auto wash = style.high_contrast ? style.raised
            : blend(style.raised, style.selected_surface, focus * .25);
        paint.rectangle(bounds, opacity(wash, std::max(focus, hovered * .65)), 4);
    }

    append_workspace_icon(paint, row.icon, bounds.x + 10, bounds.y + 10, row.icon_color);
    if (row.selected) {
        paint.rectangle({bounds.x + 1, bounds.y + 7, 2, bounds.height - 14}, row.icon_color, 1);
    }
    if (!row.selected && !expanded && row.unread) {
        paint.rectangle({bounds.x + bounds.width - 7, bounds.y + 6, 4, 4}, style.accent, 2);
    }
    if (expanded) {
        paint.label({bounds.x + 38, bounds.y + 7, bounds.width - 44, 20},
                    row.name + (!row.selected && row.unread ? " · result" : ""),
                    blend(style.secondary, style.ink, focus), 13, row.selected);
    }
}

} // namespace

void WorkspaceRailRenderer::append_scene(WindowScene& scene, const WorkspaceRailView& view,
                                         const WorkspaceRailStyle& style) const {
    if (!(view.rail.width > 0)) {
        return;
    }
    RailPaint paint(scene, style.scale);
    paint.rectangle(view.rail, style.surface, 0, {}, 0, true);
    paint.rectangle({view.rail.width - 1 / style.scale, view.caption_height,
                     1 / style.scale, view.rail.height}, style.seam);

    if (view.toggle_hover > 0) {
        paint.rectangle(view.toggle, opacity(style.raised, view.toggle_hover), 5);
    }
    const double tx = view.toggle.x + 16;
    const double ty = view.toggle.y + 14;
    paint.rectangle({tx - 7, ty - 6, 14, 12}, {0, 0, 0, 0}, 1, style.secondary, 1);
    paint.line(tx - 2, ty - 6, tx - 2, ty + 6, style.secondary);
    if (view.expanded) {
        paint.label({44, view.caption_height + 10, 132, 18}, "WORKSPACES", style.secondary, 11);
    }
    for (const auto& row : view.rows) {
        append_workspace_row(paint, row, view.expanded, style);
    }

    // Preserve hover admission even when the new-workspace glyph has no height.
    if (view.new_workspace_hover > 0) {
        paint.rectangle(view.new_workspace, opacity(style.raised, view.new_workspace_hover), 5);
    }
    if (view.new_workspace.height > 0) {
        const double x = view.new_workspace.x + 18;
        const double y = view.new_workspace.y + 18;
        paint.line(x - 5, y, x + 5, y, style.secondary);
        paint.line(x, y - 5, x, y + 5, style.secondary);
        if (view.expanded) {
            paint.label({view.new_workspace.x + 38, view.new_workspace.y + 8,
                         view.new_workspace.width - 44, 20}, "New workspace", style.secondary, 12);
        }
    }
    if (view.settings.height > 0) {
        if (view.settings_hover > 0) {
            paint.rectangle(view.settings, opacity(style.raised, view.settings_hover), 5);
        }
        append_settings_icon(scene, {view.settings.x + 18, view.settings.y + 18},
                             8, style.secondary, static_cast<float>(style.scale), 1.2,
                             {ChromeLayer::base});
        if (view.expanded) {
            paint.label({view.settings.x + 38, view.settings.y + 8,
                         view.settings.width - 44, 20}, "Settings", style.secondary, 12);
        }
    }
}

} // namespace stormtty
