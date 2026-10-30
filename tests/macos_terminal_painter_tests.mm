#import <AppKit/AppKit.h>
#include "../src/platform/macos/terminal_painter.hpp"
#include "../src/platform/macos/chrome_painter.hpp"
#include "macos_terminal_reference.hpp"
#include <iostream>
#include <limits>
#include <tuple>

namespace {
using namespace stormtty;
using namespace stormtty::macos;
constexpr std::uint32_t width = 256, height = 160;
constexpr std::size_t upload_limit = 64u * 1024u * 1024u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

auto counters(const MetalTextureStats& value) {
    return std::tuple{value.shaped_rows, value.row_cache_hits, value.image_uploads,
                      value.dirty_rectangles, value.row_bytes, value.image_bytes};
}

struct Result {
    std::vector<std::uint8_t> pixels;
    MetalTextureStats stats;
    std::size_t remaining{};
    std::string error;
};

// Keep cache owners across frames to compare reuse, partial progress and recovery.
class Harness {
public:
    Harness(float dpi, bool reference) : dpi_(dpi), reference_(reference) {
        options_.font.family = "Menlo";
        options_.font.ligatures = true;
        options_.background_opacity = .45f;
        fonts_.reset(options_.font, dpi);
        owner_.create_device(textures_);
        owner_.update_layer_geometry(dpi);
        owner_.replace_surface(width, height, dpi);
    }

    Result draw(const WindowScene& scene, std::uint64_t font_generation = 7,
                std::size_t allowance = upload_limit, bool clip_probe = false) {
        Result result;
        @autoreleasepool {
            try {
                auto frame = owner_.begin_frame(textures_);
                require(bool(frame), "Offscreen terminal frame not admitted");
                if (allowance != upload_limit) {
                    bool charged = false;
                    try {
                        frame.draw_bgra(static_cast<std::uint32_t>((upload_limit - allowance) / 4),
                                        1, {}, {0, 0, 1, 1});
                    } catch (const std::runtime_error& error) {
                        charged = std::string_view(error.what()) ==
                                  "Metal texture dimensions exceed supported limits";
                    }
                    require(charged && textures_.remaining_upload_bytes() == allowance,
                            "Precharge changed native failure/accounting order");
                }
                frame.fill({0, 0, width, height}, {23, 25, 29, 128});
                if (reference_) {
                    test::terminal_reference::Original original{
                        fonts_, options_, textures_, dpi_, font_generation};
                    original.draw(scene, frame);
                } else {
                    TerminalPainter painter(frame, textures_, fonts_.borrow_font(),
                                            options_.font.locale, fonts_.metrics(), dpi_,
                                            options_.background_opacity, font_generation,
                                            options_.font.ligatures);
                    for (const auto& pane : scene.panes) painter.draw(pane);
                }
                // Observe the intentionally persistent cursor clip before reset.
                if (clip_probe) frame.fill({0, 0, width, height}, {200, 40, 90, 80});
                frame.clip({0, 0, width, height});
                ChromePainter chrome(frame, {0, 0, width, height}, dpi_);
                for (const auto& text : scene.texts) {
                    chrome.draw(text, textures_.remaining_upload_bytes());
                }
                frame.submit();
            } catch (const std::runtime_error& error) {
                result.error = error.what();
            }
        }
        result.stats = textures_.stats();
        result.remaining = textures_.remaining_upload_bytes();
        if (result.error.empty()) result.pixels = owner_.read_pixels();
        return result;
    }

private:
    float dpi_;
    bool reference_;
    RendererOptions options_;
    CoreTextFontResources fonts_;
    MetalTextureResources textures_{{1024u * 1024u, 1024u * 1024u, 1024}};
    MetalCompositor owner_{nullptr};
};

class Comparison {
public:
    explicit Comparison(float dpi) : original_(dpi, true), extracted_(dpi, false) {}

