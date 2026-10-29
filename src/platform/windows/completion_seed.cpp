#include "stormtty/windows_completion_seed.hpp"
#include "completion_seed.hpp"
#include "stormtty_completion_seed_trust.hpp"
#include <chrono>
namespace stormtty {
CompletionSeedLaunch completion_seed_launch(const std::filesystem::path& local,
                                            const std::filesystem::path& runtime) {
    auto observation=std::make_shared<CompletionSeedObservation>();
    const auto state=local/L"StormTTY/managed-zsh-v1";
    const auto package=runtime.parent_path()/L"zsh-seed";
    auto gate=managed_shell_initialization_at(local,[local,runtime,state,package,observation] {
        const auto start=std::chrono::steady_clock::now();
        const bool adopted=completion_seed::adopt_build_trusted(runtime,package,local,state,
            build_seed::identity_sha256);
        observation->verification_publish_ms.store(std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-start).count());
        observation->adoption.store(adopted?1:0);
        return adopted;
    });
    return {std::move(gate),std::move(observation)};
}
}
