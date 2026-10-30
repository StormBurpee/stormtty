#include "stormtty/application_layout_codec.hpp"
#include "text.hpp"
#include <locale>
#include <charconv>
#include <algorithm>
#include <sstream>

namespace stormtty {
namespace {
constexpr std::string_view header="STORMTTY-APPLICATION-LAYOUT 1";
bool finished(std::istringstream& row){row>>std::ws;return row.eof();}
template<class T> bool number(std::istringstream& row,T& value) {
    std::string token;if(!(row>>token)||token.empty()||!std::all_of(token.begin(),token.end(),[](char c){return c>='0'&&c<='9';}))return false;
    const auto parsed=std::from_chars(token.data(),token.data()+token.size(),value);
    return parsed.ec==std::errc{} && parsed.ptr==token.data()+token.size();
}
bool line(std::istream& in,std::istringstream& row,std::string_view expected) {
    std::string text,kind;
    if(!std::getline(in,text)||text.size()>256)return false;
    row.clear();row.str(text);row.imbue(std::locale::classic());
    return (row>>kind)&&kind==expected;
}
bool append(std::string& out,std::string_view bytes) {
    if(bytes.size()>ApplicationLayoutDocument::maximum_bytes-out.size())return false;
    out.append(bytes);return true;
}
bool write_payload(std::string& out,std::string_view tag,const WorkspaceLibrary& library) {
    auto bytes=serialize_workspace_library(library);if(!bytes)return false;
    return append(out,std::string(tag)+" "+std::to_string(bytes.value->size())+"\n")&&append(out,*bytes.value);
}
bool read_payload(std::istream& in,std::string_view tag,WorkspaceLibrary& library) {
    std::istringstream row;std::size_t length{};
    if(!line(in,row,tag)||!number(row,length)||!finished(row)||!length||length>ApplicationLayoutDocument::maximum_bytes)return false;
    std::string bytes(length,'\0');if(!in.read(bytes.data(),static_cast<std::streamsize>(length)))return false;
    auto parsed=deserialize_workspace_library(bytes);if(!parsed)return false;
    library=std::move(*parsed.value);return true;
}
bool write_record(std::string& out,std::string_view name,const std::optional<SequencedLayout>& record) {
    if(!record)return append(out,std::string(name)+" 0 0 0\n");
    if(!validate_application_layout(record->layout))return false;
    if(!append(out,std::string(name)+" "+std::to_string(record->sequence)+" "+
        std::to_string(record->layout.windows.size())+" "+std::to_string(record->layout.active_window)+"\n"))return false;
    for(const auto& snapshot:record->layout.windows) {
        WorkspaceLibrary library;library.last_session=snapshot;
        if(!write_payload(out,"window",library))return false;
    }
    return true;
}
bool read_record(std::istream& in,std::string_view name,std::optional<SequencedLayout>& record) {
    std::istringstream row;std::uint64_t sequence{};std::size_t count{},active{};
    if(!line(in,row,name)||(!number(row,sequence)||!number(row,count)||!number(row,active))||!finished(row)||count>ApplicationWorkspaces::maximum_windows)return false;
    if(!sequence)return count==0&&active==0;
    ApplicationLayout layout;layout.active_window=active;
    for(std::size_t i=0;i<count;++i) {
        WorkspaceLibrary library;
        if(!read_payload(in,"window",library)||!library.presets.empty()||!library.restore_on_launch)return false;
        layout.windows.push_back(std::move(library.last_session));
    }
    if(!validate_application_layout(layout))return false;
    record=SequencedLayout{sequence,std::move(layout)};return true;
}
}
Result<std::string> serialize_application_layout(const ApplicationLayoutDocument& document) {
    const auto fail=[]()->Result<std::string>{return {{},"Invalid application layout or envelope exceeds 1 MiB"};};
    // Validate cross-record sequence uniqueness and individual-close shape even
    // for imported histories; never silently repair an ambiguous resume order.
    if(!WindowRestoreHistory::from_records(document.history.last_closed(),document.history.last_quit(),document.history.last_checkpoint()))return fail();
    std::string out=std::string(header)+"\nclean "+(document.clean_shutdown?"1\n":"0\n");
    if(!write_payload(out,"library",document.library)||
       !write_record(out,"closed",document.history.last_closed())||
       !write_record(out,"quit",document.history.last_quit())||
       !write_record(out,"checkpoint",document.history.last_checkpoint())||!append(out,"end\n"))return fail();
    return {std::move(out),{}};
}
Result<DecodedApplicationLayout> deserialize_application_layout(std::string_view bytes) {
    const auto fail=[]()->Result<DecodedApplicationLayout>{return {{},"Invalid or unsupported application layout"};};
    if(bytes.size()>ApplicationLayoutDocument::maximum_bytes||!detail::valid_document_text(bytes))return fail();
    if(bytes.starts_with("STORMTTY-WORKSPACES 1\n")) {
        auto legacy=deserialize_workspace_library(bytes);if(!legacy)return fail();
        ApplicationLayoutDocument document;document.library=std::move(*legacy.value);
        if(!document.library.last_session.projects.empty() && !document.history.record_closed(document.library.last_session))return fail();
        return {DecodedApplicationLayout{std::move(document),true},{}};
    }
    std::istringstream in{std::string(bytes)},row;in.imbue(std::locale::classic());
    std::string first;if(!std::getline(in,first)||first!=header)return fail();
    unsigned clean{};if(!line(in,row,"clean")||!number(row,clean)||clean>1||!finished(row))return fail();
    ApplicationLayoutDocument document;document.clean_shutdown=clean!=0;
    std::optional<SequencedLayout> closed,quit,checkpoint;
    if(!read_payload(in,"library",document.library)||!read_record(in,"closed",closed)||
       !read_record(in,"quit",quit)||!read_record(in,"checkpoint",checkpoint))return fail();
    auto history=WindowRestoreHistory::from_records(std::move(closed),std::move(quit),std::move(checkpoint));
    if(!history)return fail();
    document.history=std::move(*history.value);
    std::string end;if(!std::getline(in,end)||end!="end"||in.peek()!=std::char_traits<char>::eof())return fail();
    return {DecodedApplicationLayout{std::move(document),false},{}};
}
} // namespace stormtty
