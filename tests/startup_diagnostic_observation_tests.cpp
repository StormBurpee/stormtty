#include "stormtty/startup_diagnostic_observation.hpp"
#include "stormtty/terminal.hpp"
#include <iostream>
#include <array>
#include <stdexcept>
using namespace stormtty;
void require(bool ok) {if(!ok)throw std::runtime_error("startup diagnostic observation test failed");}
int main(int argc, char** argv) {
    if(argc==3 && std::string_view(argv[1])=="--replay") {
        const std::string_view width_text(argv[2]);
        require(width_text=="20" || width_text=="40" || width_text=="80" || width_text=="120");
        const auto width=static_cast<std::uint16_t>(std::stoi(std::string(width_text)));
        TerminalOptions options;options.size={width,32,8,16};
        TerminalCore core(options);
        std::array<char,4096> chunk{};
        std::size_t total=0;
        while(std::cin.read(chunk.data(),chunk.size()) || std::cin.gcount()) {
            const auto count=static_cast<std::size_t>(std::cin.gcount());
            total+=count;require(total<=65536);
            core.feed(std::string_view(chunk.data(),count));
        }
        core.select(0,0,width-1,31);
        const auto selected=core.selection_text();
        const auto observation=observe_startup_diagnostic(selected);
        // Raw PTY substring presence is not enough: cursor redraw can overwrite
        // a record after it was emitted. Use the real formatter and strict parser.
        require(observation.valid && observation.records.size()==1);
        require(selected.find("STARTUP_INPUT42")!=std::string::npos);
        std::cout<<"STARTUP_RENDERED_TEST code=0\n";
        return 0;
    }
    require(argc==1);
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
