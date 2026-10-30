#pragma once

#include "stormtty/renderer.hpp"
#include <cstddef>

namespace stormtty::macos {

class MetalFrame;

// Synchronous, nonescaping paint borrow. The caller owns ordering, admission,
// frame submission and a fresh upload-allowance snapshot for every draw call.
// No native resources, text, scene or cache state are retained by this painter.
class ChromePainter final {
public:
    ChromePainter(MetalFrame& frame, PixelRect viewport, float dpi) noexcept;

    void draw(const ChromePrimitive& primitive, std::size_t remaining_upload_bytes);
    void draw(const ChromeText& label, std::size_t remaining_upload_bytes);

private:
    MetalFrame& frame_;
    PixelRect viewport_;
    float dpi_;
};

} // namespace stormtty::macos
