#pragma once

#include "stormtty/config.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace stormtty {

// Portable, DIP geometry shared by painting and native hit testing. The actual
// window, metrics and input services remain platform-owned.
struct ChromeTab { TabId tab; Rect bounds, close; };
struct ChromeLayout {
    Rect band, menu, new_tab, palette, minimize, maximize, close, drag;
    std::vector<ChromeTab> tabs;
    bool compact{};
};

inline ChromeLayout layout_chrome(double width, double height,
                                 const std::vector<TabId>& tabs, TabId active,
                                 double caption_width = 46.0,
                                 double band_height = 40.0) {
    ChromeLayout result;
    width = std::max(0.0, width);
    band_height = std::min(std::max(0.0, height), band_height);
    caption_width = std::min(caption_width, width / 3.0);
    result.band = {0, 0, width, band_height};
    result.close = {width - caption_width, 0, caption_width, band_height};
    result.maximize = {width - 2 * caption_width, 0, caption_width, band_height};
    result.minimize = {width - 3 * caption_width, 0, caption_width, band_height};
    const double available = result.minimize.x;
    result.menu = {4, 4, std::min(28.0, std::max(0.0, available - 4)), std::max(0.0, band_height - 8)};
    if (available < 120) {
        result.drag = {std::min(32.0, available), 0, std::max(0.0, available - 32), band_height};
        result.compact = true;
        return result;
    }
    result.palette = {available - 36, 4, 28, std::max(0.0, band_height - 8)};
    const double tab_space = std::max(0.0, result.palette.x - 48 - 36 - 36);
    result.compact = !tabs.empty() && tab_space < 88 * static_cast<double>(tabs.size());
    std::vector<TabId> visible = tabs;
    if (result.compact) visible = active ? std::vector<TabId>{active} : std::vector<TabId>{};
    const double tab_width = visible.empty() ? 0 : std::min(160.0, tab_space / static_cast<double>(visible.size()));
    double x = 36;
    for (const auto id : visible) {
        Rect bounds{x, 4, tab_width, std::max(0.0, band_height - 8)};
        Rect close{bounds.x + std::max(0.0, bounds.width - 27), 6, std::min(24.0, bounds.width), std::max(0.0, band_height - 12)};
        result.tabs.push_back({id, bounds, close});
        x += tab_width;
    }
    result.new_tab = {x + 2, 4, 28, std::max(0.0, band_height - 8)};
    result.drag = {x + 32, 0, std::max(0.0, result.palette.x - (x + 32)), band_height};
    return result;
}

enum class NativeHit {
    client, caption, minimize, maximize, close,
    left, right, top, bottom, top_left, top_right, bottom_left, bottom_right
};

inline NativeHit chrome_hit_test(const ChromeLayout& layout, Point p,
                                Size size, double resize_border, bool maximized) noexcept {
    // Caption controls take precedence at the top edge, matching native Windows.
    if (layout.close.contains(p)) return NativeHit::close;
    if (layout.maximize.contains(p)) return NativeHit::maximize;
    if (layout.minimize.contains(p)) return NativeHit::minimize;
    if (!maximized) {
        const bool l = p.x < resize_border, r = p.x >= size.width - resize_border;
        const bool t = p.y < resize_border, b = p.y >= size.height - resize_border;
        if (t && l) return NativeHit::top_left;
        if (t && r) return NativeHit::top_right;
        if (b && l) return NativeHit::bottom_left;
        if (b && r) return NativeHit::bottom_right;
        if (l) return NativeHit::left;
        if (r) return NativeHit::right;
        if (t) return NativeHit::top;
        if (b) return NativeHit::bottom;
    }
    if (layout.menu.contains(p) || layout.new_tab.contains(p) || layout.palette.contains(p)) return NativeHit::client;
    for (const auto& tab : layout.tabs) if (tab.bounds.contains(p)) return NativeHit::client;
    return layout.band.contains(p) ? NativeHit::caption : NativeHit::client;
}

// WM_CHAR is UTF-16, not a scalar stream. A pending high surrogate is never sent
// separately; malformed input is replaced rather than producing invalid UTF-8.
class Utf16Input final {
public:
    std::string feed(char16_t unit) {
        std::string result;
        if (high_) {
            if (unit >= 0xdc00 && unit <= 0xdfff) {
                append(result, 0x10000 + ((static_cast<std::uint32_t>(*high_) - 0xd800) << 10) + unit - 0xdc00);
                high_.reset();
                return result;
            }
            append(result, 0xfffd);
            high_.reset();
        }
        if (unit >= 0xd800 && unit <= 0xdbff) high_ = unit;
        else append(result, unit >= 0xdc00 && unit <= 0xdfff ? 0xfffd : unit);
        return result;
    }
    void reset() noexcept { high_.reset(); }
    static void append(std::string& out, std::uint32_t scalar) {
        if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) scalar = 0xfffd;
        if (scalar < 0x80) out += static_cast<char>(scalar);
        else if (scalar < 0x800) {
            out += static_cast<char>(0xc0 | (scalar >> 6)); out += static_cast<char>(0x80 | (scalar & 63));
        } else if (scalar < 0x10000) {
            out += static_cast<char>(0xe0 | (scalar >> 12)); out += static_cast<char>(0x80 | ((scalar >> 6) & 63)); out += static_cast<char>(0x80 | (scalar & 63));
        } else {
            out += static_cast<char>(0xf0 | (scalar >> 18)); out += static_cast<char>(0x80 | ((scalar >> 12) & 63)); out += static_cast<char>(0x80 | ((scalar >> 6) & 63)); out += static_cast<char>(0x80 | (scalar & 63));
        }
    }
private:
    std::optional<char16_t> high_;
};

// Enters the platform message loop. Called once, from the main UI thread.
// Windows is implemented; other platforms provide their own native shell.
struct NativeRunOptions {
    bool smoke_test{}; // Explicit CI mode; never enabled on a normal launch.
    std::filesystem::path capture_path; // Smoke-only actual framebuffer PNG.
};
int run_native_application(Config config, NativeRunOptions options = {});

} // namespace stormtty
