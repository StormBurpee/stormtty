#pragma once

#include "smoke_teardown_trace.hpp"

#include <utility>

namespace stormtty::windows {

// Private native ownership primitive shared by renderer resource families.
// The label belongs to the owner slot, not the COM object: copying, moving and
// swapping preserve the renderer's existing explicit teardown tracing policy.
template<class T>
class Com {
public:
    Com() = default;

    explicit Com(T* pointer, bool add_ref = false) : pointer_(pointer) {
        if (add_ref && pointer_) pointer_->AddRef();
    }

    ~Com() { reset(); }

    Com(const Com& other) : Com(other.pointer_, true) {}

    Com& operator=(const Com& other) {
        if (this != &other) {
            Com copy(other);
            swap(copy);
        }
        return *this;
    }

    Com(Com&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}

    Com& operator=(Com&& other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = std::exchange(other.pointer_, nullptr);
        }
        return *this;
    }

    const char* teardown_label{}; // Enabled only at explicit smoke destruction.

    void reset() noexcept {
        if (auto* pointer = std::exchange(pointer_, nullptr)) {
            smoke_teardown_trace(teardown_label != nullptr, teardown_label, " begin");
            pointer->Release();
            smoke_teardown_trace(teardown_label != nullptr, teardown_label, " end");
        }
    }

    void swap(Com& other) noexcept { std::swap(pointer_, other.pointer_); }
    T* get() const noexcept { return pointer_; }
    T* operator->() const noexcept { return pointer_; }
    explicit operator bool() const noexcept { return pointer_ != nullptr; }

    T** put() noexcept {
        reset();
        return &pointer_;
    }

    template<class U>
    HRESULT as(Com<U>& out) const noexcept {
        return pointer_ ?
            pointer_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.put())) :
            E_POINTER;
    }

private:
    T* pointer_{};
};

} // namespace stormtty::windows
