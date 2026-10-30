#include "core_test.hpp"
#include "stormtty/application_layout_codec.hpp"
#include <limits>
#include <random>
using namespace stormtty;
namespace {
WorkspaceSnapshot saved(std::string name) {
    ProjectWorkspaceCatalog catalog;auto p=catalog.create({std::move(name),"/work","Local"});CHECK(p);
    CHECK(catalog.workspace(*p.value)->create_tab({}));
    auto snapshot=capture_workspace_snapshot(catalog);CHECK(snapshot);return *snapshot.value;
}
void roundtrip_and_legacy() {
    ApplicationLayoutDocument doc;
    auto empty_bytes=serialize_application_layout(doc);CHECK(empty_bytes);
    auto empty=deserialize_application_layout(*empty_bytes.value);CHECK(empty);
    CHECK(!empty.value->document.history.choose(RestoreEvent::cold_launch));
    doc.library.last_session=saved("Original");
    CHECK(save_workspace_preset(doc.library,"Saved tools",saved("Tools")));
    auto legacy=serialize_workspace_library(doc.library);CHECK(legacy);
    auto imported=deserialize_application_layout(*legacy.value);CHECK(imported);
    CHECK(imported.value->requires_legacy_backup);CHECK(imported.value->document.clean_shutdown);
    auto preserved=serialize_workspace_library(imported.value->document.library);CHECK(preserved);CHECK(*preserved.value==*legacy.value);
    CHECK(imported.value->document.history.choose(RestoreEvent::cold_launch)->layout.windows.size()==1);
    doc=std::move(imported.value->document);
    ApplicationLayout group{{doc.library.last_session,saved("Second window")},1};
    CHECK(doc.history.record_quit(group));CHECK(doc.history.checkpoint(group));doc.clean_shutdown=false;
    auto encoded=serialize_application_layout(doc);CHECK(encoded);
    auto decoded=deserialize_application_layout(*encoded.value);CHECK(decoded);CHECK(!decoded.value->requires_legacy_backup);
    CHECK(!decoded.value->document.clean_shutdown);
    auto again=serialize_application_layout(decoded.value->document);CHECK(again);CHECK(*again.value==*encoded.value);
    CHECK(decoded.value->document.history.choose(RestoreEvent::crash_recovery)->layout.active_window==1);
    CHECK(decoded.value->document.history.choose(RestoreEvent::cold_launch)->layout.windows.size()==2);
    CHECK(!decoded.value->document.history.choose(RestoreEvent::new_window));
    CHECK(decoded.value->document.history.record_closed(group.windows[1]));
    CHECK(decoded.value->document.history.last_closed()->sequence==4);
    auto next=serialize_application_layout(decoded.value->document);CHECK(next);CHECK(deserialize_application_layout(*next.value));
    // Parsed/imported data never mutates the caller's input, including legacy bytes.
    CHECK(*legacy.value==*preserved.value);
    doc.library.restore_on_launch=false;
    auto disabled=serialize_application_layout(doc);CHECK(disabled);
    auto disabled_read=deserialize_application_layout(*disabled.value);CHECK(disabled_read);
    CHECK(!disabled_read.value->document.library.restore_on_launch);
}
void bad_documents() {
    ApplicationLayoutDocument doc;doc.library.last_session=saved("Valid");
    CHECK(doc.history.record_closed(doc.library.last_session));
    auto encoded=serialize_application_layout(doc);CHECK(encoded);
    auto mutate=[&](std::string_view from,std::string_view to) {
        auto bytes=*encoded.value;auto at=bytes.find(from);CHECK(at!=std::string::npos);bytes.replace(at,from.size(),to);
        CHECK(!deserialize_application_layout(bytes));
    };
    mutate("STORMTTY-APPLICATION-LAYOUT 1","STORMTTY-APPLICATION-LAYOUT 2");
    mutate("clean 1","clean 2");mutate("clean 1","clean -1");
    mutate("closed 1 1 0","closed -1 1 0");mutate("closed 1 1 0","closed 1 65 0");
    mutate("closed 1 1 0","closed 0 1 0");mutate("closed 1 1 0","closed 1 1 3");
    mutate("quit 0 0 0","quit 1 0 0"); // duplicate sequence
    mutate("checkpoint 0 0 0","checkpoint 0 0 1");
    mutate("library ","library 999999999999999999999999999999999999999999");
    mutate("window ","window 0 ");
    CHECK(!deserialize_application_layout(*encoded.value+"extra\n"));
    CHECK(!deserialize_application_layout(encoded.value->substr(0,encoded.value->size()-2)));
    CHECK(!deserialize_application_layout(std::string(ApplicationLayoutDocument::maximum_bytes+1,'x')));
    CHECK(!deserialize_application_layout("STORMTTY-WORKSPACES 2\n"));
    CHECK(!deserialize_application_layout("STORMTTY-WORKSPACES 1\ninvalid\n"));
    auto bad=doc;CHECK(bad.library.last_session.projects[0].layout.bind_session(bad.library.last_session.projects[0].layout.active_pane(),{1}));
    CHECK(!serialize_application_layout(bad));
    auto snapshot=saved("Duplicate");
    ApplicationLayout duplicates{{snapshot,snapshot},0};
    CHECK(!WindowRestoreHistory::from_records({},SequencedLayout{1,duplicates},{}));
    CHECK(!WindowRestoreHistory::from_records(SequencedLayout{1,{}},{},{}));
    CHECK(!WindowRestoreHistory::from_records({},SequencedLayout{0,{}},{}));
    auto saturated=WindowRestoreHistory::from_records({},SequencedLayout{std::numeric_limits<std::uint64_t>::max(),{}},{});CHECK(saturated);
    CHECK(!saturated.value->checkpoint({}));
    std::mt19937 random(67);
    for(int i=0;i<200;++i) {
        auto bytes=*encoded.value;bytes[random()%bytes.size()]=static_cast<char>(random()%128);
        auto parsed=deserialize_application_layout(bytes);
        if(parsed){auto serialized=serialize_application_layout(parsed.value->document);CHECK(serialized);CHECK(deserialize_application_layout(*serialized.value));}
    }
}
void total_envelope_bound() {
    ApplicationLayoutDocument doc;auto snapshot=saved("Bounded");
    // Each individual library remains legal; repeated history/layout records must
    // nevertheless respect the one-envelope cap rather than a per-record cap.
    for(int i=0;i<20;++i) {
        auto next=saved("Long launch folder");next.projects[0].metadata.working_directory=std::string(32000,'a');
        snapshot.projects.push_back(std::move(next.projects[0]));
    }
    doc.library.last_session=snapshot;CHECK(serialize_workspace_library(doc.library));
    CHECK(doc.history.record_closed(snapshot));CHECK(!serialize_application_layout(doc));
}
}
int main(){return run_core_tests("application_layout_codec",[]{roundtrip_and_legacy();bad_documents();total_envelope_bound();});}
