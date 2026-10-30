#pragma once

#include "stormtty/application_layout_services.hpp"
#include "stormtty/workspace_save_signal.hpp"

#include <memory>
#include <mutex>
#include <thread>

namespace stormtty {

struct WorkspacePersistenceWake {
    virtual ~WorkspacePersistenceWake() = default;
    virtual void wake() noexcept = 0;
};

// The only file-operation seam. Native implementations keep the guarded services;
// tests may hold a real owner's operation without introducing UI callbacks.
struct WorkspacePersistenceServices {
    virtual ~WorkspacePersistenceServices() = default;
    virtual ApplicationLayoutSnapshotResult load(const std::filesystem::path&) = 0;
    virtual ConfigSaveResult save(
        const std::filesystem::path&,
        const ApplicationLayoutDocument&,
        const ConfigFileSnapshot&) = 0;
};

std::shared_ptr<WorkspacePersistenceServices> windows_workspace_persistence_services();

struct WorkspacePersistenceMode {
    bool enabled{};
    bool autosave{};
};

struct WorkspacePersistenceCompletion {
    bool saved{};
    bool conflict{};
    bool retry_requested{};
    std::string error;
};

// UI-thread owner of one application document and serialized file I/O. No live
// model or window is retained. The host captures declarative snapshots only when
// prepare_save permits it, owns native timers, and presents returned errors.
class ApplicationWorkspacePersistence final {
public:
    ApplicationWorkspacePersistence(
        std::shared_ptr<WorkspacePersistenceServices>,
        std::weak_ptr<WorkspacePersistenceWake>);
    ~ApplicationWorkspacePersistence();

    ApplicationWorkspacePersistence(const ApplicationWorkspacePersistence&) = delete;
    ApplicationWorkspacePersistence& operator=(const ApplicationWorkspacePersistence&) = delete;

    // Called once, with the host-selected path, before HWND creation. The joined
    // read runs off-thread. Construction deliberately does not select a path.
    Status load_before_window_creation(std::filesystem::path, WorkspacePersistenceMode);

    bool enabled() const noexcept;
    bool writes_available() const noexcept;
    bool busy() const noexcept;
    bool worker_joinable() const noexcept;
    bool clean_shutdown() const noexcept;

    // Startup/UI borrows only; invalidated by the next document mutation.
    const WorkspaceLibrary& library() const noexcept;
    const SequencedLayout* resume_record() const noexcept;

    Status save_preset(std::string name, WorkspaceSnapshot);
    Status remove_preset(std::string_view name);
    void set_restore_on_launch(bool enabled) noexcept;

    // Mutations above do not schedule I/O; the host preserves its existing edit
    // boundaries (including smoke-only mutations without automatic saving).
    bool mark_changed() noexcept;
    bool take_pending_change() noexcept;
    bool prepare_save() noexcept;
    // Requires a successful prepare_save and an intervening host capture only.
    Status save_snapshot(WorkspaceSnapshot);
    std::optional<WorkspacePersistenceCompletion> poll_completion();

    Status record_final_close(WorkspaceSnapshot);
    // Deliberately nonsticky: an older successful completion can reinstall its
    // token, matching the existing final-capture-failure ordering.
    void disable_writes() noexcept;

    // Only after leaving the message loop, or before destruction outside WndProc.
    // Normal poll_completion reaps only an already-published completion.
    void join_after_loop();

private:
    struct Mailbox {
        std::mutex mutex;
        std::optional<ConfigSaveResult> result;
    };

    std::shared_ptr<WorkspacePersistenceServices> services_;
    std::weak_ptr<WorkspacePersistenceWake> wake_;
    std::shared_ptr<Mailbox> mailbox_{std::make_shared<Mailbox>()};
    std::thread worker_;

    ApplicationLayoutDocument document_;
    std::optional<ConfigFileSnapshot> snapshot_;
    std::filesystem::path file_;
    WorkspaceSaveSignal signal_;

    bool enabled_{};
    bool autosave_{};
    bool busy_{};
    bool pending_{};
    bool finalizing_{};
};

} // namespace stormtty