    Result draw(const WindowScene& scene, std::uint64_t generation = 7,
                std::size_t allowance = upload_limit, bool clip_probe = false) {
        const auto before = original_.draw(scene, generation, allowance, clip_probe);
        auto after = extracted_.draw(scene, generation, allowance, clip_probe);
        require(before.error == after.error, "Terminal extraction changed failure precedence");
        require(before.pixels == after.pixels, "Terminal extraction changed native pixels");
        require(before.remaining == after.remaining, "Terminal extraction changed shared upload budget");
        require(counters(before.stats) == counters(after.stats), "Terminal extraction changed cache counters");
        return after;
    }

private:
    Harness original_, extracted_;
};

std::shared_ptr<RenderFrame> terminal() {
    auto frame = std::make_shared<RenderFrame>();
    frame->columns = 8;
    frame->rows = 4;
    frame->cell_width = 13;
    frame->cell_height = 23;
    frame->background = {13, 25, 39, 255};
    for (unsigned index = 0; index < 3; ++index) {
        auto row = std::make_shared<FrameRow>();
        row->id.words = {index + 1, 17};
        row->generation = 3;
        for (const auto& text : {"A", "e\xCC\x81", "界", "", "fi", "B", "C", "D"}) {
            Cell cell;
            cell.grapheme = text;
            cell.foreground = {220, 190, 130, 255};
            cell.background = {45, 60, 90, 255};
            row->cells.push_back(cell);
        }
        row->cells[0].background_is_default = false;
        row->cells[1].style.inverse = true;
        row->cells[2].span = 2;
        row->cells[3].span = 0;
        row->cells[4].selected = true;
        row->cells[5].style.blink = true;
        row->cells[6].style.underline = 3;
        frame->lines.push_back(row);
    }
    frame->lines.push_back(nullptr);
    frame->cursor = {3, 1, true, false, true, false, CursorShape::block, {190, 200, 210, 255}};
    return frame;
}

RenderPane pane(FramePtr frame, std::uint64_t id = 11) {
    return {id, {7.25f, 9.5f, 105.5f, 89.25f}, std::move(frame), true, true, false};
}

std::shared_ptr<ImagePixels> image(std::uint32_t id) {
    auto pixels = std::make_shared<ImagePixels>();
    pixels->image_id = id;
    pixels->generation = 8;
    pixels->width = pixels->height = 4;
    for (unsigned index = 0; index < 16; ++index) {
        pixels->bytes.insert(pixels->bytes.end(),
                             {static_cast<std::uint8_t>(index * 15), 180, 80, 170});
    }
    return pixels;
}

void layers_and_crops(float dpi) {
    Comparison comparison(dpi);
    auto frame = terminal();
    for (auto layer : {ImageLayer::below_background, ImageLayer::below_text, ImageLayer::above_text}) {
        ImagePlacement placement;
        placement.layer = layer;
        placement.pixels = image(static_cast<unsigned>(layer) + 1);
        placement.column = static_cast<int>(layer);
        placement.row = 1;
        placement.columns = 3;
        placement.rows = 2;
        placement.source_x = 1;
        placement.source_y = 1;
        placement.source_width = 2;
        placement.source_height = 3;
        frame->images.push_back(placement);
        placement.column = -1; // Partial cull: preserve destination and cropped UV.
        placement.pixel_width = 30;
        placement.pixel_height = 17;
        frame->images.push_back(placement);
    }
    auto invalid = frame->images.front();
    auto malformed = image(99);
    malformed->bytes.clear();
    invalid.pixels = malformed;
    invalid.source_width = 0; // Defaults to full image width, exceeding remainder.
    frame->images.push_back(invalid);
    invalid.source_x = std::numeric_limits<std::uint32_t>::max();
    frame->images.push_back(invalid);
    invalid.source_x = 0;
    invalid.column = 100; // Valid crop but culled: invalid pixel data never uploaded.
    frame->images.push_back(invalid);
    invalid.pixels.reset();
    frame->images.push_back(invalid);
    WindowScene scene;
    scene.panes.push_back(pane(frame));
    const auto first = comparison.draw(scene);
    require(first.error.empty() && first.stats.image_uploads == 3 && first.stats.shaped_rows == 4,
            "Layer/crop fixture did not exercise rows, cursor and three images");
    const auto repeat = comparison.draw(scene);
    require(repeat.remaining == upload_limit && repeat.stats.row_cache_hits == 4,
            "Repeated pane failed to reuse ordinary and cursor row variants");
    scene.panes.push_back(pane(frame, 12));
    scene.panes.back().content.x = 135.5f;
    const auto other_pane = comparison.draw(scene);
    require(other_pane.stats.shaped_rows == 8 && other_pane.stats.image_uploads == 6,
            "Pane identity no longer separates row/image cache entries");
    const auto other_font = comparison.draw(scene, 8);
    require(other_font.stats.shaped_rows == 16 && other_font.stats.image_uploads == 6,
            "Font generation no longer separates row cache entries");
    frame->cursor.column = 5;
    frame->cursor.wide_tail = false;
    const auto other_cursor = comparison.draw(scene, 8);
    require(other_cursor.stats.shaped_rows == 18,
            "Adjusted cursor column no longer separates cursor row variants");
    for (auto& item : scene.panes) item.text_blink_on = true;
    const auto blink = comparison.draw(scene, 8);
    require(blink.stats.shaped_rows == 24,
            "Ordinary blinking rows did not separate from forced-on cursor rows");
    auto changed_row = std::make_shared<FrameRow>(*frame->lines[0]);
    ++changed_row->generation;
    frame->lines[0] = changed_row;
    require(comparison.draw(scene, 8).stats.shaped_rows == 26,
            "Row generation no longer invalidates each pane's row entry");
    auto renamed_row = std::make_shared<FrameRow>(*frame->lines[0]);
    ++renamed_row->id.words[1];
    frame->lines[0] = renamed_row;
    require(comparison.draw(scene, 8).stats.shaped_rows == 28,
            "Row identity no longer separates each pane's row entry");
}

void overlapping_pane_replacement(float dpi) {
    Comparison comparison(dpi);
    auto lower = std::make_shared<RenderFrame>();
    auto upper = std::make_shared<RenderFrame>();
    lower->background = {210, 40, 20, 255};
    upper->background = {20, 190, 80, 255};
    WindowScene scene;
    scene.panes = {pane(lower), pane(upper, 12)};
    scene.panes[1].content = {40.5f, 30.25f, 100, 90};
    const auto overlap = comparison.draw(scene);
    scene.panes.erase(scene.panes.begin());
    const auto isolated = comparison.draw(scene);
    require(overlap.error.empty() && isolated.error.empty(), "Overlapping pane fixture failed");
    const auto sample = (50u * width + 60u) * 4;
    require(std::equal(overlap.pixels.begin() + sample, overlap.pixels.begin() + sample + 4,
                       isolated.pixels.begin() + sample) && overlap.pixels[sample + 3] == 115,
            "Upper pane accumulated lower-pane opacity instead of replacing it");
}

void cursors_and_geometry(float dpi) {
    Comparison comparison(dpi);
    unsigned generation = 10;
    for (auto shape : {CursorShape::block, CursorShape::bar,
                       CursorShape::underline, CursorShape::hollow_block}) {
        for (bool focused : {false, true}) {
            auto frame = terminal();
            frame->cursor.shape = shape;
            WindowScene scene;
            scene.panes.push_back(pane(frame));
            scene.panes.back().focused = focused;
            // Clip the wide cursor and rows at fractional pane edges.
            scene.panes.back().content.width = 35.5f;
            scene.panes.back().content.height = 37.25f;
            require(comparison.draw(scene, generation++, upload_limit, true).error.empty(),
                    "Cursor geometry fixture failed");
        }
    }
    for (unsigned mode = 0; mode < 8; ++mode) {
        auto frame = terminal();
        WindowScene scene;
        scene.panes.push_back(pane(frame));
        if (mode == 0) frame->cursor.row = 3; // Null row still paints cursor.
        if (mode == 1) frame->cursor.column = 8; // Bounds skip.
        if (mode == 2) scene.panes[0].cursor_on = false;
        if (mode == 3) scene.panes[0].text_blink_on = true;
        if (mode == 4) frame->cell_width = frame->cell_height = 0; // Metric fallback.
        if (mode == 5) {
            frame->cursor.column = 5; // Blink cell cursor forces visible glyph.
            frame->cursor.wide_tail = false;
        }
        if (mode == 6) {
            frame->cursor.column = 1; // Inverse lead cell under block cursor.
            frame->cursor.wide_tail = false;
        }
        if (mode == 7) frame->cursor.visible = false;
        require(comparison.draw(scene, generation++, upload_limit, true).error.empty(),
                "Cursor null-row/blink/fallback fixture failed");
    }
}

void guards_and_failures(float dpi) {
    Comparison comparison(dpi);
    WindowScene scene;
    auto excessive = terminal();
    excessive->lines.resize(4097);
    scene.panes.push_back(pane(nullptr));
    scene.panes.back().content.x = std::numeric_limits<float>::quiet_NaN();
    scene.panes.push_back(pane(excessive));
    scene.panes.back().content = {300, 200, 10, 10};
    require(comparison.draw(scene).error.empty(), "Null/culled pane did not skip before validation");
    scene.panes.back().content = {0, 0, 100, 100};
    require(comparison.draw(scene).error == "Terminal frame exceeds renderer row/placement limits",
            "Visible excessive row count was not rejected");
    excessive->lines.clear();
    excessive->images.resize(1025);
    require(comparison.draw(scene).error == "Terminal frame exceeds renderer row/placement limits",
            "Visible excessive image count was not rejected");

    scene.panes = {pane(terminal())};
    auto huge = terminal();
    huge->columns = 20000;
    huge->cell_width = huge->cell_height = 1;
    huge->lines.resize(1);
    huge->cursor.visible = false;
    scene.panes.push_back(pane(huge, 22));
    const auto failed = comparison.draw(scene);
    require(failed.error == "Metal texture dimensions exceed supported limits" &&
            failed.stats.shaped_rows == 4 && failed.remaining == upload_limit - 4 * 104 * 23 * 4 - 80000,
            "Native row upload failure lost charged partial progress");
    scene.panes.resize(1);
    const auto recovery = comparison.draw(scene);
    require(recovery.error.empty() && recovery.remaining == upload_limit &&
            recovery.stats.row_cache_hits == 4 && !recovery.pixels.empty(),
            "Next frame did not recover with retained successful cache work and fresh allowance");
}

void mixed_budget(float dpi) {
    for (std::size_t allowance : {256u, 320u, 576u, 1600u}) {
        Comparison comparison(dpi);
        auto frame = terminal();
        frame->columns = 4;
        frame->cell_width = frame->cell_height = 4;
        frame->lines.resize(2);
        frame->cursor.visible = false;
        ImagePlacement placement;
        placement.pixels = image(1); // 64 bytes, before rows.
        placement.layer = ImageLayer::below_text;
        frame->images.push_back(placement);
        WindowScene scene;
        scene.panes.push_back(pane(frame)); // Two 256-byte rows.
        ChromeText label;
        label.rect = {130, 10, 16, 16}; // 1024 bytes after terminal painting.
        label.text = "A";
        label.family = "Menlo";
        scene.texts.push_back(label);
        const auto result = comparison.draw(scene, 7, allowance);
        if (allowance < 576) {
            require(result.error == "Visible CoreText rows exceed transient texture budget" &&
                    result.stats.image_uploads == 1 &&
                    result.stats.shaped_rows == (allowance >= 320 ? 1u : 0u),
                    "Mixed allowance did not fail after expected image/row progress");
        } else {
            require(result.error.empty() && result.remaining == 0 && result.stats.shaped_rows == 2,
                    "Terminal/next chrome did not consume fresh shared allowance");
        }
        const auto recovery = comparison.draw(scene);
        require(recovery.error.empty(), "Mixed budget failure blocked next-frame recovery");
    }
}
} // namespace

int main() {
    @autoreleasepool {
        try {
            for (float dpi : {96.f, 144.f, 192.f}) {
                std::cerr << "terminal original/extracted comparison dpi=" << dpi << std::endl;
                layers_and_crops(dpi);
                overlapping_pane_replacement(dpi);
                cursors_and_geometry(dpi);
                guards_and_failures(dpi);
                mixed_budget(dpi);
            }
            std::cout << "PASS native macOS terminal pixels, cache identities, budgets and recovery\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL macOS terminal painter: " << error.what() << '\n';
            return 1;
        }
    }
}
