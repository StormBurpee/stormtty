#include "terminal_frame_materializer.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace stormtty {
namespace {

void checked(GhosttyResult result, const char* operation) {
    if (result != GHOSTTY_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (Ghostty result " + std::to_string(result) + ")");
}

template<class T>
T sized() {
    T value{};
    value.size = sizeof(T);
    return value;
}

Color color(GhosttyColorRgb value) {
    return {value.r, value.g, value.b, 255};
}

Color resolve(GhosttyStyleColor value, const GhosttyRenderStateColors& palette, Color fallback) {
    switch (value.tag) {
    case GHOSTTY_STYLE_COLOR_RGB: return color(value.value.rgb);
    case GHOSTTY_STYLE_COLOR_PALETTE: return color(palette.palette[value.value.palette]);
    default: return fallback;
    }
}

struct RowHash {
    std::size_t operator()(const RowId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.words[0]) ^ (std::hash<std::uint64_t>{}(id.words[1]) << 1);
    }
};

bool bracketed_paste_enabled(GhosttyTerminal terminal) {
    GhosttyTerminalModeConfig config{GHOSTTY_MODE_BRACKETED_PASTE, false};
    checked(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MODE, &config), "query terminal mode");
    return config.value;
}
} // namespace

void TerminalFrameMaterializer::initialize() {
    checked(ghostty_render_state_new(nullptr, &render_.value), "create render state");
    checked(ghostty_render_state_row_iterator_new(nullptr, &rows_.value), "create row iterator");
    checked(ghostty_render_state_row_cells_new(nullptr, &cells_.value), "create cells iterator");
    checked(ghostty_render_state_image_iterator_new(nullptr, &images_.value), "create image iterator");
}

GhosttyRenderStateColors TerminalFrameMaterializer::initial_colors() const {
    auto colors = sized<GhosttyRenderStateColors>();
    checked(ghostty_render_state_get(render_, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors),
            "read initial colors");
    return colors;
}

