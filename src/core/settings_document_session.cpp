#include "stormtty/settings_document_session.hpp"

#include <stdexcept>
#include <utility>

namespace stormtty {

SettingsDocumentCompletion::SettingsDocumentCompletion(Config value) noexcept
    : candidate(std::move(value)) {
}

SettingsDocumentSession::SettingsDocumentSession(
    std::shared_ptr<SettingsDocumentServices> services,
    std::weak_ptr<SettingsDocumentWake> wake)
    : services_(std::move(services)), wake_(std::move(wake)) {
    if (!services_) {
        throw std::invalid_argument("Settings document services are required.");
    }
}

SettingsDocumentSession::~SettingsDocumentSession() {
    join_after_window_destroyed();
}

void SettingsDocumentSession::join_after_window_destroyed() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

SettingsDocumentStatus SettingsDocumentSession::status() const {
    return {
        busy_, loading_, open_requested_, worker_.joinable(),
        operation_, reload_.generation(), binding_
    };
}

std::optional<ConfigFileSnapshot> SettingsDocumentSession::snapshot() const {
    return snapshot_;
}

void SettingsDocumentSession::cancel_open() noexcept {
    open_requested_ = false;
    binding_.clear();
}

void SettingsDocumentSession::start(
    SettingsDocumentCompletion::Kind kind,
    std::filesystem::path path,
    std::vector<Profile> profiles,
    Config candidate) {
    // Callers reject busy operations. Any preceding worker was already consumed.
    if (worker_.joinable()) {
        worker_.join();
    }

    busy_ = true;
    loading_ = kind == SettingsDocumentCompletion::Kind::load;
    const auto operation = ++operation_;
    reload_application_ = 0;

    worker_ = std::thread([
        mailbox = mailbox_, wake = wake_, services = services_, kind,
        path = std::move(path), profiles = std::move(profiles),
        candidate = std::move(candidate), baseline = snapshot_,
        operation, generation = reload_.generation()
    ]() mutable {
        WorkerCompletion result;
        result.kind = kind;
        result.operation = operation;
        result.generation = generation;
        result.path = std::move(path);
        result.candidate = std::move(candidate);

        try {
            if (kind == SettingsDocumentCompletion::Kind::save) {
                result.saved = services->save(result.path, result.candidate, *baseline);
            } else {
                result.loaded = services->load(result.path);
                if (kind == SettingsDocumentCompletion::Kind::load && result.loaded) {
                    const auto& configured = result.loaded.snapshot->existed
                        ? result.loaded.config->profiles
                        : profiles;
                    result.shells = services->discover(configured);
                }
            }
        } catch (const std::exception& error) {
            if (kind == SettingsDocumentCompletion::Kind::save) {
                result.saved.error = error.what();
            } else {
                result.loaded.error = error.what();
            }
        } catch (...) {
            if (kind == SettingsDocumentCompletion::Kind::save) {
                result.saved.error = "Settings could not be saved.";
            } else {
                result.loaded.error = kind == SettingsDocumentCompletion::Kind::reload
                    ? "Settings could not be reloaded."
                    : "Settings discovery failed unexpectedly.";
            }
        }

        // Publication follows all file I/O. The only remaining operation before
        // thread return is a bounded noexcept weak wake, as in the native host.
        {
            std::lock_guard lock(mailbox->mutex);
            mailbox->completion = std::move(result);
        }
        if (auto endpoint = wake.lock()) {
            endpoint->wake();
        }
    });
}

bool SettingsDocumentSession::request_open(
    std::filesystem::path path,
    std::vector<Profile> profiles,
    std::string binding) {
    if (busy_ && !loading_) {
        return false;
    }

    binding_ = std::move(binding);
    open_requested_ = true;
    if (busy_) {
        return true;
    }

    try {
        start(SettingsDocumentCompletion::Kind::load, std::move(path), std::move(profiles), {});
    } catch (...) {
        busy_ = false;
        loading_ = false;
        open_requested_ = false;
        throw;
    }
    return true;
}

Status SettingsDocumentSession::request_reload(std::filesystem::path path, bool editor_open) {
    const auto allowed = reload_.begin(editor_open, busy_);
    if (!allowed) {
        return allowed;
    }

    try {
        start(SettingsDocumentCompletion::Kind::reload, std::move(path), {}, {});
    } catch (...) {
        busy_ = false;
        reload_.cancel();
        return Status::failure("Could not start settings reload; current settings were kept.");
    }
    return {};
}

Status SettingsDocumentSession::request_save(Config candidate) {
    if (busy_ || !snapshot_) {
        return Status::failure("A settings snapshot is not ready. Close and reopen Settings.");
    }

    try {
        start(SettingsDocumentCompletion::Kind::save, snapshot_path_, {}, std::move(candidate));
    } catch (const std::exception& error) {
        busy_ = false;
        return Status::failure(error.what());
    }
    return {};
}

std::optional<SettingsDocumentCompletion> SettingsDocumentSession::poll_completion(bool editor_may_open) {
    std::optional<WorkerCompletion> worker_result;
    {
        std::lock_guard lock(mailbox_->mutex);
        worker_result.swap(mailbox_->completion);
    }
    if (!worker_result) {
        return {};
    }

    if (worker_.joinable()) {
        worker_.join();
    }
    busy_ = false;
    loading_ = false;

    SettingsDocumentCompletion result(std::move(worker_result->candidate));
    result.kind = worker_result->kind;
    result.operation = worker_result->operation;
    result.generation = worker_result->generation;
    result.shells = std::move(worker_result->shells);

    if (result.kind == SettingsDocumentCompletion::Kind::reload) {
        result.reloaded = reload_.complete(result.generation, std::move(worker_result->loaded));
        if (result.reloaded.config) {
            reload_application_ = result.operation;
        }
    } else if (result.kind == SettingsDocumentCompletion::Kind::load) {
        result.loaded.succeeded = static_cast<bool>(worker_result->loaded);
        result.loaded.existed = worker_result->loaded.snapshot && worker_result->loaded.snapshot->existed;
        result.loaded.config = std::move(worker_result->loaded.config);
        result.loaded.error = std::move(worker_result->loaded.error);
        result.open_requested = open_requested_;
        open_requested_ = false;
        result.open_blocked = result.open_requested && !editor_may_open;
        if (result.open_requested && editor_may_open && result.loaded) {
            snapshot_ = std::move(worker_result->loaded.snapshot);
            snapshot_path_ = std::move(worker_result->path);
            result.binding = std::move(binding_);
            binding_.clear();
        }
    } else {
        // Record disk success before transferring its token. No token copy or
        // allocation is introduced between persistence and host acknowledgment.
        result.saved.committed = static_cast<bool>(worker_result->saved);
        result.saved.conflict = worker_result->saved.conflict;
        result.saved.error = std::move(worker_result->saved.error);
        if (result.saved) {
            snapshot_ = std::move(worker_result->saved.snapshot);
        }
    }
    return result;
}

void SettingsDocumentSession::accept_reload_application(std::uint64_t operation) noexcept {
    if (operation && operation == reload_application_ && operation == operation_) {
        snapshot_.reset();
        snapshot_path_.clear();
        reload_application_ = 0;
    }
}

SettingsDocumentRetirement SettingsDocumentSession::poll_retirement() {
    cancel_open();
    auto completion = poll_completion(false);
    return {busy_, std::move(completion)};
}

} // namespace stormtty
