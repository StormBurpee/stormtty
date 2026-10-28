#pragma once

#include <string_view>

namespace stormtty {

enum class StartupNoticeKind { information, error };

// Pre-session fallback: no terminal, graphics device, COM apartment, or resources
// are required. This owns a finite modal loop on the calling UI thread; do not
// use it for session alerts (those belong to the renderer's modal layer).
// Returns 0 after dismissal, -1 if creation/message dispatch failed or WM_QUIT
// interrupted the notice. Errors are reported to stderr, never another dialog.
[[nodiscard]] int show_startup_notice(std::string_view title_utf8,
                                      std::string_view body_utf8,
                                      StartupNoticeKind kind = StartupNoticeKind::information) noexcept;

} // namespace stormtty
