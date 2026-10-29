// Owned STA window/dialog only. Does not send global input or touch other apps.
#include "core_test.hpp"
#include "../src/platform/windows/theme_picker.hpp"
#include <cstdio>
#include <objbase.h>
using namespace stormtty;
namespace {
HWND owner{};unsigned seen{};
BOOL CALLBACK close_owned(HWND window,LPARAM) noexcept {
    wchar_t name[32]{};GetClassNameW(window,name,32);
    if(window!=owner && GetWindow(window,GW_OWNER)==owner && lstrcmpW(name,L"#32770")==0){
        ++seen;PostMessageW(window,WM_CLOSE,0,0);
    }
    return TRUE;
}
void CALLBACK cancel_dialog(HWND,UINT,UINT_PTR,DWORD) noexcept {EnumThreadWindows(GetCurrentThreadId(),close_owned,0);}
}
int main(){return run_core_tests("native theme picker cancellation",[]{
    CHECK(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)));
    struct Apartment{~Apartment(){CoUninitialize();}} apartment;
    CHECK(!pick_theme(nullptr,true,Theme{}).imported);
    owner=CreateWindowExW(0,L"STATIC",L"StormTTY owned theme picker fixture",WS_OVERLAPPEDWINDOW,100,100,480,280,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);CHECK(owner);
    struct Window{~Window(){DestroyWindow(owner);}} window;
    ShowWindow(owner,SW_SHOW);
    for(bool importing:{true,false}) {
        seen=0;auto timer=SetTimer(nullptr,0,100,cancel_dialog);CHECK(timer);
        const auto result=pick_theme(owner,importing,Theme{});KillTimer(nullptr,timer);
        CHECK(seen>0);CHECK(result.error.empty());CHECK(!result.imported);CHECK(result.notice.empty());CHECK(IsWindow(owner));
    }
    const auto destroyed=owner;CHECK(DestroyWindow(owner));owner=nullptr;
    CHECK(!pick_theme(destroyed,false,Theme{}).imported);
    std::puts("Owned import/export dialogs canceled; invalid owner rejected; no picker selection automated.");
});}
