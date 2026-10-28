#include "stormtty/terminal.hpp"
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
#include <unordered_map>
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
Color resolve(GhosttyStyleColor value, const GhosttyRenderStateColors& palette, Color fallback) {
    switch (value.tag) {
    case GHOSTTY_STYLE_COLOR_RGB: return color(value.value.rgb);
    case GHOSTTY_STYLE_COLOR_PALETTE: return color(palette.palette[value.value.palette]);
    default: return fallback;
    }
}
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
// Each search gets its own capped allocator. Upstream search feed/tick can log
// and swallow OOM, so rejection flags are checked after EVERY engine operation.
// Failed budgets must never masquerade as a complete search with missing hits.
struct SearchAllocator {
    std::size_t limit{}, used{}, peak{};
    bool limited{}, failed{};
    static void* allocate(void* context, std::size_t length, std::uint8_t alignment, std::uintptr_t) noexcept {
        auto& self = *static_cast<SearchAllocator*>(context);
        if (length > self.limit - self.used) { self.limited = true; return nullptr; }
        if (alignment >= std::numeric_limits<std::size_t>::digits - 1) { self.failed = true; return nullptr; }
        const auto bytes = std::max<std::size_t>(alignof(std::max_align_t), std::size_t{1} << alignment);
        void* memory = ::operator new(length, std::align_val_t(bytes), std::nothrow);
        if (!memory) { self.failed = true; return nullptr; }
        self.used += length; self.peak = std::max(self.peak, self.used);
        return memory;
    }
    // Always fall back to allocate/copy/free so peak live bytes are accounted
    // accurately, including shrinks (no retained larger physical allocation).
    static bool resize(void*, void*, std::size_t, std::uint8_t, std::size_t, std::uintptr_t) noexcept { return false; }
    static void* remap(void*, void*, std::size_t, std::uint8_t, std::size_t, std::uintptr_t) noexcept { return nullptr; }
    static void release(void* context, void* memory, std::size_t length, std::uint8_t alignment, std::uintptr_t) noexcept {
        auto& self = *static_cast<SearchAllocator*>(context);
        self.used -= length;
        const auto bytes = std::max<std::size_t>(alignof(std::max_align_t), std::size_t{1} << alignment);
        ::operator delete(memory, std::align_val_t(bytes));
    }
    static constexpr GhosttyAllocatorVtable vtable{allocate, resize, remap, release};
    GhosttyAllocator abi{this, &vtable};
};
bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned count{}; std::uint32_t scalar{}, minimum{};
        if (first >= 0xc2 && first <= 0xdf) { count = 1; scalar = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; scalar = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; scalar = first & 7; minimum = 0x10000; }
        else return false;
        if (count > text.size() - i) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
    }
    return true;
}
struct RowHash {
    std::size_t operator()(const RowId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.words[0]) ^ (std::hash<std::uint64_t>{}(id.words[1]) << 1);
    }
};
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
    Handle<GhosttyRenderState, ghostty_render_state_free> render;
    Handle<GhosttyRenderStateRowIterator, ghostty_render_state_row_iterator_free> rows;
    Handle<GhosttyRenderStateRowCells, ghostty_render_state_row_cells_free> cells;
    Handle<GhosttyRenderStateImageIterator, ghostty_render_state_image_iterator_free> images;
    Handle<GhosttyKeyEncoder, ghostty_key_encoder_free> key_encoder;
    Handle<GhosttyKeyEvent, ghostty_key_event_free> key_event;
    Handle<GhosttyMouseEncoder, ghostty_mouse_encoder_free> mouse_encoder;
    Handle<GhosttyMouseEvent, ghostty_mouse_event_free> mouse_event;
    std::vector<TerminalEffect> effects;
    std::size_t effect_bytes{};
    std::exception_ptr callback_failure;
    FramePtr frame;
    std::uint64_t frame_generation{}, row_generation{};
    bool held{};
    Clock::time_point hold_started{}, animation_origin{Clock::now()}, last_tick{animation_origin};
    std::optional<Clock::time_point> animation_deadline;
    std::uint64_t content_revision{1}, search_request_id{}, search_fed_revision{}, search_work{}, search_update{}, search_generation{};
    SearchAllocator search_memory; // Must outlive the handle using it.
    Handle<GhosttySearch, ghostty_search_free> search;
    SearchOptions search_options;
    SearchSnapshotPtr search_result;
    std::vector<SearchMatch> search_building;
    std::size_t search_count{};
    bool search_materializing{};
    std::optional<std::size_t> search_selected;

    SearchSnapshotPtr publish_search(SearchStatus status, std::string message = {},
                                     std::vector<SearchMatch> matches = {}) {
        auto value = std::make_shared<SearchSnapshot>();
        value->token = {search_request_id, content_revision, search_generation};
        value->update_id = ++search_update; value->work_units = search_work;
        value->status = status; value->query = search_options.query;
        value->message = std::move(message); value->matches_found = search_count;
        GhosttyTerminalScreen screen{};
        checked(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen), "search screen");
        value->alternate_screen = screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
        value->matches = std::move(matches); value->selected_index = search_selected;
        search_result = std::move(value);
        return search_result;
    }
    SearchSnapshotPtr stop_search(SearchStatus status, std::string message) {
        search.reset(); search_building.clear(); search_materializing = false;
        search_selected.reset();
        return publish_search(status, std::move(message));
    }
    bool search_allocation_ok() {
        if (search_memory.limited) { stop_search(SearchStatus::limited, "Search memory budget reached; narrow the query or raise its budget."); return false; }
        if (search_memory.failed) { stop_search(SearchStatus::error, "Search allocation failed; no complete results were published."); return false; }
        return true;
    }
    SearchApplyResult search_action(SearchApplyResult result) {
        auto value = std::make_shared<SearchSnapshot>(*search_result);
        value->update_id = ++search_update; value->last_apply_result = result;
        value->selected_index = search_selected; search_result = std::move(value);
        return result;
    }


    explicit Impl(TerminalOptions config) : options(config), geometry(config.size) {
        valid_size(geometry);
        if ((config.default_foreground && config.default_foreground->a != 255) ||
            (config.default_background && config.default_background->a != 255))
            throw std::invalid_argument("terminal default colors must be opaque RGB; apply opacity in the renderer");
        if (config.render_hold_timeout <= std::chrono::milliseconds::zero() ||
            config.scrollback_budget_bytes > std::numeric_limits<std::size_t>::max() ||
            config.effect_queue_limit_bytes < 256 || config.clipboard_limit_bytes > config.effect_queue_limit_bytes)
            throw std::invalid_argument("invalid terminal resource limits");
        bool kitty{};
        checked(ghostty_build_info(GHOSTTY_BUILD_INFO_KITTY_GRAPHICS, &kitty), "query graphics capability");
        if (config.image_budget_bytes && !kitty) throw std::runtime_error("libghostty-vt was built without required Kitty graphics");
        terminal_created = true;
        checked(ghostty_terminal_new(nullptr, &terminal.value, geometry.columns, geometry.rows), "create terminal");
        checked(ghostty_render_state_new(nullptr, &render.value), "create render state");
        checked(ghostty_render_state_row_iterator_new(nullptr, &rows.value), "create row iterator");
        checked(ghostty_render_state_row_cells_new(nullptr, &cells.value), "create cells iterator");
        checked(ghostty_render_state_image_iterator_new(nullptr, &images.value), "create image iterator");
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
        // The pinned renderer only refreshes fg/bg when BOTH engine defaults
        // exist. Materialize unspecified defaults from its initial colors so a
        // one-sided theme still renders and answers OSC queries consistently.
        auto initial_colors = sized<GhosttyRenderStateColors>();
        checked(ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_COLORS, &initial_colors), "read initial colors");
        const auto set_default_color = [&](GhosttyTerminalOption option, Color value) {
            const GhosttyColorRgb rgb{value.r, value.g, value.b};
            set(option, &rgb);
        };
        set_default_color(GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, options.default_foreground.value_or(color(initial_colors.foreground)));
        set_default_color(GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, options.default_background.value_or(color(initial_colors.background)));
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
        publish_search(SearchStatus::idle);
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
        self.callback([&] { self.string_effect(GHOSTTY_TERMINAL_DATA_PWD, EffectKind::working_directory); });
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
                self.capture();
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
    FramePtr capture();
    void capture_images(RenderFrame& target);
};

