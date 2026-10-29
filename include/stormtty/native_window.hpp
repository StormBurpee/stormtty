#pragma once

#include "stormtty/config.hpp"
#include "stormtty/chrome_geometry.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace stormtty {

// WM_CHAR is UTF-16, not a scalar stream. A pending high surrogate is never sent
// separately; malformed input is replaced rather than producing invalid UTF-8.
class Utf16Input final {
public:
    std::string feed(char16_t unit) {
        std::string result;
        if (high_) {
            if (unit >= 0xdc00 && unit <= 0xdfff) {
                append(result, 0x10000 + ((static_cast<std::uint32_t>(*high_) - 0xd800) << 10) + unit - 0xdc00);
                high_.reset();
                return result;
            }
            append(result, 0xfffd);
            high_.reset();
        }
        if (unit >= 0xd800 && unit <= 0xdbff) high_ = unit;
        else append(result, unit >= 0xdc00 && unit <= 0xdfff ? 0xfffd : unit);
        return result;
    }
    void reset() noexcept { high_.reset(); }
    static void append(std::string& out, std::uint32_t scalar) {
        if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) scalar = 0xfffd;
        if (scalar < 0x80) out += static_cast<char>(scalar);
        else if (scalar < 0x800) {
            out += static_cast<char>(0xc0 | (scalar >> 6)); out += static_cast<char>(0x80 | (scalar & 63));
        } else if (scalar < 0x10000) {
            out += static_cast<char>(0xe0 | (scalar >> 12)); out += static_cast<char>(0x80 | ((scalar >> 6) & 63)); out += static_cast<char>(0x80 | (scalar & 63));
        } else {
            out += static_cast<char>(0xf0 | (scalar >> 18)); out += static_cast<char>(0x80 | ((scalar >> 12) & 63)); out += static_cast<char>(0x80 | ((scalar >> 6) & 63)); out += static_cast<char>(0x80 | (scalar & 63));
        }
    }
private:
    std::optional<char16_t> high_;
};

// Deadline-based visible phase. A queued old timer message cannot hide the
// caret immediately after input restarted its full visible interval.
class CursorBlinkClock final {
public:
    void configure(bool enabled, std::uint64_t interval, std::uint64_t now) noexcept {
        if (!enabled || !interval) { enabled_=false; visible_=true; interval_=0; return; }
        if (!enabled_ || interval_!=interval) { enabled_=true; interval_=interval; restart(now); }
    }
    void restart(std::uint64_t now) noexcept { visible_=true; deadline_=now+interval_; }
    bool tick(std::uint64_t now) noexcept {
        if (!enabled_ || now<deadline_) return false;
        visible_=!visible_; deadline_=now+interval_; return true;
    }
    bool visible() const noexcept { return visible_; }
    bool enabled() const noexcept { return enabled_; }
    std::uint64_t interval() const noexcept { return interval_; }
private:
    std::uint64_t interval_{}, deadline_{};
    bool enabled_{}, visible_{true};
};

// Stable managed reference: persists no installation path or inherited PATH.
// Windows resolves it only against an explicitly configured/CLI runtime.
inline constexpr std::string_view managed_zsh_profile_id="stormtty-managed-zsh";
inline constexpr std::string_view managed_zsh_profile_executable="@stormtty/managed-zsh";
inline Profile managed_zsh_profile_reference() {
    Profile profile; profile.id=managed_zsh_profile_id; profile.label="Zsh · managed";
    profile.executable=managed_zsh_profile_executable; return profile;
}
inline bool has_managed_zsh_selection(const Config& config) {
    if (config.default_profile!=managed_zsh_profile_id) return false;
    for (const auto& profile:config.profiles) if (profile.id==config.default_profile)
        return profile.executable==managed_zsh_profile_executable;
    return false;
}

// Caller invokes only when the app includes a bundled/explicit runtime.
// An existing saved choice (including old Cmd defaults) is authoritative.
inline void apply_developer_shell_default(Config& config, bool saved_config) {
    if (saved_config) return;
    auto managed=managed_zsh_profile_reference();
    config.default_profile=managed.id;
    for (auto& profile:config.profiles) if (profile.id==managed.id) { profile=std::move(managed); return; }
    config.profiles.push_back(std::move(managed));
}

