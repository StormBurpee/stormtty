#include "windows_local_path.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace stormtty::windows_local_path {
namespace {
constexpr unsigned max_path_components = 128;

bool reserved_name(std::wstring name) {
    const auto dot = name.find(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL" || name == L"CLOCK$" ||
           name == L"CONIN$" || name == L"CONOUT$" ||
           (name.size() == 4 && (name.starts_with(L"COM") || name.starts_with(L"LPT")) &&
            ((name[3] >= L'0' && name[3] <= L'9') || name[3] == L'\u00b9' || name[3] == L'\u00b2' || name[3] == L'\u00b3'));
}
} // namespace

// Restrict persistence to ordinary local drive paths. No device namespaces,
// current-directory dependence, ADS, relative traversal or Win32 alias names.
std::filesystem::path checked_path(const std::filesystem::path& input) {
    auto value = input.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.size() < 4 || value.size() > max_path_chars ||
        !((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) ||
        value[1] != L':' || value[2] != L'\\')
        throw std::runtime_error("Settings require an absolute local drive path; network and device paths are not supported.");
    unsigned count{};
    for (std::size_t begin = 3; begin <= value.size();) {
        const auto end = value.find(L'\\', begin);
        const auto part = value.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ' ||
            reserved_name(part) || ++count > max_path_components ||
            std::any_of(part.begin(), part.end(), [](wchar_t c) { return c < 32 || std::wstring_view(L"<>:\"|?*").find(c) != std::wstring_view::npos; }))
            throw std::runtime_error("Settings path contains an unsupported or ambiguous component.");
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    if (GetDriveTypeW(value.substr(0, 3).c_str()) == DRIVE_REMOTE)
        throw std::runtime_error("Settings on network drives are not supported for atomic saving.");
    return std::filesystem::path(std::move(value));
}
std::wstring native_path(const std::filesystem::path& path) { return L"\\\\?\\" + path.wstring(); }

} // namespace stormtty::windows_local_path
#endif
