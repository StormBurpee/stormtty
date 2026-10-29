#include "stormtty/session_write_accounting.hpp"
#include "core_test.hpp"
#include <array>
using namespace stormtty;
int main(){return run_core_tests("session write accounting",[]{
    SessionWriteAccounting counts;
    const std::array origins{SessionWriteOrigin::key,SessionWriteOrigin::text,SessionWriteOrigin::paste,SessionWriteOrigin::mouse,SessionWriteOrigin::focus,SessionWriteOrigin::protocol};
    std::uint64_t total=0;
    for(std::size_t i=0;i<origins.size();++i){counts.accepted(origins[i],i+1);total+=i+1;CHECK(counts.total()==total);}
    CHECK(counts.key_bytes==1&&counts.text_bytes==2&&counts.paste_bytes==3&&counts.mouse_bytes==4&&counts.focus_bytes==5&&counts.protocol_bytes==6);
    CHECK(counts.frontend()==10&&counts.total()==21);
    // Mixed queued writes split at transport capacity retain their origin.
    // Only accepted pieces enter accounting; would-block/closed/queued pieces
    // have no call, and cannot inflate either total or a category.
    const auto before=counts;
    counts.accepted(SessionWriteOrigin::paste,2);counts.accepted(SessionWriteOrigin::paste,3);
    CHECK(counts.paste_bytes-before.paste_bytes==5&&counts.total()-before.total()==5);
    const auto blocked=counts;CHECK(counts.total()==blocked.total());
    counts.accepted(SessionWriteOrigin::focus,1);counts.accepted(SessionWriteOrigin::focus,2);
    counts.accepted(SessionWriteOrigin::protocol,2);counts.accepted(SessionWriteOrigin::protocol,7);
    CHECK(counts.frontend()-before.frontend()==5);
    CHECK(counts.total()-before.total()==(counts.frontend()-before.frontend())+(counts.focus_bytes-before.focus_bytes)+(counts.protocol_bytes-before.protocol_bytes));
    for(const auto origin:origins){const auto value=counts.total();counts.accepted(origin,0);CHECK(counts.total()==value);}
});}