// Parsing canonicalizes binding order. Compare persisted meaning, while the
// file service retains the original exact bytes/identity for write conflicts.
inline bool settings_baseline_matches(const Config& loaded, const Config& running, Platform platform) {
    return serialize_config(loaded,platform)==serialize_config(running,platform);
}

// Host material policy is independent of terminal colors. Auto intentionally
// uses clear composition at zero blur, and Desktop Acrylic (3) otherwise.
// Windows may still substitute a solid material for system/power conditions.
enum class MaterialFallback { none, solid_selected, no_composition, software, high_contrast, transparency_disabled };
struct WindowMaterialPolicy {
    std::uint32_t system_backdrop{1}; // Documented DWM_SYSTEMBACKDROP_TYPE values.
    float opacity{1};
    MaterialFallback fallback{MaterialFallback::none};
    bool clear{}; // No system/host backdrop; preserve premultiplied canvas alpha.
};
inline WindowMaterialPolicy window_material_policy(const AppearanceConfig& appearance,bool composition,bool software,bool high_contrast,bool transparency) {
    MaterialFallback fallback=MaterialFallback::none;
    if(appearance.backdrop==BackdropPreference::none) fallback=MaterialFallback::solid_selected;
    else if(high_contrast) fallback=MaterialFallback::high_contrast;
    else if(!transparency) fallback=MaterialFallback::transparency_disabled;
    else if(software) fallback=MaterialFallback::software;
    else if(!composition) fallback=MaterialFallback::no_composition;
    if(fallback!=MaterialFallback::none) return {1,1,fallback};
    if(appearance.backdrop!=BackdropPreference::mica && appearance.additional_blur==0)
        return {1,static_cast<float>(appearance.background_opacity),fallback,true};
    return {appearance.backdrop==BackdropPreference::mica?2U:3U,static_cast<float>(appearance.background_opacity),fallback};
}
struct MaterialRenderState {
    bool composition{},software{};
    std::uint64_t generation{};
    bool host_backdrop{};
    bool operator==(const MaterialRenderState&) const = default;
};
class MaterialRefreshTracker {
public:
    bool observe(MaterialRenderState state) {
        if(previous_ && *previous_==state) return false;
        previous_=state;return true;
    }
private:
    std::optional<MaterialRenderState> previous_;
};

// Enters the platform message loop. Called once, from the main UI thread.
// Windows is implemented; other platforms provide their own native shell.
struct NativeRunOptions {
    bool smoke_test{}; // Explicit CI mode; never enabled on a normal launch.
    std::string smoke_workspaces; // Dedicated isolated restart qualification: seed/restore/corrupt.
    std::filesystem::path agent_settings_path; // Machine-local only; never derived from portable config/workspaces.
    std::filesystem::path codex_executable; // Inert until explicit native Connect.
    std::string codex_account_reference, codex_runtime_version;
    bool smoke_agent{}; // Authored native UI fixtures only; never starts a provider.
    bool smoke_settings{}; // Styled Settings/menu routing, reversible preview and isolated explicit Save.
    bool smoke_modal{}; // Styled modal + routing/close smoke, not desktop activation QA.
    bool smoke_projects{}; // Two actual folder contexts, actor retention and scoped close.
    bool smoke_layout{}; // Three visible real shells + two background tabs.
    std::filesystem::path config_path; // Exact absolute startup path; writes only on explicit Save.
    std::filesystem::path capture_path; // Smoke-only actual framebuffer PNG.
    // Explicit diagnostic CLI override; takes precedence over the saved backend.
    // Empty honors Config::windows_console_backend, whose default is System.
    // Windows transport verifies the pinned runtime and actual console host.
    std::filesystem::path console_runtime_directory;
    // App-owned bundled upstream Zsh or an explicit CLI override; no global environment mutation.
    std::filesystem::path shell_runtime_directory;
    std::string shell_runtime_error; // Unavailable optional runtime does not block a saved unrelated shell.
    bool shell_runtime_override{true}; // CLI forces this launch; configured builds honor saved choices.
};
int run_native_application(Config config, NativeRunOptions options = {});

} // namespace stormtty
