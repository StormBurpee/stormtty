#pragma once

#include "stormtty/commands.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace stormtty {
struct Theme {
    std::string name{"graphite"};
    std::string background{"#11151B"};
    std::string foreground{"#EEEDEC"};
    std::string chrome{"#181D24"};
    std::string accent{"#8C9EFF"};
    auto operator<=>(const Theme&) const = default;
};
enum class BackdropPreference { automatic, none, mica, acrylic };
enum class ReducedMotionPreference { system, enabled, disabled };
struct AppearanceConfig {
    // Applies ONLY to default canvas background, never glyphs/explicit ANSI
    // backgrounds. Hosts force opaque in high contrast and unsupported paths.
    double background_opacity{0.94};
    BackdropPreference backdrop{BackdropPreference::acrylic};
    ReducedMotionPreference reduced_motion{ReducedMotionPreference::system};
    auto operator<=>(const AppearanceConfig&) const = default;
};
struct FontConfig {
    std::string family{"Cascadia Mono"};
    double size_dip{15.0};
    bool ligatures{};
    auto operator<=>(const FontConfig&) const = default;
};
struct Profile {
    std::string id{"default"};
    std::string label{"Default shell"};
    // Passed as an executable plus discrete arguments, NEVER interpolated into
    // a shell string. Relative executables require host discovery/approval policy.
    std::string executable;
    std::vector<std::string> arguments;
    std::string working_directory;
    std::map<std::string, std::string> environment;
    auto operator<=>(const Profile&) const = default;
};
struct Config {
    unsigned version{1};
    Theme theme;
    AppearanceConfig appearance;
    FontConfig font;
    std::size_t scrollback_lines{10000};
    std::string default_profile{"default"};
    bool allow_terminal_shortcut_override{};
    std::vector<Profile> profiles;
    std::vector<Binding> bindings;
    auto operator<=>(const Config&) const = default;
};
struct ConfigError { std::size_t line{}, column{}; std::string message; };
struct ConfigParseResult {
    std::optional<Config> config;
    std::vector<ConfigError> errors;
    explicit operator bool() const noexcept { return config.has_value() && errors.empty(); }
};
Config default_config(Platform);
// Strict bounded declarative StormTTY config v1. See config_format_documentation().
// No includes, interpolation, script execution or silent partial reloads.
ConfigParseResult parse_config(std::string_view text, Platform);
ConfigParseResult load_config(const std::filesystem::path&, Platform);
std::string serialize_config(const Config&, Platform);
Status validate_config(const Config&, Platform);
std::string_view config_format_documentation() noexcept;
// Parse/validate first, then replace atomically in memory; invalid reload retains
// the exact previous config and returns diagnostics for visible presentation.
ConfigParseResult reload_config(Config& current, std::string_view text, Platform);
} // namespace stormtty
