#include "consent.hpp"
#include <cwchar>
namespace stormtty::windows::liquid_glass {
bool selection_current(HWND owner,const DisplaySelection& selection) noexcept {
    MONITORINFOEXW current{};current.cbSize=sizeof current;
    return IsWindow(owner) && !IsIconic(owner) &&
        GetMonitorInfoW(selection.monitor,&current) &&
        std::wcscmp(current.szDevice,selection.info.szDevice)==0 &&
        EqualRect(&current.rcMonitor,&selection.info.rcMonitor);
}
std::optional<DisplaySelection> confirm_display(HWND owner) {
    DisplaySelection selected;selected.monitor=MonitorFromWindow(owner,MONITOR_DEFAULTTONULL);
    selected.info.cbSize=sizeof selected.info;
    if(!selected.monitor||!GetMonitorInfoW(selected.monitor,&selected.info))return std::nullopt;
    const std::wstring warning=std::wstring(L"Start Experimental Liquid Glass on display ")+selected.info.szDevice+L"?\n\n"
        L"The entire selected display is captured locally while this session is enabled. Pixels stay on the GPU: no recording, uploads or saved screenshots. "
        L"The Windows capture indicator stays visible.\n\n"
        L"To prevent visual feedback, this StormTTY window will disappear from supported screenshots and screen shares while enabled. "
        L"Stop restores its prior capture setting.\n\n"
        L"This is experimental: live background correctness and appearance are still being tested. SDR and a compatible hardware adapter are required. "
        L"Large windows, display changes, minimize or graphics changes may stop it safely. Existing blur settings are not changed.\n\n"
        L"Start only for this session? (Off/Stop is available in Settings.)";
    if(MessageBoxW(owner,warning.c_str(),L"Experimental Liquid Glass — local display capture",
                   MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return std::nullopt;
    if(!selection_current(owner,selected))return std::nullopt;
    return selected;
}
}
