#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace stormtty {
enum class AgentConnection { disconnected, connecting, ready, failed };
enum class AgentConversation { idle, starting, running, interrupting, completed, interrupted, failed };
struct AgentApprovalKey {
    std::uint64_t generation{};
    std::string thread_id, turn_id, item_id, request_id; // request_id is canonical JSON (preserves string vs integer).
    bool operator==(const AgentApprovalKey&) const = default;
};
struct AgentApproval {
    AgentApprovalKey key;
    std::string kind, reason, scope; // scope is the complete provider item JSON, displayed as untrusted text.
    bool can_accept{};
};
struct AgentTranscriptItem {
    std::string id, kind, text;
};
struct WorkspaceAgentAssociation {
    // account_reference is chatgpt:<email>, verified using account/read without refresh.
    std::string workspace_persistent_id, provider{"codex"}, account_reference, thread_id;
};
struct AgentSnapshot {
    WorkspaceAgentAssociation association;
    AgentConnection connection{AgentConnection::disconnected};
    AgentConversation conversation{AgentConversation::idle};
    std::uint64_t generation{}, revision{};
    std::string turn_id, error, diff;
    std::vector<AgentTranscriptItem> transcript;
    std::vector<AgentApproval> approvals;
    bool transcript_truncated{};
};
struct AgentStartRequest {
    std::string prompt, cwd;
    // Must be supplied only after a user confirmation exposing this workspace,
    // Codex provider and read-only sandbox / untrusted-command approval scope.
    bool user_confirmed{};
};
enum class AgentApprovalDecision { accept_once, decline, cancel };
// One workspace/provider instance. Thread-safe; no process, account or network
// operation occurs here. The host owns a separately authorized stdio transport.
// feed_stdout must run off the UI thread; snapshot() returns an owned copy.
class AgentService {
public:
    static constexpr std::string_view schema_baseline = "rust-v0.165.0 / stable app-server v2";
    static constexpr std::size_t maximum_frame_bytes = 1024 * 1024;
    explicit AgentService(WorkspaceAgentAssociation association);
    ~AgentService();
    AgentService(const AgentService&) = delete;
    AgentService& operator=(const AgentService&) = delete;
    // Version is descriptive; stable response/policy/account checks gate readiness.
    bool connect(std::string_view runtime_version, std::string_view account_reference);
    bool start(const AgentStartRequest& request);
    bool interrupt();
    bool resolve(const AgentApprovalKey& key, AgentApprovalDecision decision);
    void feed_stdout(std::string_view bytes);
    void disconnect(std::string_view reason = {});
    // Complete JSONL frames only; drain on the transport worker. A write failure
    // must call disconnect; frames must NEVER be replayed after reconnect.
    std::vector<std::string> take_outbound();
    AgentSnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Versioned, bounded, metadata-only persistence. Never saves prompts/approvals.
std::string serialize_agent_associations(const std::vector<WorkspaceAgentAssociation>& associations);
bool deserialize_agent_associations(std::string_view bytes, std::vector<WorkspaceAgentAssociation>& result, std::string& error);
} // namespace stormtty
