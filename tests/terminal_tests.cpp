#include "stormtty/terminal.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace stormtty;
namespace {
unsigned png_decoder_calls{};
std::optional<DecodedPng> rejecting_png_decoder(std::span<const std::uint8_t>, std::size_t) {
    ++png_decoder_calls;
    return std::nullopt; // Tests hook invocation/rejection, NOT native PNG decoding.
}
std::string base64(std::span<const std::uint8_t> input) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const unsigned value = (unsigned(input[i]) << 16) |
            (i + 1 < input.size() ? unsigned(input[i + 1]) << 8 : 0) |
            (i + 2 < input.size() ? unsigned(input[i + 2]) : 0);
        out.push_back(alphabet[(value >> 18) & 63]); out.push_back(alphabet[(value >> 12) & 63]);
        out.push_back(i + 1 < input.size() ? alphabet[(value >> 6) & 63] : '=');
        out.push_back(i + 2 < input.size() ? alphabet[value & 63] : '=');
    }
    return out;
}
void require(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(x) require(bool(x), #x, __LINE__)
std::string line(const FramePtr& frame, std::size_t row) {
    std::string out;
    for (const auto& cell : frame->lines.at(row)->cells) {
        if (!cell.span) continue;
        out += cell.grapheme.empty() ? " " : cell.grapheme;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
std::string replies(TerminalCore& terminal) {
    std::string out;
    for (auto& effect : terminal.take_effects()) if (effect.kind == EffectKind::write_pty) out += effect.text;
    return out;
}
TerminalOptions small(std::uint16_t columns = 20, std::uint16_t rows = 4) {
    TerminalOptions options; options.size = {columns, rows, 8, 16}; return options;
}
void split_feeds() {
    const std::string bytes = "A\xC3\xA9\xE7\x95\x8C" "e\xCC\x81\x1b[38;2;1;2;3mZ\x1b[0m\r\nnext";
    TerminalCore whole(small()), split(small());
    whole.feed(bytes);
    for (const char byte : bytes) split.feed(std::string_view(&byte, 1));
    const auto a = whole.snapshot(), b = split.snapshot();
    CHECK(line(a, 0) == line(b, 0)); CHECK(line(a, 1) == "next"); CHECK(line(a, 1) == line(b, 1));
    CHECK(a->lines[0]->cells[2].grapheme == "\xE7\x95\x8C");
    CHECK(a->lines[0]->cells[2].span == 2); CHECK(a->lines[0]->cells[3].span == 0);
    CHECK(a->lines[0]->cells[4].grapheme == "e\xCC\x81");
    CHECK(a->lines[0]->cells[5].foreground == (Color{1,2,3,255}));
}
void fragmented_stream_equivalence() {
    const std::vector<std::string> tokens{"plain", "\r\n", "\t", "\x1b[2K", "\x1b[H", "\x1b[2;5H",
        "\x1b[38;5;123m", "\x1b[0m", "\xE7\x95\x8C", "e\xCC\x81", "\xF0\x9F\x98\x80",
        "\x1b[6n", "\x1b]2;fragment test\x1b\\", "\x1b[?1049h\x1b[H", "\x1b[?1049l"};
    std::uint32_t random = 0x51f15e;
    auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
    std::string bytes;
    for (unsigned i = 0; i < 1500; ++i) bytes += tokens[next() % tokens.size()];
    TerminalCore whole(small(40,8)), fragments(small(40,8));
    whole.feed(bytes);
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = std::min<std::size_t>(1 + next() % 23, bytes.size() - offset);
        fragments.feed(std::string_view(bytes).substr(offset,count)); offset += count;
    }
    const auto a = whole.snapshot(), b = fragments.snapshot();
    CHECK(a->alternate_screen == b->alternate_screen);
    CHECK(a->cursor.column == b->cursor.column && a->cursor.row == b->cursor.row);
    for (std::size_t row = 0; row < a->lines.size(); ++row) CHECK(a->lines[row]->cells == b->lines[row]->cells);
    const auto a_effects = whole.take_effects(), b_effects = fragments.take_effects();
    CHECK(a_effects.size() == b_effects.size());
    for (std::size_t i = 0; i < a_effects.size(); ++i) {
        CHECK(a_effects[i].kind == b_effects[i].kind); CHECK(a_effects[i].text == b_effects[i].text);
    }
}
void ansi_styles() {
    TerminalCore t(small());
    t.feed("\x1b[1;3;4;38;2;10;20;30;48;2;40;50;60mX\x1b[0mY");
    const auto f = t.snapshot(); const auto& x = f->lines[0]->cells[0]; const auto& y = f->lines[0]->cells[1];
    CHECK(x.style.bold && x.style.italic && x.style.underline == 1);
    CHECK(x.foreground == (Color{10,20,30,255})); CHECK(x.background == (Color{40,50,60,255}));
    CHECK(!x.background_is_default); CHECK(y.background_is_default); CHECK(!y.style.bold);
    t.feed("\x1b[2J\x1b[HZ"); CHECK(line(t.snapshot(), 0) == "Z"); CHECK(line(f, 0) == "XY");
}
void configured_theme_defaults_and_queries() {
    auto options = small();
    options.default_foreground = Color{0x12,0x34,0x56,255};
    options.default_background = Color{0x2a,0x45,0x6b,255};
    TerminalCore themed(options), baseline(small());
    themed.feed("D\x1b[38;2;1;2;3;48;2;4;5;6mE\x1b[39;49mR\x1b[31;44mP\x1b[0mZ");
    baseline.feed("\x1b[31;44mP");
    const auto frame = themed.snapshot(); const auto& cells = frame->lines[0]->cells;
    CHECK(frame->foreground == *options.default_foreground); CHECK(frame->background == *options.default_background);
    CHECK(cells[0].foreground == *options.default_foreground && cells[0].background == *options.default_background);
    CHECK(cells[0].background_is_default);
    CHECK(cells[1].foreground == (Color{1,2,3,255}) && cells[1].background == (Color{4,5,6,255}));
    CHECK(!cells[1].background_is_default);
    CHECK(cells[2].foreground == *options.default_foreground && cells[2].background == *options.default_background);
    CHECK(cells[2].background_is_default);
    CHECK(cells[3].foreground == baseline.snapshot()->lines[0]->cells[0].foreground);
    CHECK(cells[3].background == baseline.snapshot()->lines[0]->cells[0].background);
    CHECK(!cells[3].background_is_default); // The indexed ANSI palette is not overwritten by default theme colors.
    CHECK(cells[4].foreground == *options.default_foreground && cells[4].background == *options.default_background);
    themed.feed("\x1b]10;?\x1b\\\x1b]11;?\x1b\\");
    CHECK(replies(themed) == "\x1b]10;rgb:1212/3434/5656\x1b\\\x1b]11;rgb:2a2a/4545/6b6b\x1b\\");
    auto foreground_only = small(); foreground_only.default_foreground = options.default_foreground;
    TerminalCore single_fg(foreground_only);
    CHECK(single_fg.snapshot()->foreground == *options.default_foreground);
    CHECK(single_fg.snapshot()->background == baseline.snapshot()->background);
    auto background_only = small(); background_only.default_background = options.default_background;
    TerminalCore single_bg(background_only);
    CHECK(single_bg.snapshot()->background == *options.default_background);
    CHECK(single_bg.snapshot()->foreground == baseline.snapshot()->foreground);
}
void theme_overrides_reset_and_validation() {
    auto options = small();
    options.default_foreground = Color{0x12,0x34,0x56,255};
    options.default_background = Color{0x2a,0x45,0x6b,255};
    TerminalCore t(options);
    t.feed("D\x1b[38;2;1;2;3;48;2;4;5;6mE\x1b[0m"
           "\x1b]10;rgb:aa/bb/cc\x1b\\\x1b]11;rgb:dd/ee/ff\x1b\\");
    const auto overridden = t.snapshot();
    CHECK(overridden->foreground == (Color{0xaa,0xbb,0xcc,255}));
    CHECK(overridden->background == (Color{0xdd,0xee,0xff,255}));
    CHECK(overridden->lines[0]->cells[0].background_is_default);
    CHECK(overridden->lines[0]->cells[1].foreground == (Color{1,2,3,255}));
    CHECK(overridden->lines[0]->cells[1].background == (Color{4,5,6,255}));
    t.feed("\x1b]10;?\x1b\\\x1b]11;?\x1b\\");
    CHECK(replies(t) == "\x1b]10;rgb:aaaa/bbbb/cccc\x1b\\\x1b]11;rgb:dddd/eeee/ffff\x1b\\");
    t.feed("\x1b]110\x1b\\\x1b]111\x1b\\\x1b]10;?\x1b\\\x1b]11;?\x1b\\");
    CHECK(replies(t) == "\x1b]10;rgb:1212/3434/5656\x1b\\\x1b]11;rgb:2a2a/4545/6b6b\x1b\\");
    CHECK(t.snapshot()->foreground == *options.default_foreground);
    CHECK(t.snapshot()->background == *options.default_background);
    CHECK(t.snapshot()->lines[0]->cells[0].foreground == *options.default_foreground);
    CHECK(t.snapshot()->lines[0]->cells[0].background == *options.default_background);
    CHECK(overridden->background == (Color{0xdd,0xee,0xff,255})); // Published old frame stays owned/unchanged.
    t.feed("\x1b" "c"); // RIS resets protocol state, not configured theme defaults.
    CHECK(t.snapshot()->foreground == *options.default_foreground);
    CHECK(t.snapshot()->background == *options.default_background);
    t.feed("\x1b[?1049h\x1b[HX");
    CHECK(t.snapshot()->lines[0]->cells[0].foreground == *options.default_foreground);
    CHECK(t.snapshot()->lines[0]->cells[0].background == *options.default_background);
    options.default_foreground->a = 254;
    bool rejected{}; try { TerminalCore invalid(options); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    options.default_foreground->a = 255; options.default_background->a = 128;
    rejected = false; try { TerminalCore invalid(options); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}
void live_theme_defaults() {
    auto options = small();
    const Color original_fg{18,52,86,255}, original_bg{42,69,107,255};
    const Color preview_fg{224,225,226,255}, preview_bg{16,17,18,255};
    options.default_foreground = original_fg; options.default_background = original_bg;
    TerminalCore t(options);
    // M deliberately has explicit RGB values identical to the OLD defaults.
    // Origin, not RGB equality, determines whether an existing cell changes.
    t.feed("D\x1b[38;2;18;52;86;48;2;42;69;107mM\x1b[31;44mP\x1b[0mR");
    const auto before = t.snapshot(); const auto revision = t.revision();
    t.set_default_colors(preview_fg, preview_bg);
    CHECK(replies(t).empty()); CHECK(t.revision() == revision);
    const auto preview = t.snapshot();
    CHECK(preview->foreground == preview_fg && preview->background == preview_bg);
    for (const auto index : {0u,3u}) {
        CHECK(preview->lines[0]->cells[index].foreground == preview_fg);
        CHECK(preview->lines[0]->cells[index].background == preview_bg);
    }
    for (const auto index : {1u,2u}) CHECK(preview->lines[0]->cells[index] == before->lines[0]->cells[index]);
    CHECK(before->foreground == original_fg && before->background == original_bg);
    CHECK(before->lines[0]->cells[0].foreground == original_fg);
    t.set_default_colors(original_fg, original_bg); // Cancel after preview.
    const auto cancelled = t.snapshot();
    CHECK(cancelled->foreground == original_fg && cancelled->background == original_bg);
    CHECK(cancelled->lines[0]->cells == before->lines[0]->cells);
    CHECK(preview->foreground == preview_fg && preview->lines[0]->cells[0].foreground == preview_fg);
    t.feed("\x1b]10;rgb:aa/bb/cc\x1b\\\x1b]11;rgb:dd/ee/ff\x1b\\");
    const auto override = t.snapshot();
    t.set_default_colors(preview_fg, preview_bg);
    CHECK(t.snapshot()->foreground == override->foreground && t.snapshot()->background == override->background);
    CHECK(replies(t).empty());
    t.feed("\x1b]110\x1b\\\x1b]111\x1b\\\x1b]10;?\x1b\\\x1b]11;?\x1b\\");
    CHECK(replies(t) == "\x1b]10;rgb:e0e0/e1e1/e2e2\x1b\\\x1b]11;rgb:1010/1111/1212\x1b\\");
    CHECK(t.snapshot()->foreground == preview_fg && t.snapshot()->background == preview_bg);
    CHECK(t.snapshot()->lines[0]->cells[1] == before->lines[0]->cells[1]);
    const auto stable = t.snapshot();
    t.set_default_colors(preview_fg, preview_bg);CHECK(t.snapshot() == stable);
    bool rejected{};
    try { t.set_default_colors({1,2,3,128}, original_bg); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected && t.snapshot() == stable);
    rejected = false;
    try { t.set_default_colors(original_fg, {1,2,3,0}); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected && t.snapshot() == stable);
    std::atomic<bool> foreign_rejected{};
    std::thread foreign([&] { try { t.set_default_colors(original_fg, original_bg); } catch (const std::logic_error&) { foreign_rejected = true; } });
    foreign.join(); CHECK(foreign_rejected && t.snapshot() == stable);
}
void alternate_buffer() {
    TerminalCore t(small()); t.feed("primary"); auto primary = t.snapshot();
    t.feed("\x1b[?1049h\x1b[Halternate"); auto alternate = t.snapshot();
    CHECK(alternate->alternate_screen); CHECK(line(alternate, 0) == "alternate");
    t.feed("\x1b[?1049l"); auto restored = t.snapshot();
    CHECK(!restored->alternate_screen); CHECK(line(restored, 0) == "primary"); CHECK(line(primary, 0) == "primary");
}
void immutable_sharing() {
    FramePtr held;
    {
        TerminalCore t(small()); t.feed("one\r\ntwo\r\nthree"); auto first = t.snapshot();
        CHECK(t.snapshot() == first);
        t.feed("\x1b[1;1HO"); auto changed = t.snapshot();
        CHECK(line(first, 0) == "one"); CHECK(line(changed, 0) == "One");
        CHECK(first->lines[0]->id == changed->lines[0]->id);
        CHECK(first->lines[0]->generation != changed->lines[0]->generation);
        CHECK(first->lines[2] == changed->lines[2]);
        held = changed;
    }
    CHECK(line(held, 2) == "three");
}
void resize_and_wrap() {
    TerminalCore t(small(8, 4)); t.feed("abcdefghij"); auto before = t.snapshot();
    CHECK(line(before, 0) == "abcdefgh"); CHECK(line(before, 1) == "ij");
    t.resize({12, 4, 9, 18}); auto after = t.snapshot();
    CHECK(after->columns == 12 && after->rows == 4 && after->cell_width == 9 && after->cell_height == 18);
    CHECK(line(after, 0) == "abcdefghij"); CHECK(line(before, 0) == "abcdefgh");
    bool rejected{}; try { t.resize({0, 4, 8, 16}); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}
void backend_resize_policy() {
    auto conpty_options = small(20,3); auto posix_options = conpty_options;
    posix_options.resize_pull_scrollback = true;
    TerminalCore conpty(conpty_options), posix(posix_options);
    conpty.feed("first\r\nsecond\r\nthird\r\nfourth");
    posix.feed("first\r\nsecond\r\nthird\r\nfourth");
    conpty.resize({20,4,8,16}); posix.resize({20,4,8,16});
    CHECK(line(conpty.snapshot(),0) == "second");
    CHECK(line(posix.snapshot(),0) == "first");
}
void hold_captures_exact_boundary() {
    TerminalCore t(small()); t.feed("old"); const auto old = t.snapshot();
    t.feed("\rready\x1b[?2026h\rpartial");
    CHECK(t.render_held()); CHECK(t.next_deadline().has_value());
    const auto frozen = t.snapshot(); CHECK(line(frozen, 0) == "ready"); CHECK(line(old, 0) == "old");
    t.feed("\rcomplete\x1b[?2026l");
    CHECK(!t.render_held()); CHECK(line(t.snapshot(), 0) == "complete"); CHECK(line(frozen, 0) == "ready");
}
void hold_timeout_and_resize() {
    TerminalCore t(small()); t.feed("before\x1b[?2026h\r\x1b[2Kafter");
    CHECK(line(t.snapshot(), 0) == "before"); const auto deadline = *t.next_deadline();
    CHECK(line(t.snapshot(deadline + std::chrono::milliseconds(1)), 0) == "after"); CHECK(!t.render_held());
    t.feed("\x1b[?2026h\rresize"); CHECK(t.render_held());
    t.resize({22,4,8,16}); CHECK(!t.render_held()); CHECK(line(t.snapshot(), 0) == "resize");
}
void back_to_back_holds() {
    TerminalCore t(small());
    t.feed("A\x1b[?2026h\rB\x1b[?2026l\x1b[?2026h\rC");
    CHECK(t.render_held()); CHECK(line(t.snapshot(), 0) == "B");
    t.feed("\x1b[?2026l"); CHECK(line(t.snapshot(), 0) == "C");
}
void queries_and_effects() {
    TerminalCore t(small()); t.feed("\x1b[3;4H\x1b[6n\x1b[5n");
    CHECK(replies(t) == "\x1b[3;4R\x1b[0n");
    t.feed("\x1b]2;safe title\x1b\\\x1b]7;file://localhost/tmp/project\x1b\\\a");
    const auto effects = t.take_effects(); CHECK(effects.size() == 3);
    CHECK(effects[0].kind == EffectKind::title && effects[0].text == "safe title");
    CHECK(effects[1].kind == EffectKind::working_directory);
    CHECK(effects[1].text.find("project") != std::string::npos);
    CHECK(effects[2].kind == EffectKind::bell); CHECK(t.take_effects().empty());
}
void key_encoding() {
    TerminalCore t(small()); KeyInput input;
    input.key = Key::up; CHECK(t.encode_key(input) == "\x1b[A");
    t.feed("\x1b[?1h"); CHECK(t.encode_key(input) == "\x1bOA");
    input.key = Key::enter; CHECK(t.encode_key(input) == "\r");
    input.key = Key::a; input.text = "a"; input.unshifted_codepoint = 'a'; input.modifiers = Modifier::control;
    CHECK(t.encode_key(input) == std::string(1, '\x01'));
    input = {}; input.text = "\xE6\xBC\xA2"; CHECK(t.encode_key(input) == input.text);
    input.action = KeyAction::release; CHECK(t.encode_key(input).empty());
}
void focus_paste_encoding() {
    TerminalCore t(small()); CHECK(t.encode_focus(true).empty());
    t.feed("\x1b[?1004h"); CHECK(t.encode_focus(true) == "\x1b[I"); CHECK(t.encode_focus(false) == "\x1b[O");
    CHECK(TerminalCore::paste_is_safe({})); CHECK(t.encode_paste({}).empty());
    CHECK(TerminalCore::paste_is_safe("abc")); CHECK(!TerminalCore::paste_is_safe("a\nb"));
    bool rejected{}; try { (void)t.encode_paste("a\nb"); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected); CHECK(t.encode_paste("a\nb", true) == "a\rb");
    t.feed("\x1b[?2004h"); CHECK(t.encode_paste("abc") == "\x1b[200~abc\x1b[201~");
    std::string large(2048, 'x'); CHECK(t.encode_paste(large) == "\x1b[200~" + large + "\x1b[201~");
}
void mouse_encoding() {
    TerminalCore t(small()); MouseInput input; input.action = MouseAction::press; input.button = MouseButton::left;
    input.x = 16; input.y = 16; CHECK(t.encode_mouse(input).empty());
    t.feed("\x1b[?1000h\x1b[?1006h"); CHECK(t.encode_mouse(input) == "\x1b[<0;3;2M");
    input.action = MouseAction::release; CHECK(t.encode_mouse(input) == "\x1b[<0;3;2m");
    CHECK(t.snapshot()->mouse_reporting);
}
void selection() {
    TerminalCore t(small()); t.feed("hello world\r\nsecond");
    t.select(0,0,4,0); CHECK(t.selection_text() == "hello");
    bool capped{}; try { (void)t.selection_text(4); } catch (const std::invalid_argument&) { capped = true; }
    CHECK(capped);
    auto selected = t.snapshot(); CHECK(selected->lines[0]->selection.has_value());
    CHECK(selected->lines[0]->cells[0].selected && !selected->lines[0]->cells[5].selected);
    t.clear_selection(); CHECK(t.selection_text().empty()); CHECK(!t.snapshot()->lines[0]->cells[0].selected);
    CHECK(selected->lines[0]->cells[0].selected);
}
void scrollback() {
    TerminalCore t(small(20,3)); t.feed("first\r\nsecond\r\nthird\r\nfourth");
    CHECK(line(t.snapshot(), 0) == "second");
    t.scroll_viewport(-1); CHECK(line(t.snapshot(),0) == "first");
    t.scroll_to_bottom(); CHECK(line(t.snapshot(),0) == "second");
}
void configured_scrollback_lines() {
    auto options = small(20,3); options.scrollback_lines = 10;
    TerminalCore t(options);
    std::string data;
    for (unsigned i = 0; i < 20000; ++i) data += std::to_string(i) + "\r\n";
    t.feed(data); t.scroll_viewport(-1000000);
    // Limits are page-granularity, so exact row counts are intentionally not asserted.
    CHECK(std::stoul(line(t.snapshot(), 0)) > 10000);
}
void clipboard_denied() {
    TerminalCore t(small()); t.feed("\x1b]52;c;SGVsbG8=\x1b\\");
    auto effects = t.take_effects();
    auto found = std::find_if(effects.begin(), effects.end(), [](const auto& effect) { return effect.kind == EffectKind::clipboard_write_denied; });
    CHECK(found != effects.end()); CHECK(found->clipboard.size() == 1); CHECK(found->clipboard[0].bytes == "Hello");
    t.feed("\x1b]52;c;?\x1b\\"); effects = t.take_effects();
    CHECK(std::any_of(effects.begin(), effects.end(), [](const auto& effect) { return effect.kind == EffectKind::clipboard_read_denied; }));
}
void image_ownership_generation() {
    FramePtr red;
    {
        TerminalCore t(small());
        t.feed("\x1b_Ga=T,f=32,i=42,s=1,v=1,c=2,r=2,q=2;/wAA/w==\x1b\\");
        red = t.snapshot(); CHECK(red->images.size() == 1);
        const auto& image = red->images[0]; CHECK(image.pixels->image_id == 42);
        CHECK(image.pixels->bytes == (std::vector<std::uint8_t>{255,0,0,255}));
        CHECK(image.columns == 2 && image.rows == 2); CHECK(image.pixel_width == 16 && image.pixel_height == 32);
        t.feed("X"); auto text_change = t.snapshot(); CHECK(text_change->images[0].pixels == image.pixels);
        t.feed("\x1b_Ga=T,f=32,i=42,s=1,v=1,c=2,r=2,q=2;AAD//w==\x1b\\");
        auto blue = t.snapshot(); CHECK(!blue->images.empty());
        CHECK(blue->images[0].pixels->generation != image.pixels->generation);
        CHECK(blue->images[0].pixels->bytes == (std::vector<std::uint8_t>{0,0,255,255}));
        CHECK(image.pixels->bytes[0] == 255);
    }
    CHECK(red->images[0].pixels->bytes[0] == 255);
}
void unicode_image_placeholders() {
    TerminalCore t(small());
    t.feed("\x1b_Ga=T,U=1,f=32,i=3,s=1,v=1,c=2,r=1,q=2;AP8A/w==\x1b\\"
           "\r\n\x1b[38;5;3m"
           "\xf4\x8e\xbb\xae\xcc\x85\xcc\x85"
           "\xf4\x8e\xbb\xae\xcc\x85\xcc\x8d"
           "\x1b[39m");
    const auto frame = t.snapshot(); CHECK(!frame->images.empty());
    CHECK(frame->images[0].kind == ImagePlacementKind::virtual_placeholder);
    CHECK(frame->images[0].row == 1 && frame->images[0].column == 0);
    CHECK(frame->images[0].pixels->bytes == (std::vector<std::uint8_t>{0,255,0,255}));
    CHECK(frame->lines[1]->cells[0].image_placeholder); CHECK(frame->lines[1]->cells[1].image_placeholder);
    CHECK(!frame->lines[1]->cells[2].image_placeholder);
}
void relative_image_placement() {
    TerminalCore t(small());
    t.feed("\x1b_Ga=T,f=32,i=11,p=1,s=1,v=1,c=2,r=2,C=1,q=2;/wAA/w==\x1b\\"
           "\x1b_Ga=T,f=32,i=12,p=2,s=1,v=1,c=1,r=1,P=11,Q=1,H=1,V=1,q=2;AAD//w==\x1b\\");
    const auto frame = t.snapshot(); CHECK(frame->images.size() == 2);
    auto found = std::find_if(frame->images.begin(), frame->images.end(), [](const auto& image) {
        return image.kind == ImagePlacementKind::relative;
    });
    CHECK(found != frame->images.end()); CHECK(found->column == 1 && found->row == 1);
    CHECK(found->pixel_width == 8 && found->pixel_height == 16);
    CHECK(found->source_width == 1 && found->source_height == 1);
}
void png_hook_rejection_and_preflight() {
    TerminalCore t(small()); const auto before = png_decoder_calls;
    t.feed("\x1b_Ga=T,f=100,i=33,q=2;"
           "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAA"
           "DUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==\x1b\\");
    CHECK(png_decoder_calls == before + 1); CHECK(t.snapshot()->images.empty());
    auto tiny_options = small(); tiny_options.image_budget_bytes = 3; TerminalCore tiny(tiny_options);
    tiny.feed("\x1b_Ga=T,f=100,i=35,q=2;"
              "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAA"
              "DUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==\x1b\\");
    CHECK(png_decoder_calls == before + 1); CHECK(tiny.snapshot()->images.empty());
    // Oversized IHDR must not reach the platform decoder. CRC validity is left
    // to the platform decoder; it must not be called for oversized dimensions.
    std::vector<std::uint8_t> oversized{137,80,78,71,13,10,26,10,0,0,0,13,'I','H','D','R',
        0,0,32,1, 0,0,0,1, 8,6,0,0,0, 0,0,0,0};
    t.feed("\x1b_Ga=T,f=100,i=34,q=2;" + base64(oversized) + "\x1b\\");
    CHECK(png_decoder_calls == before + 1); CHECK(t.snapshot()->images.empty());
}
void image_animation() {
    TerminalCore t(small());
    t.feed("\x1b_Ga=T,f=32,i=2,s=1,v=1,q=2;/wAA/w==\x1b\\"
           "\x1b_Ga=f,f=32,i=2,s=1,v=1,z=40,q=2;AAD//w==\x1b\\"
           "\x1b_Ga=a,i=2,r=1,z=30,q=2\x1b\\"
           "\x1b_Ga=a,i=2,s=3,q=2\x1b\\");
    const auto first = t.snapshot(); CHECK(first->images.size() == 1); CHECK(t.next_deadline().has_value());
    const auto next = t.snapshot(*t.next_deadline() + std::chrono::milliseconds(1));
    CHECK(next->images[0].pixels->generation != first->images[0].pixels->generation);
    CHECK(next->images[0].pixels->bytes != first->images[0].pixels->bytes);
    t.feed("\x1b_Ga=a,i=2,s=1,q=2\x1b\\"); (void)t.snapshot(); CHECK(!t.next_deadline());
}
void finished_output_document() {
    TerminalCore t(small());
    t.feed("before\x1b[?2026h\r\x1b[2Kfinal\x1b[");
    CHECK(t.render_held()); CHECK(line(t.snapshot(),0)=="before");
    t.finish_output(); t.finish_output();
    CHECK(!t.render_held()); CHECK(!t.next_deadline());
    const auto final=t.snapshot(); CHECK(line(final,0)=="final");
    CHECK(line(t.snapshot(TerminalCore::Clock::now()+std::chrono::hours(1)),0)=="final");
    bool rejected{}; try{t.feed("mnot delivered");}catch(const std::invalid_argument&){rejected=true;}
    CHECK(rejected); CHECK(line(t.snapshot(),0)=="final");
    t.resize({30,5,8,16}); t.select(0,0,4,0,false); CHECK(t.selection_text()=="final");
    t.set_default_colors({210,211,212,255},{10,11,12,255});
    CHECK(t.snapshot()->background==(Color{10,11,12,255})); CHECK(line(final,0)=="final");
    TerminalCore animation(small());
    animation.feed("\x1b_Ga=T,f=32,i=2,s=1,v=1,q=2;/wAA/w==\x1b\\"
                   "\x1b_Ga=f,f=32,i=2,s=1,v=1,z=40,q=2;AAD//w==\x1b\\"
                   "\x1b_Ga=a,i=2,r=1,z=30,q=2\x1b\\"
                   "\x1b_Ga=a,i=2,s=3,q=2\x1b\\");
    (void)animation.snapshot(); CHECK(animation.next_deadline());
    animation.finish_output(); const auto frozen=animation.snapshot(); CHECK(!animation.next_deadline());
    const auto later=animation.snapshot(TerminalCore::Clock::now()+std::chrono::hours(1));
    CHECK(frozen->images.size()==1&&later->images.size()==1);
    CHECK(frozen->images[0].pixels->generation==later->images[0].pixels->generation);
    CHECK(frozen->images[0].pixels->bytes==later->images[0].pixels->bytes);
}
void command_lifecycle() {
    TerminalCore t(small());
    const CommandSnapshot empty;
    t.feed("\x1b]133;A\a\x1b]133;B\a\x1b]133;D;9\a");
    CHECK(t.command_snapshot() == empty);
    // Both OSC terminators must survive every possible feed boundary.
    for (const std::string& terminator : {std::string("\a"), std::string("\x1b\\")}) {
        const std::string bytes = "\x1b]133;C" + terminator + "\x1b]133;D;17" + terminator;
        for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
            TerminalCore split(small());
            split.feed(std::string_view(bytes).substr(0, cut));
            split.feed(std::string_view(bytes).substr(cut));
            const auto state = split.command_snapshot();
            CHECK(state.phase == CommandPhase::completed);
            CHECK(state.command_generation == 1 && state.completed_generation == 1);
            CHECK(state.last_exit_code == 17);
        }
    }
    t.feed("\x1b]133;C\a\x1b]133;C\a\x1b]133;A\a\x1b]133;B\a");
    CHECK(t.command_snapshot().phase == CommandPhase::running);
    CHECK(t.command_snapshot().command_generation == 1);
    CHECK(t.command_snapshot().completed_generation == 0);
    CHECK(!t.command_snapshot().last_exit_code);
    t.feed("\x1b]133;D;0\a\x1b]133;D;99\a");
    CHECK(t.command_snapshot().last_exit_code == 0);
    const auto owned = t.command_snapshot();
    t.feed("\x1b]133;C\a\x1b]133;D\a\x1b]133;C\a");
    const auto running = t.command_snapshot();
    CHECK(running.phase == CommandPhase::running);
    CHECK(running.command_generation == 3 && running.completed_generation == 2);
    CHECK(!running.last_exit_code);
    CHECK(owned.phase == CommandPhase::completed && owned.command_generation == 1);
    TerminalCore other(small());
    CHECK(other.command_snapshot() == empty);
    other.feed("\x1b]133;C\a");
    CHECK(other.command_snapshot().command_generation == 1);
    CHECK(t.command_snapshot() == running);
    t.feed("\x1b" "c\x1b]133;D;0\a"); // RIS interrupts; stale D cannot complete.
    CHECK(t.command_snapshot().phase == CommandPhase::interrupted);
    CHECK(t.command_snapshot().completed_generation == 2);
    CHECK(!t.command_snapshot().last_exit_code);
    t.feed("\x1b]133;C\a\x1b]133;D;42\a\x1b]133;C\a");
    t.finish_output(); t.finish_output();
    const auto interrupted = t.command_snapshot();
    CHECK(interrupted.phase == CommandPhase::interrupted);
    CHECK(interrupted.command_generation == 5 && interrupted.completed_generation == 4);
    CHECK(interrupted.last_exit_code == 42); // Previous explicit result, not interrupted command.
    bool rejected{};
    try { t.feed("\x1b]133;D;0\a\x1b]133;C\a"); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected && t.command_snapshot() == interrupted);
    TerminalCore completed(small());
    completed.feed("\x1b]133;C\a\x1b]133;D;0\a"); completed.finish_output();
    CHECK(completed.command_snapshot().phase == CommandPhase::completed);
    TerminalCore startup(small()); startup.finish_output();
    CHECK(startup.command_snapshot() == empty);
}
void graphics_security() {
    TerminalOptions options = small(); options.image_budget_bytes = 0; TerminalCore disabled(options);
    disabled.feed("\x1b_Ga=T,f=32,i=42,s=1,v=1,q=2;/wAA/w==\x1b\\"); CHECK(disabled.snapshot()->images.empty());
    TerminalCore t(small());
    t.feed("\x1b_Ga=T,t=f,f=32,i=9,s=1,v=1,q=2;L2V0Yy9wYXNzd2Q=\x1b\\");
    CHECK(t.snapshot()->images.empty());
    bool rejected{}; try { install_png_decoder(nullptr); } catch (const std::logic_error&) { rejected = true; } CHECK(rejected);
}
void queue_budget_failure_is_explicit() {
    auto options = small(); options.effect_queue_limit_bytes = 512; options.clipboard_limit_bytes = 128;
    TerminalCore t(options); bool rejected{};
    try { t.feed(std::string(100, '\a')); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    rejected = false; try { (void)t.snapshot(); } catch (const std::runtime_error&) { rejected = true; } CHECK(rejected);
}
void thread_confinement() {
    TerminalCore t(small()); std::atomic<bool> rejected{};
    std::thread foreign([&] { try { t.feed("bad"); } catch (const std::logic_error&) { rejected = true; } });
    foreign.join(); CHECK(rejected.load()); CHECK(line(t.snapshot(), 0).empty());
}
} // namespace
int main() {
    install_png_decoder(rejecting_png_decoder);
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"split UTF-8/ANSI feeds", split_feeds}, {"fragmented stream equivalence", fragmented_stream_equivalence}, {"ANSI styles and default backgrounds", ansi_styles},
        {"configured theme defaults/query replies", configured_theme_defaults_and_queries},
        {"theme OSC/reset/alpha validation", theme_overrides_reset_and_validation},
        {"live theme defaults/cancel/explicit colors", live_theme_defaults},
        {"alternate buffer", alternate_buffer}, {"immutable row sharing", immutable_sharing},
        {"resize and reflow", resize_and_wrap}, {"ConPTY/POSIX resize policy", backend_resize_policy}, {"DEC2026 exact pre-hold capture", hold_captures_exact_boundary},
        {"DEC2026 timeout and resize release", hold_timeout_and_resize}, {"back-to-back render holds", back_to_back_holds},
        {"ordered query/title/cwd/bell effects", queries_and_effects}, {"negotiated key encoding", key_encoding},
        {"focus and paste encoding", focus_paste_encoding}, {"negotiated mouse encoding", mouse_encoding},
        {"owned selection state", selection}, {"scrollback viewport", scrollback}, {"configured scrollback line bound", configured_scrollback_lines}, {"clipboard denied and copied", clipboard_denied},
        {"owned image pixels and generation", image_ownership_generation}, {"Unicode image placeholders", unicode_image_placeholders},
        {"resolved relative image placement", relative_image_placement}, {"PNG hook rejection/preflight", png_hook_rejection_and_preflight}, {"image animation deadlines", image_animation},
        {"finished output retains a frozen readable document", finished_output_document},
        {"owned bounded OSC 133 command lifecycle", command_lifecycle},
        {"graphics security defaults", graphics_security}, {"explicit effect budget failure", queue_budget_failure_is_explicit},
        {"actor thread confinement", thread_confinement}
    };
    unsigned failed{};
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    std::cout << tests.size() - failed << '/' << tests.size() << " terminal-core tests passed (no native UI/ConPTY verification)\n";
    return failed ? 1 : 0;
}
