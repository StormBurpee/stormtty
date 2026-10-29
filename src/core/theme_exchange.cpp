#include "stormtty/theme_exchange.hpp"
#include <charconv>
#include <set>

namespace stormtty {
namespace {
std::string_view trim(std::string_view s) {
    const auto begin=s.find_first_not_of(" \t\r");
    return begin==s.npos ? std::string_view{} : s.substr(begin,s.find_last_not_of(" \t\r")-begin+1);
}
bool rgb(std::string_view value) {
    if (value.size()!=7 || value.front()!='#') return false;
    for (const auto c:value.substr(1)) if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return false;
    return true;
}
bool valid(const Theme& theme) {
    if (!rgb(theme.background)||!rgb(theme.foreground)||!rgb(theme.chrome)||!rgb(theme.accent)) return false;
    for (const auto& color:theme.ansi) if (!rgb(color)) return false;
    return true;
}
std::optional<unsigned> index(std::string_view text) {
    unsigned number{};
    const auto result=std::from_chars(text.data(),text.data()+text.size(),number);
    if (result.ec!=std::errc{} || result.ptr!=text.data()+text.size() || number>15 || text!=std::to_string(number)) return {};
    return number;
}
}
ThemeImportResult import_theme(std::string_view text, ThemeFormat format, const Theme& current) {
    ThemeImportResult result;
    const auto fail=[&](std::size_t line,std::string message) { result.errors.push_back({line,1,std::move(message)}); };
    if (text.size()>max_theme_bytes) { fail(0,"Theme exceeds 64 KiB limit"); return result; }
    if (format!=ThemeFormat::stormtty && format!=ThemeFormat::ghostty) { fail(0,"Unknown theme format"); return result; }
    if (!valid(current)) { fail(0,"Current palette contains invalid colors"); return result; }
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    Theme theme=current; theme.name="custom";
    std::set<std::string> assigned;
    bool section=false;
    std::size_t line_number=0,colors=0;
    while (!text.empty()) {
        ++line_number;
        auto end=text.find('\n');
        auto line=trim(text.substr(0,end));
        text=end==text.npos ? std::string_view{} : text.substr(end+1);
        if (line.size()>1024) { fail(line_number,"Theme line exceeds 1024 bytes"); break; }
        bool control=false;
        for (unsigned char c:line) if ((c<32 && c!='\t') || c==127) control=true;
        if (control) { fail(line_number,"Control characters are not allowed"); break; }
        if (line.empty()||line.front()=='#') continue;
        if (format==ThemeFormat::stormtty && line=="[theme]") {
            if (section) {fail(line_number,"Duplicate theme section");break;}
            section=true; continue;
        }
        if (format==ThemeFormat::stormtty && !section) {fail(line_number,"Expected [theme] section");break;}
        const auto eq=line.find('=');
        if (eq==line.npos) {fail(line_number,"Expected a color assignment");break;}
        auto key=trim(line.substr(0,eq)), value=trim(line.substr(eq+1));
        if (format==ThemeFormat::stormtty) {
            if (value.size()<2 || value.front()!='"' || value.back()!='"') {fail(line_number,"StormTTY theme values must be quoted");break;}
            value=value.substr(1,value.size()-2);
        }
        std::string identity(key);
        std::string* target=nullptr;
        bool unsupported=false;
        if (key=="background") target=&theme.background;
        else if (key=="foreground") target=&theme.foreground;
        else if (format==ThemeFormat::stormtty && key=="chrome") target=&theme.chrome;
        else if (format==ThemeFormat::stormtty && key=="accent") target=&theme.accent;
        else if (format==ThemeFormat::stormtty && key=="name") {
            if (value!="custom") {fail(line_number,"Theme exchange name must be custom");break;}
        } else if (format==ThemeFormat::stormtty && key.starts_with("ansi")) {
            auto slot=index(key.substr(4));
            if (!slot) {fail(line_number,"ANSI index must be 0 through 15");break;}
            target=&theme.ansi[*slot];
        } else if (format==ThemeFormat::ghostty && key=="palette") {
            const auto separator=value.find('=');
            auto slot=index(trim(value.substr(0,separator)));
            if (separator==value.npos || !slot) {fail(line_number,"Supported Ghostty palette indices are 0 through 15");break;}
            identity="palette"+std::to_string(*slot);
            value=trim(value.substr(separator+1)); target=&theme.ansi[*slot];
        } else if (format==ThemeFormat::ghostty && (key=="cursor-color" || key=="cursor-text" || key=="selection-background" || key=="selection-foreground")) {
            unsupported=true;
        } else {fail(line_number,"Unsupported theme directive; only documented palette colors are allowed");break;}
        if (!assigned.insert(identity).second) {fail(line_number,"Duplicate theme color or field");break;}
        if (target || unsupported) {
            std::string normalized(value);
            if (format==ThemeFormat::ghostty && normalized.size()==6) normalized="#"+normalized;
            if (!rgb(normalized)) {fail(line_number,"Expected an RGB hex color (#RRGGBB)");break;}
            if (unsupported) result.warnings.push_back({line_number,1,"Cursor/selection color is not supported by StormTTY and was not imported"});
            else { *target=std::move(normalized); ++colors; }
        }
    }
    if (result.errors.empty() && colors==0) fail(0,"Theme contains no supported colors");
    if (result.errors.empty() && format==ThemeFormat::stormtty && colors!=20) fail(0,"StormTTY theme requires background, foreground, chrome, accent and all 16 ANSI colors");
    if (result.errors.empty()) result.theme=std::move(theme);
    return result;
}
std::optional<std::string> export_theme(const Theme& theme, ThemeFormat format) {
    if (!valid(theme) || (format!=ThemeFormat::stormtty && format!=ThemeFormat::ghostty)) return {};
    std::string output=format==ThemeFormat::stormtty ? "# StormTTY palette: colors only\n[theme]\nname = \"custom\"\n" : "# Ghostty color palette exported by StormTTY\n# StormTTY chrome and accent are not part of this format.\n";
    const auto field=[&](std::string_view key,const std::string& color) {
        output+=key; output+=" = ";
        if (format==ThemeFormat::stormtty) output+='"';
        output+=color;
        if (format==ThemeFormat::stormtty) output+='"';
        output+='\n';
    };
    field("background",theme.background); field("foreground",theme.foreground);
    if (format==ThemeFormat::stormtty) {field("chrome",theme.chrome);field("accent",theme.accent);}
    for (unsigned i=0;i<16;++i) {
        if (format==ThemeFormat::stormtty) field("ansi"+std::to_string(i),theme.ansi[i]);
        else output+="palette = "+std::to_string(i)+"="+theme.ansi[i]+"\n";
    }
    return output;
}
} // namespace stormtty
