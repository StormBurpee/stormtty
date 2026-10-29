#include "stormtty/agent_service.hpp"
#include "json.hpp"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace stormtty {
namespace {
using agent_json::Value;
using agent_json::quote;
std::atomic<std::uint64_t> provider_generation{1};
bool opaque(std::string_view s, std::size_t max=256) { return !s.empty() && s.size()<=max && detail::valid_utf8(s) && std::none_of(s.begin(),s.end(),[](unsigned char c){return c<0x20 || c==0x7f;}); }
bool workspace_id(std::string_view s) { return s.size()==32 && s.find_first_not_of("0123456789abcdef")==std::string_view::npos; }
bool association_valid(const WorkspaceAgentAssociation& a) { return workspace_id(a.workspace_persistent_id) && a.provider=="codex" && opaque(a.account_reference) && (a.thread_id.empty() || opaque(a.thread_id)); }
std::string required(const Value& v, std::string_view key) { auto s=v.at(key).text(); if(!opaque(s)) throw std::runtime_error("Missing or invalid protocol identity"); return s; }
bool active(AgentConversation c) { return c==AgentConversation::starting || c==AgentConversation::running || c==AgentConversation::interrupting; }
std::string clipped(std::string s, std::size_t n) { if(s.size()>n) { s.resize(n); while(!detail::valid_utf8(s) && !s.empty()) s.pop_back(); } return s; }
}
struct AgentService::Impl {
    mutable std::mutex mutex;
    AgentSnapshot state;
    std::string input, pending_prompt, pending_cwd;
    std::vector<std::string> outbound;
    std::size_t outbound_bytes{};
    std::uint64_t next_request{1};
    std::map<std::string,std::string> requests;
    std::map<std::string,std::string> item_scopes;
    std::set<std::string> seen_approvals;
    bool thread_loaded{};
    void changed() { ++state.revision; }
    void clear_transient() { input.clear(); pending_prompt.clear(); pending_cwd.clear(); requests.clear(); outbound.clear(); outbound_bytes=0; state.approvals.clear(); item_scopes.clear(); seen_approvals.clear(); thread_loaded=false; }
    void fail(std::string error) { clear_transient(); state.connection=AgentConnection::failed; if(active(state.conversation)) state.conversation=AgentConversation::failed; state.error=std::move(error); changed(); }
    void send(std::string frame) {
        if(outbound.size()>=64 || outbound_bytes+frame.size()+1>2*1024*1024) throw std::runtime_error("Outbound queue limit; transport must reconnect explicitly");
        frame+='\n'; outbound_bytes+=frame.size(); outbound.push_back(std::move(frame));
    }
    void request(std::string method, std::string params) {
        if(requests.size()>=32) throw std::runtime_error("Pending request limit");
        auto id=std::to_string(next_request++); requests.emplace(id,method);
        send("{\"id\":"+id+",\"method\":"+quote(method)+",\"params\":"+params+'}');
    }
    void turn_start() {
        request("turn/start","{\"threadId\":"+quote(state.association.thread_id)+",\"input\":[{\"type\":\"text\",\"text\":"+quote(pending_prompt)+",\"text_elements\":[]}],\"cwd\":"+quote(pending_cwd)+",\"approvalPolicy\":\"untrusted\",\"approvalsReviewer\":\"user\",\"sandboxPolicy\":{\"type\":\"readOnly\",\"networkAccess\":false}}");
        transcript("user-"+std::to_string(next_request),"userMessage",pending_prompt,false);
        pending_prompt.clear(); pending_cwd.clear();
    }
    void transcript(const std::string& id, std::string kind, std::string text, bool append) {
        auto& items=state.transcript;
        auto i=std::find_if(items.begin(),items.end(),[&](const auto& x){return x.id==id;});
        if(i==items.end()) { items.push_back({id,std::move(kind),{}}); i=std::prev(items.end()); }
        if(append) text=i->text+text;
        if(text.size()>65536) state.transcript_truncated=true;
        i->text=clipped(std::move(text),65536);
        std::size_t total{}; for(const auto& x:items) total+=x.text.size();
        while(items.size()>256 || total>256*1024) { total-=items.front().text.size(); items.erase(items.begin()); state.transcript_truncated=true; }
    }
    void set_turn(const Value& turn) {
        const auto id=required(turn,"id");
        if(!state.turn_id.empty() && id!=state.turn_id) throw std::runtime_error("Unexpected active turn; reconcile before retry");
        state.turn_id=id;
        const auto status=turn.at("status").text();
        if(status=="inProgress") { if(state.conversation!=AgentConversation::interrupting) state.conversation=AgentConversation::running; }
        else if(status=="completed") state.conversation=AgentConversation::completed;
        else if(status=="interrupted") state.conversation=AgentConversation::interrupted;
        else if(status=="failed") { state.conversation=AgentConversation::failed; state.error=clipped(turn.at("error").at("message").text(),4096); }
        else throw std::runtime_error("Unsupported turn status");
        if(!active(state.conversation)) { state.approvals.clear(); item_scopes.clear(); }
    }
    bool scoped(const Value& params) const { return params.at("threadId").text()==state.association.thread_id && !state.association.thread_id.empty(); }
    bool current(const Value& params) const { return scoped(params) && !state.turn_id.empty() && params.at("turnId").text()==state.turn_id && active(state.conversation); }
    void response(const Value& msg) {
        auto id=agent_json::id(msg.at("id")); auto found=requests.find(id);
        if(found==requests.end()) return; // late/duplicate response cannot mutate state
        auto method=found->second; requests.erase(found);
        if(msg.has("error")) { fail("Codex request failed ("+method+"): "+clipped(msg.at("error").at("message").text(),4096)); return; }
        if(msg.at("result").type!=Value::Type::object) throw std::runtime_error("Missing response result");
        const auto& result=msg.at("result");
        if(method=="initialize") {
            if(!opaque(result.at("userAgent").text(),4096)) throw std::runtime_error("Incompatible initialize response");
            send("{\"method\":\"initialized\",\"params\":{}}");
            request("account/read","{\"refreshToken\":false}");
        } else if(method=="account/read") {
            const auto& account=result.at("account");
            if(account.at("type").text()!="chatgpt" || !opaque(account.at("email").text()) || "chatgpt:"+account.at("email").text()!=state.association.account_reference) throw std::runtime_error("Signed-in account identity mismatch or unsupported account type; no thread resumed");
            state.connection=AgentConnection::ready;
        } else if(method=="thread/start" || method=="thread/resume") {
            auto thread=required(result.at("thread"),"id");
            if(result.at("cwd").text()!=pending_cwd) throw std::runtime_error("Provider cwd mismatch; no turn started");
            if(result.at("approvalPolicy").text()!="untrusted" || result.at("approvalsReviewer").text()!="user" || result.at("sandbox").at("type").text()!="readOnly" || result.at("sandbox").at("networkAccess").type!=Value::Type::boolean || result.at("sandbox").at("networkAccess").scalar!="false") throw std::runtime_error("Provider did not confirm the requested execution policy; no turn started");
            if(result.at("thread").at("status").at("type").text()!="idle") throw std::runtime_error("Thread is not idle; reconcile active work before starting a turn");
            if(method=="thread/resume" && thread!=state.association.thread_id) throw std::runtime_error("Resumed thread identity mismatch");
            state.association.thread_id=std::move(thread); thread_loaded=true;
            if(state.conversation==AgentConversation::starting && !pending_prompt.empty()) turn_start();
        } else if(method=="turn/start" && active(state.conversation)) set_turn(result.at("turn"));
        // turn/interrupt response is acknowledgement, NOT completion.
        changed();
    }
    void server_request(const Value& msg) {
        auto request_id=agent_json::id(msg.at("id"));
        const auto method=msg.at("method").text(); const auto& p=msg.at("params");
        if(!seen_approvals.insert(request_id).second) throw std::runtime_error("Duplicate server request identity");
        if(seen_approvals.size()>4096) throw std::runtime_error("Server request history limit");
        const bool command=method=="item/commandExecution/requestApproval", file=method=="item/fileChange/requestApproval";
        if(!command && !file) { send("{\"id\":"+request_id+",\"error\":{\"code\":-32601,\"message\":\"Unsupported server request; no permission granted\"}}"); return; }
        if(!current(p) || state.conversation==AgentConversation::interrupting) { send("{\"id\":"+request_id+",\"result\":{\"decision\":\"cancel\"}}"); return; }
        if(state.approvals.size()>=32) throw std::runtime_error("Pending approval limit");
        AgentApproval a; a.key={state.generation,required(p,"threadId"),required(p,"turnId"),required(p,"itemId"),request_id};
        a.kind=command?"commandExecution":"fileChange"; a.reason=clipped(p.at("reason").text(),4096);
        auto item=item_scopes.find(a.key.item_id);
        if(item!=item_scopes.end()) {
            a.scope=item->second;
            auto scope=agent_json::parse(a.scope);
            a.can_accept=scope.at("type").text()==a.kind && (command ? !scope.at("command").text().empty() && !scope.at("cwd").text().empty() : scope.at("changes").type==Value::Type::array);
        }
        // Never grant session roots or unknown extended scopes through accept-once.
        if(command && p.at("command").type==Value::Type::string && p.at("cwd").type==Value::Type::string) {
            a.scope=agent_json::dump(p); a.can_accept=!p.at("command").text().empty() && !p.at("cwd").text().empty();
        }
        if((p.has("kind") && p.at("kind").text()!="command") || p.at("grantRoot").type!=Value::Type::null || p.at("additionalPermissions").type!=Value::Type::null || p.at("networkApprovalContext").type!=Value::Type::null || p.at("environmentId").type!=Value::Type::null) a.can_accept=false;
        static const std::set<std::string,std::less<>> known{"threadId","turnId","itemId","reason","kind","startedAtMs","approvalId","environmentId","networkApprovalContext","command","cwd","commandActions","proposedExecpolicyAmendment","proposedNetworkPolicyAmendments","grantRoot","availableDecisions"};
        for(const auto& [field,value]:p.object) { (void)value; if(!known.contains(field)) a.can_accept=false; }
        if(p.has("availableDecisions")) {
            const auto& choices=p.at("availableDecisions");
            a.can_accept=a.can_accept && choices.type==Value::Type::array && std::any_of(choices.array.begin(),choices.array.end(),[](const auto& v){return v.text()=="accept";});
        }
        a.scope+="\nRequest: "+agent_json::dump(p);
        if(a.scope.size()>131072) throw std::runtime_error("Approval scope exceeds display limit");
        state.approvals.push_back(std::move(a)); changed();
    }
    void notification(const Value& msg) {
        const auto method=msg.at("method").text(); const auto& p=msg.at("params");
        if(method=="account/updated" && state.connection==AgentConnection::ready) { fail("Provider account changed; reconnect explicitly to verify identity"); return; }
        if(method=="turn/started" || method=="turn/completed") {
            if(!scoped(p) || !active(state.conversation)) return;
            set_turn(p.at("turn")); changed(); return;
        }
        // Provider resolution retires pending UI without reusing a decision.
        if(method=="serverRequest/resolved") {
            if(!scoped(p)) return;
            const auto id=agent_json::id(p.at("requestId"));
            std::erase_if(state.approvals,[&](const auto& a){return a.key.request_id==id;}); changed(); return;
        }
        if(!current(p)) return;
        if(method=="item/agentMessage/delta" || method=="item/commandExecution/outputDelta") {
            if(p.at("delta").type!=Value::Type::string) throw std::runtime_error("Invalid text delta");
            transcript(required(p,"itemId"),method=="item/agentMessage/delta"?"agentMessage":"commandExecution",p.at("delta").text(),true);
        } else if(method=="turn/diff/updated") { state.diff=clipped(p.at("diff").text(),256*1024-32); if(p.at("diff").text().size()>256*1024-32) state.diff+="\n[Diff display truncated]"; }
        else if(method=="item/started" || method=="item/completed") {
            const auto& item=p.at("item"); auto id=required(item,"id"); auto kind=required(item,"type");
            if(kind=="agentMessage") transcript(id,kind,item.at("text").text(),false);
            else if(kind=="commandExecution") transcript(id,kind,item.at("command").text()+"\n"+item.at("aggregatedOutput").text(),false);
            else if(kind=="fileChange") transcript(id,kind,agent_json::dump(item.at("changes")),false);
            if(method=="item/started" && (kind=="commandExecution" || kind=="fileChange")) {
                auto scope=agent_json::dump(item);
                if(scope.size()<=65536) { if(item_scopes.size()>=32) throw std::runtime_error("Pending item scope limit"); item_scopes[id]=std::move(scope); }
            }
            if(method=="item/completed") { item_scopes.erase(id); std::erase_if(state.approvals,[&](const auto& a){return a.key.item_id==id;}); }
        } else if(method=="error") state.error=clipped(p.at("error").at("message").text(),4096);
        else return; // Unknown notifications are never interpreted as permission.
        changed();
    }
    void frame(std::string_view line) {
        auto msg=agent_json::parse(line);
        if(msg.type!=Value::Type::object) throw std::runtime_error("Expected protocol object");
        if(msg.has("method")) {
            if(msg.at("method").type!=Value::Type::string || msg.at("params").type!=Value::Type::object) throw std::runtime_error("Malformed protocol method");
            if(msg.has("id")) server_request(msg); else notification(msg);
        } else if(msg.has("id")) response(msg);
        else throw std::runtime_error("Unrecognized protocol envelope");
    }
};
AgentService::AgentService(WorkspaceAgentAssociation a):impl_(std::make_unique<Impl>()) {
    if(!association_valid(a)) throw std::invalid_argument("Invalid workspace/provider/account association");
    impl_->state.association=std::move(a);
}
AgentService::~AgentService()=default;
bool AgentService::connect(std::string_view version,std::string_view account) {
    auto& x=*impl_; std::lock_guard lock(x.mutex);
    if(x.state.connection==AgentConnection::connecting || x.state.connection==AgentConnection::ready) return false;
    if(!opaque(version,128) || account!=x.state.association.account_reference) { x.fail("Missing runtime identity or changed account; explicit reconfiguration required"); return false; }
    x.clear_transient(); x.state.generation=provider_generation.fetch_add(1); x.state.connection=AgentConnection::connecting; x.state.conversation=AgentConversation::idle; x.state.turn_id.clear(); x.state.error.clear();
    try { x.request("initialize","{\"clientInfo\":{\"name\":\"stormtty\",\"title\":\"StormTTY\",\"version\":\"0.1.0\"},\"capabilities\":{\"experimentalApi\":false,\"requestAttestation\":false,\"explicitGatewayOauth\":true}}"); x.changed(); return true; } catch(const std::exception& e) { x.fail(e.what()); return false; }
}
bool AgentService::start(const AgentStartRequest& r) {
    auto& x=*impl_; std::lock_guard lock(x.mutex);
    if(!r.user_confirmed || x.state.connection!=AgentConnection::ready || active(x.state.conversation) || r.prompt.empty() || r.prompt.size()>65536 || !detail::valid_utf8(r.prompt) || !opaque(r.cwd,4096)) return false;
    try {
        x.state.conversation=AgentConversation::starting; x.state.turn_id.clear(); x.state.error.clear(); x.state.diff.clear(); x.state.approvals.clear(); x.item_scopes.clear(); x.pending_prompt=r.prompt; x.pending_cwd=r.cwd;
        if(x.thread_loaded) x.turn_start();
        else if(x.state.association.thread_id.empty()) x.request("thread/start","{\"cwd\":"+quote(r.cwd)+",\"approvalPolicy\":\"untrusted\",\"approvalsReviewer\":\"user\",\"sandbox\":\"read-only\"}");
        else x.request("thread/resume","{\"excludeTurns\":true,\"threadId\":"+quote(x.state.association.thread_id)+",\"cwd\":"+quote(r.cwd)+",\"approvalPolicy\":\"untrusted\",\"approvalsReviewer\":\"user\",\"sandbox\":\"read-only\"}");
        x.changed(); return true;
    } catch(const std::exception& e) { x.fail(e.what()); return false; }
}
bool AgentService::interrupt() {
    auto& x=*impl_; std::lock_guard lock(x.mutex);
    if(x.state.connection!=AgentConnection::ready || !active(x.state.conversation) || x.state.conversation==AgentConversation::interrupting) return false;
    // Without a turn ID, a start may already have executed. Disconnect visibly;
    // never pretend cancellation succeeded or replay the pending start.
    if(x.state.turn_id.empty()) { x.fail("Start interrupted before turn identity; execution outcome unknown, reconcile before retry"); return true; }
    try {
        for(const auto& a:x.state.approvals) x.send("{\"id\":"+a.key.request_id+",\"result\":{\"decision\":\"cancel\"}}");
        x.state.approvals.clear(); x.request("turn/interrupt","{\"threadId\":"+quote(x.state.association.thread_id)+",\"turnId\":"+quote(x.state.turn_id)+'}');
        x.state.conversation=AgentConversation::interrupting; x.changed(); return true;
    } catch(const std::exception& e) { x.fail(e.what()); return false; }
}
bool AgentService::resolve(const AgentApprovalKey& key,AgentApprovalDecision decision) {
    auto& x=*impl_; std::lock_guard lock(x.mutex);
    if(x.state.connection!=AgentConnection::ready || x.state.conversation!=AgentConversation::running) return false;
    auto it=std::find_if(x.state.approvals.begin(),x.state.approvals.end(),[&](const auto& a){return a.key==key;});
    if(it==x.state.approvals.end() || (decision==AgentApprovalDecision::accept_once && !it->can_accept)) return false;
    try { auto value=decision==AgentApprovalDecision::accept_once?"accept":decision==AgentApprovalDecision::decline?"decline":"cancel";
        x.send("{\"id\":"+key.request_id+",\"result\":{\"decision\":"+quote(value)+"}}"); x.state.approvals.erase(it); x.changed(); return true;
    } catch(const std::exception& e) { x.fail(e.what()); return false; }
}
void AgentService::feed_stdout(std::string_view bytes) {
    auto& x=*impl_; std::lock_guard lock(x.mutex);
    if(x.state.connection!=AgentConnection::connecting && x.state.connection!=AgentConnection::ready) return;
    try {
        if(bytes.size()>4*1024*1024) throw std::runtime_error("Transport read chunk exceeds bound");
        std::size_t frames{};
        while(!bytes.empty()) {
            auto end=bytes.find('\n'); auto count=end==std::string_view::npos?bytes.size():end;
            if(x.input.size()+count>maximum_frame_bytes) throw std::runtime_error("JSONL frame exceeds 1 MiB");
            x.input.append(bytes.substr(0,count)); bytes.remove_prefix(count);
            if(end==std::string_view::npos) break;
            bytes.remove_prefix(1); if(++frames>4096) throw std::runtime_error("Too many frames in one transport read");
            auto line=std::exchange(x.input,{}); if(!line.empty() && line.back()=='\r') line.pop_back();
            x.frame(line); if(x.state.connection==AgentConnection::failed) break;
        }
    } catch(const std::exception& e) { x.fail(e.what()); }
}
void AgentService::disconnect(std::string_view reason) {
    auto& x=*impl_; std::lock_guard lock(x.mutex); const bool was_active=active(x.state.conversation);
    x.clear_transient(); x.state.connection=AgentConnection::disconnected;
    if(was_active) x.state.conversation=AgentConversation::failed;
    x.state.error=was_active?"Disconnected; execution outcome unknown. Reconcile before retry.":clipped(std::string(reason),4096); x.changed();
}
std::vector<std::string> AgentService::take_outbound() { auto& x=*impl_; std::lock_guard lock(x.mutex); x.outbound_bytes=0; return std::exchange(x.outbound,{}); }
AgentSnapshot AgentService::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->state; }

