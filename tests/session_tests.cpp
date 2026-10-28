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
#else
#include <unistd.h>
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
#ifdef _WIN32
        const auto identity=std::to_string(GetCurrentProcessId());
#else
        const auto identity=std::to_string(getpid());
#endif
        std::cout << "\x1b[31mREADY:" << identity << "\x1b[0m\r\n" << std::flush;
        std::string line; std::getline(std::cin,line);
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        std::cout << "GOT:" << line << " PID:" << identity << "\r\n" << std::flush;
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
            const auto initial_text=frame_text(original);
            const auto identity_start=initial_text.find("READY:")+6;
            const auto identity=initial_text.substr(identity_start,initial_text.find_first_of(" \r\n",identity_start)-identity_start);
            const auto sent_before=session->info().bytes_sent;
            const Color preview_fg{224,225,226,255}, preview_bg{16,17,18,255};
            check(session->set_default_colors(preview_fg,preview_bg)==SessionSendResult::accepted,"live theme not accepted");
            check(await([&]{auto f=session->frame();return f && f->foreground==preview_fg && f->background==preview_bg;},wakes),"live theme did not repaint existing session");
            const auto preview=session->frame();
            check(preview->lines[0]->cells[0].foreground==original->lines[0]->cells[0].foreground,"explicit ANSI foreground changed");
            // Rapid previews use one separately bounded latest-value slot. Neither
            // obsolete previews nor the final Cancel can displace PTY input.
            for (unsigned i=0;i<2000;++i) {
                const Color fg{static_cast<std::uint8_t>(180+i%50),225,226,255};
                check(session->set_default_colors(fg,preview_bg)==SessionSendResult::accepted,"rapid theme update was backpressured");
            }
            check(session->set_default_colors(original->foreground,original->background)==SessionSendResult::accepted,"theme cancel not accepted");
            check(await([&]{auto f=session->frame();return f && f->generation>preview->generation && f->foreground==original->foreground && f->background==original->background;},wakes),"latest theme cancel was lost or reordered");
            check(session->info().bytes_sent==sent_before,"display-only update wrote PTY bytes");
            check(frame_text(session->frame())==initial_text,"theme update changed terminal content");
            check(original->background!=preview->background && preview->background==preview_bg,"old theme snapshot mutated");
            bool rejected{};
            try { session->set_default_colors({1,2,3,1},preview_bg); } catch(const std::invalid_argument&) { rejected=true; }
            check(rejected,"non-opaque session theme accepted");
            check(session->text("hello")==SessionSendResult::accepted,"text not accepted");
            KeyInput enter;enter.key=Key::enter;
            check(session->key(enter)==SessionSendResult::accepted,"enter not accepted");
            check(session->wait_closed(5s),"natural child exit did not close actor");
            check(session->info().state==SessionState::exited,"natural child exit state");
            check(session->info().exit_code==7,"exit code lost");
            check(frame_text(session->frame()).find("GOT:hello PID:"+identity)!=std::string::npos,"input ordering or child identity changed during theme preview");
            const auto exited_frame=session->frame();
            check(session->set_default_colors(preview_fg,preview_bg)==SessionSendResult::closed,"exited session accepted a theme update");
            check(session->frame()==exited_frame,"exited session final frame changed");
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
