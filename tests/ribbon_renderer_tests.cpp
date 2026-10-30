#include "core_test.hpp"
#include "stormtty/ribbon_renderer.hpp"

using namespace stormtty;

namespace {

// Characterization oracle: footer emission copied from window.cpp at eb319418.
// Only host lookups/native measurement are replaced with their resolved values.
// Keep this independent of RibbonRenderer helpers so order/geometry drift fails.
void append_original(WindowScene& result, Rect footer_bounds, const SessionFooterLayout& placement,
                     const RibbonContent& context, const RibbonActivity& activity,
                     const RibbonStyle& style) {
    const auto seam = style.seam;
    const auto raised = style.raised;
    const auto accent = style.accent;
    const auto secondary = style.secondary;
    const auto text = style.ink;
    const ChromeZOrder z{};
    const auto pixels = [&](Rect r) {
        return PixelRect{static_cast<float>(r.x * style.scale), static_cast<float>(r.y * style.scale),
            static_cast<float>(r.width * style.scale), static_cast<float>(r.height * style.scale)};
    };
    const auto rectangle = [&](Rect bounds, Color fill, double radius = 0, Color stroke = {},
                               double stroke_width = 0, bool replace = false) {
        result.primitives.push_back({pixels(bounds), fill, stroke,
            static_cast<float>(radius * style.scale), static_cast<float>(stroke_width * style.scale),
            ChromePrimitiveKind::rectangle, replace, z});
    };
    const auto line = [&](double x1, double y1, double x2, double y2, Color color, double width = 1) {
        result.primitives.push_back({pixels({x1, y1, x2 - x1, y2 - y1}), {}, color, 0,
            static_cast<float>(width * style.scale), ChromePrimitiveKind::line, false, z});
    };
    const auto label = [&](Rect bounds, std::string value, Color color, float size = 13,
                           bool bold = false, ChromeTextAlignment alignment = ChromeTextAlignment::leading) {
        result.texts.push_back({pixels(bounds), std::move(value), color, size, bold,
            "Segoe UI Variable Text", alignment, z});
    };
    const auto footer = style.surface;
    rectangle(footer_bounds, footer, 0, {}, 0, true);
    rectangle({footer_bounds.x, footer_bounds.y, footer_bounds.width, 1 / style.scale}, seam);

    if (placement.agent.width > 0) {
        rectangle(placement.agent, raised, 4);
        label(placement.agent, context.agent, context.agent_attention ? accent : secondary, 11);
    }
    if (placement.clock.width > 0) {
        label(placement.clock, context.clock, secondary, 11, false, ChromeTextAlignment::trailing);
    }
    if (context.directory_hovered) {
        rectangle(placement.directory, raised, 3);
    }
    label(placement.directory, context.directory, context.subdued ? secondary : text, 12);

    if (placement.git.width > 0 && !context.head.empty()) {
        const auto r = placement.git;
        const double x = r.x + 3;
        const double y = r.y + r.height / 2;
        line(x, y - 5, x, y + 5, secondary, 1);
        line(x, y, x + 6, y - 4, secondary, 1);
        rectangle({x - 1.5, y - 6.5, 3, 3}, secondary, 1.5);
        rectangle({x - 1.5, y + 3.5, 3, 3}, secondary, 1.5);
        rectangle({x + 4.5, y - 5.5, 3, 3}, secondary, 1.5);
        label({r.x + 16, r.y, std::max(0.0, r.width - 16), r.height},
            context.detached ? "@" + context.head : context.head,
            context.subdued ? secondary : accent, 12);
    }
    label(placement.status, context.status, context.failed ? accent : secondary,
        12, false, ChromeTextAlignment::trailing);
    if (placement.divider.height > 0) {
        rectangle(placement.divider, seam);
    }

    const auto bounds = placement.background.overflow;
    if (activity.visible && bounds.width > 0) {
        rectangle(bounds, style.activity_surface, 4);
        const bool attention = activity.attention;
        const double mark = std::min(4.0, std::max(0.0, bounds.width - 8));
        if (mark > 0 && (attention || activity.running)) {
            rectangle({bounds.x + 4, bounds.y + bounds.height / 2 - 2, mark, 4},
                attention ? accent : Color{0, 0, 0, 0}, 2, accent, attention ? 0 : 1);
        }
        const double inset = attention || activity.running ? 14 : 8;
        const double width = std::max(0.0, bounds.width - inset - 6);
        const auto& title = activity.label;
        label({bounds.x + inset, bounds.y, width, bounds.height}, title, attention ? accent : text, 12);
    }
}

void compare_scene(const WindowScene& actual, const WindowScene& expected) {
    CHECK(actual.primitives == expected.primitives);
    CHECK(actual.texts == expected.texts);
    CHECK(actual.background == expected.background);
    CHECK(actual.chrome_generation == expected.chrome_generation);
    CHECK(actual.force_redraw == expected.force_redraw);
    CHECK(actual.panes.size() == expected.panes.size());
    CHECK(actual.panes.front().id == expected.panes.front().id);

    const auto actual_order = chrome_paint_order(actual);
    const auto expected_order = chrome_paint_order(expected);
    CHECK(actual_order.size() == expected_order.size());
    for (std::size_t i = 0; i < actual_order.size(); ++i) {
        CHECK(actual_order[i].z == expected_order[i].z);
        CHECK(actual_order[i].kind == expected_order[i].kind);
        CHECK(actual_order[i].index == expected_order[i].index);
    }
}

void characterization() {
    const RibbonRenderer renderer;
    for (double scale : {1.0, 1.25, 1.5, 2.0}) {
        for (double width : {0.0, 7.0, 9.0, 19.0, 64.0, 160.0, 1000.0}) {
            for (unsigned flags = 0; flags < 512; ++flags) {
                const auto enabled = [flags](unsigned bit) { return (flags & (1u << bit)) != 0; };
                const Rect footer{3.25, 71.5, width, 28};
                auto layout = layout_session_footer(footer, enabled(0) ? 2 : 0,
                    true, true, true, enabled(1));
                // Also exercise clipped/suppressed bindings independently of
                // layout policy, including tiny activity marker/label widths.
                layout.background.overflow = {6.25, 74.5, width, 22};
                layout.agent = enabled(2) ? Rect{12, 74.5, 80, 22} : Rect{};
                const RibbonContent content{
                    .directory = "/tmp/世界",
                    .head = enabled(3) ? "" : "topic/é",
                    .status = "exit 9",
                    .detached = enabled(4),
                    .subdued = enabled(5),
                    .failed = enabled(6),
                    .directory_hovered = enabled(8),
                    .clock = "12:34",
                    .agent = "Off",
                    .agent_attention = enabled(7),
                };
                const RibbonActivity activity{
                    .label = "2 background · Watch finished",
                    .visible = enabled(0),
                    .attention = enabled(6),
                    .running = enabled(7),
                };
                const RibbonStyle style{
                    .surface = {20, 30, 40, static_cast<std::uint8_t>(enabled(5) ? 230 : 255)},
                    .raised = {40, 50, 60, 255},
                    .seam = {50, 60, 70, 255},
                    .activity_surface = enabled(8) ? Color{40, 50, 60, 255} : Color{44, 54, 64, 255},
                    .ink = {210, 220, 230, 255},
                    .secondary = {100, 110, 120, 255},
                    .accent = {170, 180, 190, 255},
                    .scale = scale,
                };

                WindowScene expected;
                expected.background = {1, 2, 3, 255};
                expected.chrome_generation = 123;
                expected.force_redraw = true;
                RenderPane existing_pane;
                existing_pane.id = 42;
                expected.panes.push_back(existing_pane);
                expected.primitives.push_back({{1, 2, 3, 4}, {9, 8, 7, 255}});
                expected.texts.push_back({{1, 2, 30, 40}, "existing", {1, 2, 3, 255}});
                auto actual = expected;

                append_original(expected, footer, layout, content, activity, style);
                renderer.append_scene(actual, footer, layout, content, activity, style);
                compare_scene(actual, expected);

                CHECK(actual.primitives[1].background_replace);
                CHECK(actual.primitives[1].fill.a == style.surface.a);
                CHECK(actual.primitives[2].rect.height == 1.0f);
                const double inset = activity.attention || activity.running ? 14 : 8;
                const auto bounds = RibbonRenderer::activity_label_bounds(
                    layout.background.overflow, activity.attention, activity.running);
                CHECK(bounds.x == layout.background.overflow.x + inset);
                CHECK(bounds.width == std::max(0.0, width - inset - 6));
            }
        }
    }
}

} // namespace

int main() {
    return run_core_tests("ribbon renderer", characterization);
}
