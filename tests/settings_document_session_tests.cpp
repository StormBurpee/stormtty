#include "core_test.hpp"
#include "stormtty/settings_document_session.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <type_traits>

using namespace stormtty;
using namespace std::chrono_literals;

namespace {

// Moving the committed token must not add a fallible allocation before the
// native host acknowledges disk success, even if presentation then fails.
static_assert(std::is_nothrow_move_assignable_v<std::optional<ConfigFileSnapshot>>);
static_assert(std::is_nothrow_move_constructible_v<Config>);

struct Wake final : SettingsDocumentWake {
    void wake() noexcept override {
        {
            std::lock_guard lock(mutex);
            ++notifications;
        }
        ready.notify_all();
    }

    void wait_for(unsigned count) {
        std::unique_lock lock(mutex);
        CHECK(ready.wait_for(lock, 5s, [&] { return notifications >= count; }));
    }

    std::mutex mutex;
    std::condition_variable ready;
    unsigned notifications{};
};

struct Gate {
    Gate() : released(release.get_future().share()) {}

    ~Gate() {
        unblock();
    }

    void block() {
        entered.set_value();
        released.wait();
    }

    void wait_until_entered() {
        CHECK(entered.get_future().wait_for(5s) == std::future_status::ready);
    }

    void unblock() {
        if (!opened) {
            release.set_value();
            opened = true;
        }
    }

    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released;
    bool opened{};
};

struct Services final : SettingsDocumentServices {
    SettingsSnapshotResult load(const std::filesystem::path& path) override {
        ++loads;
        last_path = path;
        if (load_gate) {
            load_gate->block();
        }
        if (throw_load) {
            throw std::runtime_error("read failed");
        }
        auto result = loaded;
        if (result.snapshot) {
            result.snapshot->path = path;
        }
        return result;
    }

    ShellDiscoveryResult discover(std::span<const Profile> profiles) override {
        ++discoveries;
        discovered_from.assign(profiles.begin(), profiles.end());
        if (throw_discovery) {
            throw std::runtime_error("discovery failed");
        }
        return {{}, {"nonfatal discovery warning"}};
    }

    ConfigSaveResult save(
        const std::filesystem::path& path,
        const Config& candidate,
        const ConfigFileSnapshot& baseline) override {
        ++saves;
        last_path = path;
        saved_candidate = candidate;
        saved_baseline = baseline;
        if (save_gate) {
            save_gate->block();
        }
        if (throw_save) {
            throw std::runtime_error("write failed");
        }
        if (save_conflict) {
            return {{}, "changed on disk", true};
        }
        auto committed = baseline;
        committed.existed = true;
        committed.bytes = "committed";
        ++committed.file_id;
        return {std::move(committed), {}, false};
    }

    SettingsSnapshotResult loaded{
        ConfigFileSnapshot{{}, true, "original exact bytes", 10, 20, 30},
        default_config(Platform::windows), {}
    };
    std::shared_ptr<Gate> load_gate;
    std::shared_ptr<Gate> save_gate;
    bool throw_load{};
    bool throw_discovery{};
    bool throw_save{};
    bool save_conflict{};
    unsigned loads{};
    unsigned discoveries{};
    unsigned saves{};
    std::filesystem::path last_path;
    std::vector<Profile> discovered_from;
    Config saved_candidate;
    ConfigFileSnapshot saved_baseline;
};

struct Fixture {
    ~Fixture() {
        // Assertion unwinding must release a held worker before owner teardown.
        if (services->load_gate) {
            services->load_gate->unblock();
        }
        if (services->save_gate) {
            services->save_gate->unblock();
        }
    }

    SettingsDocumentCompletion complete(bool may_open = true) {
        wake->wait_for(++consumed);
        auto result = document.poll_completion(may_open);
        CHECK(result);
        CHECK(!document.status().busy);
        CHECK(!document.status().worker_joinable);
        CHECK(!document.poll_completion(may_open));
        return std::move(*result);
    }

    void open() {
        CHECK(document.request_open(path, running.profiles));
        CHECK(complete().open_requested);
        CHECK(document.snapshot());
    }

