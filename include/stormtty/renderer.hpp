#pragma once

#include "stormtty/frame.hpp"
#include "stormtty/terminal.hpp"
#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace stormtty {
// Physical pixels, including at fractional DPI; shell owns layout/hit testing.
struct PixelRect {
    float x{}, y{}, width{}, height{};
    bool operator==(const PixelRect&) const = default;
};
struct FontOptions {
    std::string family{"Cascadia Mono"};
    std::string locale{"en-US"};
    float size_dip{14.0f};
    bool ligatures{false};
    bool operator==(const FontOptions&) const = default;
};
struct CellMetrics {
    std::uint16_t width{8}, height{19};
    float baseline{14};
};
struct RendererOptions {
    FontOptions font{};
    float background_opacity{1.0f}; // Only default backgrounds; text stays opaque.
    bool composition{true}; // Premultiplied DirectComposition, opaque HWND fallback.
    bool smoke_teardown_diagnostics{}; // Explicit native smoke only; no normal-launch logging.
    bool force_warp{}; // Deterministic software device for native Windows CI.
    std::size_t shape_cache_bytes{16u * 1024u * 1024u};
    std::size_t image_cache_bytes{64u * 1024u * 1024u};
    std::uint32_t maximum_image_dimension{8192};
};
struct RenderPane {
    std::uint64_t id{}; // Stable view/session-disambiguated identity, not array index.
    PixelRect content;
    FramePtr frame;
    bool focused{true};
    bool cursor_on{true}; // Scheduler owns blink deadlines; no renderer polling.
    bool text_blink_on{true};
    float cursor_reveal{1}; // Destination-cell presentation only; 1 is fully settled.
};
// Complete surfaces paint bottom-to-top, never all shapes followed by all
// window text. Within one surface, shapes retain insertion order and labels
// follow its shapes. Give overlapping child surfaces increasing depth (e.g.
// nested menus), rather than deleting or fading the labels underneath them.
// Terminal panes and Kitty image layers are always composed before chrome.
enum class ChromeLayer : std::uint8_t {
    base, transient, toast, palette, settings, modal, context_menu
};
struct ChromeZOrder {
    ChromeLayer layer{ChromeLayer::base};
    std::uint16_t depth{};
    auto operator<=>(const ChromeZOrder&) const = default;
};
enum class ChromePrimitiveKind { rectangle, line };
struct ChromePrimitive {
    PixelRect rect;
    Color fill{};
    Color stroke{};
    float radius{}, stroke_width{};
    ChromePrimitiveKind kind{ChromePrimitiveKind::rectangle};
    // For line: rect.x/y is start and x+width/y+height is end.
    bool background_replace{}; // Material fill replaces canvas alpha; never fade text.
    ChromeZOrder z{};
    // Physical viewport independent of geometry: clip paint, never resize/reflow it.
    // Native masks cover outward-rounded physical pixels at fractional DPI.
    std::optional<PixelRect> clip{};
    bool operator==(const ChromePrimitive&) const = default;
};
enum class ChromeTextAlignment { leading, center, trailing };
struct ChromeText {
    PixelRect rect;
    std::string text;
    Color color{236, 238, 242, 255};
    float size_dip{13};
    bool semibold{};
    std::string family{"Segoe UI Variable Text"};
    ChromeTextAlignment alignment{ChromeTextAlignment::leading}; // Per label, never terminal cells.
    ChromeZOrder z{};
    bool wrap{}; // Owned multiline notices only; other chrome stays single-line.
    // Preserve the full layout/baseline when a scroll viewport cuts through glyphs.
    std::optional<PixelRect> clip{};
    bool operator==(const ChromeText&) const = default;
};
struct WindowScene {
    Color background{23, 25, 29, 255};
    std::uint64_t chrome_generation{}; // Change for chrome/layout/focus/overlays, including z-order.
    std::vector<RenderPane> panes;
    std::vector<ChromePrimitive> primitives; // Ordered together with texts by z.
    std::vector<ChromeText> texts;
    bool force_redraw{};
};
enum class ChromeDrawKind { primitive, text };
struct ChromeDrawItem {
    ChromeZOrder z;
    ChromeDrawKind kind;
    std::size_t index;
};
// Both native renderers consume this same stable surface ordering. Indices
// remain valid only while the caller leaves the scene's arrays unmodified.
inline std::vector<ChromeDrawItem> chrome_paint_order(const WindowScene& scene) {
    std::vector<ChromeDrawItem> order;
    order.reserve(scene.primitives.size()+scene.texts.size());
    for (std::size_t i=0;i<scene.primitives.size();++i)
        order.push_back({scene.primitives[i].z,ChromeDrawKind::primitive,i});
    for (std::size_t i=0;i<scene.texts.size();++i)
        order.push_back({scene.texts[i].z,ChromeDrawKind::text,i});
    std::stable_sort(order.begin(),order.end(),[](const auto& a,const auto& b) {
        return a.z!=b.z?a.z<b.z:a.kind<b.kind;
    });
    return order;
}
enum class RenderResult { presented, unchanged, occluded, busy, suspended };
struct RendererStats {
    std::uint64_t presents{}, shaped_rows{}, shape_cache_hits{}, image_uploads{};
    std::uint64_t device_recoveries{}, dirty_rectangles{};
    std::size_t shape_cache_bytes{}, image_cache_bytes{};
    bool warp{}, color_fonts{}, composition{};
    bool host_backdrop{}; // Additional Gaussian graph active; not proof of physical DWM blur.
    bool bundled_font{}; // Actual selected terminal base is a private bundled face.
};
// Alpha-only diagnostic of a bounded, caller-selected canvas patch. Never
// returns RGB, text, images, desktop pixels or a capture of another window.
struct RendererAlphaProbe {
    std::uint8_t minimum{}, maximum{};
    std::uint32_t samples{};
    float effective_background_opacity{1};
    bool composition{}, software{};
    std::uint64_t device_generation{}, presents{};
};
// One owner thread. All OS/D3D/COM types are hidden for future Metal/CoreText.
// HWND is passed as native_window on Windows; nullptr selects an offscreen WARP
// capable surface for native tests. resize() must precede the first render.
// Methods may throw; call from guarded application dispatch, never unguarded
// across a WndProc/COM/C ABI boundary. No method waits on presentation readiness.
class Renderer final {
public:
    explicit Renderer(void* native_window, RendererOptions options = {});
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    void resize(std::uint32_t width_px, std::uint32_t height_px, float dpi = 96.0f);
    void set_font(FontOptions font);
    void set_background_opacity(float opacity);
    CellMetrics metrics() const noexcept;
#ifdef _WIN32
    // Irreversible: release presentation/GPU owners before the caller destroys
    // its HWND. No render/resize calls are valid afterward; destructor remains safe.
    void prepare_window_destruction() noexcept;
    // Background-only extra Gaussian sigma; 0 removes the host effect; the window selects clear/system policy.
    void set_additional_blur(float sigma);
    // Experimental session-only desktop material. Host obtains explicit consent.
    bool liquid_glass_available() const noexcept;
    bool start_liquid_glass(void* selected_monitor);
    void stop_liquid_glass(const char* reason="Off — session capture stopped.") noexcept;
    bool liquid_glass_active() const noexcept;
    std::string liquid_glass_status() const;
    bool poll_liquid_glass(); // True only on a new optical source/state/geometry.

