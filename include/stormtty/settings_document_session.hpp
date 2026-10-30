#pragma once

#include "stormtty/settings_reload.hpp"

#include <memory>
#include <mutex>
#include <thread>

namespace stormtty {

// The sole platform notification capability. Workers retain only a weak endpoint.
struct SettingsDocumentWake {
    virtual ~SettingsDocumentWake() = default;
    virtual void wake() noexcept = 0;
};

// One file-operation seam. Native implementations preserve the guarded services;
// qualification implementations may decorate them with deterministic I/O gates.
struct SettingsDocumentServices {
    virtual ~SettingsDocumentServices() = default;

    virtual SettingsSnapshotResult load(const std::filesystem::path& path) = 0;
    virtual ShellDiscoveryResult discover(std::span<const Profile> profiles) = 0;
    virtual ConfigSaveResult save(
        const std::filesystem::path& path,
        const Config& candidate,
        const ConfigFileSnapshot& baseline) = 0;
};

std::shared_ptr<SettingsDocumentServices> windows_settings_document_services();

// Completion events carry outcomes, not copies of the owner's concurrency token.
struct SettingsDocumentLoadOutcome {
    std::optional<Config> config;
    std::string error;
    bool succeeded{};
    bool existed{};

    explicit operator bool() const noexcept {
        return succeeded;
    }
};

struct SettingsDocumentSaveOutcome {
    std::string error;
    bool committed{};
    bool conflict{};

    explicit operator bool() const noexcept {
        return committed;
    }
};

struct SettingsDocumentCompletion {
    // Move the already-owned candidate; do not default-allocate a new Config
    // while acknowledging a successful file write.
    explicit SettingsDocumentCompletion(Config candidate) noexcept;

    enum class Kind { load, save, reload };

    Kind kind{};
    std::uint64_t generation{};
    std::uint64_t operation{};

    SettingsDocumentLoadOutcome loaded;
    ShellDiscoveryResult shells;
    SettingsDocumentSaveOutcome saved;
    Config candidate;
    SettingsReloadResult reloaded;

    bool open_requested{};
    bool open_blocked{};
    std::string binding;
};

struct SettingsDocumentStatus {
    bool busy{};
    bool loading{};
    bool open_requested{};
    bool worker_joinable{};
    std::uint64_t operation{};
    std::uint64_t reload_generation{};
    std::string binding;
};

struct SettingsDocumentRetirement {
    bool pending{};
    std::optional<SettingsDocumentCompletion> completion;
};

// UI-thread owner of serialized document I/O, exact same-read save token and
// editor-open intent. SettingsPanel remains the owner of all editor draft state.
// No live Config, native window, renderer or actor is retained. Destruction and
// join_after_window_destroyed must occur outside a native window callback.
class SettingsDocumentSession final {
public:
    SettingsDocumentSession(
        std::shared_ptr<SettingsDocumentServices> services,
        std::weak_ptr<SettingsDocumentWake> wake);
    ~SettingsDocumentSession();

    SettingsDocumentSession(const SettingsDocumentSession&) = delete;
    SettingsDocumentSession& operator=(const SettingsDocumentSession&) = delete;

    bool request_open(
        std::filesystem::path path,
        std::vector<Profile> profiles,
        std::string binding = {});
    void cancel_open() noexcept;
    Status request_reload(std::filesystem::path path, bool editor_open);
    Status request_save(Config candidate);

    // Only a published completion permits the bounded normal worker reap. The
    // host decides modal eligibility by value; no retained UI callback is used.
    std::optional<SettingsDocumentCompletion> poll_completion(bool editor_may_open);
    void accept_reload_application(std::uint64_t operation) noexcept;

    SettingsDocumentRetirement poll_retirement();
    void join_after_window_destroyed();
    SettingsDocumentStatus status() const;

    // Value-only diagnostic for existing qualification byte assertions.
    std::optional<ConfigFileSnapshot> snapshot() const;

private:
    struct WorkerCompletion {
        SettingsDocumentCompletion::Kind kind{};
        std::uint64_t generation{};
        std::uint64_t operation{};
        std::filesystem::path path;
        SettingsSnapshotResult loaded;
        ShellDiscoveryResult shells;
        ConfigSaveResult saved;
        Config candidate;
    };

    struct Mailbox {
        std::mutex mutex;
        std::optional<WorkerCompletion> completion;
    };

    void start(
        SettingsDocumentCompletion::Kind kind,
        std::filesystem::path path,
        std::vector<Profile> profiles,
        Config candidate);

    std::shared_ptr<SettingsDocumentServices> services_;
    std::weak_ptr<SettingsDocumentWake> wake_;
    std::shared_ptr<Mailbox> mailbox_{std::make_shared<Mailbox>()};
    std::thread worker_;

    SettingsReload reload_;
    std::optional<ConfigFileSnapshot> snapshot_;
    std::filesystem::path snapshot_path_;
    bool busy_{};
    bool loading_{};
    bool open_requested_{};
    std::string binding_;
    std::uint64_t operation_{};
    std::uint64_t reload_application_{};
};

} // namespace stormtty
