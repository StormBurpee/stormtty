#pragma once

#include "stormtty/settings_services.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <utility>

namespace stormtty {
namespace guarded_document_detail {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) noexcept : value_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
    HANDLE get() const noexcept { return value_; }
    void reset() noexcept {
        if (*this) CloseHandle(value_);
        value_ = INVALID_HANDLE_VALUE;
    }
private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

struct DirectoryGuard {
    std::vector<Handle> handles;
    bool complete{true};
};

struct OpenSnapshot {
    ConfigFileSnapshot token;
    Handle handle;
};

} // namespace guarded_document_detail

// Concrete ownership of existing read state, with no additional heap allocation.
// No native handle is exposed by the lease. Keep it alive through codec parsing.
class GuardedDocumentRead {
public:
    explicit GuardedDocumentRead(const std::filesystem::path& input);
    GuardedDocumentRead(const GuardedDocumentRead&) = delete;
    GuardedDocumentRead& operator=(const GuardedDocumentRead&) = delete;

    const ConfigFileSnapshot& snapshot() const noexcept { return opened_.token; }
    ConfigFileSnapshot&& take_snapshot() noexcept { return std::move(opened_.token); }

private:
    // Declaration order preserves path -> ancestor guards -> opened file, and
    // closes the target before releasing ancestors on every exit (including parse failure).
    const std::filesystem::path path_;
    const guarded_document_detail::DirectoryGuard directories_;
    guarded_document_detail::OpenSnapshot opened_;
};

// Synchronous byte persistence only. Adapters validate/encode before calling.
ConfigSaveResult save_guarded_document(const std::filesystem::path&, const std::string&,
                                      const ConfigFileSnapshot&);
// The adapter has preflighted the encoding, checked the path/token and decoded
// the exact baseline as legacy. This owns the entire verified-backup lifetime.
ConfigSaveResult save_guarded_document_with_legacy_backup(
    const std::filesystem::path&, const std::string&, const ConfigFileSnapshot&);

} // namespace stormtty
#endif
