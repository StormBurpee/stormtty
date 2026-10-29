#pragma once
#include "stormtty/config.hpp"

namespace stormtty {
// Explicit format: never infer trusted configuration from a filename or content.
enum class ThemeFormat { stormtty, ghostty };
struct ThemeImportResult {
    std::optional<Theme> theme;
    std::vector<ConfigError> errors;
    std::vector<ConfigError> warnings;
    explicit operator bool() const noexcept { return theme.has_value() && errors.empty(); }
};
inline constexpr std::size_t max_theme_bytes = 64 * 1024;
// Pure, bounded, data-only parsing. Missing Ghostty colors retain the supplied
// palette. No files, includes, URLs, processes, or live configuration changes.
// StormTTY requires a complete palette, so its exports are self-contained.
ThemeImportResult import_theme(std::string_view, ThemeFormat, const Theme& current);
// Invalid input returns nullopt, never exports directives from untrusted strings.
// Ghostty omits StormTTY-only chrome/accent; caller should disclose that loss.
std::optional<std::string> export_theme(const Theme&, ThemeFormat);
} // namespace stormtty
