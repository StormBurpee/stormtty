#pragma once

#include <windows.h>
#include <d2derr.h>
#include <dxgi.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace stormtty::windows {

// Keep the original HRESULT across resource-owner boundaries. Renderer recovery
// distinguishes device loss from other failures by catching this concrete type.
class GraphicsError : public std::runtime_error {
public:
    HRESULT result;

    GraphicsError(HRESULT hr, const char* action)
        : std::runtime_error(message(hr, action)), result(hr) {}

private:
    static std::string message(HRESULT hr, const char* action) {
        char code[16]{};
        std::snprintf(code, sizeof code, "0x%08lx", static_cast<unsigned long>(hr));
        return std::string(action) + " failed (" + code + ")";
    }
};

inline bool device_lost(HRESULT hr) noexcept {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

inline void check(HRESULT hr, const char* action) {
    if (FAILED(hr)) throw GraphicsError(hr, action);
}

} // namespace stormtty::windows
