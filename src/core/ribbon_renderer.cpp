#include "stormtty/ribbon_renderer.hpp"

#include <algorithm>
#include <utility>

namespace stormtty {
namespace {

// A deliberately local scene writer: geometry conversion and append operations
// only. It cannot reach the host, renderer resources or another domain's state.
class RibbonPaint final {
public:
    RibbonPaint(WindowScene& scene, double scale) : scene_(scene), scale_(scale) {}

    void rectangle(Rect bounds, Color fill, double radius = 0, Color stroke = {},
                   double stroke_width = 0, bool replace = false) {
        scene_.primitives.push_back({pixels(bounds), fill, stroke,
            static_cast<float>(radius * scale_), static_cast<float>(stroke_width * scale_),
            ChromePrimitiveKind::rectangle, replace, {ChromeLayer::base}});
    }

    void line(double x1, double y1, double x2, double y2, Color color) {
        scene_.primitives.push_back({pixels({x1, y1, x2 - x1, y2 - y1}), {}, color,
            0, static_cast<float>(scale_), ChromePrimitiveKind::line, false, {ChromeLayer::base}});
    }

    void label(Rect bounds, std::string value, Color color, float size,
               ChromeTextAlignment alignment = ChromeTextAlignment::leading) {
        scene_.texts.push_back({pixels(bounds), std::move(value), color, size, false,
            "Segoe UI Variable Text", alignment, {ChromeLayer::base}});
    }

private:
    PixelRect pixels(Rect bounds) const {
        return {static_cast<float>(bounds.x * scale_), static_cast<float>(bounds.y * scale_),
            static_cast<float>(bounds.width * scale_), static_cast<float>(bounds.height * scale_)};
    }

    WindowScene& scene_;
    double scale_;
};

void append_branch(RibbonPaint& paint, Rect bounds, const RibbonContent& content,
                   const RibbonStyle& style) {
    const double x = bounds.x + 3;
    const double y = bounds.y + bounds.height / 2;

    // Owned vector branch mark; no dependency on shell fonts or icon plugins.
    paint.line(x, y - 5, x, y + 5, style.secondary);
    paint.line(x, y, x + 6, y - 4, style.secondary);
    paint.rectangle({x - 1.5, y - 6.5, 3, 3}, style.secondary, 1.5);
    paint.rectangle({x - 1.5, y + 3.5, 3, 3}, style.secondary, 1.5);
    paint.rectangle({x + 4.5, y - 5.5, 3, 3}, style.secondary, 1.5);
    paint.label({bounds.x + 16, bounds.y, std::max(0.0, bounds.width - 16), bounds.height},
        content.detached ? "@" + content.head : content.head,
        content.subdued ? style.secondary : style.accent, 12);
}

void append_activity(RibbonPaint& paint, Rect bounds, const RibbonActivity& activity,
                     const RibbonStyle& style) {
    if (!activity.visible || bounds.width <= 0) {
        return;
    }

    paint.rectangle(bounds, style.activity_surface, 4);
    const double mark = std::min(4.0, std::max(0.0, bounds.width - 8));
    if (mark > 0 && (activity.attention || activity.running)) {
        paint.rectangle({bounds.x + 4, bounds.y + bounds.height / 2 - 2, mark, 4},
            activity.attention ? style.accent : Color{0, 0, 0, 0}, 2, style.accent,
            activity.attention ? 0 : 1);
    }

    paint.label(RibbonRenderer::activity_label_bounds(bounds, activity.attention, activity.running),
        activity.label, activity.attention ? style.accent : style.ink, 12);
}

} // namespace

Rect RibbonRenderer::activity_label_bounds(Rect bounds, bool attention, bool running) {
    const double inset = attention || running ? 14 : 8;
    return {bounds.x + inset, bounds.y, std::max(0.0, bounds.width - inset - 6), bounds.height};
}

void RibbonRenderer::append_scene(
    WindowScene& scene,
    Rect footer,
    const SessionFooterLayout& layout,
    const RibbonContent& content,
    const RibbonActivity& activity,
    const RibbonStyle& style) const {
    RibbonPaint paint(scene, style.scale);
    paint.rectangle(footer, style.surface, 0, {}, 0, true);
    paint.rectangle({footer.x, footer.y, footer.width, 1 / style.scale}, style.seam);

    if (layout.agent.width > 0) {
        paint.rectangle(layout.agent, style.raised, 4);
        paint.label(layout.agent, content.agent,
            content.agent_attention ? style.accent : style.secondary, 11);
    }
    if (layout.clock.width > 0) {
        paint.label(layout.clock, content.clock, style.secondary, 11, ChromeTextAlignment::trailing);
    }

    if (content.directory_hovered) {
        paint.rectangle(layout.directory, style.raised, 3);
    }
    paint.label(layout.directory, content.directory, content.subdued ? style.secondary : style.ink, 12);
    if (layout.git.width > 0 && !content.head.empty()) {
        append_branch(paint, layout.git, content, style);
    }
    paint.label(layout.status, content.status, content.failed ? style.accent : style.secondary,
        12, ChromeTextAlignment::trailing);
    if (layout.divider.height > 0) {
        paint.rectangle(layout.divider, style.seam);
    }

    append_activity(paint, layout.background.overflow, activity, style);
}

} // namespace stormtty