FramePtr TerminalFrameMaterializer::capture(
    GhosttyTerminal terminal, TerminalSize geometry, std::uint64_t image_budget_bytes) {
    // Kept under exclusive actor ownership THROUGH image pixel copying. Begin /
    // end separation is intentionally not used: image views are terminal-owned.
    checked(ghostty_render_state_update(render_, terminal), "update render state");
    GhosttyRenderStateDirty dirty{};
    checked(ghostty_render_state_get(render_, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty), "read frame dirtiness");
    std::uint64_t image_generation{};
    checked(ghostty_render_state_get(render_, GHOSTTY_RENDER_STATE_DATA_IMAGE_PLACEMENTS_GENERATION,
        &image_generation),
            "read placement generation");
    GhosttyTerminalScreen screen{};
    checked(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen), "read active screen");
    bool mouse{};
    checked(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &mouse),
            "read mouse reporting");
    const bool alternate = screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;

    // Dynamic colors and some cursor metadata are updated by Ghostty even when
    // no rows/global flags are dirty. Check these cheap fields before reusing an
    // owned frame; changed defaults require re-resolving previously clean rows.
    auto colors = sized<GhosttyRenderStateColors>();
    checked(ghostty_render_state_get(render_, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors), "read colors");
    const auto foreground = color(colors.foreground), background = color(colors.background);
    if (frame_ && (frame_->foreground != foreground || frame_->background != background))
        dirty = GHOSTTY_RENDER_STATE_DIRTY_FULL;

    auto cursor = sized<GhosttyRenderStateCursor>();
    checked(ghostty_render_state_get(render_, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cursor), "read cursor");

    FrameCursor next_cursor;
    if (cursor.viewport_has_value) {
        next_cursor.column = cursor.viewport_x;
        next_cursor.row = cursor.viewport_y;
        next_cursor.wide_tail = cursor.wide_tail;
    }
    next_cursor.visible = cursor.visible && cursor.viewport_has_value;
    next_cursor.blinking = cursor.blinking;
    next_cursor.password_input = cursor.password_input;
    next_cursor.color = colors.cursor_has_value ? color(colors.cursor) : foreground;
    switch (cursor.visual_style) {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
        next_cursor.shape = CursorShape::bar;
        break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
        next_cursor.shape = CursorShape::underline;
        break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW:
        next_cursor.shape = CursorShape::hollow_block;
        break;
    default:
        next_cursor.shape = CursorShape::block;
        break;
    }

    const bool bracketed_paste = bracketed_paste_enabled(terminal);
    if (frame_ && dirty == GHOSTTY_RENDER_STATE_DIRTY_FALSE && next_cursor == frame_->cursor &&
        image_generation == frame_->image_placements_generation && alternate == frame_->alternate_screen &&
        mouse == frame_->mouse_reporting && bracketed_paste == frame_->bracketed_paste &&
        frame_->columns == geometry.columns && frame_->rows == geometry.rows &&
        frame_->cell_width == geometry.cell_width && frame_->cell_height == geometry.cell_height) {
        return frame_;
    }

    // Counters advance during construction, as before: a failed capture keeps
    // the prior frame but does not roll back generation numbers already spent.
    auto next = std::make_shared<RenderFrame>();
    next->generation = ++frame_generation_;
    next->columns = geometry.columns;
    next->rows = geometry.rows;
    next->cell_width = geometry.cell_width;
    next->cell_height = geometry.cell_height;
    next->alternate_screen = alternate;
    next->mouse_reporting = mouse;
    next->bracketed_paste = bracketed_paste;
    next->image_placements_generation = image_generation;
    next->foreground = foreground;
    next->background = background;
    next->cursor = next_cursor;

    std::unordered_map<RowId, std::shared_ptr<const FrameRow>, RowHash> previous;
    if (frame_) {
        for (const auto& row : frame_->lines) previous.emplace(row->id, row);
    }

    checked(ghostty_render_state_get(render_, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &rows_.value),
            "read rows");
    next->lines.reserve(geometry.rows);
    while (ghostty_render_state_row_iterator_next(rows_)) {
        GhosttyRenderStateRowId raw_id{};
        bool row_dirty{};
        checked(ghostty_render_state_row_get(rows_, GHOSTTY_RENDER_STATE_ROW_DATA_ID, &raw_id), "read row ID");
        checked(ghostty_render_state_row_get(rows_, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &row_dirty),
                "read row dirty state");

        RowId id{{raw_id.bits[0], raw_id.bits[1]}};
        auto cached = previous.find(id);
        if (dirty != GHOSTTY_RENDER_STATE_DIRTY_FULL && !row_dirty && cached != previous.end()) {
            next->lines.push_back(cached->second);
            continue;
        }

        auto row = std::make_shared<FrameRow>();
        row->id = id;
        auto selection = sized<GhosttyRenderStateRowSelection>();
        const auto selection_result = ghostty_render_state_row_get(
            rows_, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &selection);
        if (selection_result == GHOSTTY_SUCCESS) row->selection = SelectionRange{selection.start_x, selection.end_x};
        else if (selection_result != GHOSTTY_NO_VALUE) checked(selection_result, "read row selection");

        checked(ghostty_render_state_row_get(rows_, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells_.value),
                "read row cells");
        row->cells.reserve(geometry.columns);
        while (ghostty_render_state_row_cells_next(cells_)) {
            Cell cell;
            GhosttyCell raw{};
            auto style = sized<GhosttyStyle>();
            const GhosttyRenderStateRowCellsData keys[]{
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE};
            void* values[]{&raw, &style};
            checked(ghostty_render_state_row_cells_get_multi(cells_, 2, keys, values, nullptr),
                    "read cell fields");

            std::uint32_t codepoint{};
            checked(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_CODEPOINT, &codepoint), "read base codepoint");
            cell.image_placeholder = codepoint == 0x10EEEE;
            GhosttyCellWide wide{};
            checked(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide), "read cell span");
            cell.span = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : wide == GHOSTTY_CELL_WIDE_NARROW ? 1 : 0;
            checked(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_HAS_HYPERLINK, &cell.hyperlink),
                    "read cell hyperlink presence");

            std::array<std::uint8_t, 64> text{};
            GhosttyBuffer buffer{text.data(), text.size(), 0};
            auto result = ghostty_render_state_row_cells_get(
                cells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buffer);
            if (result == GHOSTTY_OUT_OF_SPACE) {
                cell.grapheme.resize(buffer.len);
                buffer.ptr = reinterpret_cast<std::uint8_t*>(cell.grapheme.data());
                buffer.cap = cell.grapheme.size();
                checked(ghostty_render_state_row_cells_get(cells_,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buffer),
                        "copy grapheme");
            } else {
                checked(result, "read grapheme");
                cell.grapheme.assign(reinterpret_cast<const char*>(text.data()), buffer.len);
            }

            GhosttyColorRgb fg{}, bg{};
            auto fg_result = ghostty_render_state_row_cells_get(
                cells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);
            auto bg_result = ghostty_render_state_row_cells_get(
                cells_, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg);
            if (fg_result != GHOSTTY_SUCCESS && fg_result != GHOSTTY_INVALID_VALUE)
                checked(fg_result, "read foreground");
            if (bg_result != GHOSTTY_SUCCESS && bg_result != GHOSTTY_INVALID_VALUE)
                checked(bg_result, "read background");
            cell.foreground = fg_result == GHOSTTY_SUCCESS ? color(fg) : next->foreground;
            cell.background_is_default = bg_result != GHOSTTY_SUCCESS;
            cell.background = bg_result == GHOSTTY_SUCCESS ? color(bg) : next->background;
            cell.underline_color = resolve(style.underline_color, colors, cell.foreground);
            cell.style = {style.bold, style.italic, style.faint, style.blink, style.inverse,
                          style.invisible, style.strikethrough, style.overline,
                          static_cast<std::uint8_t>(style.underline)};
            const auto column = row->cells.size();
            cell.selected = row->selection && column >= row->selection->first && column <= row->selection->last;
            row->cells.push_back(std::move(cell));
        }

        // Cursor movement dirties its old/new rows even if their text is unchanged.
        // Preserve content generations so renderers can reuse shaped glyph runs.
        if (cached != previous.end() && cached->second->cells == row->cells &&
            cached->second->selection == row->selection)
            next->lines.push_back(cached->second);
        else {
            row->generation = ++row_generation_;
            next->lines.push_back(std::move(row));
        }
    }

    if (next->lines.size() != geometry.rows)
        throw std::runtime_error("render snapshot row count mismatch");

    if (frame_ && frame_->image_placements_generation == image_generation) next->images = frame_->images;
    else capture_images(terminal, image_budget_bytes, *next);

    // Immutable host-owned frame is now complete; cleaning cannot invalidate it.
    checked(ghostty_render_state_clean(render_), "clean consumed render state");
    frame_ = std::move(next);
    return frame_;
}

