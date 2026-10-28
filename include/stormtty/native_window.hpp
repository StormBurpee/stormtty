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

// Enters the platform message loop. Called once, from the main UI thread.
// Windows is implemented; other platforms provide their own native shell.
struct NativeRunOptions {
    bool smoke_test{}; // Explicit CI mode; never enabled on a normal launch.
    bool smoke_layout{}; // Three visible real shells + two background tabs.
    std::filesystem::path capture_path; // Smoke-only actual framebuffer PNG.
};
int run_native_application(Config config, NativeRunOptions options = {});

} // namespace stormtty
