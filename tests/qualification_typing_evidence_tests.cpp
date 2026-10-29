#include "stormtty/qualification_typing_evidence.hpp"
#include "stormtty/terminal.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
using namespace stormtty;
using E=qualification::TypingEvidence;
namespace {
void require(bool ok) { if(!ok) throw std::runtime_error("typing evidence invariant failed"); }
std::string unhex(const std::string& value) {
    require(value.size()%2==0); std::string out;
    auto digit=[](char c)->unsigned { if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("invalid trace hex"); };
    for(std::size_t i=0;i<value.size();i+=2)out+=static_cast<char>(digit(value[i])*16+digit(value[i+1]));
    return out;
}
void replay(std::istream& input) {
    TerminalOptions options; options.size={160,40,8,16}; TerminalCore core(options);
    std::string output,line; std::uint64_t fed{},applied{},ordinal{},publications{},retreats{},queued_retreats{},drainable_retreats{},writes{},input_hash=14695981039346656037ULL;
    FramePtr previous; std::uint64_t previous_hash{};
    std::map<std::uint64_t,std::pair<std::uint64_t,std::uint64_t>> enqueued;
    bool header=false,footer=false,typing=false;
    while(std::getline(input,line)) {
        const auto head=line.find("TYPING_TRACE version=");
        if(head!=std::string::npos) {require(!header);require(line.find("version=1",head)!=std::string::npos);require(line.find("overflow=0",head)!=std::string::npos);header=true;continue;}
        if(line.find("END_TYPING_TRACE")!=std::string::npos) {require(header&&!footer);footer=true;continue;}
        const auto start=line.find("TYPING_EVENT "); if(start==std::string::npos)continue;
        require(header&&!footer);std::istringstream fields(line.substr(start+13));std::map<std::string,std::string> v;std::string item;
        while(fields>>item) {const auto eq=item.find('=');require(eq!=std::string::npos);require(v.emplace(item.substr(0,eq),item.substr(eq+1)).second);}
        auto n=[&](const char* key){return std::stoull(v.at(key));};
        require(n("ordinal")==++ordinal);const auto bytes=unhex(v.at("hex"));const auto kind=v.at("kind");
        if(kind=="read") {require(n("offset")==output.size()&&n("size")==bytes.size());output+=bytes;}
        else if(kind=="write") {
            require(n("success")==1&&n("error")==0&&n("reported")<=bytes.size()&&n("size")==bytes.size());
            for(std::size_t i=0;i<n("reported");++i)input_hash=(input_hash^static_cast<unsigned char>(bytes[i]))*1099511628211ULL;
            writes+=n("reported");
        } else if(kind=="enqueue") {require(n("offset")+n("size")<=output.size());require(enqueued.emplace(n("sequence"),std::pair<std::uint64_t,std::uint64_t>{n("offset"),n("size")}).second);}
        else if(kind=="feed") {
            require(n("sequence")>applied&&n("offset")==fed&&fed+n("size")<=output.size());
            const auto found=enqueued.find(n("sequence"));require(found!=enqueued.end()&&found->second.first==fed&&found->second.second==n("size"));enqueued.erase(found);applied=n("sequence");
            core.feed(std::string_view(output).substr(fed,n("size")));fed+=n("size");(void)core.take_effects();
        } else if(kind=="command") {
            require(n("sequence")>applied);applied=n("sequence");
            // Exact authored fixture only: key/text/paste scroll to bottom;
            // mouse/focus do not mutate display. Reject unsupported commands.
            require(n("size")<=4);if(n("size")<=2)core.scroll_to_bottom();
        } else if(kind=="marker") {require(n("sequence")==1||n("sequence")==2);typing=n("sequence")==1;previous.reset();}
        else if(kind=="publication") {
            require(n("offset")==fed);
            const auto f=core.snapshot();
            require(f->cursor.row==n("row")&&f->cursor.column==n("column")&&f->cursor.visible==bool(n("visible"))&&f->alternate_screen==bool(n("alternate")));
            require(E::fingerprint(*f)==n("row_hash"));
            if(previous) {
                require(E::fingerprint(*previous)==previous_hash); // immutable owned publication
                if(typing&&previous->cursor.row==f->cursor.row&&f->cursor.column<previous->cursor.column) {++retreats;if(n("queued")) {++queued_retreats;if(!n("next_command")||n("next")<n("next_command"))++drainable_retreats;}}
            }
            previous=f;previous_hash=E::fingerprint(*f);++publications;
        } else require(false);
    }
    require(header&&footer&&publications&&fed==output.size()&&enqueued.empty());
    std::cout<<"TYPING_REPLAY code=0 input_bytes="<<writes<<" input_hash="<<input_hash<<" output_bytes="<<fed<<" publications="<<publications<<" retreats="<<retreats<<" queued_retreats="<<queued_retreats<<" drainable_retreats="<<drainable_retreats<<'\n';
}
void unit() {
    auto evidence=std::make_unique<E>(); TerminalOptions o;o.size={160,40,8,16};TerminalCore core(o);
    evidence->publication(*core.snapshot(),0,0,0,0,0);
    evidence->event(E::Kind::marker,1,0,0);
    std::uint64_t offset=0,seq=0;
    for(const auto bytes:{"ab","\b","b"}) {
        const std::string_view s(bytes);evidence->read(s);evidence->event(E::Kind::enqueue,++seq,offset,s.size());
        core.feed(s);evidence->event(E::Kind::feed,seq,offset,s.size());offset+=s.size();
        evidence->publication(*core.snapshot(),seq,offset,0,seq==2?3:0,seq==2?1:0);
    }
    evidence->write(1,0,"a",true,1,0);
    const auto frozen=evidence->snapshot();std::ostringstream text;E::dump(text,frozen);std::istringstream source(text.str());replay(source);
    evidence->event(E::Kind::command,4,0,1);require(frozen.records.size()+1==evidence->snapshot().records.size());
    for(std::size_t i=0;i<E::record_capacity;++i)evidence->event(E::Kind::command,i,0,0);
    require(evidence->snapshot().overflow);
    evidence=std::make_unique<E>();std::string huge(E::byte_capacity+1,'x');evidence->read(huge);require(evidence->snapshot().overflow&&evidence->snapshot().records.empty());
    auto bad=text.str();const auto pos=bad.find("overflow=0");bad.replace(pos,10,"overflow=1");bool rejected=false;
    try {std::istringstream invalid(bad);replay(invalid);} catch(const std::exception&) {rejected=true;}require(rejected);
    bad=text.str();const auto hash=bad.find("row_hash=");const auto hash_end=bad.find(' ',hash);
    bad.replace(hash,hash_end-hash,"row_hash=0");rejected=false;
    try {std::istringstream invalid(bad);replay(invalid);} catch(const std::exception&) {rejected=true;}require(rejected);
    bad=text.str();bad.erase(bad.find("END_TYPING_TRACE"));rejected=false;
    try {std::istringstream invalid(bad);replay(invalid);} catch(const std::exception&) {rejected=true;}require(rejected);
}
}
int main(int argc,char** argv) {
    try {
        if(argc==1) {unit();std::cout<<"PASS bounded typing trace, exact ordered replay, immutable frames and overflow rejection\n";}
        else {require(argc==3&&std::string_view(argv[1])=="--replay");std::ifstream input(argv[2]);require(bool(input));replay(input);}
        return 0;
    } catch(const std::exception& e) {std::cerr<<"TYPING_REPLAY code=1 detail="<<e.what()<<'\n';return 1;}
}
