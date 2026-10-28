#pragma once
#include <windows.h>
#include "../../app/windows_resources.h"

namespace stormtty::windows {
// Module resources are shared OS-owned handles; never DestroyIcon these.
inline HICON app_icon(UINT dpi, bool small_icon) noexcept {
    const int width=GetSystemMetricsForDpi(small_icon?SM_CXSMICON:SM_CXICON,dpi);
    const int height=GetSystemMetricsForDpi(small_icon?SM_CYSMICON:SM_CYICON,dpi);
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_STORMTTY),
        IMAGE_ICON,width,height,LR_SHARED));
}
inline void set_app_icons(HWND window) noexcept {
    const auto dpi=GetDpiForWindow(window);
    if(const auto icon=app_icon(dpi,false)) SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icon));
    if(const auto icon=app_icon(dpi,true)) SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(icon));
}
}
