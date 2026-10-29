#pragma once
#include <string_view>
#include <string>
namespace stormtty {
// Owned smoke terminals only. Keep this printable so ordinary paste safety applies.
inline constexpr std::string_view native_find_fixture_command(bool managed) noexcept {
    return managed ? "printf '%s\\n' 'STORMTTY_FIND_NEEDLE alpha' 'STORMTTY_FIND_NEEDLE beta'"
                   : "echo STORMTTY_FIND_NEEDLE alpha & echo STORMTTY_FIND_NEEDLE beta";
}
inline std::string native_mouse_child_command(std::string_view path) {
    if(path.empty()||path.find_first_of("\"\r\n")!=std::string_view::npos)return {};
    return "\""+std::string(path)+"\" --native-mouse-child";
}
inline bool native_find_fixture_output(std::string_view row) noexcept {
    while (!row.empty() && row.back() == ' ') row.remove_suffix(1);
    return row == "STORMTTY_FIND_NEEDLE beta";
}
}