    // Actual DirectWrite chrome-font bounds in pixels, using the label's wrapping.
    PixelRect measure_chrome_text(const ChromeText& label) const;
    // Actual DirectWrite word wrapping; UTF-8 lines retain all text except newlines.
    std::vector<std::string> wrap_chrome_text(const ChromeText& label) const;
#endif
    RenderResult render(const WindowScene& scene);
    // Borrowed Win32 HANDLE, invalidated on device recovery/destruction.
    // Add it to the shell's message/work wait set; never CloseHandle it.
    void* frame_latency_wait_handle() const noexcept;
    RendererStats stats() const noexcept;
    // Explicit test/readback path, NOT called by rendering; blocks for GPU copy.
    // Returns tightly packed RGBA of the persistent composited surface.
    std::vector<std::uint8_t> read_pixels();
    // Windows diagnostic only: copies at most 16x16 pixels without waiting.
    // A false return explicitly means unavailable (including other backends).
    // poll uses a nonblocking GPU map; nullopt means pending/cancelled. Host owns
    // a finite deadline and must cancel on expiry, not poll during idle forever.
    bool begin_alpha_probe(PixelRect area);
    std::optional<RendererAlphaProbe> poll_alpha_probe();
    void cancel_alpha_probe() noexcept;
    // Native CI exercises real device resource reconstruction without removing
    // the process-wide adapter. Preserves owned CPU frame data/session state.
    void recreate_device();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// WIC PNG-only decoder: rejects dimensions/decoded budget before CopyPixels.
// Callback initializes COM for the actor thread and never lets an exception cross
// the callback boundary. install once before starting TerminalCore actors.
std::optional<DecodedPng> decode_windows_png(std::span<const std::uint8_t> bytes,
                                           std::size_t max_decoded_bytes);
void install_windows_png_decoder();
} // namespace stormtty
