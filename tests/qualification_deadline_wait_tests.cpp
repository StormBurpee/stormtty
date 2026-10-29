#include "stormtty/qualification_deadline_wait.hpp"
#include <thread>
#include <iostream>
#include <stdexcept>
#include <latch>
using namespace stormtty::qualification;
using namespace std::chrono_literals;
void check(bool ok){if(!ok)throw std::runtime_error("deadline event invariant");}
int main(){try{
    auto wait=make_high_resolution_deadline_wait();
    wait->notify();check(wait->wait_until(DeadlineWait::Clock::now()+1s).reason==DeadlineWait::Reason::notified);
    auto deadline=DeadlineWait::Clock::now()+3ms;
    check(wait->wait_until(deadline).reason==DeadlineWait::Reason::deadline);
    check(DeadlineWait::Clock::now()>=deadline); // not a performance threshold
    for(int i=0;i<128;++i){
        std::latch started(1);
        std::jthread producer([&]{started.wait();wait->notify();});
        started.count_down();
        check(wait->wait_until(DeadlineWait::Clock::now()+1s).reason==DeadlineWait::Reason::notified);
    }
    // Consumed notifications must not become a permanent busy wake.
    deadline=DeadlineWait::Clock::now()+3ms;
    check(wait->wait_until(deadline).reason==DeadlineWait::Reason::deadline);
    // Actor close/shutdown uses the same latched notification as input/output.
    std::jthread closer([&]{std::this_thread::sleep_for(2ms);wait->notify();});
    check(wait->wait_until(DeadlineWait::Clock::now()+1s).reason==DeadlineWait::Reason::notified);
    closer.join();wait.reset();
    std::cout<<"PASS high-resolution deadline, pre-wait notification, racing notification, rearm, shutdown and lifetime\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
