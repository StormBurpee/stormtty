#pragma once
// Fail closed for non-CMake consumers too. Enabling the build only exposes the
// experiment: each session still requires explicit display-capture consent.
#ifndef STORMTTY_EXPERIMENTAL_LIQUID_GLASS
#define STORMTTY_EXPERIMENTAL_LIQUID_GLASS 0
#endif
namespace stormtty {
inline constexpr bool experimental_liquid_glass = STORMTTY_EXPERIMENTAL_LIQUID_GLASS != 0;
}
