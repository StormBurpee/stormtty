#include "core_test.hpp"
#include "stormtty/config.hpp"
#include <random>

using namespace stormtty;
int main() { return run_core_tests("config", [] {
    for (const auto platform : {Platform::windows, Platform::macos}) {
        auto defaults = default_config(platform); CHECK(validate_config(defaults, platform));
        CHECK(defaults.windows_console_backend == WindowsConsoleBackend::system);
        auto bundled = defaults; bundled.windows_console_backend = WindowsConsoleBackend::bundled_preview;
        const auto bundled_text = serialize_config(bundled, platform);
        CHECK(bundled_text.find("windows_console_backend = \"bundled\"") != std::string::npos);
        auto bundled_roundtrip = parse_config(bundled_text, platform);
        CHECK(bundled_roundtrip && bundled_roundtrip.config->windows_console_backend == WindowsConsoleBackend::bundled_preview);
        CHECK(parse_config("version = 1\n[terminal]\nwindows_console_backend = \"system\"", platform));
        for (const auto invalid : {"\"auto\"", "\"bundled_preview\"", "true", "1", "\"C:\\\\build\""})
            CHECK(!parse_config(std::string("version = 1\n[terminal]\nwindows_console_backend = ")+invalid, platform));
        bundled.windows_console_backend = static_cast<WindowsConsoleBackend>(99);
        CHECK(!validate_config(bundled, platform));
        auto simple = parse_config("version = 1\n", platform); CHECK(simple); CHECK(*simple.config == defaults);
        auto roundtrip = parse_config(serialize_config(defaults, platform), platform); CHECK(roundtrip);
        CHECK(serialize_config(*roundtrip.config, platform) == serialize_config(defaults, platform));
        // Binding order is not behavior; serializing canonical registry order is deterministic.
        CHECK(roundtrip.config->theme == defaults.theme); CHECK(roundtrip.config->profiles == defaults.profiles);
        auto custom = defaults; custom.font.size_dip = 16.125; custom.appearance = {0.8, BackdropPreference::none, ReducedMotionPreference::enabled}; custom.profiles[0].arguments = {"a b", "x\"y", "C:\\src", "a\nb"};
        custom.profiles[0].environment["EXAMPLE"] = "x\\y\t#literal"; custom.bindings.clear();
        auto parsed = parse_config(serialize_config(custom, platform), platform); CHECK(parsed); CHECK(*parsed.config == custom);
    }
    CHECK(builtin_themes().size()==3);CHECK(!find_builtin_theme("not-a-theme"));
    for (const auto& theme:builtin_themes()) {
        const auto parsed=parse_config("version = 1\n[theme]\nname = \""+theme.name+"\"",Platform::windows);
        CHECK(parsed && parsed.config->theme==theme);CHECK(find_builtin_theme(theme.name)==&theme);
    }
    const auto graphite = default_config(Platform::windows);
    CHECK(graphite.theme.name == "graphite" && graphite.theme.background == "#11151B");
    CHECK(graphite.appearance.background_opacity == 0.94 && graphite.appearance.backdrop == BackdropPreference::acrylic);
    CHECK(graphite.font.size_dip == 15);
    for (const auto backdrop : {BackdropPreference::automatic,BackdropPreference::none,BackdropPreference::mica,BackdropPreference::acrylic}) {
        auto config = graphite; config.appearance.backdrop = backdrop;
        auto roundtrip = parse_config(serialize_config(config,Platform::windows),Platform::windows);
        CHECK(roundtrip); CHECK(roundtrip.config->appearance.backdrop == backdrop);
    }
    auto instrument = parse_config("version = 1\n[theme]\nname = \"instrument\"",Platform::windows);
    CHECK(instrument && instrument.config->theme.background == "#17191D");
    CHECK(parse_config("version = 1\n[appearance]\nbackground_opacity = 0.7\nbackdrop = \"none\"\nreduced_motion = \"disabled\"", Platform::windows));
    for (const auto platform:{Platform::windows,Platform::macos}) for (const double opacity:{0.0,0.01,0.25,0.5,0.69,0.7,1.0}) {
        auto config=default_config(platform);config.appearance.background_opacity=opacity;
        CHECK(validate_config(config,platform));
        auto roundtrip=parse_config(serialize_config(config,platform),platform);
        CHECK(roundtrip && roundtrip.config->appearance.background_opacity==opacity);
    }
    auto invalid_opacity=graphite;
    for (const double opacity:{-0.01,1.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        invalid_opacity.appearance.background_opacity=opacity;CHECK(!validate_config(invalid_opacity,Platform::windows));
    }
    for (const auto* bad : {"background_opacity = -0.01", "background_opacity = 1.01", "background_opacity = nan", "backdrop = \"acrylic-magic\"", "reduced_motion = \"sometimes\""}) {
        auto current = default_config(Platform::windows); const auto old = current;
        CHECK(!reload_config(current, std::string("version = 1\n[appearance]\n") + bad, Platform::windows)); CHECK(current == old);
    }
    const auto good = R"(version = 1
# Exact color # comments and quoted arguments are data, never shell expressions.
default_profile = "work"
[theme]
name = "daylight"
accent = "#112233"
[font]
family = "Example Mono"
size_dip = 13.5
ligatures = true
[terminal]
scrollback_lines = 12345
[profile.work]
label = "Development"
executable = "C:\\Tools\\pwsh.exe"
arguments = ["-NoLogo", "--", "literal;not-executed"]
working_directory = "C:\\source"
[profile.work.environment]
APP_ENV = "dev # literal"
[bindings]
pane.split_right = "Primary+Shift+D"
edit.copy = []
[bindings.macos]
pane.split_right = "Cmd+Shift+E"
)";
    auto parsed = parse_config(good, Platform::windows); CHECK(parsed);
    CHECK(parsed.config->theme.background == "#FAFBFC"); CHECK(parsed.config->theme.accent == "#112233");
    CHECK(parsed.config->font.size_dip == 13.5); CHECK(parsed.config->profiles.size() == 2);
    CHECK(parsed.config->profiles[1].arguments[2] == "literal;not-executed");
    CHECK(parsed.config->profiles[1].environment.at("APP_ENV") == "dev # literal");
    CHECK(parsed.config->scrollback_lines == 12345);
    const auto serialized = serialize_config(*parsed.config, Platform::windows);
    auto again = parse_config(serialized, Platform::windows); CHECK(again); CHECK(serialize_config(*again.config, Platform::windows) == serialized);
    for (const auto* bad : {
        "", "version = 2", "version = 1\nversion = 1", "version = 1\ninclude = \"secret\"", "version = 1\n[unknown]\nx = 2",
        "version = 1\n[font]\nsize_dip = nan", "version = 1\n[font]\nsize_dip = 100", "version = 1\n[font]\nligatures = 1",
        "version = 1\n[theme]\nbackground = \"red\"", "version = 1\n[terminal]\nscrollback_lines = 1.5", "version = 1\ndefault_profile = \"missing\"",
        "version = 1\n[profile.bad]\nexecutable = \"\"", "version = 1\n[profile.a.b]\nexecutable = \"sh\"",
        "version = 1\n[profile.default]\narguments = [\"one\",]", "version = 1\n[profile.default]\narguments = [\"a\", 2]",
        "version = 1\n[font]\nfamily = \"unterminated", "version = 1\n[font]\nfamily = \"bad\\x\"", "version = 1\n[font]\nfamily = \"ok\" trailing",
        "version = 1\n[bindings]\nno.such.command = \"Ctrl+Shift+P\"", "version = 1\n[bindings]\nedit.copy = \"Ctrl+C\"",
        "version = 1\n[bindings]\nedit.copy = \"Ctrl+Shift+P\"", "version = 1\n[bindings]\nedit.copy = \"Alt+F4\"",
        "version = 1\n[profile.default.environment]\n1BAD = \"x\"", "version = 1\n[theme]\nname = \"unknown\""
    }) {
        auto invalid = parse_config(bad, Platform::windows); CHECK(!invalid); CHECK(!invalid.config); CHECK(!invalid.errors.empty());
    }
    CHECK(!parse_config(std::string(1024 * 1024 + 1, 'a'), Platform::windows));
    CHECK(!parse_config("version = 1\n" + std::string(16385, 'a'), Platform::windows));
    CHECK(!parse_config(std::string("version = 1\n\0garbage", 20), Platform::windows));
    CHECK(!parse_config("version = 1\n#\xc0\xaf", Platform::windows));
    CHECK(!parse_config("version = 1\n[bad\x1b[31m]", Platform::windows));
    CHECK(parse_config("\xef\xbb\xbfversion = 1\r\n", Platform::windows));
    CHECK(!load_config("/path/that/does/not/exist/stormtty.config", Platform::windows));
    auto active = default_config(Platform::windows); const auto before = active;
    CHECK(!reload_config(active, "version = 2", Platform::windows)); CHECK(active == before);
    CHECK(reload_config(active, good, Platform::windows)); CHECK(active.font.size_dip == 13.5);
    CHECK(parse_config("version = 1\nallow_terminal_shortcut_override = true\n[bindings]\nedit.copy = \"Ctrl+C\"", Platform::windows));
    CHECK(!parse_config("version = 1\nallow_terminal_shortcut_override = true\n[bindings]\nedit.copy = \"Alt+F4\"", Platform::windows));
    CHECK(!config_format_documentation().empty());
    auto oversized_line = default_config(Platform::windows);
    oversized_line.profiles[0].arguments = {std::string(10000, 'x'), std::string(10000, 'y')};
    CHECK(!validate_config(oversized_line, Platform::windows));
    auto oversized_encoding = default_config(Platform::windows);
    oversized_encoding.profiles[0].environment["NEWLINES"] = std::string(10000, '\n');
    CHECK(!validate_config(oversized_encoding, Platform::windows));
    // Seeded hostile-input mutations: every accepted result must validate and
    // round-trip; failed reloads must retain the exact old value object.
    std::mt19937 rng(0xC0F19); const auto seed = serialize_config(default_config(Platform::windows), Platform::windows);
    for (unsigned iteration = 0; iteration < 2500; ++iteration) {
        std::string bytes = seed;
        for (unsigned n = 0, mutations = rng() % 8 + 1; n < mutations; ++n) {
            auto pos = rng() % (bytes.size() + 1);
            if (rng() % 3 == 0) bytes.insert(pos, 1, static_cast<char>(rng() % 256));
            else if (pos < bytes.size() && rng() % 2) bytes.erase(pos, 1);
            else if (pos < bytes.size()) bytes[pos] = static_cast<char>(rng() % 256);
        }
        auto old = active; auto attempt = reload_config(active, bytes, Platform::windows);
        if (!attempt) CHECK(active == old);
        else { CHECK(validate_config(active, Platform::windows)); auto rt = parse_config(serialize_config(active, Platform::windows), Platform::windows); CHECK(rt); }
    }
}); }
