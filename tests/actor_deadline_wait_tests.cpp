#include "stormtty/actor_deadline_wait.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
using namespace stormtty;
using namespace std::chrono_literals;
void check(bool ok){if(!ok)throw std::runtime_error("actor deadline invariant");}
int main(){try{
    auto wait=make_actor_deadline_wait();check(bool(wait));
    wait->notify();check(wait->prepare_wait()); // discard stale notification under conceptual queue lock
    auto deadline=ActorDeadlineWait::Clock::now()+3ms;
    check(wait->wait_until(deadline).reason==ActorDeadlineWait::Reason::deadline);
    check(ActorDeadlineWait::Clock::now()>=deadline);
    std::mutex mutex;std::condition_variable cv;unsigned offered{},accepted{};bool stop{};
    std::jthread producer([&]{
        for(unsigned i=1;i<=512;++i){
            std::unique_lock lock(mutex);cv.wait(lock,[&]{return accepted==i-1||stop;});if(stop)return;
            offered=i;wait->notify(); // same queue lock used by prepare_wait
            lock.unlock();std::this_thread::yield();
        }
        std::lock_guard lock(mutex);stop=true;wait->notify();
    });
    bool valid=true;
    while(true){
        std::unique_lock lock(mutex);
        if(offered>accepted){accepted=offered;cv.notify_all();}
        if(stop)break;
        if(!wait->prepare_wait()){valid=false;stop=true;cv.notify_all();break;}
        lock.unlock();
        if(wait->wait_until(ActorDeadlineWait::Clock::now()+2s).reason!=ActorDeadlineWait::Reason::notified){
            lock.lock();valid=false;stop=true;cv.notify_all();break;
        }
    }
    producer.join();check(valid&&accepted==512);
    // Rearm after producer completion: no stale event or periodic wake persists.
    {std::lock_guard lock(mutex);check(wait->prepare_wait());}
    deadline=ActorDeadlineWait::Clock::now()+3ms;check(wait->wait_until(deadline).reason==ActorDeadlineWait::Reason::deadline);
    wait.reset();std::cout<<"PASS native deadline and 512 queue-locked producer/prepare races, close, stale-event clearing and handle lifetime\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
