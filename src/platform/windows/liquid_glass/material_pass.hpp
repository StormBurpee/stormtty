#pragma once
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace stormtty::windows::liquid_glass {
// UI/render-thread owned. No capture, window, presentation or CPU pixel readback.
// Caller finishes any D2D drawing before draw(), then paints crisp foreground.
class MaterialPass final {
public:
    MaterialPass(ID3D11Device* device, ID3D11DeviceContext* context);
    ~MaterialPass();
    MaterialPass(const MaterialPass&) = delete;
    MaterialPass& operator=(const MaterialPass&) = delete;
    // Opaque SDR display-capture BGRA8 sRGB-encoded pixels and an equally sized
    // BGRA8 destination. Fractional-alpha encoded-premultiplied input is NOT supported;
    // the live monitor-capture alpha assumption remains a native qualification gate.
    // Source and destination must be distinct, single-sampled, same-device textures.
    // Entire prior D3D pipeline is restored; errors throw for service fallback.
    // Bound: 4096 per axis, 4,194,304 pixels, DPI scale 0.5–4.
    void draw(ID3D11Texture2D* captured_crop, ID3D11Texture2D* destination, float dpi_scale);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
