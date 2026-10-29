#pragma once
#include <memory>
struct IUnknown;
namespace stormtty::windows {
// UI-thread-owned, background-only compositor. It never samples or modifies text.
// Non-MSVC builds retain the existing renderer and report this capability absent.
class HostBackdrop final {
public:
    HostBackdrop();
    ~HostBackdrop();
    HostBackdrop(const HostBackdrop&)=delete;
    HostBackdrop& operator=(const HostBackdrop&)=delete;
    bool attach(void* window,IUnknown* swapchain,float sigma,unsigned width,unsigned height) noexcept;
    // Native qualification only: same backend/queue, owned input instead of desktop.
    bool attach_owned_surface_for_test(void* window,IUnknown* swapchain,float sigma,unsigned width,unsigned height) noexcept;
    // Final lifetime boundary, after the caller has detached/destroyed its HWND.
    bool shutdown() noexcept;
    bool update(float sigma,unsigned width,unsigned height) noexcept;
    void detach() noexcept;
    bool active() const noexcept;
private:
    bool attach_impl(void*,IUnknown*,float,unsigned,unsigned,bool) noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
