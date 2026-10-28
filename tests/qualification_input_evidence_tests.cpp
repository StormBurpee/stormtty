#include "stormtty/qualification_input_evidence.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
using stormtty::qualification::InputEvidence;
void require(bool condition) { if (!condition) throw std::runtime_error("input evidence assertion"); }
int main() {
    InputEvidence evidence;
    // Deterministic WriteFile result script: partial command, protocol reply,
    // CR, then failed and zero-byte writes. Count only successful bytes.
    evidence.completed_write(1, 0, "stormtty_probe_size", true, 1, 0);
    evidence.completed_write(1, 1, "tormtty_probe_size", true, 18, 0);
    evidence.completed_write(2, 0, "\033[1;1R", true, 6, 0);
    evidence.completed_write(3, 0, "\r", true, 1, 0);
    evidence.completed_write(4, 0, "bad", false, 2, 995);
    evidence.completed_write(5, 0, "zero", true, 0, 0);
    auto s = evidence.snapshot();
    require(s.completed == 6 && s.overwritten == 0 && s.records.size() == 6);
    std::string accepted;
    for (const auto& r : s.records) accepted.append(reinterpret_cast<const char*>(r.bytes.data()), r.accepted);
    require(accepted == "stormtty_probe_size\033[1;1R\r");
    require(s.records[1].offset == 1 && s.records[1].message == 1);
    require(s.records[4].reported == 2 && s.records[4].accepted == 0 && s.records[4].error == 995);
    std::ostringstream out; InputEvidence::dump(out, s, "test");
    require(out.str().find("accepted_hex=73\n") != std::string::npos);
    require(out.str().find("accepted_hex=1b5b313b3152") != std::string::npos);
    const std::string large(InputEvidence::bytes_per_record + 10, 'x');
    evidence.completed_write(6, 0, large, true, static_cast<std::uint32_t>(large.size()), 0);
    s = evidence.snapshot();
    require(s.byte_truncated_records == 1 && s.records.back().stored == InputEvidence::bytes_per_record);
    out.str(""); InputEvidence::dump(out, s, "bounds");
    require(out.str().find("requested_hex_truncated=1 accepted_hex_truncated=1") != std::string::npos);
    for (unsigned i=0; i<300; ++i) evidence.completed_write(7+i, 0, "s", true, 1, 0);
    s = evidence.snapshot();
    require(s.completed == 307 && s.overwritten == 51 && s.records.size() == 256);
    require(s.records.front().sequence == 52 && s.records.back().sequence == 307);
    require(s.missing_since_after == 51);
    const auto incremental = evidence.snapshot(300);
    require(incremental.records.size() == 7 && incremental.missing_since_after == 0);
    require(incremental.records.front().sequence == 301);
    require(evidence.snapshot(307).records.empty());
    // Snapshot/writer concurrency has no callbacks, dangling views or torn rows.
    std::thread writer([&] { for (unsigned i=0; i<1000; ++i) evidence.completed_write(i, 0, "abc", true, 3, 0); });
    for (unsigned i=0; i<100; ++i) {
        s = evidence.snapshot();
        for (std::size_t j=1; j<s.records.size(); ++j) require(s.records[j].sequence == s.records[j-1].sequence+1);
    }
    writer.join();
    std::cout << "qualification input evidence partial-write/order/bounds/concurrency PASS\n";
}
