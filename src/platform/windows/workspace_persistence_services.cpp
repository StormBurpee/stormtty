#include "stormtty/application_workspace_persistence.hpp"

namespace stormtty {
namespace {

class NativeWorkspacePersistenceServices final : public WorkspacePersistenceServices {
    ApplicationLayoutSnapshotResult load(const std::filesystem::path& path) override {
        return load_application_layout_snapshot(path);
    }

    ConfigSaveResult save(
        const std::filesystem::path& path,
        const ApplicationLayoutDocument& document,
        const ConfigFileSnapshot& baseline) override {
        return save_application_layout(path, document, baseline);
    }
};

} // namespace

std::shared_ptr<WorkspacePersistenceServices> windows_workspace_persistence_services() {
    return std::make_shared<NativeWorkspacePersistenceServices>();
}

} // namespace stormtty
