#include "core_test.hpp"
#include "stormtty/application_workspace_persistence.hpp"

#include <chrono>
#include <condition_variable>
#include <type_traits>

using namespace stormtty;
using namespace std::chrono_literals;

namespace {

static_assert(std::is_nothrow_move_assignable_v<std::optional<ConfigFileSnapshot>>);
static_assert(std::is_nothrow_move_constructible_v<WorkspacePersistenceCompletion>);

WorkspaceSnapshot snapshot(std::string name) {
    ProjectWorkspaceCatalog catalog;
    CHECK(catalog.create({std::move(name), "C:/work", "Local"}));
    auto captured = capture_workspace_snapshot(catalog);
    CHECK(captured);
    return std::move(*captured.value);
}

std::string bytes(const ApplicationLayoutDocument& document) {
    auto encoded = serialize_application_layout(document);
    CHECK(encoded);
    return std::move(*encoded.value);
}

struct Wake final : WorkspacePersistenceWake {
    void wake() noexcept override {
        std::lock_guard lock(mutex);
        ++count;
        ready.notify_all();
    }

    void wait_for(unsigned expected) {
        std::unique_lock lock(mutex);
        CHECK(ready.wait_for(lock, 5s, [&] { return count >= expected; }));
    }

    std::mutex mutex;
    std::condition_variable ready;
    unsigned count{};
};

struct Services final : WorkspacePersistenceServices {
    struct Write {
        std::filesystem::path path;
        ApplicationLayoutDocument document;
        ConfigFileSnapshot baseline;
    };

    ApplicationLayoutSnapshotResult load(const std::filesystem::path& path) override {
        ++loads;
        loaded_path = path;
        if (!load_error.empty()) {
            return {{}, {}, load_error};
        }
        token.path = path;
        return {token, DecodedApplicationLayout{initial, false}, {}};
    }

    ConfigSaveResult save(
        const std::filesystem::path& path,
        const ApplicationLayoutDocument& document,
        const ConfigFileSnapshot& baseline) override {
        std::unique_lock lock(mutex);
        writes.push_back({path, document, baseline});
        ready.notify_all();
        ready.wait(lock, [&] { return !held; });
        if (!save_error.empty()) {
            return {{}, save_error, conflict};
        }
        ConfigFileSnapshot next = baseline;
        next.path = path;
        next.existed = saved_existed;
        auto encoded = serialize_application_layout(document);
        if (!encoded) {
            return {{}, std::move(encoded.error), false};
        }
        next.bytes = std::move(*encoded.value);
        ++next.file_id;
        ++next.modified_time;
        committed = next;
        return {std::move(next), {}, false};
    }

    void wait_for_write(std::size_t count) {
        std::unique_lock lock(mutex);
        CHECK(ready.wait_for(lock, 5s, [&] { return writes.size() >= count; }));
    }

    void release() {
        std::lock_guard lock(mutex);
        held = false;
        ready.notify_all();
    }

    ApplicationLayoutDocument initial;
    ConfigFileSnapshot token{{}, true, "same-read original bytes", 7, 11, 13};
    std::filesystem::path loaded_path;
    unsigned loads{};
    std::string load_error;
    std::string save_error;
    bool conflict{};
    bool saved_existed{true};

    std::mutex mutex;
    std::condition_variable ready;
    bool held{true};
    std::vector<Write> writes;
    ConfigFileSnapshot committed;
};

struct Fixture {
    Fixture() : owner(services, wake) {}
    ~Fixture() {
        services->release();
        owner.join_after_loop();
    }

    void load(WorkspacePersistenceMode mode = {true, true}) {
        CHECK(owner.load_before_window_creation(path, mode));
    }

    void save(WorkspaceSnapshot value) {
        CHECK(owner.prepare_save());
        CHECK(owner.save_snapshot(std::move(value)));
    }

    WorkspacePersistenceCompletion complete(unsigned count = 1) {
        services->release();
        wake->wait_for(count);
        auto result = owner.poll_completion();
        CHECK(result);
        return std::move(*result);
    }