FramePtr TerminalCore::Impl::capture() {
    // Kept under exclusive actor ownership THROUGH image pixel copying. Begin /
    // end separation is intentionally not used: image views are terminal-owned.
    checked(ghostty_render_state_update(render, terminal), "update render state");
    GhosttyRenderStateDirty dirty{};
    checked(ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty), "read frame dirtiness");
    std::uint64_t image_generation{};
    checked(ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_IMAGE_PLACEMENTS_GENERATION, &image_generation), "read placement generation");
    GhosttyTerminalScreen screen{};
    checked(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen), "read active screen");
    bool mouse{};
    checked(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &mouse), "read mouse reporting");
    const bool alternate = screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
    // Dynamic colors and some cursor metadata are updated by Ghostty even when
    // no rows/global flags are dirty. Check these cheap fields before reusing an
    // owned frame; changed defaults require re-resolving previously clean rows.
    auto colors = sized<GhosttyRenderStateColors>();
    checked(ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors), "read colors");
    const auto foreground = color(colors.foreground), background = color(colors.background);
    if (frame && (frame->foreground != foreground || frame->background != background))
        dirty = GHOSTTY_RENDER_STATE_DIRTY_FULL;
    auto cursor = sized<GhosttyRenderStateCursor>();
    checked(ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cursor), "read cursor");
    FrameCursor next_cursor;
    if (cursor.viewport_has_value) {
        next_cursor.column = cursor.viewport_x; next_cursor.row = cursor.viewport_y;
        next_cursor.wide_tail = cursor.wide_tail;
    }
    next_cursor.visible = cursor.visible && cursor.viewport_has_value;
    next_cursor.blinking = cursor.blinking;
    next_cursor.password_input = cursor.password_input;
    next_cursor.color = colors.cursor_has_value ? color(colors.cursor) : foreground;
    switch (cursor.visual_style) {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR: next_cursor.shape = CursorShape::bar; break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE: next_cursor.shape = CursorShape::underline; break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW: next_cursor.shape = CursorShape::hollow_block; break;
    default: next_cursor.shape = CursorShape::block; break;
    }
    if (frame && dirty == GHOSTTY_RENDER_STATE_DIRTY_FALSE && next_cursor == frame->cursor &&
        image_generation == frame->image_placements_generation && alternate == frame->alternate_screen &&
        mouse == frame->mouse_reporting && frame->columns == geometry.columns && frame->rows == geometry.rows &&
        frame->cell_width == geometry.cell_width && frame->cell_height == geometry.cell_height) return frame;

    auto next = std::make_shared<RenderFrame>();
    next->generation = ++frame_generation;
    next->columns = geometry.columns; next->rows = geometry.rows;
    next->cell_width = geometry.cell_width; next->cell_height = geometry.cell_height;
    next->alternate_screen = alternate; next->mouse_reporting = mouse;
    next->image_placements_generation = image_generation;
    next->foreground = foreground; next->background = background;
    next->cursor = next_cursor;
    std::unordered_map<RowId, std::shared_ptr<const FrameRow>, RowHash> previous;
    if (frame) for (const auto& row : frame->lines) previous.emplace(row->id, row);
    checked(ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &rows.value), "read rows");
    next->lines.reserve(geometry.rows);
    while (ghostty_render_state_row_iterator_next(rows)) {
        GhosttyRenderStateRowId raw_id{};
        bool row_dirty{};
        checked(ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_ID, &raw_id), "read row ID");
        checked(ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &row_dirty), "read row dirty state");
        RowId id{{raw_id.bits[0], raw_id.bits[1]}};
        auto cached = previous.find(id);
        if (dirty != GHOSTTY_RENDER_STATE_DIRTY_FULL && !row_dirty && cached != previous.end()) {
            next->lines.push_back(cached->second);
            continue;
        }
        auto row = std::make_shared<FrameRow>();
        row->id = id;
        auto selection = sized<GhosttyRenderStateRowSelection>();
        const auto selection_result = ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &selection);
        if (selection_result == GHOSTTY_SUCCESS) row->selection = SelectionRange{selection.start_x, selection.end_x};
        else if (selection_result != GHOSTTY_NO_VALUE) checked(selection_result, "read row selection");
        checked(ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells.value), "read row cells");
        row->cells.reserve(geometry.columns);
        while (ghostty_render_state_row_cells_next(cells)) {
            Cell cell;
            GhosttyCell raw{};
            auto style = sized<GhosttyStyle>();
            const GhosttyRenderStateRowCellsData keys[]{GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE};
            void* values[]{&raw, &style};
            checked(ghostty_render_state_row_cells_get_multi(cells, 2, keys, values, nullptr), "read cell fields");
            std::uint32_t codepoint{};
            checked(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_CODEPOINT, &codepoint), "read base codepoint");
            cell.image_placeholder = codepoint == 0x10EEEE;
            GhosttyCellWide wide{};
            checked(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide), "read cell span");
            cell.span = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : wide == GHOSTTY_CELL_WIDE_NARROW ? 1 : 0;
            checked(ghostty_cell_get(raw, GHOSTTY_CELL_DATA_HAS_HYPERLINK, &cell.hyperlink), "read cell hyperlink presence");
            std::array<std::uint8_t, 64> text{};
            GhosttyBuffer buffer{text.data(), text.size(), 0};
            auto result = ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buffer);
            if (result == GHOSTTY_OUT_OF_SPACE) {
                cell.grapheme.resize(buffer.len);
                buffer.ptr = reinterpret_cast<std::uint8_t*>(cell.grapheme.data()); buffer.cap = cell.grapheme.size();
                checked(ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buffer), "copy grapheme");
            } else {
                checked(result, "read grapheme");
                cell.grapheme.assign(reinterpret_cast<const char*>(text.data()), buffer.len);
            }
            GhosttyColorRgb fg{}, bg{};
            auto fg_result = ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);
            auto bg_result = ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg);
            if (fg_result != GHOSTTY_SUCCESS && fg_result != GHOSTTY_INVALID_VALUE) checked(fg_result, "read foreground");
            if (bg_result != GHOSTTY_SUCCESS && bg_result != GHOSTTY_INVALID_VALUE) checked(bg_result, "read background");
            cell.foreground = fg_result == GHOSTTY_SUCCESS ? color(fg) : next->foreground;
            cell.background_is_default = bg_result != GHOSTTY_SUCCESS;
            cell.background = bg_result == GHOSTTY_SUCCESS ? color(bg) : next->background;
            cell.underline_color = resolve(style.underline_color, colors, cell.foreground);
            cell.style = {style.bold, style.italic, style.faint, style.blink, style.inverse,
                          style.invisible, style.strikethrough, style.overline, static_cast<std::uint8_t>(style.underline)};
            const auto column = row->cells.size();
            cell.selected = row->selection && column >= row->selection->first && column <= row->selection->last;
            row->cells.push_back(std::move(cell));
        }
        // Cursor movement dirties its old/new rows even if their text is unchanged.
        // Preserve content generations so renderers can reuse shaped glyph runs.
        if (cached != previous.end() && cached->second->cells == row->cells && cached->second->selection == row->selection)
            next->lines.push_back(cached->second);
        else {
            row->generation = ++row_generation;
            next->lines.push_back(std::move(row));
        }
    }
    if (next->lines.size() != geometry.rows) throw std::runtime_error("render snapshot row count mismatch");
    if (frame && frame->image_placements_generation == image_generation) next->images = frame->images;
    else capture_images(*next);
    // Immutable host-owned frame is now complete; cleaning cannot invalidate it.
    checked(ghostty_render_state_clean(render), "clean consumed render state");
    frame = std::move(next);
    return frame;
}

