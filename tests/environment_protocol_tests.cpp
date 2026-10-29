#include "../tools/probes/environment_protocol.hpp"
#include <sstream>
#include <iostream>
int main() {
    std::ostringstream output;
    for (const auto& entry:std::vector<stormtty::ProcessEnvironmentEntry>{
        {"STORMTTY_EMPTY_HOME",""},{"STORMTTY_NATIVE_ZSH_NO_DUMP",std::nullopt},
        {"STORMTTY_NATIVE_ZSH_INIT_EVENT",std::nullopt},{"HOME",std::string("caf\xc3\xa9")}})
        stormtty::qualification::write_environment_override(output,entry);
    if (output.str()!="STORMTTY_EMPTY_HOME=\nSTORMTTY_NATIVE_ZSH_NO_DUMP=-\nSTORMTTY_NATIVE_ZSH_INIT_EVENT=-\nHOME=636166c3a9\n") return 1;
    std::cout << "PASS: optional deletion, empty value and Unicode environment protocol\n";
}
