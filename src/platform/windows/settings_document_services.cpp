#include "stormtty/settings_document_session.hpp"

namespace stormtty {
namespace {

class NativeSettingsDocumentServices final : public SettingsDocumentServices {
    SettingsSnapshotResult load(const std::filesystem::path& path) override {
        return load_settings_snapshot(path);
    }

    ShellDiscoveryResult discover(std::span<const Profile> profiles) override {
        return discover_windows_shells(profiles);
    }

    ConfigSaveResult save(
        const std::filesystem::path& path,
        const Config& candidate,
        const ConfigFileSnapshot& baseline) override {
        return save_settings_config(path, candidate, baseline);
    }
};

} // namespace

std::shared_ptr<SettingsDocumentServices> windows_settings_document_services() {
    return std::make_shared<NativeSettingsDocumentServices>();
}

} // namespace stormtty