void TerminalCore::Impl::capture_images(RenderFrame& target) {
    if (!options.image_budget_bytes) return;
    checked(ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_IMAGE_ITERATOR, &images.value), "read image placements");
    std::unordered_map<std::uint64_t, std::shared_ptr<const ImagePixels>> cache;
    if (frame) for (const auto& placement : frame->images) cache.emplace(placement.pixels->generation, placement.pixels);
    std::size_t copied_bytes{};
    const auto storage = graphics();
    while (ghostty_render_state_image_iterator_next(images)) {
        ImagePlacement placement;
        std::uint32_t image_id{};
        std::uint64_t generation{};
        GhosttyImageProtocol protocol{};
        GhosttyRenderStateImageLayer layer{};
        GhosttyRenderStateKittyPlacementKind kind{};
        const GhosttyRenderStateImageData keys[]{
            GHOSTTY_RENDER_STATE_IMAGE_DATA_PROTOCOL, GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_ID,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_GENERATION, GHOSTTY_RENDER_STATE_IMAGE_DATA_LAYER,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_Z, GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_COL,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_ROW, GHOSTTY_RENDER_STATE_IMAGE_DATA_GRID_COLS,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_GRID_ROWS, GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_X,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_Y, GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_WIDTH,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_HEIGHT, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_X,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_Y, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_WIDTH,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_HEIGHT, GHOSTTY_RENDER_STATE_IMAGE_DATA_KITTY_PLACEMENT_ID,
            GHOSTTY_RENDER_STATE_IMAGE_DATA_KITTY_KIND};
        void* values[]{&protocol, &image_id, &generation, &layer, &placement.z, &placement.column, &placement.row,
            &placement.columns, &placement.rows, &placement.offset_x, &placement.offset_y, &placement.pixel_width,
            &placement.pixel_height, &placement.source_x, &placement.source_y, &placement.source_width,
            &placement.source_height, &placement.placement_id, &kind};
        checked(ghostty_render_state_image_get_multi(images, std::size(keys), keys, values, nullptr), "read image placement");
        if (protocol != GHOSTTY_IMAGE_PROTOCOL_KITTY) throw std::runtime_error("unsupported image protocol from pinned engine");
        placement.layer = layer == GHOSTTY_RENDER_STATE_IMAGE_LAYER_BELOW_BACKGROUND ? ImageLayer::below_background :
                          layer == GHOSTTY_RENDER_STATE_IMAGE_LAYER_BELOW_TEXT ? ImageLayer::below_text : ImageLayer::above_text;
        placement.kind = kind == GHOSTTY_RENDER_STATE_KITTY_PLACEMENT_KIND_VIRTUAL ? ImagePlacementKind::virtual_placeholder :
                         kind == GHOSTTY_RENDER_STATE_KITTY_PLACEMENT_KIND_RELATIVE ? ImagePlacementKind::relative : ImagePlacementKind::fixed;
        if (auto found = cache.find(generation); found != cache.end()) placement.pixels = found->second;
        else {
            const auto image = ghostty_kitty_graphics_image(storage, image_id);
            if (!image) throw std::runtime_error("image disappeared during actor-confined snapshot");
            auto pixels = std::make_shared<ImagePixels>();
            pixels->image_id = image_id; pixels->generation = generation;
            GhosttyKittyImageFormat format{};
            const std::uint8_t* data{};
            std::size_t length{};
            std::uint64_t actual_generation{};
            const GhosttyKittyGraphicsImageData image_keys[]{GHOSTTY_KITTY_IMAGE_DATA_WIDTH, GHOSTTY_KITTY_IMAGE_DATA_HEIGHT,
                GHOSTTY_KITTY_IMAGE_DATA_FORMAT, GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR, GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,
                GHOSTTY_KITTY_IMAGE_DATA_GENERATION};
            void* image_values[]{&pixels->width, &pixels->height, &format, &data, &length, &actual_generation};
            checked(ghostty_kitty_graphics_image_get_multi(image, std::size(image_keys), image_keys, image_values, nullptr), "read image pixels");
            std::uint64_t components{};
            switch (format) {
            case GHOSTTY_KITTY_IMAGE_FORMAT_RGBA: pixels->format = PixelFormat::rgba; components = 4; break;
            case GHOSTTY_KITTY_IMAGE_FORMAT_RGB: pixels->format = PixelFormat::rgb; components = 3; break;
            case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA: pixels->format = PixelFormat::gray_alpha; components = 2; break;
            case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY: pixels->format = PixelFormat::gray; components = 1; break;
            default: throw std::runtime_error("unsupported decoded image format");
            }
            if (!data || generation != actual_generation || !pixels->width || !pixels->height ||
                std::uint64_t(pixels->width) * pixels->height > std::numeric_limits<std::uint64_t>::max() / components ||
                std::uint64_t(pixels->width) * pixels->height * components != length ||
                length > options.image_budget_bytes || copied_bytes > options.image_budget_bytes - length)
                throw std::runtime_error("image snapshot exceeds budget or has inconsistent pixels");
            copied_bytes += length;
            pixels->bytes.assign(data, data + length);
            placement.pixels = pixels;
            cache.emplace(generation, std::move(pixels));
        }
        // The engine budgets pixel storage but placements have independent cost.
        if (target.images.size() >= 65536) throw std::runtime_error("image placement count exceeds snapshot budget");
        target.images.push_back(std::move(placement));
    }
    std::stable_sort(target.images.begin(), target.images.end(), [](const auto& a, const auto& b) {
        return a.layer != b.layer ? a.layer < b.layer : a.z < b.z;
    });
}

