// Windows DirectWrite owner tests. Syntax checks are not native execution.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../src/platform/windows/text_resources.hpp"
#include "../src/platform/windows/graphics_error.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace stormtty;
using namespace stormtty::windows;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Apartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};

    ~Apartment() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

FontOptions system_font() {
    FontOptions font;
    font.family = "Consolas";
    return font;
}

FrameRow sample_row() {
    FrameRow row;
    row.id.words = {11, 22};
    row.generation = 3;
    row.cells.resize(4);
    for (auto& cell : row.cells) {
        cell.grapheme = "A";
    }
    return row;
}

constexpr RowShapeRequest request{7, 4, 16, 24};
constexpr std::size_t generous_budget = 1024 * 1024;

void cache_key_test() {
    TextResources text(generous_budget);
    text.configure(system_font(), 96);
    const auto row = sample_row();
    ShapedRow scratch;
    const auto& initial = text.shape(request, row, 1, scratch);
    require(!initial.clusters.empty(), "DirectWrite fixture produced no glyph clusters");
    require(&initial != &scratch, "Retained shape unexpectedly aliases caller scratch");
    require(initial.bytes == text.stats().retained_bytes, "Retained shape accounting changed");

    // Exercise every identity/geometry field independently; no frame mutation
    // or cache-map access is needed. Font generation is covered separately.
    for (unsigned dimension = 0; dimension < 7; ++dimension) {
        auto changed_request = request;
        auto changed_row = row;
        switch (dimension) {
        case 0:
            ++changed_request.pane_id;
            break;
        case 1:
            ++changed_row.id.words[0];
            break;
        case 2:
            ++changed_row.id.words[1];
            break;
        case 3:
            ++changed_row.generation;
            break;
        case 4:
            ++changed_request.columns;
            break;
        case 5:
            ++changed_request.cell_width;
            break;
        case 6:
            ++changed_request.cell_height;
            break;
        }
        const auto before = text.stats();
        text.shape(changed_request, changed_row, 2 + dimension * 2, scratch);
        require(text.stats().shaped_rows == before.shaped_rows + 1, "A row-key dimension failed to miss");
        text.shape(request, row, 3 + dimension * 2, scratch);
        require(text.stats().cache_hits == before.cache_hits + 1, "Original key was not retained independently");
    }
    require(text.stats().retained_bytes <= generous_budget, "Key fixtures exceeded retained budget");
}

void lru_test() {
    const auto row = sample_row();
    ShapedRow scratch;
    TextResources probe(generous_budget);
    probe.configure(system_font(), 96);
    const auto row_bytes = probe.shape(request, row, 1, scratch).bytes;

    // Measure today's actual allocation charge, not a font/compiler-specific
    // constant. All three requests shape identical content at identical metrics.
    TextResources text(row_bytes * 2);
    text.configure(system_font(), 96);
    auto a = request;
    auto b = request;
    auto c = request;
    ++b.pane_id;
    c.pane_id += 2;
    text.shape(a, row, 1, scratch);
    text.shape(b, row, 2, scratch);
    require(text.stats().retained_bytes == row_bytes * 2, "Two equal rows did not fit the exact budget");
    text.shape(a, row, 3, scratch);
    text.shape(c, row, 4, scratch);
    require(text.stats().retained_bytes == row_bytes * 2, "Eviction changed exact retained accounting");

    const auto before = text.stats();
    text.shape(a, row, 5, scratch);
    require(text.stats().cache_hits == before.cache_hits + 1, "Recently touched row was evicted");
    text.shape(b, row, 6, scratch);
    require(text.stats().shaped_rows == before.shaped_rows + 1, "Oldest render-tick row was not evicted");
    require(text.stats().retained_bytes <= row_bytes * 2, "LRU insertion exceeded retained budget");
}

void scratch_and_configuration_test() {
    const auto row = sample_row();
    for (const std::size_t budget : {std::size_t{0}, std::size_t{1}}) {
        TextResources text(budget);
        text.configure(system_font(), 96);
        ShapedRow scratch;
        const auto& shape = text.shape(request, row, 1, scratch);
        require(&shape == &scratch && !scratch.clusters.empty(), "Uncached result did not use caller scratch");
        require(text.stats().retained_bytes == 0, "Uncached result was charged to retained cache");
        text.shape(request, row, 2, scratch);
        require(text.stats().shaped_rows == 2 && text.stats().cache_hits == 0, "Uncached row unexpectedly hit");
    }

    TextResources text(generous_budget);
    auto font = system_font();
    require(text.configure(font, 96), "Initial font was not measured");
    ShapedRow scratch;
    text.shape(request, row, 1, scratch);
    const auto before = text.stats();
    require(!text.set_font(font) && !text.set_dpi(96), "Identical text settings remeasured");
    require(text.stats().retained_bytes == before.retained_bytes, "Identical settings discarded shapes");
    font.ligatures = !font.ligatures;
    require(text.set_font(font) && text.stats().retained_bytes == 0, "Ligature change did not invalidate shapes");
    text.shape(request, row, 2, scratch);
    require(text.set_dpi(144) && text.stats().retained_bytes == 0, "DPI change did not invalidate shapes");
    text.shape(request, row, 3, scratch);
    require(text.stats().shaped_rows == before.shaped_rows + 2, "Font generations reused stale shapes");
}

void chrome_borrow_test() {
    TextResources text(generous_budget);
    text.configure(system_font(), 96);
    ShapedRow scratch;
    const auto row = sample_row();
    const auto& shape = text.shape(request, row, 1, scratch);
    const auto glyphs = shape.clusters.front().glyphs;
    const auto before = text.stats();

    ChromeText label;
    label.family = "Consolas";
    label.text = "a long chrome label with trailing spaces  ";
    label.rect = {0, 0, 60, 40};
    const auto layout = text.chrome_paint_layout(label);
    require(bool(layout), "Paint layout has no owned DirectWrite object");
    const auto measured = text.measure_chrome_text(label);
    require(measured.width > label.rect.width, "Measurement incorrectly reused the ellipsized paint policy");
    const auto lines = text.wrap_chrome_text(label);
    std::string joined;
    for (const auto& line : lines) {
        joined += line;
    }
    require(joined == label.text, "Chrome wrapping lost text or trailing spaces");
    require(shape.clusters.front().glyphs == glyphs, "Chrome operations invalidated a borrowed row");
    require(text.stats().retained_bytes == before.retained_bytes && text.stats().shaped_rows == before.shaped_rows,
            "Chrome layout altered the terminal cache");

    Com<IDWriteTextLayout> independent_layout;
    {
        TextResources temporary(0);
        temporary.configure(system_font(), 96);
        independent_layout = temporary.chrome_paint_layout(label);
    }
    DWRITE_TEXT_METRICS metrics{};
    check(independent_layout->GetMetrics(&metrics), "Read independently owned chrome layout");
}

} // namespace

int main() {
    try {
        Apartment apartment;
        check(apartment.result, "Initialize text test COM apartment");
        cache_key_test();
        lru_test();
        scratch_and_configuration_test();
        chrome_borrow_test();
        std::cout << "PASS Windows text resources: cache keys, render-tick eviction, budgets, font/DPI, chrome ownership\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL Windows text resources: " << error.what() << '\n';
        return 1;
    }
}
