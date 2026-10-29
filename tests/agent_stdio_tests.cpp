#include "stormtty/agent_stdio_transport.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <exception>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#endif
using namespace stormtty;
#define CHECK(x) do { if(!(x)) { std::cerr<<__LINE__<<": " #x "\n"; std::exit(1); } } while(false)
template<class F> void until(F predicate){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5); while(!predicate() && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(std::chrono::milliseconds(5)); CHECK(predicate());}
std::shared_ptr<AgentService> service(){return std::make_shared<AgentService>(WorkspaceAgentAssociation{"0123456789abcdef0123456789abcdef","codex","chatgpt:fixture@example.invalid",{}});}
std::atomic<const char*> phase{"startup"};
void stage(const char* name){phase=name;std::cerr<<"agent_stdio phase="<<phase.load()<<"\n"<<std::flush;}
void remove_fixture_directory(const std::filesystem::path& dir){
    std::error_code error;const bool removed=std::filesystem::remove(dir,error);
    if(error)std::cerr<<"agent_stdio phase="<<phase.load()<<" cleanup_error="<<error.value()<<" "<<error.message()<<"\n";
    CHECK(!error&&removed);
}
int main(int argc,char** argv){
 std::set_terminate([] {
    std::cerr<<"agent_stdio phase="<<phase.load()<<" terminate";
    if(auto error=std::current_exception())try{std::rethrow_exception(error);}catch(const std::exception& e){std::cerr<<" exception="<<e.what();}catch(...){std::cerr<<" nonstandard_exception";}
    std::cerr<<"\n"<<std::flush;std::_Exit(1);
 });
 try {
    CHECK(argc==2); std::atomic_int callbacks{}; std::string error;
    AgentStdioOptions options{std::filesystem::absolute(argv[1]).string(),std::filesystem::current_path().string(),"authored-fixture","chatgpt:fixture@example.invalid",false,[&]{++callbacks;}};
    { auto s=service(); CHECK(!AgentStdioTransport::launch(options,s,error)); CHECK(s->take_outbound().empty()); }
    options.user_confirmed_launch=true;stage("normal-roundtrip");
    { auto s=service(); auto transport=AgentStdioTransport::launch(options,s,error); CHECK(transport); until([&]{return s->snapshot().connection==AgentConnection::ready;}); CHECK(s->snapshot().association.thread_id.empty());
      CHECK(!s->start({"fixture",options.working_directory,false})); CHECK(s->start({"fixture",options.working_directory,true}));
      until([&]{return !s->snapshot().approvals.empty();}); auto a=s->snapshot().approvals[0]; CHECK(a.can_accept); CHECK(s->resolve(a.key,AgentApprovalDecision::accept_once));
      until([&]{auto snap=s->snapshot();return !snap.transcript.empty() && snap.transcript.back().text=="Approved fixture only";});
      CHECK(s->interrupt()); until([&]{return s->snapshot().conversation==AgentConversation::interrupted;}); transport->stop(); transport->stop(); CHECK(s->snapshot().connection==AgentConnection::disconnected); CHECK(callbacks>0);
    }
    stage("provider-malformed-and-exit");
    for(auto prompt:{"fixture-malformed","fixture-exit"}) { auto s=service(); auto transport=AgentStdioTransport::launch(options,s,error); CHECK(transport); until([&]{return s->snapshot().connection==AgentConnection::ready;}); CHECK(s->start({prompt,options.working_directory,true})); until([&]{auto c=s->snapshot().connection;return c==AgentConnection::failed || c==AgentConnection::disconnected;}); CHECK(s->take_outbound().empty()); transport->stop(); }
    stage("relative-path-rejected");
    { auto s=service(); auto bad=options; bad.executable="relative-provider"; CHECK(!AgentStdioTransport::launch(bad,s,error)); }
    stage("nonreading-child-stop");
    { auto dir=std::filesystem::current_path()/("agent-nonreading-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())); CHECK(std::filesystem::create_directory(dir)); auto blocked=options; blocked.working_directory=dir.string(); auto s=service(); auto transport=AgentStdioTransport::launch(blocked,s,error); CHECK(transport); until([&]{return s->snapshot().connection==AgentConnection::ready;}); CHECK(s->start({std::string(65536,'x'),blocked.working_directory,true})); until([&]{return !s->snapshot().association.thread_id.empty();}); std::this_thread::sleep_for(std::chrono::milliseconds(100)); auto begin=std::chrono::steady_clock::now(); transport.reset(); CHECK(std::chrono::steady_clock::now()-begin<std::chrono::seconds(2)); CHECK(s->snapshot().connection==AgentConnection::disconnected); remove_fixture_directory(dir); }
    stage("handshake-timeout-cleanup");
    { auto dir=std::filesystem::current_path()/("agent-handshake-stall-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())); CHECK(std::filesystem::create_directory(dir)); auto stalled=options; stalled.working_directory=dir.string(); auto s=service(); auto transport=AgentStdioTransport::launch(stalled,s,error); CHECK(transport); auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(18); while(s->snapshot().connection==AgentConnection::connecting && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20)); CHECK(s->snapshot().connection==AgentConnection::disconnected); CHECK(!s->snapshot().error.empty()); transport.reset(); remove_fixture_directory(dir); }
    stage("closed-stdin-disconnect");
    // Child closes its read end before acknowledging thread/start, but keeps
    // stdout alive. The following turn write must report EPIPE/broken pipe,
    // not kill the app or hide the race behind stdout EOF.
    {
#ifndef _WIN32
        struct sigaction before{},after{}; sigset_t mask_before{},mask_after{};
        CHECK(sigaction(SIGPIPE,nullptr,&before)==0);
        CHECK(pthread_sigmask(SIG_SETMASK,nullptr,&mask_before)==0);
#endif
        auto dir=std::filesystem::current_path()/("agent-closed-stdin-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        CHECK(std::filesystem::create_directory(dir)); auto closed=options; closed.working_directory=dir.string(); auto s=service();
        auto transport=AgentStdioTransport::launch(closed,s,error); CHECK(transport);
        until([&]{return s->snapshot().connection==AgentConnection::ready;});
        CHECK(s->start({std::string(65536,'x'),closed.working_directory,true}));
        until([&]{return s->snapshot().connection==AgentConnection::disconnected;});
        // Join both transport workers before checking the final diagnostic.
        // Otherwise the stdout worker can overwrite it after this assertion.
        transport->stop(); const auto terminal=s->snapshot();
        if(terminal.error!="Disconnected; execution outcome unknown. Reconcile before retry.")
            std::cerr<<"closed-stdin final error: "<<terminal.error<<"\n";
        CHECK(terminal.association.thread_id=="fixture-thread"); // thread/start reply consumed
        CHECK(terminal.conversation==AgentConversation::failed);
        CHECK(terminal.error=="Disconnected; execution outcome unknown. Reconcile before retry.");
        CHECK(s->take_outbound().empty()); transport.reset(); remove_fixture_directory(dir);
#ifndef _WIN32
        CHECK(sigaction(SIGPIPE,nullptr,&after)==0);
        CHECK(pthread_sigmask(SIG_SETMASK,nullptr,&mask_after)==0);
        CHECK(before.sa_handler==after.sa_handler);
        CHECK(sigismember(&mask_before,SIGPIPE)==sigismember(&mask_after,SIGPIPE));
#endif
    }
    stage("repeat-process-reap");
    // Each live authored child owns its working directory. Immediate removal
    // after stop is the contract; do not hide a race with retries or sleeps.
    for(unsigned n=0;n<20;++n) {
        const auto dir=std::filesystem::current_path()/("agent-reap-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));CHECK(std::filesystem::create_directory(dir));
        auto isolated=options;isolated.working_directory=dir.string();auto s=service();auto transport=AgentStdioTransport::launch(isolated,s,error);CHECK(transport);
        until([&]{return s->snapshot().connection==AgentConnection::ready;});transport->stop();transport.reset();remove_fixture_directory(dir);
    }
    std::cout<<"native stdio fixture tests passed (authored child only)\n";
 } catch(const std::exception& error) {std::cerr<<"agent_stdio phase="<<phase.load()<<" unexpected_exception="<<error.what()<<"\n";return 1;}
 catch(...) {std::cerr<<"agent_stdio phase="<<phase.load()<<" unexpected_nonstandard_exception\n";return 1;}
}