TerminalCore::TerminalCore(TerminalOptions options) : impl_(std::make_unique<Impl>(options)) {}
TerminalCore::~TerminalCore() = default;
void TerminalCore::feed(std::string_view bytes) {
    feed(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}
void TerminalCore::feed(std::span<const std::uint8_t> bytes) {
    impl_->check_thread();
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
    selection.start = sized<GhosttyGridRef>(); selection.end = sized<GhosttyGridRef>();
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
FramePtr TerminalCore::snapshot(Clock::time_point now) {
    impl_->check_thread(); impl_->tick(now);
    if (impl_->held && impl_->frame) return impl_->frame;
    return impl_->capture();
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
    impl_->check_thread(); auto& self = *impl_;
    self.search.reset(); self.search_building.clear(); self.search_materializing = false;
    self.search_request_id = request_id; ++self.search_generation;
    self.search_fed_revision = 0; self.search_work = 0;
    self.search_count = 0; self.search_selected.reset();
    if (options.query.size() > 4096) {
        self.search_options = {};
        self.publish_search(SearchStatus::error, "Search query exceeds the 4096-byte limit.");
        return;
    }
    self.search_options = std::move(options);
    const auto& config = self.search_options;
    if (!request_id || config.query.size() > 4096 || !valid_utf8(config.query) ||
        !config.max_matches || config.max_matches > 4096 ||
        config.memory_budget_bytes < 64u * 1024u || config.memory_budget_bytes > 64u * 1024u * 1024u ||
        !config.max_work_units_per_step || config.max_work_units_per_step > 16 ||
        !config.max_total_work_units || config.max_total_work_units > 1000000) {
        self.publish_search(SearchStatus::error, "Invalid UTF-8 query or search budget (query <=4096 bytes, matches 1..4096, memory 64KiB..64MiB, step 1..16, total 1..1000000).");
        return;
    }
    if (config.query.empty()) { self.publish_search(SearchStatus::idle); return; }
    self.search_memory.limit = config.memory_budget_bytes;
    self.search_memory.limited = self.search_memory.failed = false; self.search_memory.peak = 0;
    const auto created = ghostty_search_new(&self.search_memory.abi, &self.search.value, self.terminal);
    if (!self.search_allocation_ok()) return;
    if (created != GHOSTTY_SUCCESS) { self.stop_search(SearchStatus::error, "Could not create engine search."); return; }
    GhosttyString needle{reinterpret_cast<const std::uint8_t*>(config.query.data()), config.query.size()};
    const auto set = ghostty_search_set(self.search, GHOSTTY_SEARCH_OPT_NEEDLE, &needle);
    if (!self.search_allocation_ok()) return;
    if (set != GHOSTTY_SUCCESS) { self.stop_search(SearchStatus::error, "Could not set engine search query."); return; }
    self.publish_search(SearchStatus::running);
}
void TerminalCore::cancel_search(std::uint64_t request_id) {
    impl_->check_thread(); impl_->search_request_id = request_id;
    ++impl_->search_generation; impl_->search_count = 0;
    impl_->stop_search(SearchStatus::cancelled, {});
}
SearchSnapshotPtr TerminalCore::search_snapshot() const { impl_->check_thread(); return impl_->search_result; }
std::uint64_t TerminalCore::revision() const { impl_->check_thread(); return impl_->content_revision; }
bool TerminalCore::search_needs_work() const {
    impl_->check_thread();
    return impl_->search.value && (impl_->search_result->status == SearchStatus::running ||
                                  impl_->search_fed_revision != impl_->content_revision);
}
SearchSnapshotPtr TerminalCore::search_step() {
    impl_->check_thread(); auto& self = *impl_;
    try {
        if (!search_needs_work()) return self.search_result;
        if (self.search_result->token.revision != self.content_revision) {
            self.search_building.clear(); self.search_materializing = false; self.search_selected.reset();
            self.search_count = 0;
            self.publish_search(SearchStatus::running);
        }
        for (std::uint32_t unit = 0; unit < self.search_options.max_work_units_per_step; ++unit) {
            if (self.search_work >= self.search_options.max_total_work_units)
                return self.stop_search(SearchStatus::limited, "Search work budget reached; refine or restart the query.");
            ++self.search_work;
            GhosttySearchStatus status{};
            checked(ghostty_search_get(self.search, GHOSTTY_SEARCH_DATA_STATUS, &status), "read search status");
            if (self.search_fed_revision != self.content_revision || status == GHOSTTY_SEARCH_STATUS_FEED_REQUIRED) {
                checked(ghostty_search_feed(self.search), "feed search");
                self.search_fed_revision = self.content_revision;
            } else if (self.search_materializing) {
                // Never retain GhosttySelection or GridRef across this actor call.
                // The whole getter is bounded by max_matches; only 64 results are
                // converted per unit, and any later mutation restarts conversion.
                std::vector<GhosttySelection> selections(self.search_count);
                GhosttySelectionBuffer buffer{selections.data(), selections.size(), 0};
                checked(ghostty_search_get(self.search, GHOSTTY_SEARCH_DATA_MATCHES, &buffer), "read bounded search matches");
                if (buffer.len != self.search_count)
                    return self.stop_search(SearchStatus::error, "Search changed while materializing owned results.");
                const auto end = std::min(self.search_count, self.search_building.size() + 64);
                for (auto index = self.search_building.size(); index < end; ++index) {
                    GhosttyPointCoordinate start{}, finish{};
                    if (ghostty_terminal_point_from_grid_ref(self.terminal, &selections[index].start, GHOSTTY_POINT_TAG_SCREEN, &start) != GHOSTTY_SUCCESS ||
                        ghostty_terminal_point_from_grid_ref(self.terminal, &selections[index].end, GHOSTTY_POINT_TAG_SCREEN, &finish) != GHOSTTY_SUCCESS)
                        return self.stop_search(SearchStatus::error, "Search result was evicted; restart the query.");
                    self.search_building.push_back({{start.x, start.y}, {finish.x, finish.y}});
                }
                if (self.search_building.size() == self.search_count) {
                    self.search_materializing = false;
                    return self.publish_search(SearchStatus::complete, {}, std::move(self.search_building));
                }
            } else if (status == GHOSTTY_SEARCH_STATUS_RUNNING) {
                checked(ghostty_search_tick(self.search, &status), "tick search");
            }
            if (!self.search_allocation_ok()) return self.search_result;
            checked(ghostty_search_get(self.search, GHOSTTY_SEARCH_DATA_TOTAL_MATCHES, &self.search_count), "count search matches");
            if (self.search_count > self.search_options.max_matches)
                return self.stop_search(SearchStatus::limited, "Too many matches; refine the query or raise the result limit.");
            checked(ghostty_search_get(self.search, GHOSTTY_SEARCH_DATA_STATUS, &status), "read search progress");
            if (status == GHOSTTY_SEARCH_STATUS_COMPLETE && !self.search_materializing) {
                self.search_materializing = true;
                self.search_building.clear(); self.search_building.reserve(self.search_count);
            }
        }
        return self.publish_search(SearchStatus::running);
    } catch (const std::bad_alloc&) {
        // Owned-result allocation failure is not a successful empty search.
        return self.stop_search(SearchStatus::error, "Could not allocate owned search results.");
    } catch (const std::exception& error) {
        return self.stop_search(SearchStatus::error, error.what());
    }
}
SearchApplyResult TerminalCore::select_search_match(SearchToken token, std::size_t index) {
    impl_->check_thread(); auto& self = *impl_;
    if (token != self.search_result->token || token.revision != self.content_revision)
        return self.search_action(SearchApplyResult::stale);
    if (self.search_result->status != SearchStatus::complete)
        return self.search_action(SearchApplyResult::not_ready);
    if (index >= self.search_result->matches.size()) return self.search_action(SearchApplyResult::no_match);
    const auto match = self.search_result->matches[index];
    GhosttyPoint start{}, end{}; start.tag = end.tag = GHOSTTY_POINT_TAG_SCREEN;
    start.value.coordinate = {match.start.column, match.start.row};
    end.value.coordinate = {match.end.column, match.end.row};
    auto selection = sized<GhosttySelection>();
    selection.start = sized<GhosttyGridRef>(); selection.end = sized<GhosttyGridRef>();
    if (ghostty_terminal_grid_ref(self.terminal, start, &selection.start) != GHOSTTY_SUCCESS ||
        ghostty_terminal_grid_ref(self.terminal, end, &selection.end) != GHOSTTY_SUCCESS)
        return self.search_action(SearchApplyResult::stale);
    self.set(GHOSTTY_TERMINAL_OPT_SELECTION, &selection);
    GhosttyTerminalScrollViewport viewport{}; viewport.tag = GHOSTTY_SCROLL_VIEWPORT_ROW;
    viewport.value.row = match.start.row > self.geometry.rows / 2 ? match.start.row - self.geometry.rows / 2 : 0;
    ghostty_terminal_scroll_viewport(self.terminal, viewport);
    if (self.held) {
        GhosttyTerminalModeConfig config{GHOSTTY_MODE_SYNC_OUTPUT, false};
        self.set(GHOSTTY_TERMINAL_OPT_MODE, &config); self.held = false;
    }
    self.search_selected = index;
    return self.search_action(SearchApplyResult::applied);
}
SearchApplyResult TerminalCore::navigate_search(SearchToken token, SearchDirection direction, bool wrap) {
    impl_->check_thread(); auto& self = *impl_;
    if (token != self.search_result->token || token.revision != self.content_revision)
        return self.search_action(SearchApplyResult::stale);
    if (self.search_result->status != SearchStatus::complete) return self.search_action(SearchApplyResult::not_ready);
    const auto count = self.search_result->matches.size();
    if (!count) return self.search_action(SearchApplyResult::no_match);
    std::size_t index = direction == SearchDirection::backward ? 0 : count - 1;
    if (self.search_selected) {
        index = *self.search_selected;
        if (direction == SearchDirection::backward) {
            if (index + 1 < count) ++index;
            else if (wrap) index = 0;
            else return self.search_action(SearchApplyResult::boundary);
        } else {
            if (index > 0) --index;
            else if (wrap) index = count - 1;
            else return self.search_action(SearchApplyResult::boundary);
        }
    }
    return select_search_match(token, index);
}
} // namespace stormtty
