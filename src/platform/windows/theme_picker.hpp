#pragma once
#include "stormtty/theme_exchange.hpp"
#include <windows.h>
#include <filesystem>
namespace stormtty {
struct ThemePickerResult {
    std::optional<Theme> imported;
    std::string error;
    std::string notice;
};
// Bounded local-file IO shared by the picker and owned native fixtures.
ThemePickerResult exchange_theme_file(const std::filesystem::path&, bool importing, ThemeFormat, const Theme&);
// Synchronous native dialog; no shell or provider execution. The caller must
// verify its Settings lifetime/generation after returning from the modal loop.
ThemePickerResult pick_theme(HWND owner, bool importing, const Theme& current);
}
