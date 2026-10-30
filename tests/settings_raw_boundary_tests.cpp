#include "stormtty/settings_raw_boundary.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace stormtty::qualification;
namespace {
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
const std::string payload="\x1b[200~"+std::string(settings_raw_payload)+"\x1b[201~\r";
InputEvidence::Snapshot exact() {
    InputEvidence evidence;
    evidence.completed_write(1,0,std::string_view(payload).substr(0,payload.size()-1),true,static_cast<std::uint32_t>(payload.size()-1),0);
    evidence.completed_write(2,0,"\r",true,1,0);
    return evidence.snapshot();
}
void inconclusive(const InputEvidence::Snapshot& snapshot, std::string_view child) {
    require(assess_settings_raw(snapshot,child).verdict=="INCONCLUSIVE","incomplete evidence cannot pass or diagnose mismatch");
}
}
int main() {
    const auto snapshot=exact();const auto hex=settings_raw_hex(payload);
    auto assessment=assess_settings_raw(snapshot,hex);
    require(assessment.verdict=="PASS"&&assessment.writer_complete&&assessment.child_valid,"exact boundaries pass");
    require(assessment.writer_hex==hex&&assessment.child_hex==hex&&assessment.expected_hex==hex,"exact bytes retained");
    assessment=assess_settings_raw(snapshot,std::string_view(hex).substr(2));
    require(assessment.verdict=="MISMATCH"&&assessment.writer_hex==hex&&assessment.child_hex==hex.substr(2),"missing leading ESC is strict mismatch");
    for(const std::string& invalid:std::vector<std::string>{"", "0", "gg", "AB", hex+"\n", std::string(2050,'0')}) {
        inconclusive(snapshot,invalid);require(!assess_settings_raw(snapshot,invalid).child_valid,"malformed child hex rejected");
    }
    assessment=assess_settings_raw(snapshot,std::string(2048,'0'));
    require(assessment.child_valid&&assessment.verdict=="MISMATCH","2048-byte hex bound inclusive");
    inconclusive({},hex);
    auto incomplete=snapshot;incomplete.missing_since_after=1;inconclusive(incomplete,hex);
    incomplete=snapshot;incomplete.byte_truncated_records=1;inconclusive(incomplete,hex);
    incomplete=snapshot;incomplete.records[0].success=false;inconclusive(incomplete,hex);
    incomplete=snapshot;--incomplete.records[0].accepted;inconclusive(incomplete,hex);
    incomplete=snapshot;--incomplete.records[0].stored;inconclusive(incomplete,hex);
    auto failed=std::make_unique<InputEvidence>();failed->completed_write(1,0,payload,false,static_cast<std::uint32_t>(payload.size()),995);
    assessment=assess_settings_raw(failed->snapshot(),hex);
    require(assessment.verdict=="INCONCLUSIVE"&&assessment.writer_hex.empty(),"failed reported bytes are not accepted bytes");
    auto truncated=std::make_unique<InputEvidence>();const std::string large(1025,'x');truncated->completed_write(1,0,large,true,1025,0);
    assessment=assess_settings_raw(truncated->snapshot(),hex);
    require(assessment.verdict=="INCONCLUSIVE"&&assessment.writer_hex.size()==2048,"truncated writer bytes bounded");
    auto extra=std::make_unique<InputEvidence>();extra->completed_write(1,0,payload,true,static_cast<std::uint32_t>(payload.size()),0);
    extra->completed_write(2,0,"\x1b[14;1R",true,7,0);
    assessment=assess_settings_raw(extra->snapshot(),hex);
    require(assessment.verdict=="MISMATCH"&&assessment.writer_hex==hex+"1b5b31343b3152","protocol traffic retained, not silently filtered");
    assessment=assess_settings_raw(snapshot,hex+"1b5b31343b3152");
    require(assessment.verdict=="MISMATCH"&&assessment.child_hex==hex+"1b5b31343b3152","child protocol traffic retained");
    auto wrong=std::make_unique<InputEvidence>();wrong->completed_write(1,0,std::string_view(payload).substr(1),true,static_cast<std::uint32_t>(payload.size()-1),0);
    require(assess_settings_raw(wrong->snapshot(),hex).verdict=="MISMATCH","writer mismatch cannot pass on correct child");
    require(settings_raw_hex(std::string_view("\0\xff",2))=="00ff","hex preserves unsigned bytes and NUL");
    std::cout<<"Settings raw boundary portable checks passed\n";
}
