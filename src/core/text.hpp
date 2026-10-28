#pragma once
#include <string_view>

namespace stormtty::detail {
inline bool valid_utf8(std::string_view s) noexcept {
    for (std::size_t i = 0; i < s.size();) {
        const auto a = static_cast<unsigned char>(s[i++]);
        if (a <= 0x7f) continue;
        unsigned remaining{}, value{}, minimum{};
        if (a >= 0xc2 && a <= 0xdf) { remaining = 1; value = a & 0x1f; minimum = 0x80; }
        else if (a >= 0xe0 && a <= 0xef) { remaining = 2; value = a & 0x0f; minimum = 0x800; }
        else if (a >= 0xf0 && a <= 0xf4) { remaining = 3; value = a & 7; minimum = 0x10000; }
        else return false;
        if (remaining > s.size() - i) return false;
        while (remaining--) {
            const auto b = static_cast<unsigned char>(s[i++]);
            if ((b & 0xc0) != 0x80) return false;
            value = (value << 6) | (b & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
inline bool valid_document_text(std::string_view s) noexcept {
    if (!valid_utf8(s)) return false;
    for (const unsigned char c : s)
        if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f) return false;
    return true;
}

}
