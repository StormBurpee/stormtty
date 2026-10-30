#pragma once

#include "com_owner.hpp"
#include "stormtty/renderer.hpp"

#include <dwrite_3.h>

namespace stormtty::windows {

struct RowShapeRequest {
    std::uint64_t pane_id{};
    std::uint32_t columns{};
    std::uint32_t cell_width{};
    std::uint32_t cell_height{};
};

struct GlyphCluster {
    Com<IDWriteFontFace> face;
    std::vector<UINT16> glyphs;
    std::vector<FLOAT> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;
    UINT32 column{};
    UINT32 end{};
    UINT32 bidi{};
    float size{};
    float baseline{};
    float x{};
    float natural_left{};
    float advance_total{};
    bool sideways{};
};

// Public value shape so callers can supply scratch without exposing cache maps.
// Cache entries are returned const; accounting/recency are written only by owner.
struct ShapedRow {
    std::vector<GlyphCluster> clusters;
    std::size_t bytes{};
    std::uint64_t touched{};
};

struct TextResourceStats {
    std::uint64_t shaped_rows{};
    std::uint64_t cache_hits{};
    std::size_t retained_bytes{};
    bool bundled_font{};
};

// Renderer-thread-confined owner of device-independent DirectWrite resources.
// No HWND, GPU resource, scene snapshot, damage tracker or painter is retained.
// The caller's COM apartment must outlive this owner and any returned objects.
class TextResources final {
public:
    explicit TextResources(std::size_t shape_cache_budget);
    ~TextResources();
    TextResources(const TextResources&) = delete;
    TextResources& operator=(const TextResources&) = delete;

    static void validate_font(const FontOptions& font);

    // Initial configuration follows GPU creation to preserve renderer startup
    // ordering. Metrics/shaping require a successful first configuration.
    // Returns true after remeasurement; the caller then invalidates its scene.
    bool configure(const FontOptions& font, float dpi);
    bool set_font(const FontOptions& font);
    bool set_dpi(float dpi);
    CellMetrics metrics() const noexcept;
    TextResourceStats stats() const noexcept;

    // Borrowed until the next shape/configuration call or destruction. An uncached
    // result aliases caller-owned scratch. Consume synchronously; do not retain
    // this reference in a painter, frame snapshot or across another shape call.
    // Chrome/color operations below do not invalidate the row. Font faces and
    // glyph arrays inside the row are owned, not borrowed from COM callbacks.
    const ShapedRow& shape(
        RowShapeRequest request,
        const FrameRow& row,
        std::uint64_t render_tick,
        ShapedRow& scratch);

    // Owning layouts; no chrome cache is introduced. Paint uses alignment and
    // ellipsis, measurement uses untrimmed metrics, wrap uses native line breaks.
    // Paint input is validated by the caller before allocating a layout.
    Com<IDWriteTextLayout> chrome_paint_layout(const ChromeText& label) const;
    PixelRect measure_chrome_text(const ChromeText& label) const;
    std::vector<std::string> wrap_chrome_text(const ChromeText& label) const;

    bool supports_color_glyphs() const noexcept;
    // Owning enumerator; GetCurrentRun pointers remain borrowed from it and
    // must not be retained across MoveNext or stored in the terminal row cache.
    HRESULT color_glyph_layers(
        const DWRITE_GLYPH_RUN& run,
        float origin_x,
        float origin_y,
        Com<IDWriteColorGlyphRunEnumerator1>& layers) const noexcept;
    void enable_teardown_trace() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stormtty::windows
