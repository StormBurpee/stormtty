#import <Foundation/Foundation.h>
#include "../src/platform/macos/core_text_font_resources.hpp"
#include "../src/platform/macos/core_text_row_rasterizer.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using namespace stormtty;
using namespace stormtty::macos;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct Fixture {
    CoreTextFontResources font;
    RowRasterOptions options;

    explicit Fixture(float dpi = 96) {
        FontOptions settings;
        settings.family = "Menlo";
        font.reset(settings, dpi);
        options = {128, 48, 24, font.metrics(), dpi, true, false, "en-US", 128u * 48u * 4u};
    }

    RasterizedRow draw(std::span<const Cell> cells) const {
        return CoreTextRowRasterizer{}.rasterize(cells, font.borrow_font(), options);
    }
};

Cell letter(std::string text = "A") {
    Cell cell;
    cell.grapheme = std::move(text);
    cell.foreground = {220, 140, 60, 17};
    cell.background = {30, 60, 90, 19};
    cell.underline_color = {80, 160, 240, 23};
    return cell;
}

std::size_t ink(const RasterizedRow& row) {
    std::size_t count = 0;
    for (std::size_t offset = 3; offset < row.bgra.size(); offset += 4) {
        count += row.bgra[offset] != 0;
    }
    return count;
}

RasterizedRow draw_cell(const Fixture& fixture, const Cell& cell) {
    return fixture.draw(std::span<const Cell>(&cell, 1));
}

void pixels_and_ownership_test() {
    Fixture fixture;
    const auto blank = fixture.draw({});
    require(blank.width == 128 && blank.height == 48 && blank.bgra.size() == 128u * 48u * 4u,
            "Row output dimensions/stride changed");
    require(ink(blank) == 0, "Blank row has a painted background");

    const auto cell = letter();
    auto first = draw_cell(fixture, cell);
    const auto original = first.bgra;
    require(ink(first) > 4, "CoreText row has no ink");
    bool opaque = false;
    for (std::size_t offset = 0; offset < first.bgra.size(); offset += 4) {
        const auto alpha = first.bgra[offset + 3];
        opaque |= alpha == 255;
        require(first.bgra[offset] <= alpha && first.bgra[offset + 1] <= alpha &&
                    first.bgra[offset + 2] <= alpha, "Output is not premultiplied BGRA");
    }
    require(opaque, "Cell alpha incorrectly made text translucent");
    const auto second = draw_cell(fixture, cell);
    first.bgra[0] = 255;
    fixture.font.reset(FontOptions{"Menlo", "en-US", 18, false}, 192);
    require(second.bgra == original && first.bgra != second.bgra,
            "Raster output borrowed another output or font lifetime");
}

void color_and_hiding_test() {
    Fixture fixture;
    auto actual = letter();
    auto expected = actual;
    actual.style.inverse = true;
    expected.foreground = expected.background;
    require(draw_cell(fixture, actual).bgra == draw_cell(fixture, expected).bgra,
            "Inverse foreground changed");

    actual = letter();
    expected = actual;
    actual.selected = true;
    actual.style.faint = true;
    expected.foreground = {141, 142, 145, 255}; // Truncation of selected RGB * 0.6.
    require(draw_cell(fixture, actual).bgra == draw_cell(fixture, expected).bgra,
            "Selection/faint ordering changed");

    for (unsigned mode = 0; mode < 4; ++mode) {
        actual = letter();
        if (mode == 0) actual.grapheme.clear();
        if (mode == 1) actual.image_placeholder = true;
        if (mode == 2) actual.style.invisible = true;
        if (mode == 3) actual.style.blink = true;
        fixture.options.text_blink_on = false;
        require(ink(draw_cell(fixture, actual)) == 0, "Hidden cell produced text ink");
    }
    actual = letter();
    actual.style.blink = true;
    fixture.options.text_blink_on = true;
    require(ink(draw_cell(fixture, actual)) > 0, "Visible blink phase has no text");
}

