#include "stormtty/previous_terminal.hpp"
#include <iostream>
#include <stdexcept>
using namespace stormtty;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try {
    PreviousTerminal history;
    constexpr WatchIdentity a{1,7,1},b{1,8,2},replacement{1,7,3},other_host{2,7,1};
    check(!history.previous(),"initially no previous terminal");
    history.commit(a);history.commit(a);history.commit({});
    check(history.current()==a&&!history.previous(),"same actor and absent selections do not create history");
    history.commit(b);check(history.current()==b&&history.previous()==a,"committed A to B");
    history.commit(history.previous());check(history.current()==a&&history.previous()==b,"return B to A");
    history.commit(history.previous());check(history.current()==b&&history.previous()==a,"repeat return toggles");
    // Rename, pane move, metadata update, overlay preview, Escape, deactivation
    // and output have no selection commit. Re-observing A is also idempotent.
    history.commit(a);for(int n=0;n<20;++n)history.commit(a);
    check(history.previous()==b,"placement and repeated metadata commits preserve history");
    history.commit(replacement);check(history.previous()==a&&history.current()==replacement,"reused session ID is distinct incarnation");
    history.commit(other_host);check(history.previous()==replacement,"host identity separates windows");
    for(auto state:{PreviousTerminalState::none,PreviousTerminalState::parked,PreviousTerminalState::missing,PreviousTerminalState::dormant,PreviousTerminalState::stale}) {
        const auto availability=previous_terminal_availability(state);
        check(!availability.enabled&&!availability.reason.empty(),"unavailable destination has explicit reason");
    }
    check(previous_terminal_availability(PreviousTerminalState::available).enabled,"retained destination allowed independent of process state");
    CommandContext context;
    check(!command_availability("terminal.previous",context).enabled,"macOS and other unwired hosts explicitly unavailable");
    context.supports_previous_terminal=true;
    context.previous_terminal_reason=previous_terminal_availability(PreviousTerminalState::parked).reason;
    check(command_availability("terminal.previous",context).reason==context.previous_terminal_reason,"host target reason preserved");
    context.previous_terminal_available=true;
    check(command_availability("terminal.previous",context).enabled,"wired available destination enabled without current pane requirement");
    for(auto query:{"back","previous","return"}) {
        bool found=false;for(const auto* command:search_commands(query))found|=command->id=="terminal.previous";
        check(found,"return is searchable by back previous return");
    }
    for(auto platform:{Platform::windows,Platform::macos})for(const auto& binding:default_bindings(platform))
        check(binding.command!="terminal.previous","no new default shortcut");
    std::cout<<"previous terminal policy passed\n";
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
