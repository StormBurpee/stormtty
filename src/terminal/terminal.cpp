#include "stormtty/terminal.hpp"
#include "terminal_search.hpp"
#include "terminal_frame_materializer.hpp"
#include <ghostty/vt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace stormtty {
namespace {
static_assert(Modifier::shift == GHOSTTY_MODS_SHIFT && Modifier::control == GHOSTTY_MODS_CTRL &&
              Modifier::alt == GHOSTTY_MODS_ALT && Modifier::super == GHOSTTY_MODS_SUPER &&
              Modifier::caps_lock == GHOSTTY_MODS_CAPS_LOCK && Modifier::num_lock == GHOSTTY_MODS_NUM_LOCK &&
              Modifier::right_shift == GHOSTTY_MODS_SHIFT_SIDE && Modifier::right_control == GHOSTTY_MODS_CTRL_SIDE &&
              Modifier::right_alt == GHOSTTY_MODS_ALT_SIDE && Modifier::right_super == GHOSTTY_MODS_SUPER_SIDE);
static_assert(GHOSTTY_KEY_Z - GHOSTTY_KEY_A == 25 && GHOSTTY_KEY_DIGIT_9 - GHOSTTY_KEY_DIGIT_0 == 9 &&
              GHOSTTY_KEY_F12 - GHOSTTY_KEY_F1 == 11 && GHOSTTY_KEY_NUMPAD_9 - GHOSTTY_KEY_NUMPAD_0 == 9);
void checked(GhosttyResult result, const char* operation) {
    if (result != GHOSTTY_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (Ghostty result " + std::to_string(result) + ")");
}
template<class T, void (*Free)(T)> struct Handle {
    T value{};
    Handle() = default;
    ~Handle() { if (value) Free(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(T replacement = {}) noexcept { if (value) Free(value); value = replacement; }
    operator T() const noexcept { return value; }
};
template<class T> T sized() { T value{}; value.size = sizeof(T); return value; }
Color color(GhosttyColorRgb value) { return {value.r, value.g, value.b, 255}; }
void valid_size(TerminalSize size) {
    if (!size.columns || !size.rows || !size.cell_width || !size.cell_height ||
        static_cast<std::uint64_t>(size.columns) * size.rows > 1024u * 1024u)
        throw std::invalid_argument("terminal geometry is empty or exceeds 1048576 cells");
}
std::string copied(GhosttyString value) {
    if (!value.len) return {};
    return {reinterpret_cast<const char*>(value.ptr), value.len};
}
std::atomic<PngDecoder> png_decoder{};
std::atomic<std::size_t> png_max_bytes{64u * 1024u * 1024u};
std::atomic<std::uint32_t> png_max_dimension{8192};
std::atomic<bool> terminal_created{};
// PNG hooks are process-global, but parsing is actor-thread confined. Carry the
// current terminal's stricter budget into its synchronous decoder callback.
thread_local std::uint64_t active_png_budget = std::numeric_limits<std::uint64_t>::max();
struct PngBudgetScope {
    std::uint64_t previous{active_png_budget};
    explicit PngBudgetScope(std::uint64_t budget) { active_png_budget = budget; }
    ~PngBudgetScope() { active_png_budget = previous; }
};
std::uint32_t big_endian(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
bool decode_png(void*, const GhosttyAllocator* allocator, const std::uint8_t* data,
                std::size_t length, GhosttySysImage* out) noexcept {
    try {
        const auto decoder = png_decoder.load();
        constexpr std::array<std::uint8_t, 8> signature{137,80,78,71,13,10,26,10};
        if (!decoder || length < 33 || !std::equal(signature.begin(), signature.end(), data) ||
            big_endian(data + 8) != 13 || std::memcmp(data + 12, "IHDR", 4) != 0) return false;
        const auto width = big_endian(data + 16), height = big_endian(data + 20);
        const auto budget = static_cast<std::size_t>(std::min<std::uint64_t>(png_max_bytes.load(), active_png_budget));
        const auto dimension = png_max_dimension.load();
        if (!width || !height || width > dimension || height > dimension) return false;
        const std::uint64_t bytes = std::uint64_t(width) * height * 4;
        if (bytes > budget) return false;
        auto decoded = decoder({data, length}, budget);
        if (!decoded || decoded->width != width || decoded->height != height || decoded->rgba.size() != bytes)
            return false;
        auto* allocated = ghostty_alloc(allocator, decoded->rgba.size());
        if (!allocated) return false;
        std::memcpy(allocated, decoded->rgba.data(), decoded->rgba.size());
        out->width = width; out->height = height; out->data = allocated; out->data_len = decoded->rgba.size();
        return true;
    } catch (...) { return false; }
}
GhosttyKey ghostty_key(Key key) {
    if (key >= Key::a && key <= Key::z)
        return static_cast<GhosttyKey>(GHOSTTY_KEY_A + static_cast<int>(key) - static_cast<int>(Key::a));
    if (key >= Key::digit0 && key <= Key::digit9)
        return static_cast<GhosttyKey>(GHOSTTY_KEY_DIGIT_0 + static_cast<int>(key) - static_cast<int>(Key::digit0));
    if (key >= Key::f1 && key <= Key::f12)
        return static_cast<GhosttyKey>(GHOSTTY_KEY_F1 + static_cast<int>(key) - static_cast<int>(Key::f1));
    if (key >= Key::numpad0 && key <= Key::numpad9)
        return static_cast<GhosttyKey>(GHOSTTY_KEY_NUMPAD_0 + static_cast<int>(key) - static_cast<int>(Key::numpad0));
#define KEY_MAP(a,b) case Key::a: return GHOSTTY_KEY_##b
    switch (key) {
    KEY_MAP(enter,ENTER); KEY_MAP(tab,TAB); KEY_MAP(backspace,BACKSPACE); KEY_MAP(escape,ESCAPE);
    KEY_MAP(space,SPACE); KEY_MAP(insert,INSERT); KEY_MAP(del,DELETE); KEY_MAP(home,HOME); KEY_MAP(end,END);
    KEY_MAP(page_up,PAGE_UP); KEY_MAP(page_down,PAGE_DOWN); KEY_MAP(left,ARROW_LEFT); KEY_MAP(right,ARROW_RIGHT);
    KEY_MAP(up,ARROW_UP); KEY_MAP(down,ARROW_DOWN); KEY_MAP(backquote,BACKQUOTE); KEY_MAP(backslash,BACKSLASH);
    KEY_MAP(bracket_left,BRACKET_LEFT); KEY_MAP(bracket_right,BRACKET_RIGHT); KEY_MAP(comma,COMMA);
    KEY_MAP(equal,EQUAL); KEY_MAP(minus,MINUS); KEY_MAP(period,PERIOD); KEY_MAP(quote,QUOTE);
    KEY_MAP(semicolon,SEMICOLON); KEY_MAP(slash,SLASH); KEY_MAP(numpad_enter,NUMPAD_ENTER);
    KEY_MAP(numpad_add,NUMPAD_ADD); KEY_MAP(numpad_subtract,NUMPAD_SUBTRACT); KEY_MAP(numpad_multiply,NUMPAD_MULTIPLY);
    KEY_MAP(numpad_divide,NUMPAD_DIVIDE); KEY_MAP(numpad_decimal,NUMPAD_DECIMAL);
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
#undef KEY_MAP
}
template<class Encode> std::string encode(Encode&& run) {
    // Mouse encoder records last cell only after successful output. Retry the
    // OUT_OF_SPACE case, never an already-successful stateful encode.
    std::array<char, 256> local{};
    std::size_t length{};
    auto result = run(local.data(), local.size(), &length);
    if (result == GHOSTTY_SUCCESS) return {local.data(), length};
    if (result != GHOSTTY_OUT_OF_SPACE) checked(result, "encode input");
    std::string out(length, '\0');
    checked(run(out.data(), out.size(), &length), "encode input");
    out.resize(length);
    return out;
}
} // namespace

void install_png_decoder(PngDecoder decoder, std::size_t max_decoded_bytes, std::uint32_t max_dimension) {
    if (terminal_created.load()) throw std::logic_error("install PNG decoder before creating terminals");
    if (!max_decoded_bytes || !max_dimension || max_dimension > 32768)
        throw std::invalid_argument("invalid PNG decoding limits");
    png_max_bytes = max_decoded_bytes;
    png_max_dimension = max_dimension;
    png_decoder = decoder;
    checked(ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG, decoder ? reinterpret_cast<const void*>(decode_png) : nullptr),
            "install PNG decoder");
}

struct TerminalCore::Impl {
    TerminalOptions options;
    TerminalSize geometry;
    const std::thread::id owner{std::this_thread::get_id()};
    Handle<GhosttyTerminal, ghostty_terminal_free> terminal;
    TerminalFrameMaterializer materializer; // Destroyed before terminal.
    Handle<GhosttyKeyEncoder, ghostty_key_encoder_free> key_encoder;
    Handle<GhosttyKeyEvent, ghostty_key_event_free> key_event;
    Handle<GhosttyMouseEncoder, ghostty_mouse_encoder_free> mouse_encoder;
    Handle<GhosttyMouseEvent, ghostty_mouse_event_free> mouse_event;
    std::vector<TerminalEffect> effects;
    std::size_t effect_bytes{};
    std::exception_ptr callback_failure;
    bool held{}, output_finished{};
    CommandSnapshot command;
    ShellContextState shell_context;
    Clock::time_point hold_started{}, animation_origin{Clock::now()}, last_tick{animation_origin};
    std::optional<Clock::time_point> animation_deadline;
    std::uint64_t content_revision{1};
    TerminalSearch search; // Declared after terminal: search is destroyed first.

    explicit Impl(TerminalOptions config) : options(config), geometry(config.size) {
        valid_size(geometry);
        if ((config.default_foreground && config.default_foreground->a != 255) ||
            (config.default_background && config.default_background->a != 255))
            throw std::invalid_argument("terminal default colors must be opaque RGB; apply opacity in the renderer");
        if (config.ansi) for (const auto c:*config.ansi) if (c.a!=255) throw std::invalid_argument("ANSI colors must be opaque RGB");
        if (config.render_hold_timeout <= std::chrono::milliseconds::zero() ||
            config.scrollback_budget_bytes > std::numeric_limits<std::size_t>::max() ||
            config.effect_queue_limit_bytes < 256 || config.clipboard_limit_bytes > config.effect_queue_limit_bytes)
            throw std::invalid_argument("invalid terminal resource limits");
        bool kitty{};
        checked(ghostty_build_info(GHOSTTY_BUILD_INFO_KITTY_GRAPHICS, &kitty), "query graphics capability");
        if (config.image_budget_bytes && !kitty) throw std::runtime_error("libghostty-vt was built without required Kitty graphics");
        terminal_created = true;
        checked(ghostty_terminal_new(nullptr, &terminal.value, geometry.columns, geometry.rows), "create terminal");
        materializer.initialize();
        checked(ghostty_key_encoder_new(nullptr, &key_encoder.value), "create key encoder");
        checked(ghostty_key_event_new(nullptr, &key_event.value), "create key event");
        checked(ghostty_mouse_encoder_new(nullptr, &mouse_encoder.value), "create mouse encoder");
        checked(ghostty_mouse_event_new(nullptr, &mouse_event.value), "create mouse event");
        set(GHOSTTY_TERMINAL_OPT_USERDATA, this);
        set(GHOSTTY_TERMINAL_OPT_WRITE_PTY, reinterpret_cast<const void*>(on_write));
        set(GHOSTTY_TERMINAL_OPT_BELL, reinterpret_cast<const void*>(on_bell));
        set(GHOSTTY_TERMINAL_OPT_TITLE_CHANGED, reinterpret_cast<const void*>(on_title));
        set(GHOSTTY_TERMINAL_OPT_PWD_CHANGED, reinterpret_cast<const void*>(on_pwd));
        set(GHOSTTY_TERMINAL_OPT_CLIPBOARD_READ, reinterpret_cast<const void*>(on_clipboard_read));
        set(GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE, reinterpret_cast<const void*>(on_clipboard_write));
        set(GHOSTTY_TERMINAL_OPT_RENDER_HOLD, reinterpret_cast<const void*>(on_hold));
        set(GHOSTTY_TERMINAL_OPT_SEMANTIC_PROMPT, reinterpret_cast<const void*>(on_semantic_prompt));
        set(GHOSTTY_TERMINAL_OPT_RESET, reinterpret_cast<const void*>(on_reset));
        set(GHOSTTY_TERMINAL_OPT_UNKNOWN_SEQUENCE, reinterpret_cast<const void*>(on_unknown));
        set(GHOSTTY_TERMINAL_OPT_UNKNOWN_MAX_BYTES, &shell_context_wire_limit);
        // The pinned renderer only refreshes fg/bg when BOTH engine defaults
        // exist. Materialize unspecified defaults from its initial colors so a
        // one-sided theme still renders and answers OSC queries consistently.
        auto initial_colors = materializer.initial_colors();
        const auto set_default_color = [&](GhosttyTerminalOption option, Color value) {
            const GhosttyColorRgb rgb{value.r, value.g, value.b};
            set(option, &rgb);
        };
        set_default_color(GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, options.default_foreground.value_or(color(initial_colors.foreground)));
        set_default_color(GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, options.default_background.value_or(color(initial_colors.background)));
        if (options.ansi) {
            auto palette=initial_colors.palette;
            for (std::size_t i=0;i<16;++i) { const auto c=(*options.ansi)[i]; palette[i]={c.r,c.g,c.b}; }
            set(GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
        }
        set(GHOSTTY_TERMINAL_OPT_RESIZE_PULL_SCROLLBACK, &options.resize_pull_scrollback);
        set(GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &options.image_budget_bytes);
        const bool disabled = false;
        set(GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &disabled);
        set(GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE, nullptr);
        set(GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM, &disabled);
        const auto scrollback = static_cast<std::size_t>(options.scrollback_budget_bytes);
        set(GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &scrollback);
        if (options.scrollback_lines) set(GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &*options.scrollback_lines);
        set(GHOSTTY_TERMINAL_OPT_APC_MAX_BYTES, &options.apc_limit_bytes);
        set(GHOSTTY_TERMINAL_OPT_APC_MAX_BYTES_KITTY, &options.apc_limit_bytes);
        set(GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE_MAX_BYTES, &options.clipboard_limit_bytes);
        checked(ghostty_terminal_resize(terminal, geometry.columns, geometry.rows, geometry.cell_width, geometry.cell_height), "set cell geometry");
        search.initialize(terminal, content_revision);
    }
    void check_thread() const {
        if (owner != std::this_thread::get_id()) throw std::logic_error("TerminalCore used outside its owner thread");
        if (callback_failure) std::rethrow_exception(callback_failure);
    }
    void set(GhosttyTerminalOption option, const void* value) { checked(ghostty_terminal_set(terminal, option, value), "set terminal option"); }
    bool mode(GhosttyMode mode_value) const {
        GhosttyTerminalModeConfig config{mode_value, false};
        checked(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MODE, &config), "query terminal mode");
        return config.value;
    }
    template<class Work> void callback(Work&& work) noexcept {
        if (callback_failure) return;
        try { work(); } catch (...) { callback_failure = std::current_exception(); }
    }
    void enqueue(TerminalEffect effect) {
        std::size_t bytes = sizeof(TerminalEffect) + effect.text.size();
        for (const auto& item : effect.clipboard) bytes += sizeof(ClipboardItem) + item.mime.size() + item.bytes.size();
        if (effects.size() >= 4096 || bytes > options.effect_queue_limit_bytes - effect_bytes)
            throw std::runtime_error("terminal effect queue budget exhausted; session must stop, not drop protocol replies");
        effect_bytes += bytes;
        effects.push_back(std::move(effect));
    }
    void interrupt_command() noexcept {
        if (command.phase == CommandPhase::running) command.phase = CommandPhase::interrupted;
    }
    static void on_reset(GhosttyTerminal, void* data) noexcept {
        auto& self = *static_cast<Impl*>(data);
        self.interrupt_command();
        self.shell_context.reset();
    }
    static void on_unknown(GhosttyTerminal, void* data,
                           const GhosttyTerminalUnknownSequence* sequence) noexcept {
        auto& self = *static_cast<Impl*>(data);
        if (self.output_finished || !sequence || sequence->tag != GHOSTTY_TERMINAL_UNKNOWN_SEQUENCE_OSC) return;
        self.callback([&] {
            const auto& osc = sequence->value.osc;
            // Borrowed bytes exist only during this callback. The parser owns
            // accepted fields; Ghostty owns framing, cancellation and the cap.
            const std::string_view content{reinterpret_cast<const char*>(osc.content.ptr), osc.content.len};
            self.shell_context.apply(parse_shell_context_osc(content, osc.truncated));
        });
    }
    static void on_semantic_prompt(GhosttyTerminal, void* data,
                                   const GhosttyTerminalSemanticPrompt* event) noexcept {
        auto& self = *static_cast<Impl*>(data);
        if (self.output_finished || !event) return;
        auto& state = self.command;
        // Copy scalar values synchronously; never inspect/copy command or error.
        // A/B can redraw or appear at startup, so neither can end a command.
        if (event->kind == GHOSTTY_SEMANTIC_PROMPT_OUTPUT_START) {
            if (state.phase == CommandPhase::running) return; // Duplicate C.
            // Saturation disables further observation rather than wrapping IDs.
            if (state.command_generation == UINT64_MAX) return;
            ++state.command_generation;
            state.phase = CommandPhase::running;
        } else if (event->kind == GHOSTTY_SEMANTIC_PROMPT_COMMAND_END &&
                   state.phase == CommandPhase::running) {
            state.phase = CommandPhase::completed;
            state.completed_generation = state.command_generation;
            state.last_exit_code = event->has_exit_code ? std::optional<std::int32_t>(event->exit_code) : std::nullopt;
        }
    }
    static void on_write(GhosttyTerminal, void* data, const std::uint8_t* bytes, std::size_t length) noexcept {
        auto& self = *static_cast<Impl*>(data);
        self.callback([&] {
            TerminalEffect effect; effect.kind = EffectKind::write_pty;
            if (length) effect.text.assign(reinterpret_cast<const char*>(bytes), length);
            self.enqueue(std::move(effect));
        });
    }
    static void on_bell(GhosttyTerminal, void* data) noexcept {
        auto& self = *static_cast<Impl*>(data);
        self.callback([&] { self.enqueue(TerminalEffect{}); });
    }
    void string_effect(GhosttyTerminalData field, EffectKind kind) {
        GhosttyString value{};
        checked(ghostty_terminal_get(terminal, field, &value), "read terminal effect string");
        TerminalEffect effect; effect.kind = kind;
        constexpr std::size_t max_label = 64u * 1024u;
        effect.truncated = value.len > max_label;
        value.len = std::min(value.len, max_label);
        effect.text = copied(value);
        enqueue(std::move(effect));
    }
    static void on_title(GhosttyTerminal, void* data) noexcept {
        auto& self = *static_cast<Impl*>(data);
        self.callback([&] { self.string_effect(GHOSTTY_TERMINAL_DATA_TITLE, EffectKind::title); });
    }
    static void on_pwd(GhosttyTerminal, void* data) noexcept {
        auto& self = *static_cast<Impl*>(data);
        self.callback([&] {
            GhosttyString pwd{};
            checked(ghostty_terminal_get(self.terminal, GHOSTTY_TERMINAL_DATA_PWD, &pwd), "read shell context directory");
            // Correlate synchronously before the next OSC in this same feed.
            self.shell_context.observe_directory({reinterpret_cast<const char*>(pwd.ptr), pwd.len});
            self.string_effect(GHOSTTY_TERMINAL_DATA_PWD, EffectKind::working_directory);
        });
    }
    static void on_clipboard_write(GhosttyTerminal, void* data, const GhosttyClipboardWrite* request) noexcept {
        auto& self = *static_cast<Impl*>(data);
        self.callback([&] {
            TerminalEffect effect; effect.kind = EffectKind::clipboard_write_denied;
            effect.clipboard_location = static_cast<ClipboardLocation>(request->location);
            std::size_t budget = self.options.clipboard_limit_bytes;
            for (std::size_t i = 0; i < request->contents_len; ++i) {
                const auto& item = request->contents[i];
                if (effect.clipboard.size() >= 256) { effect.truncated = true; break; }
                if (item.mime.len > budget || item.data.len > budget - item.mime.len) { effect.truncated = true; break; }
                budget -= item.mime.len + item.data.len;
                effect.clipboard.push_back({copied(item.mime), copied(item.data)});
            }
            self.enqueue(std::move(effect));
        });
        auto reply = sized<GhosttyClipboardWriteReply>();
        reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_DENIED;
        request->reply(request, &reply);
    }
    static void on_clipboard_read(GhosttyTerminal, void* data, const GhosttyClipboardRead* request) noexcept {
        auto& self = *static_cast<Impl*>(data);
        self.callback([&] {
            TerminalEffect effect; effect.kind = EffectKind::clipboard_read_denied;
            effect.clipboard_location = static_cast<ClipboardLocation>(request->location);
            std::size_t budget = self.options.clipboard_limit_bytes;
            for (std::size_t i = 0; i < request->mimes_len; ++i) {
                if (effect.clipboard.size() >= 256) { effect.truncated = true; break; }
                if (request->mimes[i].len > budget) { effect.truncated = true; break; }
                budget -= request->mimes[i].len;
                effect.clipboard.push_back({copied(request->mimes[i]), {}});
            }
            self.enqueue(std::move(effect));
        });
        auto reply = sized<GhosttyClipboardReadReply>();
        reply.result = GHOSTTY_CLIPBOARD_READ_RESULT_DENIED;
        request->reply(request, &reply);
    }
    static void on_hold(GhosttyTerminal, void* data, bool hold) noexcept {
        auto& self = *static_cast<Impl*>(data);
        self.callback([&] {
            if (hold) {
                // Explicitly allowed by Ghostty's render-hold callback contract.
                // Capture NOW, before remaining bytes in this same feed mutate VT.
                self.materializer.capture(self.terminal, self.geometry, self.options.image_budget_bytes);
                self.hold_started = Clock::now();
            }
            self.held = hold;
        });
    }
    GhosttyKittyGraphics graphics() {
        GhosttyKittyGraphics result{};
        checked(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &result), "get image storage");
        return result; // Borrowed only inside this actor operation, never published.
    }
    void tick(Clock::time_point now) {
        if (output_finished) return;
        now = std::max(now, last_tick); last_tick = now;
        if (held && now >= hold_started + options.render_hold_timeout) {
            GhosttyTerminalModeConfig config{GHOSTTY_MODE_SYNC_OUTPUT, false};
            set(GHOSTTY_TERMINAL_OPT_MODE, &config);
            held = false;
        }
        if (held || !options.image_budget_bytes) return;
        std::uint64_t next{};
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - animation_origin).count();
        const auto result = ghostty_kitty_graphics_animation_tick(graphics(), static_cast<std::uint64_t>(elapsed), &next);
        // Rate-limit animation wakeups to at most 120 Hz, including overdue frames.
        if (result == GHOSTTY_SUCCESS) animation_deadline = std::max(now + std::chrono::milliseconds(8),
            animation_origin + std::chrono::milliseconds(next));
        else if (result == GHOSTTY_NO_VALUE) animation_deadline.reset();
        else checked(result, "tick image animation");
    }
};

TerminalCore::TerminalCore(TerminalOptions options) : impl_(std::make_unique<Impl>(options)) {}
TerminalCore::~TerminalCore() = default;
void TerminalCore::feed(std::string_view bytes) {
    feed(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}
void TerminalCore::feed(std::span<const std::uint8_t> bytes) {
    impl_->check_thread();
    if (impl_->output_finished) throw std::invalid_argument("terminal output stream has finished");
    const PngBudgetScope png_scope(impl_->options.image_budget_bytes);
    if (!bytes.empty()) {
        ++impl_->content_revision;
        ghostty_terminal_vt_write(impl_->terminal, bytes.data(), bytes.size());
    }
    impl_->check_thread();
}
void TerminalCore::resize(TerminalSize size) {
    impl_->check_thread(); valid_size(size);
    if (size == impl_->geometry) return;
    ++impl_->content_revision;
    checked(ghostty_terminal_resize(impl_->terminal, size.columns, size.rows, size.cell_width, size.cell_height), "resize terminal");
    impl_->geometry = size;
    ghostty_mouse_encoder_reset(impl_->mouse_encoder);
    impl_->check_thread();
}
void TerminalCore::set_default_colors(Color foreground, Color background, std::optional<std::array<Color,16>> ansi) {
    impl_->check_thread();
    if (foreground.a != 255 || background.a != 255)
        throw std::invalid_argument("terminal default colors must be opaque RGB; apply opacity in the renderer");
    if (ansi) for (const auto c:*ansi) if (c.a!=255) throw std::invalid_argument("ANSI colors must be opaque RGB");
    if (impl_->options.default_foreground == foreground && impl_->options.default_background == background && (!ansi || impl_->options.ansi==ansi)) return;
    const GhosttyColorRgb fg{foreground.r, foreground.g, foreground.b}, bg{background.r, background.g, background.b};
    // These options change defaults, not the effective OSC override or cell
    // styles. The pinned engine marks the palette dirty for render-state update.
    impl_->set(GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
    impl_->set(GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
    if (ansi) {
        GhosttyColorRgb palette[256];
        checked(ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT, &palette), "read default palette");
        for (std::size_t i=0;i<16;++i) { const auto c=(*ansi)[i]; palette[i]={c.r,c.g,c.b}; }
        impl_->set(GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, &palette);
        impl_->options.ansi=ansi;
    }
    impl_->options.default_foreground = foreground;
    impl_->options.default_background = background;
}
std::string TerminalCore::encode_key(const KeyInput& input) {
    impl_->check_thread();
    const auto event = impl_->key_event.value;
    ghostty_key_event_set_key(event, ghostty_key(input.key));
    ghostty_key_event_set_action(event, input.action == KeyAction::release ? GHOSTTY_KEY_ACTION_RELEASE :
                                       input.action == KeyAction::repeat ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS);
    ghostty_key_event_set_mods(event, input.modifiers);
    ghostty_key_event_set_consumed_mods(event, input.consumed_modifiers);
    ghostty_key_event_set_composing(event, input.composing);
    ghostty_key_event_set_unshifted_codepoint(event, input.unshifted_codepoint);
    ghostty_key_event_set_utf8(event, input.text.data(), input.text.size());
    struct ClearBorrowedText {
        GhosttyKeyEvent event;
        ~ClearBorrowedText() { ghostty_key_event_set_utf8(event, nullptr, 0); }
    } clear_borrowed{event};
    ghostty_key_encoder_setopt_from_terminal(impl_->key_encoder, impl_->terminal);
    return encode([&](char* out, std::size_t size, std::size_t* written) {
        return ghostty_key_encoder_encode(impl_->key_encoder, event, out, size, written);
    });
}
std::string TerminalCore::encode_mouse(const MouseInput& input) {
    impl_->check_thread();
    if (!std::isfinite(input.x) || !std::isfinite(input.y) ||
        std::abs(input.x) > std::numeric_limits<float>::max() || std::abs(input.y) > std::numeric_limits<float>::max()) throw std::invalid_argument("invalid mouse coordinates");
    const auto event = impl_->mouse_event.value;
    ghostty_mouse_event_set_action(event, input.action == MouseAction::press ? GHOSTTY_MOUSE_ACTION_PRESS :
        input.action == MouseAction::release ? GHOSTTY_MOUSE_ACTION_RELEASE : GHOSTTY_MOUSE_ACTION_MOTION);
    if (input.button == MouseButton::none) ghostty_mouse_event_clear_button(event);
    else {
        GhosttyMouseButton button = GHOSTTY_MOUSE_BUTTON_LEFT;
        switch (input.button) {
        case MouseButton::right: button = GHOSTTY_MOUSE_BUTTON_RIGHT; break;
        case MouseButton::middle: button = GHOSTTY_MOUSE_BUTTON_MIDDLE; break;
        case MouseButton::wheel_up: button = GHOSTTY_MOUSE_BUTTON_FOUR; break;
        case MouseButton::wheel_down: button = GHOSTTY_MOUSE_BUTTON_FIVE; break;
        case MouseButton::wheel_left: button = GHOSTTY_MOUSE_BUTTON_SIX; break;
        case MouseButton::wheel_right: button = GHOSTTY_MOUSE_BUTTON_SEVEN; break;
        default: break;
        }
        ghostty_mouse_event_set_button(event, button);
    }
    ghostty_mouse_event_set_mods(event, input.modifiers);
    ghostty_mouse_event_set_position(event, {static_cast<float>(input.x), static_cast<float>(input.y)});
    ghostty_mouse_encoder_setopt_from_terminal(impl_->mouse_encoder, impl_->terminal);
    auto size = sized<GhosttyMouseEncoderSize>();
    size.screen_width = std::uint32_t(impl_->geometry.columns) * impl_->geometry.cell_width;
    size.screen_height = std::uint32_t(impl_->geometry.rows) * impl_->geometry.cell_height;
    size.cell_width = impl_->geometry.cell_width; size.cell_height = impl_->geometry.cell_height;
    ghostty_mouse_encoder_setopt(impl_->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
    ghostty_mouse_encoder_setopt(impl_->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &input.any_button_pressed);
    return encode([&](char* out, std::size_t capacity, std::size_t* written) {
        return ghostty_mouse_encoder_encode(impl_->mouse_encoder, event, out, capacity, written);
    });
}
std::string TerminalCore::encode_focus(bool focused) {
    impl_->check_thread();
    if (!impl_->mode(GHOSTTY_MODE_FOCUS_EVENT)) return {};
    return encode([&](char* out, std::size_t size, std::size_t* written) {
        return ghostty_focus_encode(focused ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST, out, size, written);
    });
}
bool TerminalCore::paste_is_safe(std::string_view text) noexcept { return ghostty_paste_is_safe(text.empty() ? "" : text.data(), text.size()); }
std::string TerminalCore::encode_paste(std::string_view text, bool allow_unsafe) {
    impl_->check_thread();
    if (!allow_unsafe && !paste_is_safe(text)) throw std::invalid_argument("paste requires confirmation of unsafe bytes");
    const bool bracketed = impl_->mode(GHOSTTY_MODE_BRACKETED_PASTE);
    // The C encoder mutates input, so use a fresh private copy on each attempt.
    return encode([&](char* out, std::size_t size, std::size_t* written) {
        std::string owned(text);
        return ghostty_paste_encode(owned.data(), owned.size(), bracketed, out, size, written);
    });
}
void TerminalCore::select(std::uint16_t start_column, std::uint16_t start_row,
                          std::uint16_t end_column, std::uint16_t end_row, bool rectangle) {
    impl_->check_thread();
    if (start_column >= impl_->geometry.columns || end_column >= impl_->geometry.columns ||
        start_row >= impl_->geometry.rows || end_row >= impl_->geometry.rows) throw std::invalid_argument("selection outside viewport");
    GhosttyPoint start{}, end{};
    start.tag = end.tag = GHOSTTY_POINT_TAG_VIEWPORT;
    start.value.coordinate = {start_column, start_row}; end.value.coordinate = {end_column, end_row};
    auto selection = sized<GhosttySelection>();
    selection.start = sized<GhosttyGridRef>();
    selection.end = sized<GhosttyGridRef>();
    checked(ghostty_terminal_grid_ref(impl_->terminal, start, &selection.start), "selection start");
    checked(ghostty_terminal_grid_ref(impl_->terminal, end, &selection.end), "selection end");
    selection.rectangle = rectangle;
    impl_->set(GHOSTTY_TERMINAL_OPT_SELECTION, &selection);
}
void TerminalCore::clear_selection() { impl_->check_thread(); impl_->set(GHOSTTY_TERMINAL_OPT_SELECTION, nullptr); }
std::string TerminalCore::selection_text(std::size_t max_bytes) {
    impl_->check_thread();
    auto options = sized<GhosttyTerminalSelectionFormatOptions>();
    options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN; options.unwrap = true; options.trim = true;
    std::size_t length{};
    const auto result = ghostty_terminal_selection_format_buf(impl_->terminal, options, nullptr, 0, &length);
    if (result == GHOSTTY_NO_VALUE) return {};
    if (result != GHOSTTY_SUCCESS && result != GHOSTTY_OUT_OF_SPACE) checked(result, "measure selection text");
    if (length > max_bytes) throw std::invalid_argument("selection exceeds clipboard safety limit");
    std::string text(length, '\0');
    checked(ghostty_terminal_selection_format_buf(impl_->terminal, options,
        reinterpret_cast<std::uint8_t*>(text.data()), text.size(), &length), "format selection text");
    text.resize(length);
    return text;
}

void TerminalCore::scroll_viewport(std::int64_t rows) {
    impl_->check_thread();
    GhosttyTerminalScrollViewport scroll{}; scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
    scroll.value.delta = static_cast<std::intptr_t>(std::clamp(rows, static_cast<std::int64_t>(std::numeric_limits<std::intptr_t>::min()),
                                                           static_cast<std::int64_t>(std::numeric_limits<std::intptr_t>::max())));
    ghostty_terminal_scroll_viewport(impl_->terminal, scroll);
}
void TerminalCore::scroll_to_bottom() {
    impl_->check_thread();
    GhosttyTerminalScrollViewport scroll{}; scroll.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
    ghostty_terminal_scroll_viewport(impl_->terminal, scroll);
}
std::optional<ShellContext> TerminalCore::shell_context() const {
    impl_->check_thread();
    return impl_->shell_context.context();
}
CommandSnapshot TerminalCore::command_snapshot() const {
    impl_->check_thread();
    return impl_->command;
}
void TerminalCore::finish_output() {
    impl_->check_thread();
    if (impl_->output_finished) return;
    if (impl_->held) {
        GhosttyTerminalModeConfig config{GHOSTTY_MODE_SYNC_OUTPUT, false};
        impl_->set(GHOSTTY_TERMINAL_OPT_MODE, &config);
        impl_->held = false;
    }
    impl_->tick(Clock::now());
    impl_->output_finished = true;
    impl_->interrupt_command();
    impl_->animation_deadline.reset();
}
FramePtr TerminalCore::snapshot(Clock::time_point now) {
    impl_->check_thread(); impl_->tick(now);
    if (impl_->held) {
        if (auto frame = impl_->materializer.last_frame()) return frame;
    }
    return impl_->materializer.capture(impl_->terminal, impl_->geometry, impl_->options.image_budget_bytes);
}
std::optional<TerminalCore::Clock::time_point> TerminalCore::next_deadline() const {
    impl_->check_thread();
    if (impl_->held) return impl_->hold_started + impl_->options.render_hold_timeout;
    return impl_->animation_deadline;
}
bool TerminalCore::render_held() const { impl_->check_thread(); return impl_->held; }
TerminalSize TerminalCore::size() const { impl_->check_thread(); return impl_->geometry; }
std::vector<TerminalEffect> TerminalCore::take_effects() {
    impl_->check_thread();
    std::vector<TerminalEffect> result;
    result.swap(impl_->effects); impl_->effect_bytes = 0;
    return result;
}

void TerminalCore::begin_search(std::uint64_t request_id, SearchOptions options) {
    impl_->check_thread();
    impl_->search.begin(impl_->terminal, impl_->content_revision, request_id, std::move(options));
}

void TerminalCore::cancel_search(std::uint64_t request_id) {
    impl_->check_thread();
    impl_->search.cancel(impl_->terminal, impl_->content_revision, request_id);
}

SearchSnapshotPtr TerminalCore::search_snapshot() const {
    impl_->check_thread();
    return impl_->search.snapshot();
}

std::uint64_t TerminalCore::revision() const {
    impl_->check_thread();
    return impl_->content_revision;
}

bool TerminalCore::search_needs_work() const {
    impl_->check_thread();
    return impl_->search.needs_work(impl_->content_revision);
}

SearchSnapshotPtr TerminalCore::search_step() {
    impl_->check_thread();
    return impl_->search.step(impl_->terminal, impl_->content_revision);
}

SearchApplyResult TerminalCore::select_search_match(SearchToken token, std::size_t index) {
    impl_->check_thread();
    auto& self = *impl_;
    const auto choice = self.search.choose(token, self.content_revision, index);
    if (choice.result != SearchApplyResult::applied)
        return self.search.record_action(choice.result);
    const auto match = choice.match;
    GhosttyPoint start{}, end{};
    start.tag = end.tag = GHOSTTY_POINT_TAG_SCREEN;
    start.value.coordinate = {match.start.column, match.start.row};
    end.value.coordinate = {match.end.column, match.end.row};
    auto selection = sized<GhosttySelection>();
    selection.start = sized<GhosttyGridRef>();
    selection.end = sized<GhosttyGridRef>();
    if (ghostty_terminal_grid_ref(self.terminal, start, &selection.start) != GHOSTTY_SUCCESS ||
        ghostty_terminal_grid_ref(self.terminal, end, &selection.end) != GHOSTTY_SUCCESS)
        return self.search.record_action(SearchApplyResult::stale);
    self.set(GHOSTTY_TERMINAL_OPT_SELECTION, &selection);
    GhosttyTerminalScrollViewport viewport{};
    viewport.tag = GHOSTTY_SCROLL_VIEWPORT_ROW;
    viewport.value.row = match.start.row > self.geometry.rows / 2 ? match.start.row - self.geometry.rows / 2 : 0;
    ghostty_terminal_scroll_viewport(self.terminal, viewport);
    if (self.held) {
        GhosttyTerminalModeConfig config{GHOSTTY_MODE_SYNC_OUTPUT, false};
        self.set(GHOSTTY_TERMINAL_OPT_MODE, &config);
        self.held = false;
    }
    return self.search.record_action(SearchApplyResult::applied, index);
}

SearchApplyResult TerminalCore::navigate_search(SearchToken token, SearchDirection direction, bool wrap) {
    impl_->check_thread();
    auto& self = *impl_;
    const auto choice = self.search.choose(token, self.content_revision, direction, wrap);
    if (choice.result != SearchApplyResult::applied)
        return self.search.record_action(choice.result);
    return select_search_match(token, choice.index);
}

} // namespace stormtty
