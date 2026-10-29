#pragma once
#include "stormtty/config.hpp"
#include "stormtty/agent_service.hpp"
#include "stormtty/agent_provider_settings.hpp"
#include "stormtty/renderer.hpp"
#include <map>
#include <functional>
#include <optional>

namespace stormtty {
enum class AgentPanelState { unavailable, disconnected, connecting, ready, running, completed, interrupted, failed };
struct AgentPanelApproval {
    // Opaque full provider/thread/turn/item/request key. Never reconstructed from display text.
    AgentApprovalKey key;
    std::string title, scope;
    bool allow_supported{};
};
struct AgentPanelMessage { std::string role, text; };
struct AgentPanelView {
    std::string workspace_id, workspace_name, directory, provider{"Codex"};
    AgentPanelState state{AgentPanelState::unavailable};
    std::string execution_scope, connection_scope, error;
    std::vector<AgentPanelMessage> transcript;
    std::vector<AgentPanelApproval> approvals;
    AgentProviderSettings provider_settings;
    bool setup_available{}, setup_reload_available{}, setup_busy{}, disconnect_available{};
    std::string setup_status;
    bool fixture{};
    std::uint64_t provider_generation{}, settings_generation{};
};
enum class AgentPanelActionKind { reload_settings, save_settings, connect, disconnect, start, interrupt, allow, decline };
struct AgentPanelAction {
    AgentPanelActionKind kind{};
    std::string workspace_id, prompt;
    AgentApprovalKey approval_key;
    AgentProviderSettings provider_settings;
    std::uint64_t provider_generation{}, settings_generation{};
};
struct AgentPanelControl { std::string id, label; Rect bounds; bool enabled{true}; };
AgentPanelView agent_panel_view(const AgentSnapshot&, std::string workspace_name, std::string directory, bool provider_available);
// Presentation only. No provider calls, transport I/O, credentials or task replay.
// The host consumes explicit actions outside rendering and validates generation/key.
class AgentPanel {
public:
    void open(AgentPanelView);
    void update(AgentPanelView);
    void close();
    void settings_result(bool success,std::string error = {});
    void settings_reloaded(const AgentProviderSettings&,std::string error = {});
    bool editing_settings() const {return editing_settings_;}
    bool is_open() const noexcept { return open_; }
    void resize(Size);
    bool key(const KeyEvent&);
    void text(std::string_view);
    void pointer_down(Point);
    void pointer_up(Point);
    bool pointer_move(Point);
    void pointer_cancel() { pressed_.clear(); }
    void scroll(double);
    using TextWrapper=std::function<std::vector<std::string>(const ChromeText&)>;
    void append_scene(WindowScene&, const Theme&, float scale = 1, const TextWrapper& native_wrap = {}) const;
    std::optional<AgentPanelAction> take_action();
    std::string status_label() const;
    const AgentPanelView& view() const { return view_; }
    const std::vector<AgentPanelControl>& controls() const { return controls_; }
    const std::string& focused_control() const { return focus_; }
    const std::string& draft() const;
private:
    void layout();
    void activate(std::string_view);
    bool can_start() const;
    AgentPanelView view_;
    Size viewport_{1024,768};
    Rect panel_, transcript_, composer_;
    std::map<std::string,std::string> drafts_;
    std::vector<AgentPanelControl> controls_;
    std::optional<AgentPanelAction> pending_;
    std::string focus_{"composer"}, pressed_, hover_;
    mutable std::string wrapped_input_;
    mutable std::vector<std::string> wrapped_lines_;
    mutable float wrapped_width_{}, wrapped_scale_{};
    mutable bool wrapped_native_{};
    std::size_t scroll_{};
    AgentProviderSettings settings_draft_;
    std::string settings_error_;
    bool editing_settings_{}, settings_select_all_{}, settings_reload_confirm_{};
    bool open_{}, confirming_{}, connecting_{};
};
}
