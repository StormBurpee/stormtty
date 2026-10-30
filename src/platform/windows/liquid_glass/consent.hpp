#pragma once
#include <windows.h>
#include <optional>
#include <string>
namespace stormtty::windows::liquid_glass {
struct DisplaySelection { HMONITOR monitor{}; MONITORINFOEXW info{}; };
std::optional<DisplaySelection> confirm_display(HWND owner);
bool selection_current(HWND owner,const DisplaySelection&) noexcept;
}
