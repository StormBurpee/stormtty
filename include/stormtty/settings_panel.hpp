#pragma once

#include "stormtty/config.hpp"
#include "stormtty/renderer.hpp"
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace stormtty {

enum class SettingsSection { appearance, shell, shortcuts };
enum class SettingsActionKind { preview, save, cancel };
struct SettingsAction {
    SettingsActionKind kind{};
    // Owned. Preview only reversible theme/font/appearance; commit profiles/bindings
    // and console backend after persistence. Cancel carries the original for preview rollback.
    Config config;
};
struct SettingsControl {
    std::string id; // Stable semantic identity, including "shell:<profile id>".
    Rect bounds; // DIP, relative to the client window.
    bool visible{}, enabled{true};
};
struct SettingsPanelLayout {
    Rect panel, navigation, content, footer, preview;
    std::vector<SettingsControl> controls;
    bool compact{};
};

// Portable nonblocking modal. The host owns persistence, theme/font/material preview,
// OS input capture and focus restoration; this class never launches a shell.
// While is_open(), intercept ALL keyboard/text/mouse/IME input before the VT or
// workspace route, not only inputs that change this model. Geometry is DIP;
// append_scene converts it to physical pixels (text sizes remain DIP).
class SettingsPanel {
public:
    explicit SettingsPanel(Platform platform = Platform::windows);
    // Existing profile IDs take precedence over discovery; only a selected
    // discovered profile is copied into the draft. Invalid candidates are ignored.
    void open(const Config&, std::span<const Profile> discovered = {});
    // Fresh disk content is the editor/dirty baseline; the running value remains
    // the rollback baseline. Opening never commits shell or shortcut changes.
    void open(const Config& running, const Config& editor,
              std::span<const Profile> discovered = {});
    void open_for_binding(const Config&, std::string_view command_id,
                          std::span<const Profile> discovered = {});
    bool capturing_shortcut() const noexcept { return !capture_command_.empty(); }
    bool is_open() const noexcept { return open_; }
    bool saving() const noexcept { return saving_; }
    bool dirty() const { return draft_ != initial_draft_; }
    bool loaded_changes_from_disk() const noexcept { return loaded_changes_; }
    const Config& draft() const noexcept { return draft_; }
    const Config& original() const noexcept { return original_; }
    SettingsSection section() const noexcept { return section_; }
    const std::string& focused_control() const noexcept { return focus_; }
    const std::string& error() const noexcept { return error_; }
    std::uint64_t generation() const noexcept { return generation_; }
    const SettingsPanelLayout& layout() const noexcept { return layout_; }
    void resize(Size viewport);
    // Actual OS palette supplied by host; no transparency/dimming in this mode.
    void set_high_contrast(bool enabled, Color background = {0,0,0,255},
                           Color foreground = {255,255,255,255},
                           Color highlight = {0,120,215,255},
                           Color highlight_text = {255,255,255,255},
                           Color disabled_text = {128,128,128,255});
    // Host-observed material status; presentation only, never a saved setting.
    void set_material_status(std::string status);
    // Host-observed availability/override information only; never changes a draft.
    // An unavailable saved choice remains visible, with System always selectable.
    void set_console_backend_status(bool bundled_available, std::string status);
    void append_scene(WindowScene&, float dpi_scale = 1.0f) const;
    // Return true for every event while open, including unmatched/key-up events.
    bool key(const KeyEvent&);
    bool pointer_down(Point);
    bool pointer_move(Point);
    bool pointer_up(Point);
    void pointer_cancel() noexcept; // Host capture/focus loss; never commits a click.
    bool wheel(Point, double lines);
    // Coalesced previews keep rapid slider input bounded. A save/cancel action
    // supersedes pending preview. Save remains open and read-only until result.
    std::optional<SettingsAction> take_action();
    void persistence_result(bool success, std::string error = {});
    void cancel();
private:
    void rebuild_layout();
    void changed();
    void activate(std::string_view id, int direction = 1);
    void focus_control(std::string id);
    void ensure_focus_visible();
    void adjust_opacity(Point);
    void request_save();
    void capture_binding(const KeyEvent&);
    void assign_binding(const KeyChord&, bool confirm_terminal_override);
    const SettingsControl* hit(Point) const noexcept;
    Platform platform_;
    Config original_, initial_draft_, draft_;
    std::vector<Profile> profiles_;
    std::vector<std::string> families_;
    Size viewport_{1024,768};
    SettingsPanelLayout layout_;
    SettingsSection section_{SettingsSection::appearance};
    std::string focus_{"nav.appearance"}, hover_, pressed_, error_, capture_command_, material_status_, console_backend_status_;
    double scroll_{}, document_height_{};
    bool bundled_console_available_{};
    bool open_{}, saving_{}, dragging_opacity_{}, high_contrast_{}, loaded_changes_{};
    Color high_background_{0,0,0,255}, high_foreground_{255,255,255,255};
    Color high_highlight_{0,120,215,255}, high_highlight_text_{255,255,255,255};
    Color high_disabled_text_{128,128,128,255};
    std::uint64_t generation_{};
    std::optional<SettingsAction> pending_;
    std::optional<KeyChord> terminal_override_;
};

} // namespace stormtty
