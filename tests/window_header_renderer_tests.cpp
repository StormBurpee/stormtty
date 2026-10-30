#include "core_test.hpp"
#include "stormtty/window_header_renderer.hpp"

#include <array>
#include <cmath>

using namespace stormtty;

namespace {

// Independent characterization oracle: original crown/tab emission at aea9609.
// Only host lookups are replaced with resolved snapshot values. Do not call the
// new renderer or its private helpers here, or normalize away emission details.
void append_original(
    WindowScene& result,
    const WindowHeaderView& view,
    const WindowHeaderStyle& style) {
    const auto& chrome_ = view.layout;
    const auto zoom_return_ = view.zoom_return;
    const struct { Rect focus; } navigation_{view.focus};
    const double width_ = view.width;
    const double scale_ = style.scale;
    const bool high_contrast_ = style.high_contrast;
    const bool focus_mode_ = view.focus_mode;
    const bool maximized_ = view.maximized;
    const Color chrome = style.surface;
    const Color raised = style.raised;
    const Color seam = style.seam;
    const Color text = style.ink;
    const Color secondary = style.secondary;
    const Color accent = style.accent;
    struct TabColors { Color surface; Color border; };
    const auto tab_state = [&](TabId id) -> const HeaderTabPresentation& {
        const auto found = std::find_if(view.tabs.begin(), view.tabs.end(),
            [id](const auto& tab) { return tab.geometry.tab == id; });
        if (found == view.tabs.end()) {
            throw std::runtime_error("Oracle missing stable tab identity");
        }
        return *found;
    };
    const auto hover_amount = [&](std::uint64_t id) {
        switch (id) {
        case 1: return view.hover.menu;
        case 2: return view.hover.new_tab;
        case 3: return view.hover.palette;
        case 4: return view.hover.minimize;
        case 5: return view.hover.maximize;
        case 6: return view.hover.close;
        case 7: return view.hover.zoom_return;
        case 10: return view.hover.focus;
        default:
            for (const auto& tab : view.tabs) {
                if (id == 16 + tab.geometry.tab.value * 2) return tab.hover_amount;
                if (id == 17 + tab.geometry.tab.value * 2) return tab.close_hover_amount;
            }
            throw std::runtime_error("Oracle missing hover identity");
        }
    };
    const ChromeZOrder z{};
    const auto pixels = [&](Rect r) {
        return PixelRect{static_cast<float>(r.x * scale_), static_cast<float>(r.y * scale_),
            static_cast<float>(r.width * scale_), static_cast<float>(r.height * scale_)};
    };
    const auto rectangle = [&](Rect bounds, Color fill, double radius = 0, Color stroke = {},
                               double stroke_width = 0, bool replace = false) {
        result.primitives.push_back({pixels(bounds), fill, stroke,
            static_cast<float>(radius * scale_), static_cast<float>(stroke_width * scale_),
            ChromePrimitiveKind::rectangle, replace, z});
    };
    const auto line = [&](double x1, double y1, double x2, double y2, Color color, double width = 1) {
        result.primitives.push_back({pixels({x1, y1, x2 - x1, y2 - y1}), {}, color, 0,
            static_cast<float>(width * scale_), ChromePrimitiveKind::line, false, z});
    };
    const auto label = [&](Rect bounds, std::string value, Color color, float size = 13,
                           bool bold = false, ChromeTextAlignment alignment = ChromeTextAlignment::leading) {
        result.texts.push_back({pixels(bounds), std::move(value), color, size, bold,
            "Segoe UI Variable Text", alignment, z});
    };
    const auto opacity = [](Color color, double amount) {
        color.a = static_cast<std::uint8_t>(std::clamp(std::lround(color.a * amount), 0L, 255L));
        return color;
    };
    const auto blend = [](Color a, Color b, double t) {
        const auto channel = [t](int x, int y) {
            return static_cast<std::uint8_t>(std::lround(x + (y - x) * t));
        };
        return Color{channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a)};
    };
    {
        rectangle(chrome_.band, chrome, 0, {}, 0, true);
        rectangle(chrome_.tabs_band,chrome,0,{},0,true);
        rectangle({0,chrome_.band.height-1/scale_,width_,1/scale_},seam);
        rectangle({chrome_.tabs_band.x,chrome_.tabs_band.y+chrome_.tabs_band.height-1/scale_,chrome_.tabs_band.width,1/scale_},seam);
        const double title_right=zoom_return_.width>0?zoom_return_.x-8:chrome_.drag.x+chrome_.drag.width;
        const Rect app_title=layout_window_title(chrome_,title_right);
        label(app_title,"StormTTY",secondary,12,false,ChromeTextAlignment::center);
        auto button = [&](Rect bounds, std::uint64_t id, bool close = false) {
            const auto amount=hover_amount(id);
            if (amount>0) rectangle(bounds, opacity(close ? Color{196,43,28,255} : raised,amount), close ? 0 : 5);
        };
        button(chrome_.menu,1); button(chrome_.new_tab,2); button(chrome_.palette,3);
        if (zoom_return_.width) {
            button(zoom_return_,7);
            label({zoom_return_.x+8,zoom_return_.y+5,zoom_return_.width-16,20},"Return to split",accent,12,false,ChromeTextAlignment::center);
        }
        const auto tab_colors = TabColors{style.selected_tab_surface, style.selected_tab_border};
        for (const auto& tab : chrome_.tabs) {
            const bool selected = tab_state(tab.tab).selected;
            const double settle=tab_state(tab.tab).selection_amount, hovered=hover_amount(16+tab.tab.value*2);
            if (settle>0 || hovered>0) rectangle(tab.bounds, opacity(blend(raised,tab_colors.surface,settle),std::max(settle,hovered*.65)),4,
                high_contrast_?opacity(tab_colors.border,settle):Color{0,0,0,0},high_contrast_?1/scale_:0);
            if (selected) rectangle({tab.bounds.x+3,tab.bounds.y+tab.bounds.height-2/scale_,std::max(0.0,tab.bounds.width-6),2/scale_},accent,1/scale_);
            const auto watch=tab_state(tab.tab).watch;
            const auto caption=layout_tab_label(tab.bounds,watch);
            if(caption.indicator.width>0) {
                // Hollow = watching, filled = unread. Shape carries state even
                // in high contrast; neither implies command success or animates.
                const auto ink=selected?text:secondary;
                rectangle(caption.indicator,watch==TabWatchIndicator::unread?ink:Color{0,0,0,0},3,ink,1);
            }
            label(caption.text,tab_state(tab.tab).title,selected?text:secondary,13,selected);
            if (chrome_.compact) label({tab.bounds.x+std::max(0.0,tab.bounds.width-28),tab.bounds.y+6,std::min(24.0,tab.bounds.width),20},"▾",text,13,false,ChromeTextAlignment::center);
            if (!chrome_.compact && (selected || hovered>0)) {
                const double x = tab.close.x + tab.close.width / 2, y = tab.close.y + tab.close.height / 2;
                if (const auto amount=hover_amount(17+tab.tab.value*2); amount>0) rectangle(tab.close,opacity(chrome,amount),4);
                line(x - 3,y - 3,x + 3,y + 3,secondary); line(x + 3,y - 3,x - 3,y + 3,secondary);
            }
        }
        for (int row = 0; row < 3; ++row) line(chrome_.menu.x + 8,chrome_.menu.y + 10 + row * 5,chrome_.menu.x + 20,chrome_.menu.y + 10 + row * 5,secondary,1.2);
        const double nx = chrome_.new_tab.x + chrome_.new_tab.width/2, ny = chrome_.new_tab.y + chrome_.new_tab.height/2;
        if (chrome_.new_tab.width>0) { line(nx-5,ny,nx+5,ny,secondary,1.3); line(nx,ny-5,nx,ny+5,secondary,1.3); }
        const double px = chrome_.palette.x + 8, py = chrome_.palette.y + 10;
        for (int y=0;y<2;++y) for (int x=0;x<2;++x) rectangle({px+x*7,py+y*7,4,4},secondary,1);
        if(navigation_.focus.width>0 && navigation_.focus.height>0) {
            button(navigation_.focus,10);
            const auto ink=focus_mode_?accent:secondary;
            const double x=navigation_.focus.x+navigation_.focus.width/2,y=navigation_.focus.y+navigation_.focus.height/2;
            // Outward corners enter focus mode; inward corners restore navigation.
            for(const int sx:{-1,1})for(const int sy:{-1,1}) {
                const double corner=focus_mode_?3:6,end=focus_mode_?6:3;
                line(x+sx*corner,y+sy*end,x+sx*corner,y+sy*corner,ink);
                line(x+sx*corner,y+sy*corner,x+sx*end,y+sy*corner,ink);
            }
        }
        button(chrome_.minimize,4); button(chrome_.maximize,5); button(chrome_.close,6,true);
        const double cy = chrome_.band.height / 2;
        auto center = [](Rect r) { return r.x+r.width/2; };
        const double minx = center(chrome_.minimize), maxx = center(chrome_.maximize), closex = center(chrome_.close);
        line(minx-5,cy+3,minx+5,cy+3,text);
        if (maximized_) {
            rectangle({maxx-3,cy-5,8,8}, {0,0,0,0}, 0, text, 1);
            rectangle({maxx-5,cy-3,8,8}, chrome, 0, text, 1);
        } else rectangle({maxx-5,cy-5,10,10}, {0,0,0,0}, 0, text,1);
        line(closex-4,cy-4,closex+4,cy+4,text); line(closex+4,cy-4,closex-4,cy+4,text);
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
    CHECK(actual.panes.front().content == expected.panes.front().content);
    CHECK(actual.panes.front().frame == expected.panes.front().frame);
    CHECK(actual.panes.front().focused == expected.panes.front().focused);
    CHECK(actual.panes.front().cursor_on == expected.panes.front().cursor_on);
    CHECK(actual.panes.front().text_blink_on == expected.panes.front().text_blink_on);
    CHECK(actual.panes.front().cursor_reveal == expected.panes.front().cursor_reveal);
    const auto actual_order = chrome_paint_order(actual);
    const auto expected_order = chrome_paint_order(expected);
    CHECK(actual_order.size() == expected_order.size());
    for (std::size_t i = 0; i < actual_order.size(); ++i) {
        CHECK(actual_order[i].z == expected_order[i].z);
        CHECK(actual_order[i].kind == expected_order[i].kind);
        CHECK(actual_order[i].index == expected_order[i].index);
    }
    for (std::size_t i = 1; i < actual.primitives.size(); ++i) {
        CHECK(!actual.primitives[i].clip.has_value());
        CHECK(actual.primitives[i].z == ChromeZOrder{});
    }
    for (std::size_t i = 1; i < actual.texts.size(); ++i) {
        CHECK(!actual.texts[i].clip.has_value());
        CHECK(actual.texts[i].z == ChromeZOrder{});
    }
}

void emission_matrix() {
    const WindowHeaderRenderer renderer;
    constexpr std::array<double, 4> scales{1, 1.25, 1.5, 2};
    constexpr std::array<double, 7> widths{0, 45, 46, 100, 319, 640, 1280};
    constexpr std::array<unsigned, 4> counts{0, 1, 3, 16};
    constexpr std::array<double, 3> samples{0, .37, 1};
    for (const auto scale : scales) {
        for (const auto width : widths) {
            for (const auto count : counts) {
                for (unsigned flags = 0; flags < 256; ++flags) {
                    std::vector<TabId> ids;
                    for (unsigned i = 0; i < count; ++i) {
                        ids.push_back(TabId{101 + i * 17});
                    }
                    if (flags & 128) std::reverse(ids.begin(), ids.end());
                    const auto active = ids.empty() ? TabId{} : ids[flags % ids.size()];
                    auto layout = layout_chrome(width, 200, ids, active, 46, 32, 0, 40, 28);
                    layout.compact = (flags & 2) != 0;
                    std::vector<HeaderTabPresentation> tabs;
                    for (const auto& tab : layout.tabs) {
                        const unsigned seed = static_cast<unsigned>(tab.tab.value + flags);
                        tabs.push_back({
                            .geometry = tab,
                            .title = seed % 2 ? "Task café 文件 👩‍💻" : "",
                            .watch = static_cast<TabWatchIndicator>(seed % 3),
                            .selected = tab.tab == active,
                            .selection_amount = samples[(seed / 3) % samples.size()],
                            .hover_amount = samples[(seed / 5) % samples.size()],
                            .close_hover_amount = samples[(seed / 7) % samples.size()],
                        });
                    }
                    const auto amount = samples[flags % samples.size()];
                    const WindowHeaderView view{
                        .layout = layout,
                        .tabs = tabs,
                        .zoom_return = flags & 4 ? Rect{width - 140, 2, 120, 28} : Rect{},
                        .focus = flags & 8 ? Rect{width - 40, 38, 28, 28} : Rect{},
                        .hover = {amount, 1 - amount, amount, 1 - amount,
                                  amount, 1 - amount, amount, 1 - amount},
                        .width = width,
                        .maximized = (flags & 16) != 0,
                        .focus_mode = (flags & 32) != 0,
                    };
                    const WindowHeaderStyle style{
                        .surface = {23, 25, 29, static_cast<std::uint8_t>(flags & 64 ? 230 : 255)},
                        .raised = {45, 51, 63, 255},
                        .seam = {78, 87, 99, 140},
                        .ink = {236, 238, 242, 255},
                        .secondary = {144, 161, 177, 255},
                        .accent = {171, 209, 238, 255},
                        .selected_tab_surface = {64, 73, 81, 255},
                        .selected_tab_border = {201, 218, 233, 255},
                        .scale = scale,
                        .high_contrast = (flags & 1) != 0,
                    };
                    WindowScene original;
                    original.background = {3, 7, 11, 129};
                    original.chrome_generation = 912;
                    original.force_redraw = true;
                    auto retained_frame = std::make_shared<RenderFrame>();
                    retained_frame->generation = 481;
                    original.panes.push_back({
                        777, {2, 3, 4, 5}, retained_frame, false, false, false, .4f});
                    ChromePrimitive marker;
                    marker.rect = {2, 3, 4, 5};
                    marker.fill = {1, 2, 3, 4};
                    marker.clip = PixelRect{1, 1, 1, 1};
                    marker.z = {ChromeLayer::modal, 3};
                    original.primitives.push_back(marker);
                    ChromeText label;
                    label.rect = {4, 5, 6, 7};
                    label.text = "Existing clipped modal";
                    label.clip = PixelRect{3, 4, 5, 6};
                    label.z = {ChromeLayer::modal, 3};
                    original.texts.push_back(label);
                    auto actual = original;
                    auto expected = original;
                    append_original(expected, view, style);
                    renderer.append_scene(actual, view, style);
                    compare_scene(actual, expected);
                    CHECK(actual.primitives.front() == original.primitives.front());
                    CHECK(actual.texts.front() == original.texts.front());
                    CHECK(layout.tabs.size() == tabs.size());
                    for (std::size_t i = 0; i < tabs.size(); ++i) {
                        CHECK(tabs[i].geometry.tab == layout.tabs[i].tab);
                    }
                }
            }
        }
    }
}

} // namespace

int main() {
    return run_core_tests("window header renderer", emission_matrix);
}
