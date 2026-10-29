#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stormtty {
namespace agent_display_detail {
struct Token { std::string text; std::size_t columns{}; bool newline{}; };
inline std::string hex_marker(std::uint32_t value, bool byte = false) {
    constexpr char digits[]="0123456789ABCDEF";
    std::string hex;do {hex.insert(hex.begin(),digits[value&15]);value>>=4;} while(value);
    while(hex.size()<(byte?2u:4u))hex.insert(hex.begin(),'0');
    return std::string(byte?"[byte:":"[U+")+hex+"]";
}
inline bool visible_escape(std::uint32_t cp) {
    return (cp<0x20 && cp!='\n') || (cp>=0x7f&&cp<=0x9f) || cp==0xad || cp==0x61c ||
        cp==0x180e || (cp>=0x200b&&cp<=0x200f) || (cp>=0x2028&&cp<=0x202e) ||
        (cp>=0x2060&&cp<=0x206f) || cp==0xfeff || (cp>=0xfff9&&cp<=0xfffb) ||
        cp==0xe0001 || (cp>=0xe0020&&cp<=0xe007f);
}
// Decode complete Unicode scalars. Invalid UTF-8 is shown byte-for-byte instead
// of relying on platform replacement/normalization that could conceal evidence.
inline Token next(std::string_view raw, std::size_t& offset) {
    const auto start=offset;const auto lead=static_cast<unsigned char>(raw[offset]);
    std::uint32_t cp=lead;std::size_t length=1;
    if(lead>=0xc2&&lead<=0xdf){cp=lead&31;length=2;}
    else if(lead>=0xe0&&lead<=0xef){cp=lead&15;length=3;}
    else if(lead>=0xf0&&lead<=0xf4){cp=lead&7;length=4;}
    bool valid=lead<0x80||length>1;
    if(length>raw.size()-offset)valid=false;
    if(valid)for(std::size_t i=1;i<length;++i){const auto c=static_cast<unsigned char>(raw[offset+i]);if((c&0xc0)!=0x80){valid=false;break;}cp=(cp<<6)|(c&63);}
    if(valid&&((length==2&&cp<0x80)||(length==3&&cp<0x800)||(length==4&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)))valid=false;
    if(!valid){++offset;auto text=hex_marker(lead,true);return {text,text.size(),false};}
    offset+=length;
    if(visible_escape(cp)){auto text=hex_marker(cp);return {text,text.size(),false};}
    return {std::string(raw.substr(start,length)),cp=='\n'?0u:1u,cp=='\n'};
}
}
// Presentation only: never replace canonical approval scope, keys or prompts
// with this escaped representation. Escape before wrapping so bidi controls
// cannot reorder adjacent trusted labels or be split across rendered lines.
inline std::string agent_display_text(std::string_view raw, std::size_t limit=8192) {
    std::string out;std::size_t offset=0;
    while(offset<raw.size()) {
        auto token=agent_display_detail::next(raw,offset);
        if(token.text.size()>limit-std::min(limit,out.size())) {out+=" [truncated]";break;}
        out+=token.text;
    }
    return out;
}
inline std::vector<std::string> agent_display_lines(std::string_view raw,std::size_t columns) {
    columns=std::max<std::size_t>(1,columns);
    std::vector<std::string> lines;std::string line;std::size_t width=0,offset=0;
    while(offset<raw.size()) {
        auto token=agent_display_detail::next(raw,offset);
        if(token.newline){lines.push_back(std::move(line));line.clear();width=0;continue;}
        if(width&&width+token.columns>columns){lines.push_back(std::move(line));line.clear();width=0;}
        line+=token.text;width+=token.columns;
    }
    if(!line.empty())lines.push_back(std::move(line));
    return lines;
}
}
