#include "stormtty/agent_service.hpp"
#include "../src/agent/json.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace stormtty;
#define CHECK(x) do { if(!(x)) { std::cerr<<__LINE__<<": " #x "\n"; std::exit(1); } } while(false)
WorkspaceAgentAssociation association(){return {"0123456789abcdef0123456789abcdef","codex","chatgpt:fixture@example.invalid",{}};}
void ready(AgentService& s){
    CHECK(s.connect("0.165.0","chatgpt:fixture@example.invalid")); auto frames=s.take_outbound(); CHECK(frames.size()==1); CHECK(frames[0].find("initialize")!=std::string::npos);
    auto id=agent_json::parse(frames[0]).at("id").scalar;
    s.feed_stdout("{\"id\":"+id+",\"result\":{\"userAgent\":\"fixture/0.165.0\"}}\n");
    frames=s.take_outbound(); CHECK(frames.size()==2); id=agent_json::parse(frames[1]).at("id").scalar;
    s.feed_stdout("{\"id\":"+id+",\"result\":{\"account\":{\"type\":\"chatgpt\",\"email\":\"fixture@example.invalid\"},\"requiresOpenaiAuth\":true}}\n");
    CHECK(s.snapshot().connection==AgentConnection::ready); CHECK(s.take_outbound().empty());
}
void running(AgentService& s){
    ready(s); CHECK(s.start({"fixture prompt","/fixture",true})); auto out=s.take_outbound(); CHECK(out.size()==1);
    auto id=agent_json::parse(out[0]).at("id").scalar;
    s.feed_stdout("{\"id\":"+id+",\"result\":{\"thread\":{\"id\":\"thread-a\",\"status\":{\"type\":\"idle\"}},\"cwd\":\"/fixture\",\"approvalPolicy\":\"untrusted\",\"approvalsReviewer\":\"user\",\"sandbox\":{\"type\":\"readOnly\",\"networkAccess\":false}}}\n");
    out=s.take_outbound(); CHECK(out.size()==1); CHECK(out[0].find("text_elements")!=std::string::npos);
    id=agent_json::parse(out[0]).at("id").scalar;
    s.feed_stdout("{\"id\":"+id+",\"result\":{\"turn\":{\"id\":\"turn-a\",\"status\":\"inProgress\"}}}\n");
    CHECK(s.snapshot().conversation==AgentConversation::running);
}
void approval(AgentService& s,std::string id="7"){
    s.feed_stdout("{\"method\":\"item/started\",\"params\":{\"threadId\":\"thread-a\",\"turnId\":\"turn-a\",\"item\":{\"id\":\"item-a\",\"type\":\"commandExecution\",\"command\":\"echo fixture\",\"cwd\":\"/fixture\"}}}\n");
    s.feed_stdout("{\"id\":"+id+",\"method\":\"item/commandExecution/requestApproval\",\"params\":{\"threadId\":\"thread-a\",\"turnId\":\"turn-a\",\"itemId\":\"item-a\",\"reason\":\"fixture approval\"}}\n");
}
int main(){
    { AgentService s(association()); CHECK(s.take_outbound().empty()); CHECK(!s.start({"test","/fixture",true})); CHECK(!s.connect("0.165.0","different-account")); CHECK(s.take_outbound().empty()); }
    { AgentService s(association()); ready(s); CHECK(!s.start({"test","/fixture",false})); CHECK(s.take_outbound().empty()); }
    { AgentService s(association()); running(s); approval(s); auto snap=s.snapshot(); CHECK(snap.approvals.size()==1); auto key=snap.approvals[0].key; CHECK(snap.approvals[0].can_accept); CHECK(s.resolve(key,AgentApprovalDecision::accept_once)); CHECK(!s.resolve(key,AgentApprovalDecision::accept_once)); auto out=s.take_outbound(); CHECK(out.size()==1 && out[0].find("accept")!=std::string::npos); }
    { AgentService s(association()); running(s); approval(s,"\"7\""); auto key=s.snapshot().approvals[0].key; CHECK(key.request_id=="\"7\""); s.feed_stdout("{\"method\":\"serverRequest/resolved\",\"params\":{\"threadId\":\"thread-a\",\"requestId\":\"7\"}}\n"); CHECK(!s.resolve(key,AgentApprovalDecision::accept_once)); }
    { AgentService s(association()); running(s); approval(s); auto key=s.snapshot().approvals[0].key; CHECK(s.interrupt()); CHECK(s.snapshot().conversation==AgentConversation::interrupting); CHECK(!s.resolve(key,AgentApprovalDecision::accept_once)); auto out=s.take_outbound(); CHECK(out.size()==2); auto id=agent_json::parse(out.back()).at("id").scalar; s.feed_stdout("{\"id\":"+id+",\"result\":{}}\n"); CHECK(s.snapshot().conversation==AgentConversation::interrupting); s.feed_stdout("{\"method\":\"turn/completed\",\"params\":{\"threadId\":\"thread-a\",\"turn\":{\"id\":\"turn-a\",\"status\":\"interrupted\"}}}\n"); CHECK(s.snapshot().conversation==AgentConversation::interrupted); }
    { AgentService s(association()); running(s); approval(s); auto key=s.snapshot().approvals[0].key; s.disconnect(); CHECK(s.snapshot().approvals.empty()); ready(s); CHECK(s.take_outbound().empty()); CHECK(!s.resolve(key,AgentApprovalDecision::accept_once)); CHECK(s.snapshot().conversation==AgentConversation::idle); }
    { AgentService s(association()); running(s); approval(s); approval(s); CHECK(s.snapshot().connection==AgentConnection::failed); CHECK(s.snapshot().approvals.empty()); }
    { AgentService s(association()); running(s); s.feed_stdout("{\"id\":99,\"method\":\"unknown/approveEverything\",\"params\":{}}\n"); auto out=s.take_outbound(); CHECK(out.size()==1 && out[0].find("-32601")!=std::string::npos); }
    { AgentService s(association()); running(s); s.feed_stdout("{\"id\":8,\"method\":\"item/commandExecution/requestApproval\",\"params\":{\"threadId\":\"other\",\"turnId\":\"turn-a\",\"itemId\":\"item-a\"}}\n"); CHECK(s.snapshot().approvals.empty()); CHECK(s.take_outbound()[0].find("cancel")!=std::string::npos); }
    { AgentService s(association()); running(s); std::string delta="{\"method\":\"item/agentMessage/delta\",\"params\":{\"threadId\":\"thread-a\",\"turnId\":\"turn-a\",\"itemId\":\"answer\",\"delta\":\"A\\ud83d\\ude80\"}}\n"; for(char c:delta) s.feed_stdout(std::string_view(&c,1)); CHECK(s.snapshot().transcript.back().text=="A🚀"); }
    for(auto bad:{"{\"id\":1,\"id\":2}","{\"x\":\"\\ud800\"}","{\"x\":01}","{\"x\":1e}","{\"x\":true,}","[]"}) { AgentService s(association()); ready(s); s.feed_stdout(std::string(bad)+"\n"); CHECK(s.snapshot().connection==AgentConnection::failed); }
    { AgentService s(association()); ready(s); s.feed_stdout(std::string(AgentService::maximum_frame_bytes+1,'x')); CHECK(s.snapshot().connection==AgentConnection::failed); }
    { AgentService s(association()); running(s); for(int n=0;n<300;++n) s.feed_stdout("{\"method\":\"item/agentMessage/delta\",\"params\":{\"threadId\":\"thread-a\",\"turnId\":\"turn-a\",\"itemId\":\"i"+std::to_string(n)+"\",\"delta\":\"fixture\"}}\n"); CHECK(s.snapshot().transcript.size()==256); CHECK(s.snapshot().transcript_truncated); }
    { AgentService s(association()); ready(s); CHECK(s.start({"fixture","/fixture",true})); auto out=s.take_outbound(); auto id=agent_json::parse(out[0]).at("id").scalar; s.feed_stdout("{\"id\":"+id+",\"result\":{\"thread\":{\"id\":\"x\",\"status\":{\"type\":\"idle\"}},\"cwd\":\"/fixture\",\"approvalPolicy\":\"never\"}}\n"); CHECK(s.snapshot().connection==AgentConnection::failed); CHECK(s.take_outbound().empty()); }
    { std::vector<WorkspaceAgentAssociation> before{association()}, after; before[0].thread_id="saved-thread"; auto bytes=serialize_agent_associations(before); std::string error; CHECK(deserialize_agent_associations(bytes,after,error)); CHECK(after[0].thread_id=="saved-thread"); AgentService restored(after[0]); CHECK(restored.snapshot().conversation==AgentConversation::idle); CHECK(restored.take_outbound().empty()); auto saved=after; CHECK(!deserialize_agent_associations("{\"version\":2,\"associations\":[]}",after,error)); CHECK(after[0].thread_id==saved[0].thread_id); }
    { AgentService s(association()); running(s); std::thread reader([&]{for(int n=0;n<1000;++n)(void)s.snapshot();}); for(int n=0;n<1000;++n)s.feed_stdout("{\"method\":\"unknown/event\",\"params\":{}}\n"); reader.join(); }
    { AgentService s(association()); CHECK(s.connect("future-compatible-version","chatgpt:fixture@example.invalid")); auto init=s.take_outbound(); auto id=agent_json::parse(init[0]).at("id").scalar; s.feed_stdout("{\"id\":"+id+",\"result\":{\"userAgent\":\"fixture\"}}\n"); auto frames=s.take_outbound(); CHECK(frames.size()==2); id=agent_json::parse(frames.back()).at("id").scalar; s.feed_stdout("{\"id\":"+id+",\"result\":{\"account\":{\"type\":\"chatgpt\",\"email\":\"other@example.invalid\"}}}\n"); CHECK(s.snapshot().connection==AgentConnection::failed); CHECK(s.take_outbound().empty()); }
    { AgentService a(association()), b(association()); ready(a); ready(b); CHECK(a.snapshot().generation!=b.snapshot().generation); }
    { AgentService s(association()); running(s); s.feed_stdout("{\"id\":19,\"method\":\"item/commandExecution/requestApproval\",\"params\":{\"threadId\":\"thread-a\",\"turnId\":\"turn-a\",\"itemId\":\"missing-scope\"}}\n"); auto a=s.snapshot().approvals[0]; CHECK(!a.can_accept); CHECK(!s.resolve(a.key,AgentApprovalDecision::accept_once)); CHECK(s.resolve(a.key,AgentApprovalDecision::decline)); }
    { AgentService s(association()); running(s); s.feed_stdout("{\"method\":\"account/updated\",\"params\":{}}\n"); CHECK(s.snapshot().connection==AgentConnection::failed); }
    { AgentService s(association()); running(s); for(int n=0;n<33;++n) s.feed_stdout("{\"id\":"+std::to_string(n+100)+",\"method\":\"item/commandExecution/requestApproval\",\"params\":{\"threadId\":\"thread-a\",\"turnId\":\"turn-a\",\"itemId\":\"missing-scope\"}}\n"); CHECK(s.snapshot().connection==AgentConnection::failed); CHECK(s.snapshot().approvals.empty()); }
    std::cout<<"agent service fixtures passed (no live provider)\n";
}