    std::filesystem::path path{"fixture/final/workspaces.stormtty"};
    std::shared_ptr<Services> services{std::make_shared<Services>()};
    std::shared_ptr<Wake> wake{std::make_shared<Wake>()};
    ApplicationWorkspacePersistence owner;
};

void explicit_load_and_dirty_boundaries() {
    Fixture disabled;
    CHECK(disabled.services->loads == 0); // Constructor never freezes a path.
    disabled.path = "workspace-smoke/final/workspaces.stormtty";
    disabled.load({false, true});
    CHECK(!disabled.owner.enabled());
    CHECK(!disabled.owner.writes_available());
    CHECK(disabled.services->loads == 0);
    CHECK(!disabled.owner.mark_changed());
    CHECK(!disabled.owner.prepare_save());

    Fixture smoke;
    smoke.path = "workspace-smoke/final/workspaces.stormtty";
    smoke.load({true, false});
    CHECK(smoke.services->loaded_path == smoke.path);
    CHECK(smoke.owner.enabled());
    CHECK(smoke.owner.writes_available());
    CHECK(!smoke.owner.mark_changed());
    CHECK(!smoke.owner.take_pending_change());
    smoke.save(snapshot("Explicit smoke flush"));
    CHECK(smoke.complete().saved);

    Fixture normal;
    normal.load();
    CHECK(!normal.owner.take_pending_change());
    auto value = snapshot("Model");
    CHECK(normal.owner.save_preset("Saved", value));
    normal.owner.set_restore_on_launch(false);
    CHECK(!normal.owner.take_pending_change());
    for (int i = 0; i < 1000; ++i) {
        CHECK(normal.owner.mark_changed());
    }
    CHECK(normal.owner.take_pending_change());
    CHECK(!normal.owner.take_pending_change());
    CHECK(normal.services->writes.empty());
    CHECK(normal.owner.remove_preset("Saved"));
    CHECK(!normal.owner.take_pending_change());
    CHECK(!normal.owner.library().restore_on_launch);

    Fixture invalid;
    invalid.services->load_error = "Unsupported envelope";
    CHECK(!invalid.owner.load_before_window_creation(invalid.path, {true, true}));
    CHECK(!invalid.owner.writes_available());
    CHECK(!invalid.owner.prepare_save());
}

void held_write_uses_latest_model_and_library() {
    Fixture fixture;
    fixture.load();
    auto first = snapshot("Old model");
    fixture.save(first);
    fixture.services->wait_for_write(1);
    CHECK(fixture.owner.busy());
    CHECK(!fixture.owner.poll_completion());
    CHECK(!fixture.owner.prepare_save()); // No host capture until completion.
    CHECK(!fixture.owner.prepare_save()); // Many requests coalesce to one retry.

    auto latest = snapshot("New model");
    CHECK(fixture.owner.save_preset("Newer preset", latest));
    fixture.owner.set_restore_on_launch(false);
    first.projects.front().metadata.name = "Caller mutation";
    auto completed = fixture.complete();
    CHECK(completed.saved && completed.retry_requested);
    CHECK(!fixture.owner.busy());
    CHECK(!fixture.owner.worker_joinable());
    CHECK(fixture.owner.library().presets.size() == 1);
    CHECK(!fixture.owner.library().restore_on_launch);
    CHECK(fixture.services->writes[0].document.library.last_session.projects[0].metadata.name == "Old model");
    CHECK(fixture.services->writes[0].baseline == fixture.services->token);
    const auto committed = fixture.services->committed;

    fixture.save(latest); // Host's new capture, not a queued old snapshot.
    completed = fixture.complete(2);
    CHECK(completed.saved && !completed.retry_requested);
    CHECK(fixture.services->writes.size() == 2);
    CHECK(fixture.services->writes[1].baseline == committed);
    CHECK(fixture.services->writes[1].path == fixture.path);
    CHECK(fixture.services->writes[1].document.library.presets.size() == 1);
    CHECK(fixture.services->writes[1].document.library.last_session.projects[0].metadata.name == "New model");
    CHECK(!fixture.owner.poll_completion());
}

void final_close_precedes_old_completion() {
    Fixture fixture;
    fixture.load();
    fixture.save(snapshot("Running"));
    fixture.services->wait_for_write(1);
    CHECK(!fixture.owner.prepare_save());
    auto closed = snapshot("Closed latest");
    CHECK(fixture.owner.save_preset("Close-time preset", closed));
    CHECK(fixture.owner.record_final_close(closed));
    CHECK(fixture.owner.clean_shutdown());
    CHECK(fixture.owner.resume_record()->layout.windows[0].projects[0].metadata.name == "Closed latest");

    // Actual post-loop order: close record, old join/poll, pending save, explicit
    // save while that worker is busy, final join/poll and equal-byte retry.
    fixture.services->release();
    fixture.owner.join_after_loop();
    auto completed = fixture.owner.poll_completion();
    CHECK(completed && completed->saved && completed->retry_requested);
    CHECK(fixture.owner.clean_shutdown());
    fixture.save(closed);
    CHECK(!fixture.owner.prepare_save());
    fixture.owner.join_after_loop();
    completed = fixture.owner.poll_completion();
    CHECK(completed && completed->saved && completed->retry_requested);
    fixture.save(closed); // Exact bytes: no third write and no new sequence.
    CHECK(!fixture.owner.worker_joinable());
    CHECK(fixture.services->writes.size() == 2);
    const auto& document = fixture.services->writes.back().document;
    CHECK(document.clean_shutdown);
    CHECK(document.library.presets.size() == 1);
    CHECK(document.history.choose(RestoreEvent::cold_launch)->layout.windows[0].projects[0].metadata.name == "Closed latest");
    CHECK(document.history.last_checkpoint()->layout.windows[0].projects[0].metadata.name == "Running");
}

void failures_disable_retry_without_reload() {
    for (bool conflict : {false, true}) {
        Fixture fixture;
        fixture.services->save_error = conflict ? "External edit" : "Access denied";
        fixture.services->conflict = conflict;
        fixture.load();
        fixture.save(snapshot("Candidate"));
        fixture.services->wait_for_write(1);
        CHECK(!fixture.owner.prepare_save());
        auto completed = fixture.complete();
        CHECK(!completed.saved);
        CHECK(completed.conflict == conflict);
        CHECK(completed.error == fixture.services->save_error);
        CHECK(completed.retry_requested);
        try {
            // Model a caller's allocation/presentation failure after consuming
            // the error. A UI failure must not preserve a stale writable token.
            throw std::runtime_error("Error presentation failed");
        } catch (const std::runtime_error&) {
            CHECK(!fixture.owner.writes_available());
            CHECK(!fixture.owner.prepare_save());
        }
        CHECK(!fixture.owner.writes_available());
        CHECK(!fixture.owner.mark_changed());
        CHECK(!fixture.owner.prepare_save());
        CHECK(fixture.services->loads == 1);
        CHECK(fixture.services->writes.size() == 1);
    }
}

void nonsticky_final_capture_failure() {
    for (bool invalid_record : {false, true}) {
        Fixture fixture;
        fixture.load();
        fixture.save(snapshot("Running before failed final capture"));
        fixture.services->wait_for_write(1);
        if (invalid_record) {
            WorkspaceSnapshot invalid;
            invalid.active_index = 1;
            CHECK(!fixture.owner.record_final_close(std::move(invalid)));
        } else {
            fixture.owner.disable_writes(); // Host capture failed before drain.
        }
        CHECK(!fixture.owner.writes_available());
        CHECK(fixture.complete().saved);
        CHECK(fixture.owner.writes_available()); // Existing edge, not silently fixed.
        CHECK(!fixture.owner.clean_shutdown());
        fixture.save(snapshot("Subsequent explicit save"));
        CHECK(fixture.complete(2).saved);
    }
}

void restoration_remains_host_gated() {
    Fixture cold;
    auto closed = snapshot("Cold close");
    auto checkpoint = snapshot("Crash checkpoint");
    CHECK(cold.services->initial.history.record_closed(closed));
    CHECK(cold.services->initial.history.checkpoint({{checkpoint}, 0}));
    cold.load();
    CHECK(cold.owner.clean_shutdown());
    CHECK(cold.owner.resume_record()->layout.windows[0].projects[0].metadata.name == "Cold close");

    Fixture crash;
    crash.services->initial = cold.services->initial;
    crash.services->initial.clean_shutdown = false;
    crash.load();
    CHECK(crash.owner.resume_record()->layout.windows[0].projects[0].metadata.name == "Crash checkpoint");

    Fixture grouped;
    grouped.services->initial.clean_shutdown = false;
    CHECK(grouped.services->initial.history.checkpoint({{closed, checkpoint}, 1}));
    grouped.load();
    CHECK(grouped.owner.resume_record()->layout.windows.size() == 2);
    grouped.owner.disable_writes(); // Host rejects unsupported groups, not flattening.
    CHECK(!grouped.owner.prepare_save());
    CHECK(grouped.owner.resume_record()->layout.windows.size() == 2);
    CHECK(grouped.services->writes.empty());

    Fixture no_checkpoint;
    no_checkpoint.services->initial.clean_shutdown = false;
    no_checkpoint.load();
    CHECK(!no_checkpoint.owner.resume_record());
    no_checkpoint.owner.disable_writes(); // Same host missing-checkpoint gate.
    CHECK(!no_checkpoint.owner.prepare_save());

    Fixture restore_off;
    restore_off.services->initial = grouped.services->initial;
    restore_off.services->initial.library.restore_on_launch = false;
    restore_off.load();
    CHECK(!restore_off.owner.library().restore_on_launch);
    CHECK(restore_off.owner.writes_available()); // Host skips restore checks first.
    CHECK(restore_off.owner.resume_record()->layout.windows.size() == 2);
}

void no_op_and_missing_token_rules() {
    Fixture fixture;
    auto value = snapshot("Unchanged");
    fixture.services->initial.library.last_session = value;
    fixture.services->initial.clean_shutdown = false;
    CHECK(fixture.services->initial.history.checkpoint({{value}, 0}));
    fixture.services->token.bytes = bytes(fixture.services->initial);
    fixture.load();
    fixture.save(value);
    CHECK(fixture.services->writes.empty());
    CHECK(!fixture.owner.worker_joinable());

    // Preset-only changes retain the checkpoint sequence.
    const auto checkpoint = fixture.services->initial.history.last_checkpoint()->sequence;
    CHECK(fixture.owner.save_preset("Only preset changed", value));
    fixture.save(value);
    CHECK(fixture.complete().saved);
    CHECK(fixture.services->writes[0].document.history.last_checkpoint()->sequence == checkpoint);

    Fixture missing;
    missing.services->initial = fixture.services->initial;
    missing.services->token.bytes = bytes(missing.services->initial);
    missing.services->token.existed = false;
    missing.load();
    missing.save(value); // Missing file must still be created despite equal bytes.
    CHECK(missing.complete().saved);
    CHECK(missing.services->writes.size() == 1);
}

void invalid_candidate_preserves_writable_document() {
    Fixture fixture;
    auto original = snapshot("Original loaded layout");
    fixture.services->initial.library.last_session = original;
    CHECK(fixture.services->initial.history.record_closed(original));
    fixture.load();
    const auto sequence = fixture.owner.resume_record()->sequence;

    WorkspaceSnapshot invalid;
    invalid.active_index = 1;
    CHECK(fixture.owner.prepare_save());
    CHECK(!fixture.owner.save_snapshot(std::move(invalid)));
    CHECK(!fixture.owner.busy());
    CHECK(!fixture.owner.worker_joinable());
    CHECK(fixture.services->writes.empty());
    CHECK(fixture.owner.writes_available());
    CHECK(fixture.owner.clean_shutdown());
    CHECK(fixture.owner.library().last_session.projects[0].metadata.name == "Original loaded layout");
    CHECK(fixture.owner.resume_record()->sequence == sequence);
    CHECK(fixture.owner.resume_record()->layout.windows[0].projects[0].metadata.name == "Original loaded layout");

    fixture.save(snapshot("Valid retry"));
    CHECK(fixture.complete().saved);
    CHECK(fixture.services->writes.size() == 1);
    CHECK(fixture.services->writes[0].baseline == fixture.services->token);
    CHECK(fixture.owner.library().last_session.projects[0].metadata.name == "Valid retry");
}

void mailbox_survives_missing_wake() {
    auto services = std::make_shared<Services>();
    auto wake = std::make_shared<Wake>();
    ApplicationWorkspacePersistence owner(services, wake);
    CHECK(owner.load_before_window_creation("isolated/workspaces.stormtty", {true, true}));
    CHECK(owner.prepare_save());
    CHECK(owner.save_snapshot(snapshot("No endpoint")));
    services->wait_for_write(1);
    wake.reset(); // Destroy original endpoint while real owner write is held.
    services->release();
    owner.join_after_loop();
    const auto completed = owner.poll_completion();
    CHECK(completed && completed->saved);
    CHECK(!owner.poll_completion());
    CHECK(!owner.worker_joinable());
}

} // namespace

int main() {
    return run_core_tests("application_workspace_persistence", [] {
        explicit_load_and_dirty_boundaries();
        held_write_uses_latest_model_and_library();
        final_close_precedes_old_completion();
        failures_disable_retry_without_reload();
        nonsticky_final_capture_failure();
        restoration_remains_host_gated();
        no_op_and_missing_token_rules();
        invalid_candidate_preserves_writable_document();
        mailbox_survives_missing_wake();
    });
}
