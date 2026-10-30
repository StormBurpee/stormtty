#pragma once

#include "stormtty/settings_document_session.hpp"
#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace stormtty::windows {

inline constexpr UINT wake_message = WM_APP + 41;

// Shared notification/mailbox record, not an HWND owner. The host clears window
// before native destruction. Completion fallback is fixed at view construction;
// neither this endpoint nor an actor route keeps another endpoint alive.
struct WakeState : SettingsDocumentWake {
    std::atomic<HWND> window{};
    std::atomic<DWORD> thread{};
    std::weak_ptr<WakeState> completion_fallback;
    std::atomic<bool> posted{};

    std::mutex mutex;
    std::optional<std::string> clipboard;
    std::optional<std::string> error;

    void wake() noexcept override;
};

// One-shot waits avoid the native handle-array ceiling. This object and its
// endpoint outlive callbacks. The latency handle is borrowed: the UI thread must
// cancel/drain before replacing or destroying it, including device recovery.
class RendererReadyWake final {
public:
    explicit RendererReadyWake(std::shared_ptr<WakeState> endpoint);
    ~RendererReadyWake();

    RendererReadyWake(const RendererReadyWake&) = delete;
    RendererReadyWake& operator=(const RendererReadyWake&) = delete;

    void arm(HANDLE handle) noexcept;
    void cancel() noexcept;

private:
    static void CALLBACK callback(
        PTP_CALLBACK_INSTANCE instance,
        void* context,
        PTP_WAIT wait,
        TP_WAIT_RESULT result) noexcept;

    std::shared_ptr<WakeState> endpoint_;
    PTP_WAIT wait_{};
    HANDLE armed_{}; // UI thread only; never owned.
    std::atomic<bool> fired_{};
};

} // namespace stormtty::windows
