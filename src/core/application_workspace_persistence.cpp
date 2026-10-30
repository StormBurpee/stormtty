#include "stormtty/application_workspace_persistence.hpp"

#include <stdexcept>
#include <utility>

namespace stormtty {

ApplicationWorkspacePersistence::ApplicationWorkspacePersistence(
    std::shared_ptr<WorkspacePersistenceServices> services,
    std::weak_ptr<WorkspacePersistenceWake> wake)
    : services_(std::move(services)), wake_(std::move(wake)) {
    if (!services_) {
        throw std::invalid_argument("Workspace persistence services are required.");
    }
}

ApplicationWorkspacePersistence::~ApplicationWorkspacePersistence() {
    join_after_loop();
}

Status ApplicationWorkspacePersistence::load_before_window_creation(
    std::filesystem::path path,
    WorkspacePersistenceMode mode) {
    enabled_ = mode.enabled;
    autosave_ = mode.enabled && mode.autosave;
    if (!enabled_) {
        return {};
    }
    file_ = std::move(path);

    ApplicationLayoutSnapshotResult loaded;
    std::thread reader([services = services_, path = file_, &loaded] {
        loaded = services->load(path);
    });
    reader.join();
    if (!loaded) {
        return Status::failure(std::move(loaded.error));
    }

    snapshot_ = std::move(loaded.snapshot);
    document_ = std::move(loaded.layout->document);
    return {};
}

bool ApplicationWorkspacePersistence::enabled() const noexcept {
    return enabled_;
}

bool ApplicationWorkspacePersistence::writes_available() const noexcept {
    return snapshot_.has_value();
}

bool ApplicationWorkspacePersistence::busy() const noexcept {
    return busy_;
}

bool ApplicationWorkspacePersistence::worker_joinable() const noexcept {
    return worker_.joinable();
}

bool ApplicationWorkspacePersistence::clean_shutdown() const noexcept {
    return document_.clean_shutdown;
}

const WorkspaceLibrary& ApplicationWorkspacePersistence::library() const noexcept {
    return document_.library;
}

const SequencedLayout* ApplicationWorkspacePersistence::resume_record() const noexcept {
    return document_.history.choose(document_.clean_shutdown
        ? RestoreEvent::cold_launch : RestoreEvent::crash_recovery);
}

Status ApplicationWorkspacePersistence::save_preset(std::string name, WorkspaceSnapshot snapshot) {
    return save_workspace_preset(document_.library, std::move(name), std::move(snapshot));
}

Status ApplicationWorkspacePersistence::remove_preset(std::string_view name) {
    return remove_workspace_preset(document_.library, name);
}

void ApplicationWorkspacePersistence::set_restore_on_launch(bool enabled) noexcept {
    document_.library.restore_on_launch = enabled;
}

bool ApplicationWorkspacePersistence::mark_changed() noexcept {
    if (!autosave_ || !snapshot_) {
        return false;
    }
    signal_.mark_changed();
    return true;
}

bool ApplicationWorkspacePersistence::take_pending_change() noexcept {
    return signal_.take_pending();
}

bool ApplicationWorkspacePersistence::prepare_save() noexcept {
    if (!enabled_ || !snapshot_) {
        return false;
    }
    if (busy_) {
        pending_ = true;
        return false;
    }
    return true;
}

Status ApplicationWorkspacePersistence::save_snapshot(WorkspaceSnapshot snapshot) {
    auto document = document_;
    document.library.last_session = std::move(snapshot);
    if (!finalizing_) {
        bool changed = true;
        const auto& checkpoint = document.history.last_checkpoint();
        if (checkpoint && checkpoint->layout.windows.size() == 1) {
            WorkspaceLibrary previous;
            WorkspaceLibrary current;
            previous.last_session = checkpoint->layout.windows.front();
            current.last_session = document.library.last_session;
            const auto old_bytes = serialize_workspace_library(previous);
            const auto new_bytes = serialize_workspace_library(current);
            if (!old_bytes || !new_bytes) {
                return Status::failure("Could not validate workspace checkpoint.");
            }
            changed = *old_bytes.value != *new_bytes.value;
        }
        if (changed || document.clean_shutdown) {
            ApplicationLayout layout{{document.library.last_session}, 0};
            if (auto recorded = document.history.checkpoint(std::move(layout)); !recorded) {
                return recorded;
            }
        }
        document.clean_shutdown = false;
    }

    auto bytes = serialize_application_layout(document);
    if (!bytes) {
        return Status::failure(std::move(bytes.error));
    }
    if (snapshot_->existed && *bytes.value == snapshot_->bytes) {
        return {};
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    const auto path = file_;
    const auto baseline = *snapshot_;
    document_ = std::move(document);
    const auto saved_document = document_;
    busy_ = true;
    try {
        worker_ = std::thread([
            path, baseline, saved_document, mailbox = mailbox_,
            services = services_, wake = wake_
        ] {
            auto result = services->save(path, saved_document, baseline);
            {
                std::lock_guard lock(mailbox->mutex);
                mailbox->result = std::move(result);
            }
            if (auto endpoint = wake.lock()) {
                endpoint->wake();
            }
        });
    } catch (...) {
        busy_ = false;
        throw;
    }
    return {};
}

std::optional<WorkspacePersistenceCompletion> ApplicationWorkspacePersistence::poll_completion() {
    std::optional<ConfigSaveResult> completed;
    {
        std::lock_guard lock(mailbox_->mutex);
        completed.swap(mailbox_->result);
    }
    if (!completed) {
        return {};
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    busy_ = false;

    const bool saved = static_cast<bool>(*completed);
    if (saved) {
        snapshot_ = std::move(completed->snapshot);
    } else {
        snapshot_.reset();
    }
    return WorkspacePersistenceCompletion{
        saved, completed->conflict, std::exchange(pending_, false),
        std::move(completed->error)
    };
}

Status ApplicationWorkspacePersistence::record_final_close(WorkspaceSnapshot snapshot) {
    auto recorded = document_.history.record_closed(std::move(snapshot));
    if (!recorded) {
        snapshot_.reset();
        return recorded;
    }
    document_.clean_shutdown = true;
    finalizing_ = true;
    return {};
}

void ApplicationWorkspacePersistence::disable_writes() noexcept {
    snapshot_.reset();
}

void ApplicationWorkspacePersistence::join_after_loop() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

} // namespace stormtty
