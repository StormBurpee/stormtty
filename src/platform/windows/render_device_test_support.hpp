#pragma once

// Compiled only into the dedicated native owner test, never stormtty_renderer.
#ifndef STORMTTY_RENDER_DEVICE_TESTS
#error "Render-device acquisition checkpoints are test-only"
#endif

namespace stormtty::windows {
class RenderDevice;
namespace testing {

enum class AcquisitionPoint {
    none,
    factory,
    device,
    drawing_device,
    context,
    brush,
    text_configured,
    canvas_texture,
    canvas_bitmap
};

// A typed one-shot exception checkpoint, not a callback or runtime policy.
void fail_after(AcquisitionPoint point) noexcept;
void checkpoint(AcquisitionPoint point);
// Simulate stale sensitive canvas state using no captured pixels or source.
void taint_canvas(RenderDevice& device) noexcept;

} // namespace testing
} // namespace stormtty::windows
