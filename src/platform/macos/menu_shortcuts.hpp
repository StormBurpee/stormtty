#pragma once
#include "stormtty/commands.hpp"
#include <optional>

namespace stormtty::macos {
struct MenuShortcut { char16_t character{}; CommandModifier modifiers{}; };
// AppKit Unicode function-key values. Kept portable for contract tests; native
// tests additionally compare against the SDK constants and dispatch real menus.
inline std::optional<MenuShortcut> menu_shortcut(std::string_view command,std::span<const Binding> bindings) {
    for(const auto& binding:bindings) {
        if(binding.command!=command)continue;
        const auto& chord=binding.chord;
        if(is_os_reserved(chord,Platform::macos) || (static_cast<unsigned>(chord.modifiers)&~15u))return {};
        const auto& key=chord.key;char16_t character{};
        if(key.size()==1 && key[0]>=33 && key[0]<=126) character=key[0]>='A'&&key[0]<='Z'?key[0]+('a'-'A'):key[0];
        else if(key=="TAB")character=0x09;
        else if(key=="ENTER")character=0x0d;
        else if(key=="ESCAPE")character=0x1b;
        else if(key=="SPACE")character=0x20;
        else if(key=="BACKSPACE")character=0x08; // AppKit menu API, not NSEvent's delete byte.
        else if(key=="DELETE")character=0x7f;
        else if(key=="PLUS")character='+';
        else if(key=="UP")character=0xf700;
        else if(key=="DOWN")character=0xf701;
        else if(key=="LEFT")character=0xf702;
        else if(key=="RIGHT")character=0xf703;
        else if(key=="INSERT")character=0xf727;
        else if(key=="HOME")character=0xf729;
        else if(key=="END")character=0xf72b;
        else if(key=="PAGEUP")character=0xf72c;
        else if(key=="PAGEDOWN")character=0xf72d;
        else if(key.size()>=2 && key.size()<=3 && key[0]=='F' && key[1]>='1' && key[1]<='9') {
            unsigned number=key[1]-'0';
            if(key.size()==3){if(key[2]<'0'||key[2]>'9')return {};number=number*10+key[2]-'0';}
            if(number>24)return {};
            character=static_cast<char16_t>(0xf703+number);
        }
        if(!character)return {};
        return MenuShortcut{character,chord.modifiers};
    }
    return {};
}
inline bool menu_commands_available(bool composing,bool closing,bool modal=false) noexcept {return !composing&&!closing&&!modal;}
}
#ifdef __OBJC__
#import <AppKit/AppKit.h>
namespace stormtty::macos {
void apply_menu_shortcut(NSMenuItem*,std::string_view,std::span<const Binding>);
}
#endif
