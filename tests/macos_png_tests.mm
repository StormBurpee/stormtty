#include "../src/platform/macos/platform.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
constexpr std::array<std::uint8_t,70> png{
137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,
0,0,0,13,73,68,65,84,120,156,99,248,175,224,208,0,0,5,97,1,224,213,172,251,223,
0,0,0,0,73,69,78,68,174,66,96,130};
void test(){
    auto image=stormtty::decode_macos_png(png,4);require(bool(image),"Valid PNG rejected");
    require(image->width==1&&image->height==1&&image->rgba.size()==4,"PNG dimensions/size");
    const std::array<unsigned,4> expected{255,32,64,128};
    for(unsigned i=0;i<4;++i)require(std::abs(int(image->rgba[i])-int(expected[i]))<=2,"ImageIO did not produce straight RGBA");
    require(!stormtty::decode_macos_png(png,3),"Decoder exceeded output byte budget");
    require(!stormtty::decode_macos_png(std::span(png).first(32),4096),"Truncated PNG accepted");
    auto bad=png;bad[16]=127;require(!stormtty::decode_macos_png(bad,64u*1024u*1024u),"Huge IHDR accepted");
    bad=png;bad[1]='J';require(!stormtty::decode_macos_png(bad,4096),"Non-PNG accepted");
    std::atomic<bool> threaded{false};std::jthread actor([&]{threaded=stormtty::decode_macos_png(png,4).has_value();});actor.join();require(threaded,"Actor-thread ImageIO failed");
}
}
int main(){try{test();std::cout<<"PASS native macOS ImageIO PNG: RGBA, bounds, malformed input, actor thread\n";return 0;}
catch(const std::exception& e){std::cerr<<"FAIL macOS PNG: "<<e.what()<<'\n';return 1;}}
