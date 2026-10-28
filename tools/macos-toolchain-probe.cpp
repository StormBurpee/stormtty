// Native compile + runtime proof for the pinned LLVM/libc++ cancellation API.
// CTest runs this with a timeout. No experimental feature macro is permitted.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION < 200000 || _LIBCPP_VERSION >= 210000
#error "The pinned macOS probe requires libc++ major 20 headers"
#endif
#if !defined(__clang_major__) || __clang_major__ != 20
#error "The pinned macOS probe requires LLVM clang major 20"
#endif
#if defined(_LIBCPP_ENABLE_EXPERIMENTAL) || defined(_LIBCPP_HAS_EXPERIMENTAL_STOP_TOKEN)
#error "Experimental libc++ feature macros are not an acceptable toolchain fix"
#endif
#endif
#ifndef STORMTTY_EXPECTED_LLVM_ROOT
#define STORMTTY_EXPECTED_LLVM_ROOT ""
#endif
namespace {
void require(bool condition,const char* error){if(!condition)throw std::runtime_error(error);}
void cancellation_probe(){
    std::stop_source source;const auto token=source.get_token();std::atomic<unsigned> calls{0};
    require(token.stop_possible()&&!token.stop_requested(),"Fresh stop token state is invalid");
    std::stop_callback callback(token,[&]{++calls;});
    require(source.request_stop(),"First request_stop failed");
    require(!source.request_stop()&&token.stop_requested()&&calls==1,"Stop callback did not run exactly once");
    {std::stop_callback late(token,[&]{++calls;});}
    require(calls==2,"Registration after cancellation did not invoke callback");
    std::mutex mutex;std::condition_variable_any condition;
    std::promise<void> entered;auto ready=entered.get_future();std::atomic<bool> cancelled{false};
    std::jthread worker([&](std::stop_token stop){
        std::unique_lock lock(mutex);entered.set_value();
        const bool predicate=condition.wait(lock,stop,[]{return false;});
        cancelled=!predicate&&stop.stop_requested();
    });
    require(ready.wait_for(std::chrono::seconds(2))==std::future_status::ready,"Cancellation worker did not start");
    worker.request_stop();worker.join();require(cancelled,"Cancellable condition_variable_any failed");
    std::atomic<bool> destructor_cancelled{false};
    {std::jthread automatic([&](std::stop_token stop){std::unique_lock lock(mutex);condition.wait(lock,stop,[]{return false;});destructor_cancelled=stop.stop_requested();});}
    require(destructor_cancelled,"jthread destruction did not stop and join its worker");
}
void runtime_probe(const std::filesystem::path& expected){
#ifdef __APPLE__
    require(!expected.empty(),"Pass the expected LLVM prefix to verify the loaded libc++ runtime");
    const auto root=std::filesystem::weakly_canonical(expected);
    bool matching=false;
    for(std::uint32_t i=0;i<_dyld_image_count();++i){
        const char* raw=_dyld_get_image_name(i);if(!raw)continue;
        const std::filesystem::path path(raw);const auto name=path.filename().string();
        if(name.starts_with("libc++.")&&name.ends_with(".dylib")){
            std::cout<<"Loaded libc++: "<<path<<'\n';
            const auto actual=std::filesystem::weakly_canonical(path);
            matching|=actual.parent_path()==root/"lib/c++";
        }
    }
    require(matching,"The pinned LLVM libc++ runtime was not loaded; header-only substitution is invalid");
    std::cout<<"Compiler: "<<__clang_version__<<"; libc++ headers: "<<_LIBCPP_VERSION<<'\n';
#else
    (void)expected;
    std::cout<<"Non-Apple semantic check only; not macOS toolchain evidence\n";
#endif
}
}
int main(int argc,char** argv){
    try{runtime_probe(argc>1?argv[1]:STORMTTY_EXPECTED_LLVM_ROOT);cancellation_probe();
        std::cout<<"PASS stop_source/token/callback, jthread, cancellable wait and destructor join\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL macOS toolchain probe: "<<e.what()<<'\n';return 1;}
}
