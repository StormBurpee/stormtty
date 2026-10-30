#pragma once
#include <windows.h>
#include <d3d11.h>
#include <winrt/base.h>
#include <memory>

namespace stormtty::windows::liquid_glass {
// UI-thread owned. Never exposes pixels to the CPU or persists captured content.
// Full display capture is consented separately; one latest GPU frame and client crop are retained.
class CaptureSource final {
public:
    CaptureSource();
    ~CaptureSource();
    CaptureSource(const CaptureSource&) = delete;
    CaptureSource& operator=(const CaptureSource&) = delete;
    void start(HWND window, HMONITOR selected_monitor, ID3D11Device* device);
    // Throws on invalid source/geometry. Caller must stop on every error.
    bool update(const RECT& client_screen_rect);
    // Returns false if affinity restoration failed; keep window alive and retry.
    bool stop() noexcept;
    bool active() const noexcept;
    ID3D11Texture2D* texture() const noexcept;
    unsigned long long frames() const noexcept;
    long long timestamp() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
