#pragma once
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace stormtty {
// Qualification-only parsing of actor-formatted selection text. Callers must
// preserve hard newlines; never concatenate viewport rows to manufacture a record.
struct StartupDiagnosticRecord {
    std::string native_state;
    bool persistent{}, dump_before{}, dump_after{};
};
struct StartupDiagnosticObservation {
    bool valid{true};
    std::vector<StartupDiagnosticRecord> records;
};
inline StartupDiagnosticObservation observe_startup_diagnostic(std::string_view text) {
    StartupDiagnosticObservation result;
    if(text.size()>65536) {result.valid=false;return result;}
    static const std::regex record(R"(STORMTTY_STARTUP schema=1 native_state=(ready|disabled|unavailable) state_prepare_ms=([0-9]+) persistent=([01]) dump_before=([01]) dump_after=([01]) shell_source_ms=(-?[0-9]+\.[0-9]{3}) omz_ms=(-?[0-9]+\.[0-9]{3}) source_to_line_init_ms=(-?[0-9]+\.[0-9]{3}))");
    while(!text.empty()) {
        const auto end=text.find('\n');
        auto line=text.substr(0,end);
        if(!line.empty() && line.back()=='\r')line.remove_suffix(1);
        if(line.find("STORMTTY_STARTUP")!=std::string_view::npos) {
            std::match_results<std::string_view::const_iterator> match;
            if(!std::regex_match(line.begin(),line.end(),match,record)) {result.valid=false;return result;}
            result.records.push_back({match[1].str(),match[3]=="1",match[4]=="1",match[5]=="1"});
        }
        if(end==std::string_view::npos)break;
        text.remove_prefix(end+1);
    }
    return result;
}
} // namespace stormtty