void TerminalFrameMaterializer::capture_images(
    GhosttyTerminal terminal, std::uint64_t image_budget_bytes, RenderFrame& target) {
    if (!image_budget_bytes) return;
    checked(ghostty_render_state_get(render_, GHOSTTY_RENDER_STATE_DATA_IMAGE_ITERATOR, &images_.value),
            "read image placements");

    std::unordered_map<std::uint64_t, std::shared_ptr<const ImagePixels>> cache;
    if (frame_) {
        for (const auto& placement : frame_->images)
            cache.emplace(placement.pixels->generation, placement.pixels);
    }

    // Only newly copied payloads consume this capture's byte allowance. Cached
    // generations and repeated placements continue sharing their immutable data.
    std::size_t copied_bytes{};
    GhosttyKittyGraphics storage{};
    checked(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &storage),
            "get image storage");
    while (ghostty_render_state_image_iterator_next(images_)) {
        ImagePlacement placement;
        std::uint32_t image_id{};
        std::uint64_t generation{};
        GhosttyImageProtocol protocol{};
        GhosttyRenderStateImageLayer layer{};
        GhosttyRenderStateKittyPlacementKind kind{};
        const GhosttyRenderStateImageData keys[]{
            GHOSTTY_RENDER_STATE_IMAGE_DATA_PROTOCOL, GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_ID,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_GENERATION, GHOSTTY_RENDER_STATE_IMAGE_DATA_LAYER,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_Z, GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_COL,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_ROW, GHOSTTY_RENDER_STATE_IMAGE_DATA_GRID_COLS,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_GRID_ROWS, GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_X,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_Y, GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_WIDTH,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_HEIGHT, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_X,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_Y, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_WIDTH,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_HEIGHT, GHOSTTY_RENDER_STATE_IMAGE_DATA_KITTY_PLACEMENT_ID,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_KITTY_KIND};
        void* values[]{&protocol, &image_id, &generation, &layer, &placement.z, &placement.column, &placement.row,
            &placement.columns, &placement.rows, &placement.offset_x, &placement.offset_y, &placement.pixel_width,
            &placement.pixel_height, &placement.source_x, &placement.source_y, &placement.source_width,
            &placement.source_height, &placement.placement_id, &kind};
        checked(ghostty_render_state_image_get_multi(images_, std::size(keys), keys, values, nullptr),
                "read image placement");
        if (protocol != GHOSTTY_IMAGE_PROTOCOL_KITTY)
            throw std::runtime_error("unsupported image protocol from pinned engine");
        placement.layer =
            layer == GHOSTTY_RENDER_STATE_IMAGE_LAYER_BELOW_BACKGROUND ? ImageLayer::below_background :
            layer == GHOSTTY_RENDER_STATE_IMAGE_LAYER_BELOW_TEXT ? ImageLayer::below_text :
            ImageLayer::above_text;
        placement.kind =
            kind == GHOSTTY_RENDER_STATE_KITTY_PLACEMENT_KIND_VIRTUAL ? ImagePlacementKind::virtual_placeholder :
            kind == GHOSTTY_RENDER_STATE_KITTY_PLACEMENT_KIND_RELATIVE ? ImagePlacementKind::relative :
            ImagePlacementKind::fixed;

        if (auto found = cache.find(generation); found != cache.end()) {
            placement.pixels = found->second;
        } else {
            const auto image = ghostty_kitty_graphics_image(storage, image_id);
            if (!image) throw std::runtime_error("image disappeared during actor-confined snapshot");

            auto pixels = std::make_shared<ImagePixels>();
            pixels->image_id = image_id;
            pixels->generation = generation;
            GhosttyKittyImageFormat format{};
            const std::uint8_t* data{};
            std::size_t length{};
            std::uint64_t actual_generation{};
            const GhosttyKittyGraphicsImageData image_keys[]{
                GHOSTTY_KITTY_IMAGE_DATA_WIDTH, GHOSTTY_KITTY_IMAGE_DATA_HEIGHT,
                GHOSTTY_KITTY_IMAGE_DATA_FORMAT, GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR, GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,
                GHOSTTY_KITTY_IMAGE_DATA_GENERATION};
            void* image_values[]{&pixels->width, &pixels->height, &format, &data, &length, &actual_generation};
            checked(ghostty_kitty_graphics_image_get_multi(image, std::size(image_keys), image_keys,
                image_values, nullptr),
                    "read image pixels");

            std::uint64_t components{};
            switch (format) {
            case GHOSTTY_KITTY_IMAGE_FORMAT_RGBA:
                pixels->format = PixelFormat::rgba;
                components = 4;
                break;
            case GHOSTTY_KITTY_IMAGE_FORMAT_RGB:
                pixels->format = PixelFormat::rgb;
                components = 3;
                break;
            case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA:
                pixels->format = PixelFormat::gray_alpha;
                components = 2;
                break;
            case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY:
                pixels->format = PixelFormat::gray;
                components = 1;
                break;
            default: throw std::runtime_error("unsupported decoded image format");
            }
            if (!data || generation != actual_generation || !pixels->width || !pixels->height ||
                std::uint64_t(pixels->width) * pixels->height >
                    std::numeric_limits<std::uint64_t>::max() / components ||
                std::uint64_t(pixels->width) * pixels->height * components != length ||
                length > image_budget_bytes || copied_bytes > image_budget_bytes - length)
                throw std::runtime_error("image snapshot exceeds budget or has inconsistent pixels");

            copied_bytes += length;
            pixels->bytes.assign(data, data + length);
            placement.pixels = pixels;
            cache.emplace(generation, std::move(pixels));
        }

        // The engine budgets pixel storage but placements have independent cost.
        if (target.images.size() >= 65536)
            throw std::runtime_error("image placement count exceeds snapshot budget");
        target.images.push_back(std::move(placement));
    }

    std::stable_sort(target.images.begin(), target.images.end(), [](const auto& a, const auto& b) {
        return a.layer != b.layer ? a.layer < b.layer : a.z < b.z;
    });
}

} // namespace stormtty
