#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <utility>

namespace stormtty::macos {
// ARC manages Objective-C/Metal objects; Create/Copy Core Foundation references
// remain explicit RAII, including all exceptional exits.
template<class T> class CfOwner {
    T value_{};
public:
    explicit CfOwner(T value = nullptr) noexcept : value_(value) {}
    ~CfOwner() { if (value_) CFRelease(value_); }
    CfOwner(const CfOwner&) = delete;
    CfOwner& operator=(const CfOwner&) = delete;
    CfOwner(CfOwner&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    CfOwner& operator=(CfOwner&& other) noexcept {
        if (this != &other) { if (value_) CFRelease(value_); value_ = std::exchange(other.value_, nullptr); }
        return *this;
    }
    T get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }
};
}