    const std::filesystem::path path{"settings.stormtty"};
    Config running{default_config(Platform::windows)};
    std::shared_ptr<Services> services{std::make_shared<Services>()};
    std::shared_ptr<Wake> wake{std::make_shared<Wake>()};
    SettingsDocumentSession document{services, wake};
    unsigned consumed{};
};

void loading_intent() {
    Fixture fixture;
    auto gate = std::make_shared<Gate>();
    fixture.services->load_gate = gate;
    auto profiles = fixture.running.profiles;
    const auto original_profiles = profiles;

    CHECK(fixture.document.request_open(fixture.path, profiles, "terminal.find"));
    gate->wait_until_entered();
    const auto operation = fixture.document.status().operation;
    profiles.clear(); // The worker cannot borrow the host's vector.
    CHECK(!fixture.document.request_save(fixture.running));
    CHECK(!fixture.document.request_reload(fixture.path, false));
    CHECK(!fixture.document.poll_completion(true));

    fixture.document.cancel_open();
    CHECK(fixture.document.status().busy);
    CHECK(fixture.document.status().loading);
    CHECK(!fixture.document.status().open_requested);
    CHECK(fixture.document.status().binding.empty());
    CHECK(fixture.document.request_open("ignored-reopen-path", {}, "pane.split_right"));
    CHECK(fixture.document.request_open("ignored-again", {}, ""));
    CHECK(fixture.document.status().operation == operation);
    CHECK(fixture.document.status().binding.empty());

    gate->unblock();
    const auto loaded = fixture.complete();
    CHECK(loaded.open_requested && !loaded.open_blocked && loaded.binding.empty());
    CHECK(fixture.document.snapshot()->path == fixture.path);
    CHECK(fixture.services->loads == 1 && fixture.services->discoveries == 1);
    CHECK(fixture.services->discovered_from == original_profiles);
    CHECK(!loaded.shells.warnings.empty());
    CHECK(fixture.document.snapshot()->bytes == "original exact bytes");
}

void rejected_loads_preserve_token() {
    Fixture fixture;
    fixture.open();
    const auto original = fixture.document.snapshot();
    fixture.services->loaded.snapshot->bytes = "new disk bytes";

    CHECK(fixture.document.request_open(fixture.path, {}, "terminal.find"));
    fixture.document.cancel_open();
    CHECK(!fixture.complete().open_requested);
    CHECK(fixture.document.snapshot() == original);

    CHECK(fixture.document.request_open(fixture.path, {}));
    CHECK(fixture.complete(false).open_blocked);
    CHECK(fixture.document.snapshot() == original);

    fixture.services->throw_load = true;
    CHECK(fixture.document.request_open(fixture.path, {}));
    CHECK(fixture.complete().loaded.error == "read failed");
    CHECK(fixture.document.snapshot() == original);
    fixture.services->throw_load = false;

    fixture.services->throw_discovery = true;
    CHECK(fixture.document.request_open(fixture.path, {}));
    CHECK(fixture.complete().loaded.error == "discovery failed");
    CHECK(fixture.document.snapshot() == original);
}

void exact_save_token() {
    Fixture fixture;
    fixture.open();
    const auto baseline = *fixture.document.snapshot();
    auto candidate = fixture.running;
    candidate.font.size_dip = 24;
    fixture.services->loaded.snapshot->bytes = "external change";
    fixture.services->save_conflict = true;

    CHECK(fixture.document.request_save(candidate));
    CHECK(!fixture.document.request_open(fixture.path, {}));
    CHECK(!fixture.document.request_reload(fixture.path, false));
    auto failed = fixture.complete();
    CHECK(!failed.saved && failed.saved.conflict);
    CHECK(fixture.services->saved_baseline == baseline);
    CHECK(fixture.services->saved_candidate == candidate);
    CHECK(fixture.document.snapshot() == baseline);

    fixture.services->save_conflict = false;
    fixture.services->throw_save = true;
    CHECK(fixture.document.request_save(candidate));
    CHECK(fixture.complete().saved.error == "write failed");
    CHECK(fixture.document.snapshot() == baseline);

    fixture.services->throw_save = false;
    CHECK(fixture.document.request_save(candidate));
    const auto saved = fixture.complete();
    CHECK(saved.saved && saved.candidate == candidate);
    CHECK(saved.saved.committed);
    CHECK(fixture.document.snapshot()->file_id == baseline.file_id + 1);
    CHECK(fixture.document.snapshot()->bytes == "committed");
    // A host presentation exception occurs after this authoritative commit.
    CHECK(fixture.services->last_path == fixture.path);
}

void reload_acknowledgment() {
    Fixture fixture;
    fixture.open();
    const auto baseline = fixture.document.snapshot();
    CHECK(!fixture.document.request_reload(fixture.path, true));
    CHECK(fixture.document.request_reload(fixture.path, false));
    const auto generation = fixture.document.status().reload_generation;
    CHECK(!fixture.document.request_reload(fixture.path, false));
    CHECK(fixture.document.status().reload_generation == generation);
    auto reload = fixture.complete();
    CHECK(reload.reloaded.config == fixture.running);
    CHECK(fixture.services->discoveries == 1); // Reload never performs discovery.
    CHECK(fixture.document.snapshot() == baseline);

    fixture.document.accept_reload_application(reload.operation + 1);
    CHECK(fixture.document.snapshot() == baseline);
    // A failed host apply sends no acknowledgment and retains the old token.
    CHECK(fixture.document.request_reload(fixture.path, false));
    auto newer = fixture.complete();
    fixture.document.accept_reload_application(reload.operation);
    CHECK(fixture.document.snapshot() == baseline);
    fixture.document.accept_reload_application(newer.operation);
    CHECK(!fixture.document.snapshot());
    fixture.document.accept_reload_application(newer.operation);
    CHECK(!fixture.document.snapshot());

    fixture.services->loaded.snapshot->existed = false;
    CHECK(fixture.document.request_reload(fixture.path, false));
    CHECK(!fixture.complete().reloaded.config);
    fixture.services->loaded.snapshot->existed = true;
    fixture.services->loaded.config->font.size_dip = -1;
    CHECK(fixture.document.request_reload(fixture.path, false));
    CHECK(!fixture.complete().reloaded.config);
}

void retirement_and_final_join() {
    Fixture fixture;
    fixture.open();
    auto gate = std::make_shared<Gate>();
    fixture.services->save_gate = gate;
    fixture.services->throw_save = true;
    CHECK(fixture.document.request_save(fixture.running));
    gate->wait_until_entered();

    const auto pending = fixture.document.poll_retirement();
    CHECK(pending.pending && !pending.completion);
    CHECK(fixture.document.status().worker_joinable);
    gate->unblock();
    fixture.wake->wait_for(++fixture.consumed);
    const auto retired = fixture.document.poll_retirement();
    CHECK(!retired.pending && retired.completion);
    CHECK(retired.completion->saved.error == "write failed");
    CHECK(!fixture.document.poll_retirement().completion);

    // No surviving endpoint is needed for ownership and final shutdown reaping.
    fixture.wake.reset();
    fixture.services->save_gate.reset();
    fixture.services->throw_save = false;
    CHECK(fixture.document.request_save(fixture.running));
    fixture.document.join_after_window_destroyed();
    CHECK(!fixture.document.status().worker_joinable);
    const auto saved = fixture.document.poll_retirement();
    CHECK(!saved.pending && saved.completion && saved.completion->saved);

    CHECK(fixture.document.request_open(fixture.path, {}, "terminal.find"));
    fixture.document.join_after_window_destroyed();
    const auto loaded = fixture.document.poll_retirement();
    CHECK(!loaded.pending && loaded.completion && !loaded.completion->open_requested);
}

void missing_file_uses_copied_running_profiles() {
    Fixture fixture;
    fixture.services->loaded.snapshot->existed = false;
    fixture.services->loaded.config->profiles.clear();
    CHECK(fixture.document.request_open(fixture.path, fixture.running.profiles));
    const auto loaded = fixture.complete();
    CHECK(loaded.loaded && !loaded.loaded.existed);
    CHECK(fixture.services->discovered_from == fixture.running.profiles);
}

} // namespace

int main() {
    return run_core_tests("settings document session", [] {
        loading_intent();
        rejected_loads_preserve_token();
        exact_save_token();
        reload_acknowledgment();
        retirement_and_final_join();
        missing_file_uses_copied_running_profiles();
    });
}
