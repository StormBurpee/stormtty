#pragma once

#include "stormtty/config.hpp"
#include "stormtty/chrome_geometry.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace stormtty {

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

// Deadline-based visible phase. A queued old timer message cannot hide the
// caret immediately after input restarted its full visible interval.
class CursorBlinkClock final {
public:
    void configure(bool enabled, std::uint64_t interval, std::uint64_t now) noexcept {
        if (!enabled || !interval) { enabled_=false; visible_=true; interval_=0; return; }
        if (!enabled_ || interval_!=interval) { enabled_=true; interval_=interval; restart(now); }
    }
    void restart(std::uint64_t now) noexcept { visible_=true; deadline_=now+interval_; }
    bool tick(std::uint64_t now) noexcept {
        if (!enabled_ || now<deadline_) return false;
        visible_=!visible_; deadline_=now+interval_; return true;
    }
    bool visible() const noexcept { return visible_; }
    bool enabled() const noexcept { return enabled_; }
    std::uint64_t interval() const noexcept { return interval_; }
private:
    std::uint64_t interval_{}, deadline_{};
    bool enabled_{}, visible_{true};
};

// Enters the platform message loop. Called once, from the main UI thread.
// Windows is implemented; other platforms provide their own native shell.
struct NativeRunOptions {
    bool smoke_test{}; // Explicit CI mode; never enabled on a normal launch.
    bool smoke_settings{}; // Styled Settings/menu routing, reversible preview and isolated explicit Save.
    bool smoke_modal{}; // Styled modal + routing/close smoke, not desktop activation QA.
    bool smoke_projects{}; // Two actual folder contexts, actor retention and scoped close.
    bool smoke_layout{}; // Three visible real shells + two background tabs.
    std::filesystem::path config_path; // Exact absolute startup path; writes only on explicit Save.
    std::filesystem::path capture_path; // Smoke-only actual framebuffer PNG.
    // Explicit per-launch experimental opt-in; never persisted or selected by default.
    // Windows transport verifies the pinned runtime and actual console host.
    std::filesystem::path console_runtime_directory;
    // Opt-in staged upstream Zsh + StormTTY preset; no persisted/default shell change.
    std::filesystem::path shell_runtime_directory;
};
int run_native_application(Config config, NativeRunOptions options = {});

} // namespace stormtty
