#include "core_test.hpp"
#include "../src/platform/macos/menu_shortcuts.hpp"
@interface STMenuFixture : NSObject
@property NSUInteger calls;
- (void)invoke:(id)sender;
@end
@implementation STMenuFixture
- (void)invoke:(id)sender {(void)sender;++_calls;}
@end
using namespace stormtty;
using namespace stormtty::macos;
int main(){@autoreleasepool{return run_core_tests("native AppKit menu shortcuts",[]{
    [NSApplication sharedApplication];
    auto bindings=default_bindings(Platform::macos);
    NSMenu* menu=[[NSMenu alloc] initWithTitle:@"Owned shortcut fixture"];menu.autoenablesItems=NO;
    STMenuFixture* target=[STMenuFixture new];
    NSMenuItem* item=[[NSMenuItem alloc] initWithTitle:@"New terminal tab" action:@selector(invoke:) keyEquivalent:@""];
    item.target=target;[menu addItem:item];apply_menu_shortcut(item,"tab.new",bindings);
    CHECK([item.keyEquivalent isEqualToString:@"t"]);CHECK(item.keyEquivalentModifierMask==NSEventModifierFlagCommand);
    auto event=[](NSString* text,NSEventModifierFlags modifiers){return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:modifiers timestamp:0 windowNumber:0 context:nil characters:text charactersIgnoringModifiers:text isARepeat:NO keyCode:17];};
    CHECK([menu performKeyEquivalent:event(@"t",NSEventModifierFlagCommand)]);CHECK(target.calls==1);
    CHECK(![menu performKeyEquivalent:event(@"t",NSEventModifierFlagControl)]);CHECK(target.calls==1);
    item.enabled=NO;CHECK(![menu performKeyEquivalent:event(@"t",NSEventModifierFlagCommand)]);CHECK(target.calls==1);item.enabled=YES;
    apply_menu_shortcut(item,"app.command_palette",bindings);
    CHECK([item.keyEquivalent isEqualToString:@"p"]);CHECK(item.keyEquivalentModifierMask==(NSEventModifierFlagCommand|NSEventModifierFlagShift));
    const std::vector<Binding> custom{{"tab.new",{"Y",CommandModifier::command}}};apply_menu_shortcut(item,"tab.new",custom);
    CHECK([item.keyEquivalent isEqualToString:@"y"]);CHECK(![menu performKeyEquivalent:event(@"t",NSEventModifierFlagCommand)]);
    CHECK([menu performKeyEquivalent:event(@"y",NSEventModifierFlagCommand)]);CHECK(target.calls==2);
    for(const auto* key:{"BACKSPACE","DELETE","LEFT","F24"}) {
        const std::vector<Binding> mapping{{"tab.new",{key,CommandModifier::command|CommandModifier::alt}}};
        apply_menu_shortcut(item,"tab.new",mapping);CHECK(item.keyEquivalent.length==1);
        CHECK([item.keyEquivalent characterAtIndex:0]==menu_shortcut("tab.new",mapping)->character);
        CHECK(item.keyEquivalentModifierMask==(NSEventModifierFlagCommand|NSEventModifierFlagOption));
    }
    apply_menu_shortcut(item,"missing",custom);CHECK(item.keyEquivalent.length==0);CHECK(item.keyEquivalentModifierMask==0);
    const std::vector<Binding> reserved{{"tab.new",{"Q",CommandModifier::command}}};apply_menu_shortcut(item,"tab.new",reserved);CHECK(item.keyEquivalent.length==0);
    CHECK(!menu_commands_available(true,false));CHECK(!menu_commands_available(false,true));CHECK(!menu_commands_available(false,false,true));
    CHECK(menu_commands_available(false,false));
});}}
