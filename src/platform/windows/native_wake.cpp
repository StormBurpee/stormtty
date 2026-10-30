#include "native_wake.hpp"

#include <stdexcept>
#include <utility>

namespace stormtty::windows {

void WakeState::wake() noexcept {
    if (auto hwnd = window.load(std::memory_order_acquire)) {
        if (!posted.exchange(true, std::memory_order_acq_rel) &&
            !PostMessageW(hwnd, wake_message, 0, 0)) {
            posted.store(false, std::memory_order_release);
        }
    } else if (const auto owner = thread.load(std::memory_order_acquire)) {
        if (!posted.exchange(true, std::memory_order_acq_rel) &&
            !PostThreadMessageW(owner, wake_message, 0, 0)) {
            posted.store(false, std::memory_order_release);
        }
    } else if (auto fallback = completion_fallback.lock()) {
        fallback->wake();
    }
}

RendererReadyWake::RendererReadyWake(std::shared_ptr<WakeState> endpoint)
    : endpoint_(std::move(endpoint)) {
    wait_ = CreateThreadpoolWait(callback, this, nullptr);
    if (!wait_) {
        throw std::runtime_error("Could not create renderer readiness wait.");
    }
}

RendererReadyWake::~RendererReadyWake() {
    cancel();
    if (wait_) {
        CloseThreadpoolWait(wait_);
    }
}

void RendererReadyWake::arm(HANDLE handle) noexcept {
    if (fired_.exchange(false, std::memory_order_acq_rel)) {
        armed_ = nullptr;
    }
    if (handle == armed_) {
        return;
    }

    SetThreadpoolWait(wait_, nullptr, nullptr);
    armed_ = handle;
    if (handle) {
        SetThreadpoolWait(wait_, handle, nullptr);
    }
}

void RendererReadyWake::cancel() noexcept {
    if (!wait_) {
        return;
    }
    SetThreadpoolWait(wait_, nullptr, nullptr);
    WaitForThreadpoolWaitCallbacks(wait_, TRUE);
    armed_ = nullptr;
    fired_.store(false, std::memory_order_release);
}

void CALLBACK RendererReadyWake::callback(
    PTP_CALLBACK_INSTANCE,
    void* context,
    PTP_WAIT,
    TP_WAIT_RESULT) noexcept {
    auto* self = static_cast<RendererReadyWake*>(context);
    self->fired_.store(true, std::memory_order_release);
    self->endpoint_->wake();
}

} // namespace stormtty::windows
