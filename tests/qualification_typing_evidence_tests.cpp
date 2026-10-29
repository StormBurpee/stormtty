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
    bool header=false,footer=false,typing=false,armed=false,frozen=false;
    std::uint64_t expected_records{},expected_bytes{},stored_bytes{},prelude_bytes{},last_enqueued{},fed_sequence{},command_sequence{},last_wake{};
    while(std::getline(input,line)) {
        const auto head=line.find("TYPING_TRACE version=");
        if(head!=std::string::npos) {require(!header);std::istringstream h(line.substr(head+13));std::map<std::string,std::string> values;std::string f;
            while(h>>f){auto e=f.find('=');require(e!=std::string::npos);require(values.emplace(f.substr(0,e),f.substr(e+1)).second);}
            require(values.size()==5&&values.at("version")=="2"&&values.at("overflow")=="0"&&values.at("phase")=="2");
            for(const auto key:{"records","bytes"})require(!values.at(key).empty()&&values.at(key).find_first_not_of("0123456789")==std::string::npos);
            expected_records=std::stoull(values.at("records"));expected_bytes=std::stoull(values.at("bytes"));
            require(expected_records<=E::record_capacity&&expected_bytes<=E::byte_capacity);header=true;continue;}
        if(line.find("END_TYPING_TRACE")!=std::string::npos) {require(header&&!footer);footer=true;continue;}
        const auto start=line.find("TYPING_EVENT "); if(start==std::string::npos)continue;
        require(header&&!footer&&!frozen);std::istringstream fields(line.substr(start+13));std::map<std::string,std::string> v;std::string item;
        while(fields>>item) {const auto eq=item.find('=');require(eq!=std::string::npos);require(v.emplace(item.substr(0,eq),item.substr(eq+1)).second);}
        auto n=[&](const char* key){std::size_t used{};const auto& value=v.at(key);require(!value.empty()&&value.find_first_not_of("0123456789")==std::string::npos);auto result=std::stoull(value,&used);require(used==value.size());return result;};
        require(v.size()==20);
        for(const auto key:{"ordinal","us","sequence","offset","size","auxiliary","next","next_command","queued","generation","row","column","row_hash","visible","alternate","success","reported","error"})(void)n(key);
        require(n("visible")<=1&&n("alternate")<=1&&n("success")<=1);
        require(n("ordinal")==++ordinal);const auto bytes=unhex(v.at("hex"));const auto kind=v.at("kind");stored_bytes+=bytes.size();require(stored_bytes<=E::byte_capacity);
        if(kind!="read"&&kind!="write")require(bytes.empty());
        if(kind=="read") {require(n("offset")==output.size()&&n("size")==bytes.size());output+=bytes;}
        else if(kind=="write") {
            require(typing);
            require(n("success")==1&&n("error")==0&&n("reported")<=bytes.size()&&n("size")==bytes.size());
            for(std::size_t i=0;i<n("reported");++i)input_hash=(input_hash^static_cast<unsigned char>(bytes[i]))*1099511628211ULL;
            writes+=n("reported");
        } else if(kind=="enqueue") {require(typing&&n("sequence")>last_enqueued);last_enqueued=n("sequence");require(n("offset")<=output.size()&&n("size")<=output.size()-n("offset"));require(enqueued.emplace(n("sequence"),std::pair<std::uint64_t,std::uint64_t>{n("offset"),n("size")}).second);}
        else if(kind=="feed"||kind=="prelude_feed") {
            require((kind=="feed")==typing);
            require(n("sequence")>applied&&n("offset")==fed&&fed<=output.size()&&n("size")<=output.size()-fed);
            const auto found=enqueued.find(n("sequence"));
            if(typing) {
                // A read/enqueue may precede arm while its actor feed follows it.
                if(found==enqueued.end())require(fed<=prelude_bytes&&n("size")<=prelude_bytes-fed);
                else {require(found->second.first==fed&&found->second.second==n("size"));enqueued.erase(found);}
            }
            applied=n("sequence");fed_sequence=applied;
            core.feed(std::string_view(output).substr(fed,n("size")));fed+=n("size");(void)core.take_effects();
        } else if(kind=="command") {
            require(n("sequence")>applied);applied=n("sequence");command_sequence=applied;
            // Exact authored fixture only: key/text/paste scroll to bottom;
            // mouse/focus do not mutate display. Reject unsupported commands.
            require(n("size")<=4);if(n("size")<=2)core.scroll_to_bottom();
        } else if(kind=="marker") {
            require(n("sequence")>applied&&n("offset")==fed&&n("auxiliary")==output.size());applied=n("sequence");
            if(n("size")==1){require(!armed);armed=true;typing=true;prelude_bytes=output.size();}
            else {require(n("size")==2&&armed&&typing);typing=false;frozen=true;}
            previous.reset();
        }
        else if(kind=="wake") {
            require(typing&&n("sequence")>last_wake&&n("reported")<=1);
            require(n("size")<=n("auxiliary")&&n("auxiliary")<=n("next")&&n("next")<=n("us"));
            if(n("reported")==1)require(n("auxiliary")>=n("offset"));
            last_wake=n("sequence");
        }
        else if(kind=="publication") {
            require(typing&&n("offset")==fed&&n("sequence")==fed_sequence&&n("auxiliary")==command_sequence);
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
    require(header&&footer&&armed&&frozen&&publications&&ordinal==expected_records&&stored_bytes==expected_bytes);
    for(const auto& [sequence,range]:enqueued)require(sequence>applied&&range.first>=fed);
    std::cout<<"TYPING_REPLAY code=0 input_bytes="<<writes<<" input_hash="<<input_hash<<" output_bytes="<<fed<<" publications="<<publications<<" retreats="<<retreats<<" queued_retreats="<<queued_retreats<<" drainable_retreats="<<drainable_retreats<<'\n';
}
void unit() {
    auto evidence=std::make_unique<E>(); TerminalOptions o;o.size={160,40,8,16};TerminalCore core(o);
    std::uint64_t offset=0,seq=0;
    // Startup far beyond the old 2048-record cap remains exact and compact.
    for(int i=0;i<3000;++i){evidence->read("a");evidence->event(E::Kind::enqueue,++seq,offset,1);core.feed("a");evidence->event(E::Kind::feed,seq,offset++,1);evidence->publication(*core.snapshot(),seq,offset,0,0,0);}
    require(evidence->snapshot().records.size()==2&&!evidence->snapshot().overflow);
    evidence->event(E::Kind::command,++seq,0,1);core.scroll_to_bottom();
    evidence->phase(true,++seq,offset);
    evidence->publication(*core.snapshot(),seq-2,offset,seq-1,0,0);
    for(const auto bytes:{"ab","\b","b"}) {
        const std::string_view s(bytes);evidence->read(s);evidence->event(E::Kind::enqueue,++seq,offset,s.size());
        core.feed(s);evidence->event(E::Kind::feed,seq,offset,s.size());offset+=s.size();
        evidence->publication(*core.snapshot(),seq,offset,3001,0,0);
    }
    evidence->write(1,0,"a",true,1,0);
    const auto now=std::chrono::steady_clock::now();evidence->wake(1,now,now,now,now,1,false,true);
    evidence->phase(false,++seq,offset);
    const auto frozen=evidence->snapshot();std::ostringstream text;E::dump(text,frozen);std::istringstream source(text.str());replay(source);
    evidence->read("cleanup");evidence->write(2,0,"exit",true,4,0);evidence->event(E::Kind::command,++seq,0,1);
    require(frozen.records.size()==evidence->snapshot().records.size()&&frozen.bytes==evidence->snapshot().bytes);
    evidence=std::make_unique<E>();evidence->phase(true,1,0);
    for(std::size_t i=0;i<E::record_capacity;++i)evidence->event(E::Kind::command,i+2,0,0);
    require(evidence->snapshot().overflow);
    evidence=std::make_unique<E>();std::string huge(E::byte_capacity+1,'x');evidence->read(huge);require(evidence->snapshot().overflow&&evidence->snapshot().records.empty());
    auto bad=text.str();const auto pos=bad.find("overflow=0");bad.replace(pos,10,"overflow=1");bool rejected=false;
    try {std::istringstream invalid(bad);replay(invalid);} catch(const std::exception&) {rejected=true;}require(rejected);
    bad=text.str();const auto hash=bad.find("row_hash=",bad.find("kind=publication"));const auto hash_end=bad.find(' ',hash);
    bad.replace(hash,hash_end-hash,"row_hash=0");rejected=false;
    try {std::istringstream invalid(bad);replay(invalid);} catch(const std::exception&) {rejected=true;}require(rejected);
    bad=text.str();bad.erase(bad.find("END_TYPING_TRACE"));rejected=false;
    try {std::istringstream invalid(bad);replay(invalid);} catch(const std::exception&) {rejected=true;}require(rejected);
    auto reject=[&](std::string invalid){bool caught=false;try{std::istringstream in(invalid);replay(in);}catch(const std::exception&){caught=true;}require(caught);};
    auto replace=[&](std::string from,std::string to){auto invalid=text.str();auto at=invalid.find(from);require(at!=std::string::npos);invalid.replace(at,from.size(),to);reject(invalid);};
    replace("version=2","version=1");replace("phase=2","phase=1");
    replace("ordinal=1 ","ordinal=2 ");replace("ordinal=1 ","ordinal=1x ");
    replace("kind=prelude_feed","kind=feed");replace("size=3000 ","size=3001 ");
    replace("size=1 auxiliary=3000","size=2 auxiliary=3000");
    {
        auto invalid=text.str();auto begin=invalid.find("records=")+8;auto end=invalid.find(' ',begin);
        invalid.replace(begin,end-begin,"0");reject(invalid);
        invalid=text.str();begin=invalid.find("bytes=")+6;end=invalid.find('\n',begin);
        invalid.replace(begin,end-begin,"0");reject(invalid);
    }
    // Coalescing is not a way around the total byte cap.
    evidence=std::make_unique<E>();evidence->read(std::string(E::byte_capacity,'x'));evidence->read("x");
    require(evidence->snapshot().overflow&&evidence->snapshot().bytes.size()==E::byte_capacity);
    // A duplicate arm/freeze fails closed, including after normal freeze.
    evidence=std::make_unique<E>();evidence->phase(false,1,0);require(evidence->snapshot().overflow);
    evidence=std::make_unique<E>();evidence->phase(true,1,0);evidence->phase(true,2,0);require(evidence->snapshot().overflow);
    evidence=std::make_unique<E>();evidence->phase(true,1,0);evidence->phase(false,2,0);evidence->phase(false,3,0);require(evidence->snapshot().overflow);
    // Prelude commands prevent feed coalescing across an actor operation.
    evidence=std::make_unique<E>();TerminalCore ordered(o);
    evidence->read("a");ordered.feed("a");evidence->event(E::Kind::feed,1,0,1);
    evidence->event(E::Kind::command,2,0,1);ordered.scroll_to_bottom();
    evidence->read("b");ordered.feed("b");evidence->event(E::Kind::feed,3,1,1);
    require(evidence->snapshot().records.size()==4);
    evidence->phase(true,4,2);evidence->publication(*ordered.snapshot(),3,2,2,0,0);evidence->phase(false,5,2);
    std::ostringstream ordered_log;E::dump(ordered_log,evidence->snapshot());std::istringstream ordered_in(ordered_log.str());replay(ordered_in);
    // Read crossing arm and queued output crossing freeze are permitted, not fabricated as feeds.
    evidence=std::make_unique<E>();TerminalCore boundary(o);evidence->read("ab");evidence->event(E::Kind::enqueue,2,0,1);evidence->phase(true,1,0);
    boundary.feed("a");evidence->event(E::Kind::feed,2,0,1);evidence->publication(*boundary.snapshot(),2,1,0,3,1);
    evidence->event(E::Kind::enqueue,3,1,1);evidence->phase(false,4,1);
    // Actor ordering forbids sequence 3 still queued behind freeze 4: replay rejects it.
    std::ostringstream invalid;E::dump(invalid,evidence->snapshot());reject(invalid.str());
    evidence=std::make_unique<E>();evidence->read("ab");evidence->event(E::Kind::enqueue,2,0,1);evidence->phase(true,1,0);
    evidence->event(E::Kind::feed,2,0,1);evidence->publication(*boundary.snapshot(),2,1,0,4,1);
    evidence->event(E::Kind::enqueue,4,1,1);evidence->phase(false,3,1);
    std::ostringstream crossing;E::dump(crossing,evidence->snapshot());std::istringstream crossing_in(crossing.str());replay(crossing_in);

}
}
int main(int argc,char** argv) {
    try {
        if(argc==1) {unit();std::cout<<"PASS bounded typing trace, exact ordered replay, immutable frames and overflow rejection\n";}
        else {require(argc==3&&std::string_view(argv[1])=="--replay");std::ifstream input(argv[2]);require(bool(input));replay(input);}
        return 0;
    } catch(const std::exception& e) {std::cerr<<"TYPING_REPLAY code=1 detail="<<e.what()<<'\n';return 1;}
}
