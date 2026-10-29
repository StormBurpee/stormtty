#include "stormtty/qualification_deadline_wait.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <stdexcept>
namespace stormtty::qualification {
namespace {
struct Handle {
    HANDLE value{};
    ~Handle(){if(value)CloseHandle(value);}
    Handle()=default;
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
class WindowsDeadlineWait final:public DeadlineWait {
    Handle event_,timer_;
    std::atomic<DWORD> notify_error_{};
public:
    WindowsDeadlineWait() {
        // Unnamed, non-inheritable, auto-reset; a pre-wait signal stays latched.
        event_.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        timer_.value=CreateWaitableTimerExW(nullptr,nullptr,0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,TIMER_MODIFY_STATE|SYNCHRONIZE);
        if(!event_.value||!timer_.value)throw std::runtime_error("Qualification high-resolution deadline timer unavailable");
    }
    bool prepare_wait() noexcept override {
        return !notify_error_.load(std::memory_order_acquire) && ResetEvent(event_.value);
    }
    void notify() noexcept override {
        if(!SetEvent(event_.value))notify_error_.store(GetLastError(),std::memory_order_release);
    }
    Result wait_until(Clock::time_point deadline) noexcept override {
        if(const auto error=notify_error_.load(std::memory_order_acquire))return {Reason::failed,error};
        const auto now=Clock::now();
        if(deadline<=now)return {Reason::deadline};
        if(deadline==Clock::time_point::max())return {Reason::failed,ERROR_INVALID_PARAMETER};
        // Round UP to 100ns: never deliberately wake earlier than the policy deadline.
        const auto ticks=std::chrono::ceil<std::chrono::duration<long long,std::ratio<1,10000000>>>(deadline-now).count();
        LARGE_INTEGER due;due.QuadPart=-std::max(1LL,ticks);
        if(!SetWaitableTimerEx(timer_.value,&due,0,nullptr,nullptr,nullptr,0))return {Reason::failed,GetLastError()};
        // Producer event first: pending work wins a simultaneous timer signal.
        const HANDLE handles[]{event_.value,timer_.value};
        const auto result=WaitForMultipleObjects(2,handles,FALSE,INFINITE);
        const auto error=result==WAIT_FAILED?GetLastError():ERROR_INVALID_FUNCTION;
        if(!CancelWaitableTimer(timer_.value))return {Reason::failed,GetLastError()};
        if(const auto notification_error=notify_error_.load(std::memory_order_acquire))return {Reason::failed,notification_error};
        if(result==WAIT_OBJECT_0)return {Reason::notified};
        if(result==WAIT_OBJECT_0+1)return {Reason::deadline};
        return {Reason::failed,error};
    }
};
}
std::unique_ptr<DeadlineWait> make_high_resolution_deadline_wait(){return std::make_unique<WindowsDeadlineWait>();}
}
