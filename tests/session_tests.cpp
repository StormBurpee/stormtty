#include "stormtty/session.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace stormtty;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string frame_text(const FramePtr& frame) {
    std::string text;
    if (frame) for (const auto& row : frame->lines) {
        for (const auto& cell : row->cells) text += cell.grapheme.empty() ? " " : cell.grapheme;
        text += '\n';
    }
    return text;
}
struct WakeCounter {
    std::mutex mutex; std::condition_variable cv; unsigned wakes{};
    void signal() { { std::lock_guard lock(mutex); ++wakes; } cv.notify_all(); }
};
template<class Predicate> bool await(Predicate predicate, const std::shared_ptr<WakeCounter>& wake, std::chrono::milliseconds timeout=5s) {
    const auto end=std::chrono::steady_clock::now()+timeout;
    while (!predicate()) {
        std::unique_lock lock(wake->mutex);
        if (wake->cv.wait_until(lock,end)==std::cv_status::timeout) return predicate();
    }
    return true;
}
int child(std::string_view mode) {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8); SetConsoleOutputCP(CP_UTF8);
    DWORD output_mode{};
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &output_mode))
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    if(mode=="echo") {
        std::cout << "\x1b[31mREADY\x1b[0m\r\n" << std::flush;
        std::string line; std::getline(std::cin,line);
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        std::cout << "GOT:" << line << "\r\n" << std::flush;
        return 7;
    }
    if(mode=="flood") {
        for(std::uint64_t i=0;;++i) std::cout << "STREAM:" << i << " \x1b[32moutput\x1b[0m\r\n" << std::flush;
    }
    return 3;
}
}
int main(int argc,char** argv) {
    if(argc==3 && std::string_view(argv[1])=="--child")return child(argv[2]);
    try {
        const auto executable=std::filesystem::absolute(argv[0]).string();
        {
            auto wakes=std::make_shared<WakeCounter>();
            ProcessOptions p; p.executable=executable; p.arguments={"--child","echo"};
            TerminalOptions t; t.size={80,24,8,16};
            auto session=Session::start(p,t,[wakes]{wakes->signal();});
            check(await([&]{return frame_text(session->frame()).find("READY")!=std::string::npos;},wakes),"real child output did not reach frame");
            const auto original=session->frame();
            check(session->text("hello")==SessionSendResult::accepted,"text not accepted");
            KeyInput enter;enter.key=Key::enter;
            check(session->key(enter)==SessionSendResult::accepted,"enter not accepted");
            check(session->wait_closed(5s),"natural child exit did not close actor");
            check(session->info().state==SessionState::exited,"natural child exit state");
            check(session->info().exit_code==7,"exit code lost");
            check(frame_text(session->frame()).find("GOT:hello")!=std::string::npos,"input/last output ordering failed");
            check(frame_text(original).find("READY")!=std::string::npos,"published old frame invalidated");
            check(session->text("late")==SessionSendResult::closed,"closed input accepted");
            session->request_close();session->request_close();
        }
        {
            auto wakes=std::make_shared<WakeCounter>();
            ProcessOptions p;p.executable=executable;p.arguments={"--child","flood"};
            auto session=Session::start(p,{},[wakes]{wakes->signal();});
            check(await([&]{return session->info().bytes_received>10000;},wakes),"flood child failed");
            check(session->paste(std::string(2*1024*1024,'x'))==SessionSendResult::too_large,"input bound not enforced");
            const auto start=std::chrono::steady_clock::now();session->request_close();
            check(std::chrono::steady_clock::now()-start<100ms,"UI close blocked");
            check(session->wait_closed(5s),"close-under-flood did not finish");
        }
        {
            auto wakes=std::make_shared<WakeCounter>();
            ProcessOptions p;p.executable=executable+".missing";
            auto session=Session::start(p,{},[wakes]{wakes->signal();});
            check(session->wait_closed(5s),"launch failure hung");
            check(session->info().state==SessionState::failed,"launch failure not reported");
            check(!session->info().error.empty(),"launch failure lacks diagnostic");
        }
        for(int i=0;i<8;++i) {
            ProcessOptions p;p.executable=executable;p.arguments={"--child","flood"};
            auto session=Session::start(p,{});session->request_close();
            check(session->wait_closed(5s),"immediate close race hung");
        }
        std::cout << "Session: real child, input ordering, immutable frame, bounded queue, flood close, launch failure, immediate-close races passed\n";
        return 0;
    } catch(const std::exception& e){std::cerr << "FAIL: " << e.what() << '\n';return 1;}
}
