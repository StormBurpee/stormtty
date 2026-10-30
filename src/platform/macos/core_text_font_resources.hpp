#pragma once

#include "cf_owner.hpp"
#include "stormtty/renderer.hpp"
#include <CoreText/CoreText.h>

namespace stormtty::macos {

// Owns only the terminal font and its physical-pixel metrics. The renderer owns
// font options, validated DPI, cache generations and invalidation policy.
class CoreTextFontResources final {
public:
    void reset(const FontOptions& options, float dpi);

    CellMetrics metrics() const noexcept;

    // Synchronous borrow only: do not release or retain this pointer past reset
    // or destruction. Trait-font Create/Copy results need their own CfOwner.
    CTFontRef borrow_font() const noexcept;

private:
    CfOwner<CTFontRef> font_;
    CellMetrics metrics_;
};

} // namespace stormtty::macos
