#pragma once

#include "stormtty/settings_services.hpp"

namespace stormtty {
struct SettingsReloadResult {
    std::optional<Config> config;
    std::string error;
    bool ignored{};
};
// UI-thread controller. I/O stays in the host's serialized background service;
// this gate never saves, discovers shells, loads layouts or touches sessions.
class SettingsReload {
public:
    Status begin(bool editor_open, bool io_busy) {
        if (editor_open) return Status::failure("Close Settings with Save or Cancel before reloading; your draft has been kept.");
        if (pending_ || io_busy) return Status::failure("A settings operation is already in progress. Try Reload settings again when it finishes.");
        pending_=true; ++generation_; return {};
    }
    std::uint64_t generation() const noexcept { return generation_; }
    bool pending() const noexcept { return pending_; }
    void cancel() noexcept { pending_=false; ++generation_; }
    SettingsReloadResult complete(std::uint64_t generation, SettingsSnapshotResult loaded) {
        if (!pending_ || generation!=generation_) return {{},{},true};
        pending_=false;
        if (!loaded) return {{},loaded.error.empty()?"Settings could not be read; current settings were kept.":std::move(loaded.error),false};
        // Unlike the editor's first-run load, reload must never turn a missing
        // file into defaults and unexpectedly overwrite the working settings.
        if (!loaded.snapshot->existed) return {{},"Settings file does not exist; current settings were kept.",false};
        const auto valid=validate_config(*loaded.config,Platform::windows);
        if (!valid) return {{},valid.error,false};
        return {std::move(loaded.config),{},false};
    }
private:
    std::uint64_t generation_{};
    bool pending_{};
};
} // namespace stormtty
