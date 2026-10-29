#pragma once
#include "stormtty/config.hpp"
#include "stormtty/agent_service.hpp"
#include "stormtty/renderer.hpp"
#include <map>
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
    bool fixture{};
    std::uint64_t provider_generation{};
};
enum class AgentPanelActionKind { connect, disconnect, start, interrupt, allow, decline };
struct AgentPanelAction {
    AgentPanelActionKind kind{};
    std::string workspace_id, prompt;
    AgentApprovalKey approval_key;
    std::uint64_t provider_generation{};
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
    bool is_open() const noexcept { return open_; }
    void resize(Size);
    bool key(const KeyEvent&);
    void text(std::string_view);
    void pointer_down(Point);
    void pointer_up(Point);
    void pointer_cancel() { pressed_.clear(); }
    void scroll(double);
    void append_scene(WindowScene&, const Theme&, float scale = 1) const;
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
    std::string focus_{"composer"}, pressed_;
    std::size_t scroll_{};
    bool open_{}, confirming_{}, connecting_{};
};
}
