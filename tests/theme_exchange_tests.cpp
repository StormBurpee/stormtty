#include "core_test.hpp"
#include "stormtty/theme_exchange.hpp"
#include <random>
using namespace stormtty;
int main() { return run_core_tests("theme exchange", [] {
    const Theme seed=*find_builtin_theme("daylight");
    for (const auto& theme:builtin_themes()) {
        auto expected=theme; expected.name="custom";
        auto native=export_theme(theme,ThemeFormat::stormtty); CHECK(native);
        auto parsed=import_theme(*native,ThemeFormat::stormtty,seed);
        CHECK(parsed); CHECK(*parsed.theme==expected); CHECK(parsed.warnings.empty());
        // The native palette is also a valid standalone config fragment.
        auto config=parse_config("version = 1\n"+*native,Platform::windows);CHECK(config);CHECK(config.config->theme==expected);
        auto ghostty=export_theme(theme,ThemeFormat::ghostty);CHECK(ghostty);
        parsed=import_theme(*ghostty,ThemeFormat::ghostty,seed);CHECK(parsed);
        expected.chrome=seed.chrome; expected.accent=seed.accent;
        CHECK(*parsed.theme==expected);
        CHECK(!import_theme(*native,ThemeFormat::ghostty,seed));
        CHECK(!import_theme(*ghostty,ThemeFormat::stormtty,seed));
    }
    auto imported=import_theme("\xEF\xBB\xBF# Comment\r\nforeground = aBcD01\r\n",ThemeFormat::ghostty,seed);
    CHECK(imported);CHECK(imported.theme->foreground=="#aBcD01");
    CHECK(imported.theme->background==seed.background);CHECK(imported.theme->ansi==seed.ansi);
    imported=import_theme("background=#123456\ncursor-color=#ffffff\nselection-background=112233\n",ThemeFormat::ghostty,seed);
    CHECK(imported);CHECK(imported.warnings.size()==2);CHECK(imported.warnings.front().line==2);
    const auto rejects=[&](std::string_view value,ThemeFormat format=ThemeFormat::ghostty) {
        const auto result=import_theme(value,format,seed);CHECK(!result);CHECK(!result.theme);CHECK(!result.errors.empty());
    };
    for (auto value:{"", "# comments only\n", "cursor-color=#123456", "background=#fff", "background=#12345g",
        "background=https://example.com", "background=$(touch bad)", "command=sh", "config-file=/tmp/evil",
        "font-family=anything", "background=#123456\ncommand=sh", "background=#123456\nbackground=#ffffff",
        "palette=16=#123456", "palette=256=#123456", "palette=-1=#123456", "palette=01=#123456",
        "palette=1=#123456\npalette=1=#ffffff", "palette=1", "foreground=red", "background=#123456 # comment",
        "cursor-color=not-a-color\nbackground=#123456", "[profiles]\nexecutable=sh"}) rejects(value);
    rejects(std::string("background=#123456\n")+std::string(1,'\0'));
    rejects(std::string(max_theme_bytes+1,'#'));
    rejects(std::string(1025,'#'));
    rejects("[theme]\nbackground = \"#123456\"",ThemeFormat::stormtty);
    auto complete=*export_theme(seed,ThemeFormat::stormtty);
    rejects(complete+"[theme]\n",ThemeFormat::stormtty);
    rejects(complete+"command = \"evil\"\n",ThemeFormat::stormtty);
    rejects(complete+"name = \"custom\"\n",ThemeFormat::stormtty);
    rejects(complete+"ansi16 = \"#ffffff\"\n",ThemeFormat::stormtty);
    CHECK(!import_theme(complete,static_cast<ThemeFormat>(99),seed));
    CHECK(!export_theme(seed,static_cast<ThemeFormat>(99)));
    auto invalid=seed;invalid.accent="\"\ncommand=sh";
    CHECK(!export_theme(invalid,ThemeFormat::stormtty));CHECK(!export_theme(invalid,ThemeFormat::ghostty));
    CHECK(!import_theme("background=#123456",ThemeFormat::ghostty,invalid));
    // Parsing hostile bytes cannot change caller-owned palette, even on errors.
    std::mt19937 random(41);
    for (int n=0;n<300;++n) {
        std::string bytes;for(int i=0;i<n;++i) bytes+=static_cast<char>(random()%256);
        const auto copy=seed; (void)import_theme(bytes,ThemeFormat::ghostty,seed);CHECK(seed==copy);
    }
}); }
