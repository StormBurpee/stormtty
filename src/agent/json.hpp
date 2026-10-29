#pragma once
// Small bounded JSON codec for the app-server wire contract. No coercions,
// duplicate keys, invalid UTF-8, unpaired surrogates or non-finite numbers.
#include "../core/text.hpp"
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace stormtty::agent_json {
struct Value {
    enum class Type { null, boolean, number, string, array, object } type{Type::null};
    std::string scalar;
    std::vector<Value> array;
    std::map<std::string, Value, std::less<>> object;
    const Value& at(std::string_view key) const { static const Value missing; auto i = object.find(key); return i == object.end() ? missing : i->second; }
    bool has(std::string_view key) const { return object.contains(key); }
    std::string text() const { return type == Type::string ? scalar : std::string{}; }
};
inline std::string quote(std::string_view s) {
    if (!detail::valid_utf8(s)) throw std::runtime_error("Invalid UTF-8");
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
inline std::string dump(const Value& v) {
    if (v.type == Value::Type::string) return quote(v.scalar);
    if (v.type == Value::Type::null) return "null";
    if (v.type == Value::Type::boolean || v.type == Value::Type::number) return v.scalar;
    std::string out; bool first = true;
    if (v.type == Value::Type::array) {
        out = "["; for (const auto& item : v.array) { if (!first) out += ','; first = false; out += dump(item); } return out + ']';
    }
    out = "{"; for (const auto& [key,item] : v.object) { if (!first) out += ','; first = false; out += quote(key) + ':' + dump(item); } return out + '}';
}
class Parser {
    std::string_view s; std::size_t p{}, nodes{};
    [[noreturn]] void fail() const { throw std::runtime_error("Malformed or excessive JSON"); }
    void ws() { while (p < s.size() && (s[p]==' ' || s[p]=='\r' || s[p]=='\n' || s[p]=='\t')) ++p; }
    bool take(char c) { ws(); if (p < s.size() && s[p] == c) { ++p; return true; } return false; }
    unsigned hex4() {
        unsigned n{}; for (int i=0;i<4;++i) { if(p==s.size()) fail(); char c=s[p++]; n <<= 4; if(c>='0'&&c<='9') n+=c-'0'; else if(c>='a'&&c<='f') n+=c-'a'+10; else if(c>='A'&&c<='F') n+=c-'A'+10; else fail(); } return n;
    }
    static void utf8(std::string& out, unsigned c) {
        if(c<0x80) out+=static_cast<char>(c);
        else if(c<0x800) { out+=static_cast<char>(0xc0|(c>>6)); out+=static_cast<char>(0x80|(c&63)); }
        else if(c<0x10000) { out+=static_cast<char>(0xe0|(c>>12)); out+=static_cast<char>(0x80|((c>>6)&63)); out+=static_cast<char>(0x80|(c&63)); }
        else { out+=static_cast<char>(0xf0|(c>>18)); out+=static_cast<char>(0x80|((c>>12)&63)); out+=static_cast<char>(0x80|((c>>6)&63)); out+=static_cast<char>(0x80|(c&63)); }
    }
    std::string string() {
        if(!take('"')) fail();
        std::string out;
        while(p<s.size()) { unsigned char c=s[p++]; if(c=='"') return out; if(c<0x20) fail();
            if(c!='\\') { out+=static_cast<char>(c); continue; }
            if(p==s.size()) fail();
            char e=s[p++];
            switch(e) { case '"': case '\\': case '/': out+=e; break; case 'b': out+='\b'; break; case 'f': out+='\f'; break; case 'n': out+='\n'; break; case 'r': out+='\r'; break; case 't': out+='\t'; break;
            case 'u': { unsigned cp=hex4(); if(cp>=0xd800&&cp<=0xdbff) { if(p+2>s.size()||s.substr(p,2)!="\\u") fail(); p+=2; unsigned low=hex4(); if(low<0xdc00||low>0xdfff) fail(); cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00); } else if(cp>=0xdc00&&cp<=0xdfff) fail(); utf8(out,cp); break; }
            default: fail(); }
        } fail();
    }
    Value value(unsigned depth) {
        if(depth>32 || ++nodes>32768) fail();
        ws(); if(p==s.size()) fail(); Value v;
        if(s[p]=='"') { v.type=Value::Type::string; v.scalar=string(); return v; }
        if(take('{')) { v.type=Value::Type::object; if(take('}')) return v; do { auto key=string(); if(!take(':')) fail(); auto item=value(depth+1); if(!v.object.emplace(std::move(key),std::move(item)).second) fail(); } while(take(',')); if(!take('}')) fail(); return v; }
        if(take('[')) { v.type=Value::Type::array; if(take(']')) return v; do { v.array.push_back(value(depth+1)); } while(take(',')); if(!take(']')) fail(); return v; }
        for(auto literal : {"null", "true", "false"}) if(s.substr(p,std::char_traits<char>::length(literal))==literal) { p+=std::char_traits<char>::length(literal); if(literal[0]!='n') { v.type=Value::Type::boolean; v.scalar=literal; } return v; }
        auto start=p; if(s[p]=='-') ++p; if(p==s.size()) fail();
        if(s[p]=='0') ++p; else { if(s[p]<'1'||s[p]>'9') fail(); while(p<s.size()&&s[p]>='0'&&s[p]<='9') ++p; }
        if(p<s.size()&&s[p]=='.') { ++p; auto digits=p; while(p<s.size()&&s[p]>='0'&&s[p]<='9') ++p; if(p==digits) fail(); }
        if(p<s.size()&&(s[p]=='e'||s[p]=='E')) { ++p; if(p<s.size()&&(s[p]=='+'||s[p]=='-')) ++p; auto digits=p; while(p<s.size()&&s[p]>='0'&&s[p]<='9') ++p; if(p==digits) fail(); }
        v.type=Value::Type::number; v.scalar=std::string(s.substr(start,p-start)); return v;
    }
public:
    explicit Parser(std::string_view input):s(input) {}
    Value parse() { if(s.size()>1024*1024 || !detail::valid_utf8(s)) fail(); auto v=value(0); ws(); if(p!=s.size()) fail(); return v; }
};
inline Value parse(std::string_view s) { return Parser(s).parse(); }
inline std::string id(const Value& v) {
    if(v.type==Value::Type::string && !v.scalar.empty() && v.scalar.size()<=256) return quote(v.scalar);
    if(v.type==Value::Type::number && v.scalar.size()<=20 && v.scalar.find_first_not_of("0123456789")==std::string::npos) return v.scalar;
    throw std::runtime_error("Invalid request identity");
}
} // namespace stormtty::agent_json
