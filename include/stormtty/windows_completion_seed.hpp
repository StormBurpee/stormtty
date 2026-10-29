#pragma once
#ifdef _WIN32
#include "stormtty/process.hpp"
#include <atomic>
#include <filesystem>
namespace stormtty {
struct CompletionSeedObservation {
    // -1: existing/custom/contended state; 0: attempted/rejected; 1: adopted.
    std::atomic<int> adoption{-1};
    std::atomic<double> verification_publish_ms{};
};
struct CompletionSeedLaunch {
    std::shared_ptr<WindowsProcessInitialization> initialization;
    std::shared_ptr<CompletionSeedObservation> observation;
};
// Defined only in explicitly enabled native builds. No environment/sidecar anchor.
CompletionSeedLaunch completion_seed_launch(const std::filesystem::path& local_app_data,
                                           const std::filesystem::path& runtime);
}
#endif
