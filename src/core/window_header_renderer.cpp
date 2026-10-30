#include "stormtty/window_header_renderer.hpp"

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
class HeaderPaint final {
public:
    HeaderPaint(WindowScene& scene, double scale) : scene_(scene), scale_(scale) {}

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

void append_tab(
    HeaderPaint& paint,
    const HeaderTabPresentation& tab,
    bool compact,
    const WindowHeaderStyle& style) {
    const auto& bounds = tab.geometry.bounds;
    const double settle = tab.selection_amount;
    const double hovered = tab.hover_amount;
    if (settle > 0 || hovered > 0) {
        paint.rectangle(
            bounds,
            opacity(blend(style.raised, style.selected_tab_surface, settle),
                    std::max(settle, hovered * .65)),
            4,
            style.high_contrast ? opacity(style.selected_tab_border, settle) : Color{0, 0, 0, 0},
            style.high_contrast ? 1 / style.scale : 0);
    }
    if (tab.selected) {
        paint.rectangle(
            {bounds.x + 3, bounds.y + bounds.height - 2 / style.scale,
             std::max(0.0, bounds.width - 6), 2 / style.scale},
            style.accent, 1 / style.scale);
    }
    const auto caption = layout_tab_label(bounds, tab.watch);
    if (caption.indicator.width > 0) {
        // Shape, not color alone, distinguishes watching from unread.
        const auto ink = tab.selected ? style.ink : style.secondary;
        paint.rectangle(
            caption.indicator,
            tab.watch == TabWatchIndicator::unread ? ink : Color{0, 0, 0, 0},
            3, ink, 1);
    }
    paint.label(caption.text, tab.title, tab.selected ? style.ink : style.secondary, 13, tab.selected);
    if (compact) {
        paint.label(
            {bounds.x + std::max(0.0, bounds.width - 28), bounds.y + 6,
             std::min(24.0, bounds.width), 20},
            "▾", style.ink, 13, false, ChromeTextAlignment::center);
    }
    if (!compact && (tab.selected || hovered > 0)) {
        const auto& close = tab.geometry.close;
        const double x = close.x + close.width / 2;
        const double y = close.y + close.height / 2;
        if (tab.close_hover_amount > 0) {
            paint.rectangle(close, opacity(style.surface, tab.close_hover_amount), 4);
        }
        paint.line(x - 3, y - 3, x + 3, y + 3, style.secondary);
        paint.line(x + 3, y - 3, x - 3, y + 3, style.secondary);
    }
}

} // namespace

void WindowHeaderRenderer::append_scene(
    WindowScene& scene,
    const WindowHeaderView& view,
    const WindowHeaderStyle& style) const {
    HeaderPaint paint(scene, style.scale);
    const auto& chrome = view.layout;
    paint.rectangle(chrome.band, style.surface, 0, {}, 0, true);
    paint.rectangle(chrome.tabs_band, style.surface, 0, {}, 0, true);
    paint.rectangle({0, chrome.band.height - 1 / style.scale, view.width, 1 / style.scale}, style.seam);
    paint.rectangle(
        {chrome.tabs_band.x, chrome.tabs_band.y + chrome.tabs_band.height - 1 / style.scale,
         chrome.tabs_band.width, 1 / style.scale},
        style.seam);
    const double title_right = view.zoom_return.width > 0
        ? view.zoom_return.x - 8 : chrome.drag.x + chrome.drag.width;
    paint.label(
        layout_window_title(chrome, title_right), "StormTTY", style.secondary,
        12, false, ChromeTextAlignment::center);

    const auto button = [&](Rect bounds, double amount, bool close = false) {
        if (amount > 0) {
            paint.rectangle(
                bounds, opacity(close ? Color{196, 43, 28, 255} : style.raised, amount),
                close ? 0 : 5);
        }
    };
    button(chrome.menu, view.hover.menu);
    button(chrome.new_tab, view.hover.new_tab);
    button(chrome.palette, view.hover.palette);
    if (view.zoom_return.width) {
        button(view.zoom_return, view.hover.zoom_return);
        paint.label(
            {view.zoom_return.x + 8, view.zoom_return.y + 5,
             view.zoom_return.width - 16, 20},
            "Return to split", style.accent, 12, false, ChromeTextAlignment::center);
    }
    for (const auto& tab : view.tabs) {
        append_tab(paint, tab, chrome.compact, style);
    }
    for (int row = 0; row < 3; ++row) {
        paint.line(
            chrome.menu.x + 8, chrome.menu.y + 10 + row * 5,
            chrome.menu.x + 20, chrome.menu.y + 10 + row * 5, style.secondary, 1.2);
    }
    const double nx = chrome.new_tab.x + chrome.new_tab.width / 2;
    const double ny = chrome.new_tab.y + chrome.new_tab.height / 2;
    if (chrome.new_tab.width > 0) {
        paint.line(nx - 5, ny, nx + 5, ny, style.secondary, 1.3);
        paint.line(nx, ny - 5, nx, ny + 5, style.secondary, 1.3);
    }
    const double px = chrome.palette.x + 8;
    const double py = chrome.palette.y + 10;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            paint.rectangle({px + x * 7, py + y * 7, 4, 4}, style.secondary, 1);
        }
    }
    if (view.focus.width > 0 && view.focus.height > 0) {
        button(view.focus, view.hover.focus);
        const auto ink = view.focus_mode ? style.accent : style.secondary;
        const double x = view.focus.x + view.focus.width / 2;
        const double y = view.focus.y + view.focus.height / 2;
        for (const int sx : {-1, 1}) {
            for (const int sy : {-1, 1}) {
                const double corner = view.focus_mode ? 3 : 6;
                const double end = view.focus_mode ? 6 : 3;
                paint.line(x + sx * corner, y + sy * end, x + sx * corner, y + sy * corner, ink);
                paint.line(x + sx * corner, y + sy * corner, x + sx * end, y + sy * corner, ink);
            }
        }
    }
    button(chrome.minimize, view.hover.minimize);
    button(chrome.maximize, view.hover.maximize);
    button(chrome.close, view.hover.close, true);
    const double cy = chrome.band.height / 2;
    const auto center = [](Rect bounds) { return bounds.x + bounds.width / 2; };
    const double minx = center(chrome.minimize);
    const double maxx = center(chrome.maximize);
    const double closex = center(chrome.close);
    paint.line(minx - 5, cy + 3, minx + 5, cy + 3, style.ink);
    if (view.maximized) {
        paint.rectangle({maxx - 3, cy - 5, 8, 8}, {0, 0, 0, 0}, 0, style.ink, 1);
        paint.rectangle({maxx - 5, cy - 3, 8, 8}, style.surface, 0, style.ink, 1);
    } else {
        paint.rectangle({maxx - 5, cy - 5, 10, 10}, {0, 0, 0, 0}, 0, style.ink, 1);
    }
    paint.line(closex - 4, cy - 4, closex + 4, cy + 4, style.ink);
    paint.line(closex + 4, cy - 4, closex - 4, cy + 4, style.ink);
}

} // namespace stormtty
