#include "stormtty/config.hpp"
#include "text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <variant>
#include <type_traits>

namespace stormtty {
namespace {
constexpr std::size_t max_bytes = 1024 * 1024;
using Value = std::variant<std::string, double, bool, std::vector<std::string>>;
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
bool identifier(std::string_view s, std::size_t max = 128) {
    return !s.empty() && s.size() <= max && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
bool plain_text(std::string_view s, std::size_t max = 4096) {
    return s.size() <= max && detail::valid_utf8(s) && std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
bool color(std::string_view s) {
    return s.size() == 7 && s[0] == '#' && std::all_of(s.begin() + 1, s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
// A deliberately small declarative grammar, not a permissive TOML interpreter.
struct ValueReader {
    std::string_view s; std::size_t position{}; std::string error;
    void space() { while (position < s.size() && (s[position] == ' ' || s[position] == '\t' || s[position] == '\r')) ++position; }
    std::optional<std::string> string() {
        space(); if (position >= s.size() || s[position++] != '"') { error = "Expected a double-quoted string"; return {}; }
        std::string result;
        while (position < s.size()) {
            auto c = s[position++];
            if (c == '"') return result;
            if (c == '\\') {
                if (position == s.size()) break;
                c = s[position++];
                if (c == 'n') result += '\n'; else if (c == 'r') result += '\r'; else if (c == 't') result += '\t';
                else if (c == '\\' || c == '"') result += c;
                else { error = "Unknown string escape (allowed: \\\\, \\\", \\n, \\r, \\t)"; return {}; }
            } else {
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f) { error = "Unescaped control character in string"; return {}; }
                result += c;
            }
        }
        error = "Unterminated quoted string"; return {};
    }
    std::optional<Value> read() {
        space(); if (position == s.size() || s[position] == '#') { error = "Missing value"; return {}; }
        if (s[position] == '"') { auto v = string(); if (!v) return {}; return Value{std::move(*v)}; }
        if (s[position] == '[') {
            ++position; space(); std::vector<std::string> result;
            if (position < s.size() && s[position] == ']') { ++position; return Value{std::move(result)}; }
            while (position < s.size()) {
                auto v = string(); if (!v) return {};
                result.push_back(std::move(*v)); if (result.size() > 256) { error = "Array has more than 256 entries"; return {}; }
                space(); if (position == s.size()) break;
                if (s[position] == ']') { ++position; return Value{std::move(result)}; }
                if (s[position++] != ',') { error = "Expected comma or closing bracket"; return {}; }
            }
            error = "Unterminated string array"; return {};
        }
        auto end = s.find_first_of(" \t\r#", position); if (end == std::string_view::npos) end = s.size();
        const auto token = s.substr(position, end - position); position = end;
        if (token == "true") return Value{true};
        if (token == "false") return Value{false};
        double number{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() && std::isfinite(number)) return Value{number};
        error = "Expected quoted string, finite number, boolean or string array"; return {};
    }
};
std::string quote(std::string_view text) {
    std::string result{"\""};
    for (const char c : text) {
        if (c == '\\' || c == '"') { result += '\\'; result += c; }
        else if (c == '\n') result += "\\n"; else if (c == '\r') result += "\\r"; else if (c == '\t') result += "\\t";
        else result += c;
    }
    result += '"'; return result;
}

}
std::span<const Theme> builtin_themes() {
    static const std::array themes{Theme{},
        Theme{"instrument", "#17191D", "#ECEEF2", "#22252B", "#A7C4D2"},
        Theme{"daylight", "#FAFBFC", "#232B33", "#EDF0F3", "#315E78", {"#232B33","#A82432","#326D35","#805B15","#245EB0","#7943A8","#19717A","#626B75","#687580","#B72E40","#397D43","#906419","#306DC0","#8D50B8","#237D87","#151D25"}},
        Theme{"midnight", "#0C1425", "#DCE5F4", "#131E34", "#88B4F8", {"#101B30","#EF8597","#90CBA9","#E8C58B","#89B4FA","#C7A0E8","#83D6E8","#DCE5F4","#60718E","#FFA0AC","#ABE3BC","#F7DBA6","#ACCCFF","#DEBEF7","#A7ECF5","#F5F8FF"}},
        Theme{"evergreen", "#121B18", "#DFE8DF", "#1B2923", "#9CC9A1", {"#17231D","#D98C85","#A8C698","#D9C28B","#8CBAC2","#BEA4C9","#8BC7B1","#DFE8DF","#668273","#EEA69A","#BEDCAD","#EDD7A5","#A6D0D4","#D3BCDE","#A6DFCB","#F2F7EF"}},
        Theme{"aubergine", "#1C1521", "#ECE0EA", "#2A2031", "#D3A5CE", {"#251B2B","#E58EAB","#A8C4A0","#DEC394","#A8B6E3","#D3A5CE","#98CBCD","#ECE0EA","#806B87","#F6A7BF","#C0D9B5","#F1D8AF","#C1CDF5","#E6BFE3","#B3E0E0","#FFF1FB"}},
        Theme{"ember", "#211915", "#EDE1D4", "#30241E", "#E7AE7E", {"#2B211B","#E69580","#BCC08B","#E3BA79","#97B6CB","#CDA6BB","#9CC8BD","#EDE1D4","#887366","#F3AF98","#D2D6A4","#F5D398","#B4CCDC","#E0C0D0","#B7DED2","#FFF4E6"}},
        Theme{"ink", "#101010", "#E4E4E4", "#1D1D1D", "#BFC7D5", {"#202020","#D88B8B","#A5BF9F","#CFBD91","#A0B7D4","#BCA6CF","#9DBFC5","#D8D8D8","#777777","#ECA5A5","#BFD6B8","#E6D5AE","#BACDE5","#D3BEE3","#B6D7DD","#F5F5F5"}}};
    return themes;
}
const Theme* find_builtin_theme(std::string_view name) {
    for (const auto& theme:builtin_themes()) if (theme.name==name) return &theme;
    return nullptr;
}
bool theme_is_light(const Theme& theme) noexcept {
    if (!color(theme.background)) return false;
    const auto channel=[&](std::size_t i) {unsigned v{}; std::from_chars(theme.background.data()+i,theme.background.data()+i+2,v,16); return v;};
    return channel(1)*299+channel(3)*587+channel(5)*114>128000;
}
Config default_config(Platform platform) {
    Config c;
    if (platform == Platform::macos) c.font.family = "Menlo";
    Profile p; p.executable = platform == Platform::windows ? "cmd.exe" : "/bin/zsh";
    p.label = platform == Platform::windows ? "Command Prompt" : "Z shell";
    c.profiles.push_back(std::move(p)); c.bindings = default_bindings(platform); return c;
}
Status validate_config(const Config& c, Platform platform) {
    if (!valid_ribbon(c.ribbon)) return Status::failure("Ribbon order requires each registered binding exactly once");
    if (c.version != 1) return Status::failure("Unsupported config version; expected version = 1");
    if (!find_builtin_theme(c.theme.name) && c.theme.name != "custom") return Status::failure("Theme name must be a built-in name or custom");
    if (!color(c.theme.background) || !color(c.theme.foreground) || !color(c.theme.chrome) || !color(c.theme.accent)) return Status::failure("Theme colors must be #RRGGBB");
    for (const auto& value:c.theme.ansi) if (!color(value)) return Status::failure("ANSI colors must be #RRGGBB");
    if (!std::isfinite(c.appearance.additional_blur) || c.appearance.additional_blur < 0.0 || c.appearance.additional_blur > 64.0)
        return Status::failure("additional_blur must be between 0.0 and 64.0");
    if (!std::isfinite(c.appearance.background_opacity) || c.appearance.background_opacity < 0.0 || c.appearance.background_opacity > 1.0)
        return Status::failure("background_opacity must be between 0.0 and 1.0");
    if (c.appearance.backdrop != BackdropPreference::automatic && c.appearance.backdrop != BackdropPreference::none && c.appearance.backdrop != BackdropPreference::mica && c.appearance.backdrop != BackdropPreference::acrylic)
        return Status::failure("Invalid backdrop preference");
    if (c.appearance.reduced_motion != ReducedMotionPreference::system && c.appearance.reduced_motion != ReducedMotionPreference::enabled && c.appearance.reduced_motion != ReducedMotionPreference::disabled)
        return Status::failure("Invalid reduced motion preference");
    if (c.font.family.empty() || !plain_text(c.font.family, 256) || !std::isfinite(c.font.size_dip) || c.font.size_dip < 6 || c.font.size_dip > 72)
        return Status::failure("Font requires a family and size_dip between 6 and 72");
    if (c.windows_console_backend != WindowsConsoleBackend::system && c.windows_console_backend != WindowsConsoleBackend::bundled_preview)
        return Status::failure("Invalid Windows console backend");
    if (c.scrollback_lines > 1000000) return Status::failure("scrollback_lines must be between 0 and 1000000");
    if (c.profiles.empty() || c.profiles.size() > 64) return Status::failure("Configuration requires between 1 and 64 profiles");
    std::set<std::string> ids;
    std::size_t raw_bytes{};
    for (const auto& p : c.profiles) {
        if (!identifier(p.id) || !ids.insert(p.id).second) return Status::failure("Invalid or duplicate profile ID");
        if (!plain_text(p.label, 256) || p.label.empty()) return Status::failure("Profile label is empty or invalid");
        if (p.executable.empty() || !plain_text(p.executable) || !plain_text(p.working_directory)) return Status::failure("Profile executable/directory is empty or invalid");
        if (p.arguments.size() > 128 || p.environment.size() > 256) return Status::failure("Profile has too many arguments or environment entries");
        raw_bytes += p.id.size() + p.label.size() + p.executable.size() + p.working_directory.size();
        for (const auto& arg : p.arguments) {
            if (arg.size() > 16384 || arg.find('\0') != std::string::npos || !detail::valid_utf8(arg)) return Status::failure("Profile argument is invalid or too long");
            raw_bytes += arg.size();
        }
        for (const auto& [key, value] : p.environment) {
            if (!identifier(key, 256) || key.find('-') != std::string::npos || (key[0] >= '0' && key[0] <= '9')) return Status::failure("Invalid environment variable name");
            if (value.size() > 16384 || value.find('\0') != std::string::npos || !detail::valid_utf8(value)) return Status::failure("Invalid environment value");
            raw_bytes += key.size() + value.size();
        }
        if (raw_bytes > max_bytes) return Status::failure("Configuration exceeds the 1 MiB limit");
    }
    if (!ids.contains(c.default_profile)) return Status::failure("default_profile does not name a configured profile");
    auto valid_bindings = validate_bindings(c.bindings, platform, c.allow_terminal_shortcut_override);
    if (!valid_bindings) return valid_bindings;
    // Keep every accepted value object representable by the documented format.
    // The preceding raw budget bounds allocation before escaped-size validation.
    const auto encoded = serialize_config(c, platform);
    if (encoded.size() > max_bytes) return Status::failure("Encoded configuration exceeds the 1 MiB limit");
    std::size_t line_size{};
    for (const char ch : encoded) {
        if (ch == '\n') line_size = 0;
        else if (++line_size > 16384) return Status::failure("Encoded configuration line exceeds the 16 KiB limit");
    }
    return {};
}
ConfigParseResult parse_config(std::string_view text, Platform platform) {
    ConfigParseResult result;
    auto error = [&](std::size_t line, std::size_t column, std::string message) {
        if (result.errors.size() < 64) result.errors.push_back({line, column, std::move(message)});
    };
    if (text.size() > max_bytes) { error(0, 0, "Config exceeds the 1 MiB limit"); return result; }
    if (!detail::valid_document_text(text)) { error(0, 0, "Config must contain valid UTF-8 without unescaped control characters"); return result; }
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    Config config = default_config(platform);
    std::string section; std::set<std::pair<std::string, std::string>> assigned;
    std::size_t line_number{};
    bool have_version{};
    while (!text.empty()) {
        ++line_number;
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        if (end == std::string_view::npos) text = {}; else text.remove_prefix(end + 1);
        if (line.size() > 16384) { error(line_number, 1, "Line exceeds the 16 KiB limit"); continue; }
        line = trim(line); if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            const auto close = line.find(']');
            if (close == std::string_view::npos) { error(line_number, 1, "Unterminated section header"); continue; }
            const auto tail = trim(line.substr(close + 1));
            if (!tail.empty() && tail[0] != '#') { error(line_number, close + 2, "Unexpected text after section header"); continue; }
            section = std::string(trim(line.substr(1, close - 1)));
            const bool known = section == "features" || section == "theme" || section == "ribbon" || section == "appearance" || section == "font" || section == "terminal" || section == "bindings" ||
                section == "bindings.windows" || section == "bindings.macos";
            if (!known && !section.starts_with("profile.")) error(line_number, 1, "Unknown section: " + section);
            if (section.starts_with("profile.")) {
                auto id = std::string_view(section).substr(8); if (id.ends_with(".environment")) id.remove_suffix(12);
                if (!identifier(id)) { error(line_number, 1, "Invalid profile section ID"); continue; }
                auto existing = std::find_if(config.profiles.begin(), config.profiles.end(), [&](const Profile& p) { return p.id == id; });
                if (existing == config.profiles.end()) {
                    if (config.profiles.size() >= 64) { error(line_number, 1, "Too many profiles"); continue; }
                    Profile profile; profile.id = std::string(id); profile.label = std::string(id); config.profiles.push_back(std::move(profile));
                }
            }
            continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string_view::npos) { error(line_number, 1, "Expected key = value"); continue; }
        const std::string key(trim(line.substr(0, equal)));
        if (key.empty() || !assigned.emplace(section, key).second) { error(line_number, 1, "Empty or duplicate key: " + key); continue; }
        ValueReader reader{line.substr(equal + 1), 0, {}}; auto value = reader.read(); reader.space();
        if (!value) { error(line_number, equal + reader.position + 2, reader.error); continue; }
        if (reader.position < reader.s.size() && reader.s[reader.position] != '#') { error(line_number, equal + reader.position + 2, "Unexpected text after value"); continue; }
        bool handled = false;
        auto string_field = [&](std::string_view name, std::string& field) {
            if (key != name) return;
            handled = true; if (auto* s = std::get_if<std::string>(&*value)) field = *s; else error(line_number, equal + 2, "Expected a quoted string");
        };
        auto bool_field = [&](std::string_view name, bool& field) {
            if (key != name) return;
            handled = true; if (auto* b = std::get_if<bool>(&*value)) field = *b; else error(line_number, equal + 2, "Expected true or false");
        };
        auto number_field = [&](std::string_view name, auto& field, double min, double max, bool integer) {
            if (key != name) return;
            handled = true; auto* n = std::get_if<double>(&*value);
            if (!n || *n < min || *n > max || (integer && std::floor(*n) != *n)) error(line_number, equal + 2, "Number is out of range or has the wrong type");
            else field = static_cast<std::remove_reference_t<decltype(field)>>(*n);
        };
        if (section.empty()) {
            if (key == "version") have_version = true;
            number_field("version", config.version, 1, 1, true);
            string_field("default_profile", config.default_profile);
            bool_field("allow_terminal_shortcut_override", config.allow_terminal_shortcut_override);
        } else if (section == "theme") {
            string_field("name", config.theme.name); string_field("background", config.theme.background);
            string_field("foreground", config.theme.foreground); string_field("chrome", config.theme.chrome); string_field("accent", config.theme.accent);
            for (std::size_t i=0;i<16;++i) string_field("ansi"+std::to_string(i), config.theme.ansi[i]);
        } else if (section == "features") {
            bool_field("experimental_ai", config.experimental_ai);
        } else if (section == "appearance") {
            bool_field("show_clock", config.appearance.show_clock);
            number_field("additional_blur", config.appearance.additional_blur, 0.0, 64.0, false);
            number_field("background_opacity", config.appearance.background_opacity, 0.0, 1.0, false);
            if (key == "backdrop") {
                handled = true; const auto* text_value = std::get_if<std::string>(&*value);
                if (text_value && *text_value == "auto") config.appearance.backdrop = BackdropPreference::automatic;
                else if (text_value && *text_value == "none") config.appearance.backdrop = BackdropPreference::none;
                else if (text_value && *text_value == "mica") config.appearance.backdrop = BackdropPreference::mica;
                else if (text_value && *text_value == "acrylic") config.appearance.backdrop = BackdropPreference::acrylic;
                else error(line_number, equal + 2, "backdrop must be auto, none, mica or acrylic");
            } else if (key == "reduced_motion") {
                handled = true; const auto* text_value = std::get_if<std::string>(&*value);
                if (text_value && *text_value == "system") config.appearance.reduced_motion = ReducedMotionPreference::system;
                else if (text_value && *text_value == "enabled") config.appearance.reduced_motion = ReducedMotionPreference::enabled;
                else if (text_value && *text_value == "disabled") config.appearance.reduced_motion = ReducedMotionPreference::disabled;
                else error(line_number, equal + 2, "reduced_motion must be system, enabled or disabled");
            }
        } else if (section == "ribbon") {
            if (key == "order" || key == "hidden") {
                handled=true;
                const auto* array=std::get_if<std::vector<std::string>>(&*value);
                if (!array || array->size()>6 || (key=="order" && array->size()!=6)) error(line_number,equal+2,"Ribbon lists must contain registered bindings; order requires all six");
                else {
                    std::array<bool,6> seen{};
                    if(key=="hidden") config.ribbon.hidden.fill(false);
                    for(std::size_t i=0;i<array->size();++i) {
                        const auto binding=ribbon_binding((*array)[i]);
                        if(!binding || seen[ribbon_index(*binding)]) {error(line_number,equal+2,"Unknown or duplicate ribbon binding");break;}
                        seen[ribbon_index(*binding)]=true;
                        if(key=="order") config.ribbon.order[i]=*binding;
                        else config.ribbon.hidden[ribbon_index(*binding)]=true;
                    }
                }
            }
        } else if (section == "font") {
            string_field("family", config.font.family); number_field("size_dip", config.font.size_dip, 6, 72, false); bool_field("ligatures", config.font.ligatures);
        } else if (section == "terminal") {
            number_field("scrollback_lines", config.scrollback_lines, 0, 1000000, true);
            if (key == "windows_console_backend") {
                handled = true; const auto* text_value = std::get_if<std::string>(&*value);
                if (text_value && *text_value == "system") config.windows_console_backend = WindowsConsoleBackend::system;
                else if (text_value && *text_value == "bundled") config.windows_console_backend = WindowsConsoleBackend::bundled_preview;
                else error(line_number, equal + 2, "windows_console_backend must be system or bundled");
            }
        }
        else if (section == "bindings" || section == "bindings.windows" || section == "bindings.macos") {
            handled = true;
            if (!find_command(key)) { error(line_number, 1, "Unknown command ID: " + key); continue; }
            std::vector<std::string> chords;
            if (auto* s = std::get_if<std::string>(&*value)) { if (!s->empty()) chords.push_back(*s); }
            else if (auto* list = std::get_if<std::vector<std::string>>(&*value)) chords = *list;
            else { error(line_number, equal + 2, "Binding must be a shortcut string or an array of shortcut strings"); continue; }
            const auto target = section == "bindings.windows" ? Platform::windows : section == "bindings.macos" ? Platform::macos : platform;
            std::vector<Binding> bindings;
            for (const auto& chord : chords) {
                auto parsed = parse_key_chord(chord, target);
                if (!parsed) error(line_number, equal + 2, parsed.error);
                else bindings.push_back({key, *parsed.value});
            }
            if (section == "bindings" || target == platform) {
                std::erase_if(config.bindings, [&](const Binding& b) { return b.command == key; });
                config.bindings.insert(config.bindings.end(), bindings.begin(), bindings.end());
            }
        } else if (section.starts_with("profile.")) {
            auto id = std::string_view(section).substr(8); const bool environment = id.ends_with(".environment");
            if (environment) id.remove_suffix(12);
            auto it = std::find_if(config.profiles.begin(), config.profiles.end(), [&](const Profile& p) { return p.id == id; });
            if (it != config.profiles.end()) {
                if (environment) {
                    handled = true; if (auto* s = std::get_if<std::string>(&*value)) it->environment[key] = *s;
                    else error(line_number, equal + 2, "Environment value must be a quoted string");
                } else {
                    string_field("label", it->label); string_field("executable", it->executable); string_field("working_directory", it->working_directory);
                    if (key == "arguments") {
                        handled = true;
                        if (auto* array = std::get_if<std::vector<std::string>>(&*value)) it->arguments = *array;
                        else error(line_number, equal + 2, "arguments must be an array of quoted strings");
                    }
                }
            }
        }
        if (!handled) error(line_number, 1, "Unknown key: " + (section.empty() ? "" : section + ".") + key);
    }
    if (!have_version) error(1, 1, "Missing version = 1 at the top level");
    if (const auto* named=find_builtin_theme(config.theme.name)) {
        const auto& base=*named;
        if (!assigned.contains({"theme", "background"})) config.theme.background = base.background;
        if (!assigned.contains({"theme", "foreground"})) config.theme.foreground = base.foreground;
        if (!assigned.contains({"theme", "chrome"})) config.theme.chrome = base.chrome;
        if (!assigned.contains({"theme", "accent"})) config.theme.accent = base.accent;
        for (std::size_t i=0;i<16;++i) if (!assigned.contains({"theme", "ansi"+std::to_string(i)})) config.theme.ansi[i]=base.ansi[i];
    }
    auto valid = validate_config(config, platform); if (!valid) error(0, 0, valid.error);
    if (result.errors.empty()) result.config = std::move(config);
    return result;
}
ConfigParseResult load_config(const std::filesystem::path& path, Platform platform) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {{}, {{0, 0, "Unable to open configuration file"}}};
    std::string bytes; bytes.resize(max_bytes + 1);
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto count = file.gcount();
    if (file.bad()) return {{}, {{0, 0, "Unable to read configuration file"}}};
    bytes.resize(static_cast<std::size_t>(count)); return parse_config(bytes, platform);
}
ConfigParseResult reload_config(Config& current, std::string_view text, Platform platform) {
    auto result = parse_config(text, platform); if (result) current = *result.config; return result;
}
std::string serialize_config(const Config& config, Platform platform) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(17);
    out << "# StormTTY declarative configuration. No executable config or includes.\nversion = " << config.version
        << "\ndefault_profile = " << quote(config.default_profile) << "\nallow_terminal_shortcut_override = "
        << (config.allow_terminal_shortcut_override ? "true" : "false") << "\n\n[theme]\nname = " << quote(config.theme.name)
        << "\nbackground = " << quote(config.theme.background) << "\nforeground = " << quote(config.theme.foreground)
        << "\nchrome = " << quote(config.theme.chrome) << "\naccent = " << quote(config.theme.accent);
    for (std::size_t i=0;i<16;++i) out << "\nansi" << i << " = " << quote(config.theme.ansi[i]);
    out << "\n\n[features]\nexperimental_ai = " << (config.experimental_ai?"true":"false");
    out << "\n\n[appearance]\nbackground_opacity = " << config.appearance.background_opacity
        << "\nadditional_blur = " << config.appearance.additional_blur
        << "\nshow_clock = " << (config.appearance.show_clock ? "true" : "false")
        << "\nbackdrop = " << quote(config.appearance.backdrop == BackdropPreference::automatic ? "auto" : config.appearance.backdrop == BackdropPreference::mica ? "mica" : config.appearance.backdrop == BackdropPreference::acrylic ? "acrylic" : "none")
        << "\nreduced_motion = " << quote(config.appearance.reduced_motion == ReducedMotionPreference::system ? "system" : config.appearance.reduced_motion == ReducedMotionPreference::enabled ? "enabled" : "disabled")
        << "\n\n[font]\nfamily = " << quote(config.font.family) << "\nsize_dip = " << config.font.size_dip
        << "\nligatures = " << (config.font.ligatures ? "true" : "false") << "\n\n[terminal]\nscrollback_lines = " << config.scrollback_lines
        << "\nwindows_console_backend = " << quote(config.windows_console_backend == WindowsConsoleBackend::bundled_preview ? "bundled" : "system") << '\n';
    for (const auto& profile : config.profiles) {
        out << "\n[profile." << profile.id << "]\nlabel = " << quote(profile.label) << "\nexecutable = " << quote(profile.executable)
            << "\narguments = [";
        for (std::size_t i = 0; i < profile.arguments.size(); ++i) { if (i) out << ", "; out << quote(profile.arguments[i]); }
        out << "]\nworking_directory = " << quote(profile.working_directory) << '\n';
        if (!profile.environment.empty()) {
            out << "\n[profile." << profile.id << ".environment]\n";
            for (const auto& [key, value] : profile.environment) out << key << " = " << quote(value) << '\n';
        }
    }
    out << "\n[bindings." << (platform == Platform::windows ? "windows" : "macos") << "]\n";
    // Write every command, including unbound ones; absence otherwise means default.
    for (const auto& command : command_registry()) {
        out << command.id << " = ["; bool first = true;
        for (const auto& b : config.bindings) if (b.command == command.id) {
            if (!first) out << ", ";
            first = false; out << quote(format_key_chord(b.chord, platform));
        }
        out << "]\n";
    }
    out << "\n[ribbon]\norder = [";
    for(std::size_t i=0;i<config.ribbon.order.size();++i) {if(i) out << ", ";out << quote(ribbon_bindings[ribbon_index(config.ribbon.order[i])].id);}
    out << "]\nhidden = [";bool first=true;
    for(const auto& binding:ribbon_bindings) if(config.ribbon.hidden[ribbon_index(binding.binding)]) {if(!first) out << ", ";first=false;out << quote(binding.id);}
    out << "]\n";
    return out.str();
}
std::string_view config_format_documentation() noexcept {
    return R"DOC(StormTTY config v1 (UTF-8, maximum 1 MiB)
This is a strict declarative subset, not general TOML. Unknown/duplicate keys,
unsupported versions, invalid values and shortcut conflicts are visible errors;
an invalid reload leaves the old configuration intact. No includes, expressions,
network access, environment expansion, scripts or implicit shell evaluation.

version = 1
# Top-level keys must precede sections.
default_profile = "default"
allow_terminal_shortcut_override = false
[theme]
name = "graphite" # graphite, instrument, daylight, custom
# Optional #RRGGBB: background, foreground, chrome, accent, ansi0 through ansi15.
# Built-ins: graphite, instrument, daylight, midnight, evergreen, aubergine, ember, ink.
# Use name = "custom" for a data-only palette; no scripts, URLs or plugins.
[features]
experimental_ai = false # Restart required; missing in old files means disabled.
[appearance]
show_clock = true
additional_blur = 0 # 0..64 additional Windows Gaussian blur; 0 retains OS material
background_opacity = 0.94 # 0.0..1.0; default canvas only, not text or ANSI colors
backdrop = "acrylic" # auto, none, mica or acrylic; unsupported hosts use opaque
reduced_motion = "system" # system, enabled or disabled; default follows the OS
# High contrast and unsupported renderers use opaque backgrounds.
[ribbon]
# All six allowlisted bindings, once each; left-to-right placement.
order = ["session.directory", "session.git", "session.status", "workspace.background", "agent.status", "clock.local"]
hidden = [] # Any subset; appearance.show_clock remains the legacy clock switch.
# Lower-priority items yield at narrow widths. Agent UI requires features.experimental_ai.
# workspace.background includes Watch activity; unread results remain visible.
# Workspace activity and background restore remain available in Commands.
[font]
family = "Cascadia Mono"
size_dip = 15
ligatures = false
[terminal]
scrollback_lines = 10000 # 0..1000000
windows_console_backend = "system" # Windows: system or bundled (preview); new sessions only
[profile.default]
label = "PowerShell"
executable = "pwsh.exe"
arguments = ["-NoLogo"]
working_directory = "C:\\src"
[profile.default.environment]
STORMTTY_PROFILE = "work"
[bindings]
app.command_palette = "Primary+Shift+P"
pane.split_right = "Primary+Shift+D"
[bindings.windows]
edit.copy = "Ctrl+Shift+C"
[bindings.macos]
edit.copy = "Cmd+C"

Strings are double-quoted; escapes: backslash, quote, n, r, t. Comments start
with # outside strings. Arrays contain only strings on one line (no trailing
comma). Numbers are finite decimals, booleans are true/false. Repeated section
headers are allowed but assigning the same section/key twice is an error.
Profiles use [profile.ID] and optional [profile.ID.environment]; IDs use ASCII
letters, digits, underscore or hyphen. Maximum 64 profiles, 128 arguments/profile,
256 environment entries/profile, 16 KiB/line. Executable and arguments are passed
separately by the host. A profile is launch metadata, not permission to run it.

Bindings are canonical command IDs with a shortcut string or string array;
"" or [] unbinds a command. Unspecified commands retain defaults. [bindings]
applies to the current platform; platform sections apply only to that platform.
Sections apply in file order, so place common bindings before platform overrides.
Primary means Ctrl on Windows and Cmd on macOS. Supported modifiers: Ctrl, Cmd,
Shift, Alt/Option. Keys: letters, digits, punctuation, PLUS, TAB, ENTER, ESCAPE,
SPACE, BACKSPACE, DELETE, INSERT, HOME, END, PAGEUP, PAGEDOWN, arrows, F1..F24.
OS shortcuts cannot be overridden. Terminal input shortcuts require the explicit
allow_terminal_shortcut_override opt-in; composition and AltGr always bypass
application bindings. No split/move/resize shortcuts steal TUI keys by default.

Workspace files are separate STORMTTY-WORKSPACE version 1 metadata snapshots.
They retain tabs, panes, profiles and fractional split geometry, but never live
session handles, shell commands/history, environment, PTY state or screen output.
Restore binds no sessions and does not silently resume or replay shell work.
)DOC";
}
} // namespace stormtty
