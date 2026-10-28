#pragma once

#include "stormtty/process.hpp"
#include <limits>

// Pure bookkeeping shared with focused tests; no Win32 types or test-only
// failure switches in the production process API. The caller holds its mutex.
namespace stormtty::windows::detail {
inline void count(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}
inline void record_resize_result(WindowsProcessControlDiagnostics& state,
                                 std::uint64_t generation, ProcessGridSize grid,
                                 bool succeeded) noexcept {
    if (!succeeded) { count(state.resize_failures); return; }
    state.applied_resize_generation = generation;
    state.applied_grid = grid;
}
}
