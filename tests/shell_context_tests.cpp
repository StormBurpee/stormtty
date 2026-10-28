#include "core_test.hpp"
#include "stormtty/shell_context.hpp"
#include <array>

using namespace stormtty;
namespace {
std::string encode(std::string_view bytes) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : bytes) {
        if (c == '%' || c == ';' || c < 0x20 || c >= 0x7f) {
            result += '%'; result += digits[c >> 4]; result += digits[c & 15];
        } else result += static_cast<char>(c);
    }
    return result;
}
std::string packet(std::string key = "file:///c/src", std::string label = "C:/src",
                   std::string root = "C:/src", std::string kind = "branch", std::string head = "main") {
    return std::string(shell_context_namespace) + "1;set;" + encode(key) + ";" + encode(label) + ";" +
        encode(root) + ";" + kind + ";" + encode(head);
}
}
int main() { return run_core_tests("shell context", [] {
    const auto clear = parse_shell_context_osc("7400;stormtty-context;1;clear");
    CHECK(clear.kind == ShellContextMessageKind::clear && !clear.context);
    auto valid = parse_shell_context_osc(packet());
    CHECK(valid.kind == ShellContextMessageKind::snapshot && valid.context);
    CHECK(valid.context->directory_key == "file:///c/src");
    CHECK(valid.context->display_directory == "C:/src");
    CHECK(valid.context->git == GitHeadKind::branch && valid.context->head == "main");
    CHECK(parse_shell_context_osc(packet(), true).kind == ShellContextMessageKind::invalid);
    for (auto value : {"", "133;C", "7400;other;1;set", "7400;stormtty-context;2;future", "7400;stormtty-context;11;set"})
        CHECK(parse_shell_context_osc(value).kind == ShellContextMessageKind::ignored);
    for (auto value : {"7400;stormtty-context;1", "7400;stormtty-context;1;clear;", "7400;stormtty-context;1;set", "7400;stormtty-context;1;foo",
                       "7400;stormtty-context;1;set;a;b;c;branch;d;extra"})
        CHECK(parse_shell_context_osc(value).kind == ShellContextMessageKind::invalid);
    for (const auto suffix : {"%", "%0", "%G0", "%0x", "%00", "%1B", "%07", "%0D", "%0A", "%7F", "%C2%85", "%E2%80%A8"}) {
        const auto value = packet() + suffix;
        CHECK(parse_shell_context_osc(value).kind == ShellContextMessageKind::invalid);
    }
    const std::array<std::string, 7> malformed = {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x80", "\x80", "\xc2x", "\xf0\x80\x80\x80"};
    for (const auto& value : malformed) CHECK(parse_shell_context_osc(packet("key", "label", "root", "branch", value)).kind == ShellContextMessageKind::invalid);
    const std::string literal = "feature/$(touch nope);`cmd`%25 \xe8\xb7\xaf\xe5\xbe\x84";
    auto data = parse_shell_context_osc(packet("file:///c/a%20b", "~/a b", "C:/a b", "branch", literal));
    CHECK(data.context && data.context->head == literal);
    CHECK(data.context->directory_key == "file:///c/a%20b"); // Percent decoding exactly once.
    CHECK(data.context->display_directory == "~/a b");
    auto directional = parse_shell_context_osc(packet("key", "label", "root", "branch", "x\xe2\x81\xa6y"));
    CHECK(directional.context);
    CHECK(directional.context->head == "x\xef\xbf\xbdy");
    // Use separate strings because C++ hex escapes consume subsequent hex digits.
    const auto label_bidi = parse_shell_context_osc(packet("key", std::string("a\xe2\x80\xae") + "b"));
    CHECK(label_bidi.context && label_bidi.context->display_directory == std::string("a\xef\xbf\xbd") + "b");
    for (const auto kind : {"", "worktree", "detached;branch"}) CHECK(parse_shell_context_osc(packet("key", "label", "root", kind)).kind == ShellContextMessageKind::invalid);
    CHECK(parse_shell_context_osc(packet("key", "label", "", "none", "")).context->git == GitHeadKind::none);
    CHECK(parse_shell_context_osc(packet("key", "label", "root", "none", "")).kind == ShellContextMessageKind::invalid);
    CHECK(parse_shell_context_osc(packet("key", "label", "", "branch", "main")).kind == ShellContextMessageKind::invalid);
    CHECK(parse_shell_context_osc(packet("key", "label", "root", "detached", "abcdef1")).context->git == GitHeadKind::detached);
    for (const auto head : {"abc", "abcdefg", "abcdef1x"}) CHECK(parse_shell_context_osc(packet("key", "label", "root", "detached", head)).kind == ShellContextMessageKind::invalid);
    CHECK(parse_shell_context_osc(packet("", "label")).kind == ShellContextMessageKind::invalid);
    CHECK(parse_shell_context_osc(packet("key", "")).kind == ShellContextMessageKind::invalid);
    CHECK(parse_shell_context_osc(packet(std::string(2048, 'k'), std::string(2048, 'd'), std::string(2048, 'r'), "branch", std::string(256, 'h'))).context);
    CHECK(parse_shell_context_osc(packet(std::string(2049, 'k'))).kind == ShellContextMessageKind::invalid);
    CHECK(parse_shell_context_osc(packet("key", std::string(2049, 'd'))).kind == ShellContextMessageKind::invalid);
    CHECK(parse_shell_context_osc(packet("key", "label", "root", "branch", std::string(257, 'h'))).kind == ShellContextMessageKind::invalid);
    CHECK(parse_shell_context_osc(packet() + std::string(8192, 'x')).kind == ShellContextMessageKind::invalid);

    ShellContextState first, second;
    CHECK(!first.apply(valid)); // Context cannot establish its own directory.
    CHECK(!first.observe_directory("file:///c/src"));
    CHECK(first.apply(valid)); CHECK(first.context() == valid.context);
    CHECK(!second.context()); CHECK(!first.apply(valid));
    const auto copy = first.context();
    CHECK(first.observe_directory("file:///c/elsewhere")); CHECK(!first.context());
    CHECK(copy && copy->head == "main"); // Publication copies remain immutable.
    CHECK(!first.apply(valid));
    CHECK(!first.observe_directory("file:///c/src")); CHECK(first.apply(valid));
    CHECK(!first.apply(parse_shell_context_osc("7400;unrelated;broken"))); CHECK(first.context());
    CHECK(!first.apply(parse_shell_context_osc("7400;stormtty-context;2;future"))); CHECK(first.context());
    CHECK(first.apply(parse_shell_context_osc(packet(), true))); CHECK(!first.context());
    CHECK(first.apply(valid));
    CHECK(first.apply(parse_shell_context_osc(packet("file:///c/src", "C:/src", "", "none", ""))));
    CHECK(first.context()->head.empty() && first.context()->repository_root.empty());
    CHECK(first.apply(clear)); CHECK(!first.apply(clear)); CHECK(first.apply(valid));
    CHECK(first.observe_directory("")); CHECK(!first.context()); CHECK(!first.apply(valid));
    first.observe_directory("file:///c/src"); first.apply(valid);
    CHECK(first.observe_directory(std::string(2049, 'x'))); CHECK(!first.apply(valid));
    first.observe_directory("file:///c/src"); first.apply(valid);
    CHECK(first.observe_directory("file:///c/\x1b")); CHECK(!first.apply(valid));
    first.observe_directory("file:///c/src"); first.apply(valid);
    CHECK(first.reset()); CHECK(!first.context()); CHECK(!first.apply(valid)); CHECK(!first.reset());
    // Repeated packets keep one latest snapshot; no event queue or generation growth.
    first.observe_directory("file:///c/src"); CHECK(first.apply(valid));
    for (unsigned i = 0; i < 10000; ++i) CHECK(!first.apply(valid));
}); }
