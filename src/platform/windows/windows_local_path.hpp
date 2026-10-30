#pragma once

#include <filesystem>
#include <string>

namespace stormtty::windows_local_path {

inline constexpr std::size_t max_path_chars = 32700;

// Shared lexical/local-drive policy for persistence and bounded discovery.
// This does not inspect ancestors or retain handles. The guarded document
// service alone owns persistence's stronger link checks and transaction guards.
std::filesystem::path checked_path(const std::filesystem::path& input);
std::wstring native_path(const std::filesystem::path& path);

} // namespace stormtty::windows_local_path
