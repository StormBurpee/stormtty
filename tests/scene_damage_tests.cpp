#include "stormtty/scene_damage.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace stormtty;

namespace {
constexpr PixelRect surface{0, 0, 200, 160};
constexpr CellMetrics metrics{8, 10, 7};

void expect(const std::vector<PixelRect>& actual, std::vector<PixelRect> expected, const char* name) {
    if (actual != expected) {
        std::cerr << name << ": unexpected damage\n";
        for (const auto& rect : actual) {
            std::cerr << rect.x << ',' << rect.y << ',' << rect.width << ',' << rect.height << '\n';
        }
        std::exit(1);
    }
}

std::shared_ptr<RenderFrame> frame() {
    auto result = std::make_shared<RenderFrame>();
    result->columns = 10;
    result->rows = 6;
    result->cell_width = 8;
    result->cell_height = 10;
    for (std::uint64_t row = 0; row < 6; ++row) {
        auto line = std::make_shared<FrameRow>();
        line->id.words[0] = row + 1;
        line->cells.resize(10);
        line->cells[2].style.blink = row == 3;
        line->cells[2].span = 2;
        result->lines.push_back(line);
    }
    result->cursor.visible = true;
    result->cursor.row = 1;
    result->cursor.column = 2;
    return result;
}

WindowScene scene() {
    WindowScene result;
    result.panes.push_back({1, {10, 20, 80, 60}, frame()});
    return result;
}

std::vector<PixelRect> difference(const WindowScene& before, const WindowScene& after) {
    SceneDamageTracker tracker;
    tracker.commit(before);
    return tracker.damage(after, metrics, surface);
}

void retained_state() {
    SceneDamageTracker tracker;
    auto current = scene();
    expect(tracker.damage(current, metrics, surface), {surface}, "initial invalidation");
    tracker.commit(current);
    expect(tracker.damage(current, metrics, surface), {}, "committed scene");
    current.force_redraw = true;
    expect(tracker.damage(current, metrics, surface), {surface}, "explicit redraw");
    tracker.commit(current);
    current.force_redraw = false;
    expect(tracker.damage(current, metrics, surface), {}, "redraw not retained");
    tracker.invalidate();
    expect(tracker.damage(current, metrics, surface), {surface}, "draw/device/material failure invalidates");
    expect(tracker.damage(current, metrics, surface), {surface}, "query does not consume invalidation");
    tracker.commit(current);
    // Canvas commit precedes presentation. Busy/occluded retries do not redraw.
    expect(tracker.damage(current, metrics, surface), {}, "presentation retry after canvas commit");

    auto replacement = current;
    replacement.panes[0].frame = std::make_shared<RenderFrame>(*current.panes[0].frame);
    expect(tracker.damage(replacement, metrics, surface), {}, "equivalent immutable frame");
    std::weak_ptr<const RenderFrame> old = current.panes[0].frame;
    current.panes.clear();
    tracker.commit(replacement);
    if (!old.expired()) {
        std::cerr << "unchanged commit must replace retained frame ownership\n";
        std::exit(1);
    }
}

void full_and_pane_changes() {
    const auto before = scene();
    auto after = before;
    after.background.r++;
    expect(difference(before, after), {surface}, "scene background");
    after = before;
    after.panes.push_back(before.panes[0]);
    expect(difference(before, after), {surface}, "pane count");
    after = before;
    after.panes[0].id++;
    expect(difference(before, after), {surface}, "pane identity");
    after = before;
    after.panes[0].content.x++;
    expect(difference(before, after), {surface}, "pane geometry");
    after = before;
    after.panes[0].frame.reset();
    expect(difference(before, after), {{10, 20, 80, 60}}, "frame removed");
    expect(difference(after, before), {{10, 20, 80, 60}}, "frame inserted");
    expect(difference(after, after), {}, "both frames absent");
    for (int field = 0; field < 7; ++field) {
        after = before;
        auto changed = std::make_shared<RenderFrame>(*before.panes[0].frame);
        switch (field) {
        case 0: ++changed->columns; break;
        case 1: ++changed->rows; break;
        case 2: ++changed->cell_width; break;
        case 3: ++changed->cell_height; break;
        case 4: ++changed->background.r; break;
        case 5: ++changed->foreground.r; break;
        case 6: ++changed->image_placements_generation; break;
        }
        after.panes[0].frame = changed;
        expect(difference(before, after), {{10, 20, 80, 60}}, "frame dimensions/colors/images");
    }
}

void rows_and_cursor() {
    auto before = scene();
    auto after = before;
    auto changed = std::make_shared<RenderFrame>(*before.panes[0].frame);
    for (auto row : {0, 1, 4}) {
        auto line = std::make_shared<FrameRow>(*changed->lines[row]);
        ++line->generation;
        changed->lines[row] = line;
    }
    after.panes[0].frame = changed;
    expect(difference(before, after), {{10, 20, 80, 20}, {10, 60, 80, 10}}, "row coalescing and order");
    for (int field = 0; field < 4; ++field) {
        after = before;
        changed = std::make_shared<RenderFrame>(*before.panes[0].frame);
        if (field == 0) {
            changed->lines[4].reset();
        } else if (field == 1) {
            changed->lines.resize(4);
        } else if (field == 2) {
            auto line = std::make_shared<FrameRow>(*changed->lines[4]);
            ++line->id.words[0];
            changed->lines[4] = line;
        } else {
            // The row is clipped against pane height, not blindly appended.
            changed->lines.push_back(changed->lines[0]);
        }
        after.panes[0].frame = changed;
        const std::vector<PixelRect> expected = field == 3 ? std::vector<PixelRect>{} :
            std::vector<PixelRect>{{10, 60, 80, field == 1 ? 20.f : 10.f}};
        expect(difference(before, after), expected, "null/removed/identity/extra rows");
        expect(difference(after, before), expected, "reverse row transitions");
    }
    after = before;
    after.panes[0].text_blink_on = false;
    expect(difference(before, after), {{10, 50, 80, 10}}, "only blinking row");
    for (int field = 0; field < 8; ++field) {
        after = before;
        changed = std::make_shared<RenderFrame>(*before.panes[0].frame);
        switch (field) {
        case 0: changed->cursor.column = 6; break;
        case 1: changed->cursor.row = 4; break;
        case 2: changed->cursor.visible = false; break;
        case 3: changed->cursor.shape = CursorShape::bar; break;
        case 4: ++changed->cursor.color.r; break;
        case 5: after.panes[0].cursor_on = false; break;
        case 6: after.panes[0].focused = false; break;
        case 7: after.panes[0].cursor_reveal = .5f; break;
        }
        after.panes[0].frame = changed;
        std::vector<PixelRect> expected{{26, 30, 16, 10}};
        if (field == 0) {
            expected.push_back({58, 30, 8, 10});
        } else if (field == 1) {
            expected.push_back({26, 60, 16, 10});
        }
        expect(difference(before, after), expected, "cursor comparisons and old/new order");
    }
    after = before;
    changed = std::make_shared<RenderFrame>(*before.panes[0].frame);
    changed->cursor.blinking = true;
    changed->cursor.password_input = true;
    after.panes[0].frame = changed;
    expect(difference(before, after), {}, "nonvisual cursor metadata ignored");
    changed->cursor.column = 3;
    changed->cursor.wide_tail = true;
    expect(difference(before, after), {{26, 30, 16, 10}}, "wide tail uses lead cell span");

    changed->cell_width = 0;
    changed->cell_height = 0;
    expect({render_geometry::cursor_bounds(after.panes[0], metrics)}, {{26, 30, 16, 10}}, "fallback metrics");
    after.panes[0].content.width = 25;
    expect({render_geometry::cursor_bounds(after.panes[0], metrics)}, {{26, 30, 9, 10}}, "cursor pane clipping");
    changed->cursor.row = changed->rows;
    expect({render_geometry::cursor_bounds(after.panes[0], metrics)}, {{}}, "out of bounds cursor");
}

void images_and_clipping() {
    auto before = scene();
    auto old_frame = std::make_shared<RenderFrame>(*before.panes[0].frame);
    auto pixels = std::make_shared<ImagePixels>();
    pixels->image_id = 7;
    old_frame->images.push_back({pixels});
    before.panes[0].frame = old_frame;
    for (int field = 0; field < 5; ++field) {
        auto after = before;
        auto changed = std::make_shared<RenderFrame>(*old_frame);
        auto replacement = std::make_shared<ImagePixels>(*pixels);
        changed->images[0].pixels = replacement;
        switch (field) {
        case 0: ++replacement->image_id; break;
        case 1: ++replacement->generation; break;
        case 2: changed->images[0].pixels.reset(); break;
        case 3: changed->images.clear(); break;
        case 4: ++changed->images[0].placement_id; break;
        }
        after.panes[0].frame = changed;
        expect(difference(before, after), field == 4 ? std::vector<PixelRect>{} :
               std::vector<PixelRect>{{10, 20, 80, 60}}, "image identity/generation contract");
    }
    before.panes[0].content = {-10, 150, 80, 60};
    auto after = before;
    after.panes[0].frame.reset();
    expect(difference(before, after), {{0, 150, 70, 10}}, "surface clipping");
}

void chrome_and_fragmentation() {
    WindowScene before;
    before.primitives.push_back({{10, 10, 10, 10}});
    before.texts.push_back({{80, 20, 20, 10}, "before"});
    auto after = before;
    after.primitives[0].rect.x = 40;
    after.texts[0].rect.y = 50;
    expect(difference(before, after), {}, "chrome generation authoritative");
    ++after.chrome_generation;
    expect(difference(before, after), {{39, 9, 12, 12}, {9, 9, 12, 12},
                                      {80, 50, 20, 10}, {80, 20, 20, 10}}, "chrome old/new stable order");
    before = {};
    after = {};
    after.chrome_generation = 1;
    ChromePrimitive line{{30, 30, -20, -10}};
    line.kind = ChromePrimitiveKind::line;
    line.stroke_width = 4;
    line.clip = PixelRect{12.4f, 22.4f, 5.2f, 5.2f};
    after.primitives.push_back(line);
    expect(difference(before, after), {{12, 22, 6, 6}}, "line padding and outward viewport");
    after.primitives[0].radius = std::numeric_limits<float>::quiet_NaN();
    expect(difference(before, after), {}, "nonfinite primitive ignored");
    after.primitives.clear();
    after.texts.push_back({{0, 0, 20, 20}, "clipped"});
    after.texts[0].clip = PixelRect{5, 5, 0, 3};
    expect(difference(before, after), {}, "empty chrome clip");

    after.texts.clear();
    for (int i = 0; i < 128; ++i) {
        after.texts.push_back({{0, float(i), 20, 1}, "row"});
    }
    expect(difference(before, after), {{0, 0, 20, 128}}, "128 fragments still merge");
    after.texts.push_back({{0, 128, 20, 1}, "row"});
    expect(difference(before, after), {surface}, "129 fragments full before merge");
}
} // namespace

int main() {
    retained_state();
    full_and_pane_changes();
    rows_and_cursor();
    images_and_clipping();
    chrome_and_fragmentation();
    std::cout << "scene damage ownership and exact geometry passed\n";
    return 0;
}
