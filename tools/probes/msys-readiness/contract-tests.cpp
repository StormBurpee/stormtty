#include "evidence-contract.hpp"
#include <iostream>
#include <stdexcept>
using stormtty::qualification::InputEvidence;
using stormtty::readiness::intact_authored_writes;
void require(bool ok) { if (!ok) throw std::runtime_error("readiness write-shape contract"); }
int main() {
    constexpr std::string_view expected = "\x1b[200~PASTE_QUAL_0123456789_[x]_end\x1b[201~\r";
    const auto good = [&] {
        InputEvidence value;
        value.completed_write(1, 0, expected.substr(0, 41), true, 41, 0);
        value.completed_write(2, 0, expected.substr(41), true, 1, 0);
        return value.snapshot();
    };
    require(intact_authored_writes(good(), expected));
    for (std::size_t split = 1; split < expected.size(); ++split) {
        InputEvidence value;
        value.completed_write(1, 0, expected.substr(0, split), true, static_cast<unsigned>(split), 0);
        value.completed_write(2, 0, expected.substr(split), true, static_cast<unsigned>(expected.size() - split), 0);
        require(intact_authored_writes(value.snapshot(), expected) == (split == 41));
    }
    InputEvidence combined;
    combined.completed_write(1, 0, expected, true, 42, 0);
    require(!intact_authored_writes(combined.snapshot(), expected));
    for (const auto reply : {std::string_view("\x1b[2;1R"), std::string_view("\x1b[0;1R"), std::string_view("\x1b[2;1Rpayload")}) {
        InputEvidence value;
        value.completed_write(1, 0, reply, true, static_cast<unsigned>(reply.size()), 0);
        value.completed_write(2, 0, expected.substr(0, 41), true, 41, 0);
        value.completed_write(3, 0, expected.substr(41), true, 1, 0);
        require(intact_authored_writes(value.snapshot(), expected) == (reply == "\x1b[2;1R"));
    }
    for (unsigned fault = 0; fault < 8; ++fault) {
        auto value = good();
        switch (fault) {
        case 0: value.records[0].offset = 1; break;
        case 1: value.records[0].success = false; break;
        case 2: value.records[0].error = 5; break;
        case 3: --value.records[0].accepted; break;
        case 4: --value.records[0].stored; break;
        case 5: value.missing_since_after = 1; break;
        case 6: value.records.push_back(value.records.back()); break;
        case 7: value.records.clear(); break;
        }
        require(!intact_authored_writes(value, expected));
    }
    std::cout << "readiness exact authored-write shape and CPR attribution PASS\n";
}
