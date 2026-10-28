#include "stormtty/shell_context.hpp"
#include <array>

namespace stormtty {
namespace {
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool bidi(char32_t c) {
    return c == 0x061c || c == 0x200e || c == 0x200f ||
        (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069);
}
std::optional<std::string> text(std::string_view value, std::size_t limit, bool display) {
    if (value.size() > limit) return {};
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        const auto start = i;
        const auto lead = static_cast<unsigned char>(value[i++]);
        char32_t cp = lead;
        unsigned rest = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { cp = lead & 31; rest = 1; }
        else if (lead >= 0xe0 && lead <= 0xef) { cp = lead & 15; rest = 2; }
        else if (lead >= 0xf0 && lead <= 0xf4) { cp = lead & 7; rest = 3; }
        else if (lead >= 0x80) return {};
        if (rest > value.size() - i) return {};
        for (unsigned n = 0; n < rest; ++n) {
            const auto c = static_cast<unsigned char>(value[i++]);
            if ((c & 0xc0) != 0x80) return {};
            cp = (cp << 6) | (c & 63);
        }
        if ((rest == 1 && cp < 0x80) || (rest == 2 && cp < 0x800) ||
            (rest == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) return {};
        // C0/C1, DEL and Unicode line/paragraph separators must not enter chrome.
        if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp == 0x2028 || cp == 0x2029) return {};
        if (display && bidi(cp)) result += "\xef\xbf\xbd"; // Visible replacement, not silent removal.
        else result.append(value.substr(start, i - start));
        if (result.size() > limit) return {};
    }
    return result;
}
std::optional<std::string> field(std::string_view value, std::size_t limit, bool display) {
    std::string decoded;
    decoded.reserve(value.size() < limit ? value.size() : limit);
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '%') {
            if (value.size() - i < 3) return {};
            const auto a = hex(value[i + 1]), b = hex(value[i + 2]);
            if (a < 0 || b < 0) return {};
            c = static_cast<char>((a << 4) | b); i += 2;
        }
        if (decoded.size() == limit) return {};
        decoded.push_back(c);
    }
    return text(decoded, limit, display);
}
ShellContextMessage invalid() { return {ShellContextMessageKind::invalid, {}}; }
}
ShellContextMessage parse_shell_context_osc(std::string_view content, bool truncated) {
    if (!content.starts_with(shell_context_namespace)) return {};
    if (truncated || content.size() > shell_context_wire_limit) return invalid();
    content.remove_prefix(shell_context_namespace.size());
    if (content == "1") return invalid();
    if (!content.starts_with("1;")) return {}; // Version negotiation is deliberately absent.
    content.remove_prefix(2);
    if (content == "clear") return {ShellContextMessageKind::clear, {}};
    if (!content.starts_with("set;")) return invalid();
    content.remove_prefix(4);
    std::array<std::string_view, 5> fields;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto delimiter = content.find(';');
        if (i == fields.size() - 1) {
            if (delimiter != std::string_view::npos) return invalid();
            fields[i] = content;
        } else {
            if (delimiter == std::string_view::npos) return invalid();
            fields[i] = content.substr(0, delimiter); content.remove_prefix(delimiter + 1);
        }
    }
    auto key = field(fields[0], shell_context_directory_limit, false);
    auto directory = field(fields[1], shell_context_directory_limit, true);
    auto root = field(fields[2], shell_context_directory_limit, true);
    auto head = field(fields[4], shell_context_head_limit, true);
    if (!key || key->empty() || !directory || directory->empty() || !root || !head) return invalid();
    GitHeadKind kind;
    if (fields[3] == "none") {
        kind = GitHeadKind::none;
        if (!root->empty() || !head->empty()) return invalid();
    } else if (fields[3] == "branch") {
        kind = GitHeadKind::branch;
        if (root->empty() || head->empty()) return invalid();
    } else if (fields[3] == "detached") {
        kind = GitHeadKind::detached;
        if (root->empty() || head->size() < 7 || head->size() > 64) return invalid();
        for (const auto c : *head) if (hex(c) < 0) return invalid();
    } else return invalid();
    return {ShellContextMessageKind::snapshot,
        ShellContext{std::move(*key), std::move(*directory), std::move(*root), std::move(*head), kind}};
}
bool ShellContextState::observe_directory(std::string_view raw_pwd) {
    auto accepted = text(raw_pwd, shell_context_directory_limit, false);
    std::string next = accepted ? std::move(*accepted) : std::string{};
    if (next == directory_) return false;
    directory_ = std::move(next);
    const bool changed = context_.has_value(); context_.reset(); return changed;
}
bool ShellContextState::apply(const ShellContextMessage& message) {
    if (message.kind == ShellContextMessageKind::ignored) return false;
    std::optional<ShellContext> next;
    if (message.kind == ShellContextMessageKind::snapshot && message.context &&
        !directory_.empty() && message.context->directory_key == directory_) next = message.context;
    if (next == context_) return false;
    context_ = std::move(next); return true;
}
bool ShellContextState::reset() {
    directory_.clear();
    const bool changed = context_.has_value(); context_.reset(); return changed;
}
}
