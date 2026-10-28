#pragma once

#include <windows.h>
#include <cstdio>

namespace stormtty::windows {
// Explicit native smoke only. Flush each bounded marker so a killed process
// retains the last entered teardown operation in its redirected stderr file.
inline void smoke_teardown_trace(bool enabled, const char* stage, const char* phase = "") noexcept {
    if (!enabled) return;
    std::fprintf(stderr, "Native CI teardown: tick_ms=%llu thread=%lu stage=%s%s\n",
                 GetTickCount64(), static_cast<unsigned long>(GetCurrentThreadId()), stage, phase);
    std::fflush(stderr);
}
struct SmokeTeardownEnd {
    bool enabled{};
    const char* stage{};
    ~SmokeTeardownEnd() { smoke_teardown_trace(enabled, stage); }
};
}
