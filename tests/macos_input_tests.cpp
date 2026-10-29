#include "../src/platform/macos/input.hpp"
#include "../src/platform/macos/menu_shortcuts.hpp"
#include "stormtty/commands.hpp"
#include <iostream>
#include <stdexcept>
int main() {
    using namespace stormtty;using namespace stormtty::macos;
    try {
        auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
        require(physical_key(0)==Key::a&&physical_key(36)==Key::enter&&physical_key(123)==Key::left,"Mac key identities");
        require(physical_key(999)==Key::unidentified,"Unknown key must not become text");
        require(!direct_key(Key::a)&&!direct_key(Key::space)&&direct_key(Key::backspace),"IME printable input routing");
        auto bindings=default_bindings(Platform::macos);
        CommandContext context;context.tabs=1;context.panes=1;context.session_running=true;context.has_selection=true;
        require(resolve_command({{"C",CommandModifier::command}},bindings,context,Platform::macos)=="edit.copy","Cmd+C must copy");
        require(!resolve_command({{"C",CommandModifier::control}},bindings,context,Platform::macos),"Ctrl+C must reach terminal");
        require(!resolve_command({{"T",CommandModifier::command},true},bindings,context,Platform::macos),"Marked text must suppress commands");
        require(is_os_reserved({"Q",CommandModifier::command},Platform::macos),"Cmd+Q belongs to AppKit");
        auto shortcut=menu_shortcut("tab.new",bindings);
        require(shortcut && shortcut->character=='t' && shortcut->modifiers==CommandModifier::command,"Configured Cmd+T menu equivalent");
        shortcut=menu_shortcut("app.command_palette",bindings);
        require(shortcut && shortcut->character=='p' && has_modifier(shortcut->modifiers,CommandModifier::shift),"Shift preserved without uppercasing key equivalent");
        require(!menu_shortcut("missing",bindings),"No invented menu shortcut");
        const std::vector<Binding> custom{{"tab.new",{"LEFT",CommandModifier::command|CommandModifier::alt}}};
        require(menu_shortcut("tab.new",custom)->character==0xf702,"Native arrow equivalent");
        for(const auto* key:{"Q","H"}){const std::vector<Binding> reserved{{"tab.new",{key,CommandModifier::command}}};require(!menu_shortcut("tab.new",reserved),"OS shortcuts retained by AppKit");}
        require(!menu_commands_available(false,false,true)&&!menu_commands_available(true,false)&&!menu_commands_available(false,true)&&menu_commands_available(false,false),"Composition and close block menu command dispatch");
        std::cout<<"PASS portable macOS physical-key and shared-command contract (not native IME verification)\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
