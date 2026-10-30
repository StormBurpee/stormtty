#pragma once
#include <d3d11.h>
#include <memory>
#include <cstdint>
#include <string>

namespace stormtty::windows::liquid_glass {
// Renderer-thread session. Consent/UI belongs to the host. Never persists or auto-starts.
class Session final {
public:
    Session();
    ~Session();
    bool start(void* window, void* monitor, ID3D11Device*, ID3D11DeviceContext*) noexcept;
    void stop(const char* reason="Off — session capture stopped.") noexcept;
    bool active() const noexcept;
    std::string status() const;
    std::uint64_t generation() const noexcept;
    // Returns only a stable GPU-owned background; never terminal foreground.
    ID3D11Texture2D* background(float scale) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
