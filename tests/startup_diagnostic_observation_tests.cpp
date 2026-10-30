#include "stormtty/startup_diagnostic_observation.hpp"
#include "stormtty/terminal.hpp"
#include <iostream>
#include <stdexcept>
using namespace stormtty;
void require(bool ok) {if(!ok)throw std::runtime_error("startup diagnostic observation test failed");}
int main() {
    const std::string record="STORMTTY_STARTUP schema=1 native_state=ready state_prepare_ms=3 persistent=1 dump_before=0 dump_after=1 shell_source_ms=4123.456 omz_ms=3900.123 source_to_line_init_ms=4200.789";
    for(const auto width:{20,40,80,120}) {
        TerminalOptions options;options.size={static_cast<std::uint16_t>(width),32,8,16};
        TerminalCore core(options);
        core.feed("prompt\r\n"+record+"\r\n> ");
        core.select(0,0,static_cast<std::uint16_t>(width-1),31);
        const auto observation=observe_startup_diagnostic(core.selection_text());
        require(observation.valid && observation.records.size()==1);
        const auto& parsed=observation.records.front();
        require(parsed.native_state=="ready" && parsed.persistent && !parsed.dump_before && parsed.dump_after);
        // Same characters split by a hard newline must not become a record.
        TerminalCore hard(options);
        hard.feed(record.substr(0,80)+"\r\n"+record.substr(80)+"\r\n");
        hard.select(0,0,static_cast<std::uint16_t>(width-1),31);
        require(!observe_startup_diagnostic(hard.selection_text()).valid);
    }
    require(!observe_startup_diagnostic(record+"extra").valid);
    require(!observe_startup_diagnostic("prefix"+record).valid);
    require(!observe_startup_diagnostic(record+"\nSTORMTTY_STARTUP malformed").valid);
    require(observe_startup_diagnostic("ordinary prompt\n").records.empty());
    require(observe_startup_diagnostic(record+"\n"+record).records.size()==2);
    std::cout<<"STARTUP_OBSERVATION_TEST widths=4 malformed=3 duplicate=1 code=0\n";
}
