#pragma once

#include "stormtty/frame.hpp"
#include "stormtty/terminal.hpp"
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
};
enum class ChromePrimitiveKind { rectangle, line };
struct ChromePrimitive {
    PixelRect rect;
    Color fill{};
    Color stroke{};
    float radius{}, stroke_width{};
    ChromePrimitiveKind kind{ChromePrimitiveKind::rectangle};
    // For line: rect.x/y is start and x+width/y+height is end.
};
struct ChromeText {
    PixelRect rect;
    std::string text;
    Color color{236, 238, 242, 255};
    float size_dip{13};
    bool semibold{};
    std::string family{"Segoe UI Variable Text"};
};
struct WindowScene {
    Color background{23, 25, 29, 255};
    std::uint64_t chrome_generation{}; // Change for chrome/layout/focus/overlays.
    std::vector<RenderPane> panes;
    std::vector<ChromePrimitive> primitives; // Painted after panes, in order.
    std::vector<ChromeText> texts;
    bool force_redraw{};
};
enum class RenderResult { presented, unchanged, occluded, busy, suspended };
struct RendererStats {
    std::uint64_t presents{}, shaped_rows{}, shape_cache_hits{}, image_uploads{};
    std::uint64_t device_recoveries{}, dirty_rectangles{};
    std::size_t shape_cache_bytes{}, image_cache_bytes{};
    bool warp{}, color_fonts{}, composition{};
    bool bundled_font{}; // Actual selected terminal base is a private bundled face.
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
    RenderResult render(const WindowScene& scene);
    // Borrowed Win32 HANDLE, invalidated on device recovery/destruction.
    // Add it to the shell's message/work wait set; never CloseHandle it.
    void* frame_latency_wait_handle() const noexcept;
    RendererStats stats() const noexcept;
    // Explicit test/readback path, NOT called by rendering; blocks for GPU copy.
    // Returns tightly packed RGBA of the persistent composited surface.
    std::vector<std::uint8_t> read_pixels();
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
