#pragma once

#include <algorithm>

namespace stormtty {

// Win32 Settings-only input policy, portable so units and accessibility choices
// can be tested without an HWND. Callers supply SPI_GETWHEELSCROLLLINES and mark
// WHEEL_PAGESCROLL explicitly. No terminal-row or hardware-device inference.
inline double settings_wheel_distance(int delta, unsigned lines, bool page_scroll,
                                      double viewport_height) noexcept {
    const double distance=page_scroll?std::max(48.0,viewport_height-48.0):static_cast<double>(lines)*36.0;
    return -static_cast<double>(delta)/120.0*distance;
}
inline bool settings_wheel_packet_is_discrete(int delta) noexcept {
    return delta!=0 && delta%120==0;
}

} // namespace stormtty