std::string serialize_agent_associations(const std::vector<WorkspaceAgentAssociation>& entries) {
    if(entries.size()>256) throw std::invalid_argument("Too many agent associations");
    std::set<std::string> seen; std::string out="{\"version\":1,\"associations\":["; bool first=true;
    for(const auto& a:entries) {
        if(!association_valid(a) || !seen.insert(a.workspace_persistent_id).second) throw std::invalid_argument("Invalid or duplicate agent association");
        if(!first) out+=',';
        first=false;
        out+="{\"workspace\":"+quote(a.workspace_persistent_id)+",\"provider\":"+quote(a.provider)+",\"account\":"+quote(a.account_reference)+",\"thread\":"+quote(a.thread_id)+'}';
    }
    return out+"]}";
}
bool deserialize_agent_associations(std::string_view bytes,std::vector<WorkspaceAgentAssociation>& result,std::string& error) {
    try {
        auto root=agent_json::parse(bytes);
        if(root.type!=Value::Type::object || root.object.size()!=2 || root.at("version").type!=Value::Type::number || root.at("version").scalar!="1" || root.at("associations").type!=Value::Type::array) throw std::runtime_error("Unsupported agent association schema");
        std::vector<WorkspaceAgentAssociation> entries;
        for(const auto& v:root.at("associations").array) {
            if(v.type!=Value::Type::object || v.object.size()!=4 || v.at("thread").type!=Value::Type::string) throw std::runtime_error("Invalid association record");
            entries.push_back({v.at("workspace").text(),v.at("provider").text(),v.at("account").text(),v.at("thread").text()});
        }
        (void)serialize_agent_associations(entries); result=std::move(entries); error.clear(); return true;
    } catch(const std::exception& e) { error=e.what(); return false; }
}
} // namespace stormtty