void decorations_test() {
    for (const float dpi : {96.f, 192.f}) {
        Fixture fixture(dpi);
        for (unsigned decoration = 0; decoration < 7; ++decoration) {
            auto cell = letter("");
            if (decoration < 5) cell.style.underline = static_cast<std::uint8_t>(decoration + 1);
            if (decoration == 5) cell.style.overline = true;
            if (decoration == 6) cell.style.strikethrough = true;
            const auto empty = draw_cell(fixture, cell);
            require(ink(empty) > 0, "Empty cell lost its decoration");
            cell.image_placeholder = true;
            cell.grapheme = "A";
            require(draw_cell(fixture, cell).bgra == empty.bgra,
                    "Image placeholder changed decoration visibility");
            cell.style.invisible = true;
            require(ink(draw_cell(fixture, cell)) == 0, "Invisible decoration produced ink");
            cell.style.invisible = false;
            cell.style.blink = true;
            fixture.options.text_blink_on = false;
            require(ink(draw_cell(fixture, cell)) == 0, "Off blink decoration produced ink");
            fixture.options.text_blink_on = true;
        }
    }
}

void clusters_and_replacements_test() {
    Fixture fixture;
    std::vector<Cell> cells{letter("界"), letter("ignored continuation"), letter("e\xCC\x81"), letter("👩‍💻")};
    cells[0].span = 2;
    cells[1].span = 0;
    cells[3].span = 2;
    const auto original = cells;
    const auto result = fixture.draw(cells);
    require(ink(result) > 0 && cells == original, "Rasterization mutated VT cells or produced no ink");
    cells[1].grapheme.clear();
    require(fixture.draw(cells).bgra == result.bgra, "Continuation cell changed shaping");

    auto replacement = letter("\xEF\xBF\xBD");
    auto oversized = letter(std::string(4097, 'x'));
    require(draw_cell(fixture, oversized).bgra == draw_cell(fixture, replacement).bgra,
            "Oversized grapheme substitution changed");
    const auto invalid = letter(std::string(1, char(0xff)));
    require(draw_cell(fixture, invalid).bgra == draw_cell(fixture, replacement).bgra,
            "Invalid UTF-8 replacement changed");

    // Preserve 64 KiB text accounting, including headroom reserved for later
    // replacement glyphs. Fifteen 4 KiB clusters fit; the final two substitute.
    Fixture text_budget;
    text_budget.options.width = 512;
    text_budget.options.maximum_output_bytes = 512u * 48u * 4u;
    std::vector<Cell> long_row(17, letter(std::string(4096, ' ')));
    auto substituted = long_row;
    substituted[15].grapheme = "\xEF\xBF\xBD";
    substituted[16].grapheme = "\xEF\xBF\xBD";
    const auto bounded = text_budget.draw(long_row);
    require(bounded.bgra == text_budget.draw(substituted).bgra && ink(bounded) > 0,
            "Text-byte headroom or replacement mapping changed");
    require(long_row[15].grapheme.size() == 4096, "Text budgeting modified source cells");

    auto styled = letter("fi");
    styled.style.bold = true;
    styled.style.italic = true;
    fixture.options.ligatures = true;
    fixture.options.locale = "ja-JP";
    require(ink(draw_cell(fixture, styled)) > 0, "Trait/ligature/locale input produced no ink");
}

void bounds_test() {
    Fixture fixture;
    std::vector<Cell> cells(4097);
    const auto fails_with = [&](std::string_view expected) {
        bool failed = false;
        try {
            (void)fixture.draw(cells);
        } catch (const std::runtime_error& error) {
            failed = error.what() == expected;
        }
        require(failed, "Row bound or failure ordering changed");
    };
    --fixture.options.maximum_output_bytes;
    fails_with("Visible CoreText rows exceed transient texture budget");
    ++fixture.options.maximum_output_bytes;
    fails_with("CoreText row exceeds the 4096-cell shaping budget");
    cells.resize(4096);
    require(ink(fixture.draw(cells)) == 0, "Boundary-size blank row failed");
}

} // namespace

int main() {
    @autoreleasepool {
        try {
            pixels_and_ownership_test();
            color_and_hiding_test();
            decorations_test();
            clusters_and_replacements_test();
            bounds_test();
            std::cout << "PASS native macOS CPU CoreText row rasterization\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL macOS row rasterizer: " << error.what() << '\n';
            return 1;
        }
    }
}
