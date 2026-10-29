#pragma once
#include <string_view>
namespace stormtty {
// Owned smoke terminals only. Keep this printable so ordinary paste safety applies.
inline constexpr std::string_view native_find_fixture_command(bool managed) noexcept {
    return managed ? "printf '%s\\n' 'STORMTTY_FIND_NEEDLE alpha' 'STORMTTY_FIND_NEEDLE beta'"
                   : "echo STORMTTY_FIND_NEEDLE alpha & echo STORMTTY_FIND_NEEDLE beta";
}
inline constexpr std::string_view native_mouse_fixture_command(bool managed, bool enable) noexcept {
    return managed?(enable?"printf '\\033[?1003h\\033[?1006h'":"printf '\\033[?1003l\\033[?1006l'"):(enable?"prompt $E[?1003h$E[?1006h$P$G":"prompt $E[?1003l$E[?1006l$P$G");
}
inline bool native_find_fixture_output(std::string_view row) noexcept {
    while (!row.empty() && row.back() == ' ') row.remove_suffix(1);
    return row == "STORMTTY_FIND_NEEDLE beta";
}
}
