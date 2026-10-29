#include "menu_shortcuts.hpp"
namespace stormtty::macos {
static_assert(NSUpArrowFunctionKey==0xf700 && NSLeftArrowFunctionKey==0xf702 && NSF1FunctionKey==0xf704 && NSF24FunctionKey==0xf71b);
static_assert(NSInsertFunctionKey==0xf727 && NSHomeFunctionKey==0xf729 && NSEndFunctionKey==0xf72b && NSPageUpFunctionKey==0xf72c && NSPageDownFunctionKey==0xf72d);
static_assert(NSBackspaceCharacter==0x08 && NSDeleteCharacter==0x7f);
void apply_menu_shortcut(NSMenuItem* item,std::string_view command,std::span<const Binding> bindings) {
    item.keyEquivalent=@"";item.keyEquivalentModifierMask=0;
    const auto shortcut=menu_shortcut(command,bindings);if(!shortcut)return;
    const unichar character=shortcut->character;
    item.keyEquivalent=[NSString stringWithCharacters:&character length:1];
    NSEventModifierFlags flags=0;
    if(has_modifier(shortcut->modifiers,CommandModifier::command))flags|=NSEventModifierFlagCommand;
    if(has_modifier(shortcut->modifiers,CommandModifier::control))flags|=NSEventModifierFlagControl;
    if(has_modifier(shortcut->modifiers,CommandModifier::alt))flags|=NSEventModifierFlagOption;
    if(has_modifier(shortcut->modifiers,CommandModifier::shift))flags|=NSEventModifierFlagShift;
    item.keyEquivalentModifierMask=flags;
}
}
