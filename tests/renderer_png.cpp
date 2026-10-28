// Native Windows WIC test; link stormtty_core + platform PNG decoder.
#include "stormtty/renderer.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>

namespace {
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
constexpr std::array<std::uint8_t,70> png{
137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,
0,0,0,13,73,68,65,84,120,156,99,248,175,224,208,0,0,5,97,1,224,213,172,251,223,
0,0,0,0,73,69,78,68,174,66,96,130};
void test() {
    const auto decoded=stormtty::decode_windows_png(png,4);
    require(decoded.has_value(),"valid RGBA PNG rejected");
    require(decoded->width==1 && decoded->height==1,"PNG dimensions wrong");
    require(decoded->rgba==std::vector<std::uint8_t>({255,32,64,128}),"PNG output is not straight RGBA");
    require(!stormtty::decode_windows_png(png,3),"PNG decoded beyond byte budget");
    require(!stormtty::decode_windows_png(std::span(png).first(32),4096),"truncated header accepted");
    auto bomb=png;bomb[16]=127;bomb[17]=255;bomb[18]=255;bomb[19]=255;
    require(!stormtty::decode_windows_png(bomb,64u*1024u*1024u),"enormous PNG header accepted");
    auto wrong=png;wrong[1]='J';require(!stormtty::decode_windows_png(wrong,4096),"non-PNG input accepted");
    std::atomic<bool> threaded_ok{false};
    std::jthread actor([&] {const auto image=stormtty::decode_windows_png(png,4);threaded_ok=image && image->rgba.size()==4;});
    actor.join();require(threaded_ok,"WIC actor-thread COM initialization failed");
}
}
int main() {
    try {test();std::cout<<"PASS Windows WIC PNG: RGBA, byte/dimension budget, malformed input, actor thread\n";return 0;}
    catch(const std::exception& error) {std::cerr<<"FAIL PNG decoder: "<<error.what()<<'\n';return 1;}
}
