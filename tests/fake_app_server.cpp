// Authored fixture, NOT Codex and never makes network/model/auth requests.
#include "../src/agent/json.hpp"
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <string>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
using namespace stormtty::agent_json;
void send(std::string s) { std::cout<<s<<'\n'<<std::flush; }
int main(int argc,char** argv) {
    if(argc!=2 || std::string(argv[1])!="app-server") return 2;
    const auto mode=std::filesystem::current_path().filename().string();
    if(mode.starts_with("agent-handshake-stall-")) for(;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    std::string line;
    while(std::getline(std::cin,line)) {
        try {
            auto msg=parse(line); auto method=msg.at("method").text();
            if(method=="initialized") continue;
            if(method.empty()) { if(msg.at("result").at("decision").text()=="accept") send("{\"method\":\"item/agentMessage/delta\",\"params\":{\"threadId\":\"fixture-thread\",\"turnId\":\"fixture-turn\",\"itemId\":\"answer\",\"delta\":\"Approved fixture only\"}}"); continue; }
            auto request_id=id(msg.at("id"));
            if(method=="thread/start" && mode.starts_with("agent-closed-stdin-")) {
#ifdef _WIN32
                _close(0);
#else
                close(STDIN_FILENO);
#endif
            }
            if(method=="initialize") { std::cerr<<"fixture stderr (not protocol)\n"<<std::flush; send("{\"id\":"+request_id+",\"result\":{\"userAgent\":\"stormtty-authored-fixture\"}}"); }
            else if(method=="account/read") send("{\"id\":"+request_id+",\"result\":{\"account\":{\"type\":\"chatgpt\",\"email\":\"fixture@example.invalid\"},\"requiresOpenaiAuth\":true}}");
            else if(method=="thread/start" || method=="thread/resume") send("{\"id\":"+request_id+",\"result\":{\"thread\":{\"id\":\"fixture-thread\",\"status\":{\"type\":\"idle\"}},\"cwd\":"+quote(msg.at("params").at("cwd").text())+",\"approvalPolicy\":\"untrusted\",\"approvalsReviewer\":\"user\",\"sandbox\":{\"type\":\"readOnly\",\"networkAccess\":false}}}");
            else if(method=="turn/start") {
                auto prompt=msg.at("params").at("input").array[0].at("text").text();
                if(prompt=="fixture-malformed") { send("{broken-json"); continue; }
                if(prompt=="fixture-exit") return 0;
                send("{\"id\":"+request_id+",\"result\":{\"turn\":{\"id\":\"fixture-turn\",\"status\":\"inProgress\"}}}");
                send("{\"id\":\"fixture-approval\",\"method\":\"item/commandExecution/requestApproval\",\"params\":{\"kind\":\"command\",\"threadId\":\"fixture-thread\",\"turnId\":\"fixture-turn\",\"itemId\":\"fixture-item\",\"command\":\"echo authored fixture\",\"cwd\":\"/fixture\"}}");
            } else if(method=="turn/interrupt") {
                send("{\"id\":"+request_id+",\"result\":{}}");
                send("{\"method\":\"turn/completed\",\"params\":{\"threadId\":\"fixture-thread\",\"turn\":{\"id\":\"fixture-turn\",\"status\":\"interrupted\"}}}");
            } else return 3;
            if((method=="thread/start" || method=="thread/resume") && (mode.starts_with("agent-nonreading-") || mode.starts_with("agent-closed-stdin-"))) for(;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        } catch(...) { return 4; }
    }
}
