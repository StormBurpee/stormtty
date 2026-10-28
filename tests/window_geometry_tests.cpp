#include "stormtty/native_window.hpp"
#include <cstdlib>
#include <iostream>

namespace { void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
} }
int main() {
    using namespace stormtty;
    auto chrome = layout_chrome(1100, 700, {{1}, {2}}, {1});
    require(chrome.tabs.size() == 2 && !chrome.compact, "two normal tabs");
    require(chrome.drag.width >= 48, "deliberate draggable gap");
    require(chrome.band.height == 48, "48-DIP crown");
    require(chrome.tabs[0].bounds.y == 8 && chrome.tabs[0].bounds.height == 32, "fixed 32-DIP tabs at y8, not band minus8");
    require(chrome.tabs[0].bounds.width == 188, "preferred tab width");
    require(chrome.tabs[1].bounds.x - chrome.tabs[0].bounds.x - chrome.tabs[0].bounds.width == 4, "4-DIP tab gap");
    require(chrome_hit_test(chrome, {chrome.maximize.x + 10, 15}, {1100, 700}, 6, false) == NativeHit::maximize, "Snap maximize region");
    require(chrome_hit_test(chrome, {60, 20}, {1100, 700}, 6, false) == NativeHit::client, "tab exclusion");
    require(chrome_hit_test(chrome, {1, 1}, {1100, 700}, 6, false) == NativeHit::top_left, "resize corner");
    require(chrome_hit_test(chrome, {1, 699}, {1100, 700}, 6, true) == NativeHit::client, "no maximized resize border");
    chrome = layout_chrome(330, 240, {{1}, {2}, {3}}, {2});
    require(chrome.compact && chrome.tabs.size() == 1 && chrome.tabs.front().tab == TabId{2}, "compact active tab");
    require(chrome.new_tab.x + chrome.new_tab.width <= chrome.palette.x, "compact controls don't overlap");
    require(chrome.drag.width >= 48, "compact drag gap");
    for (int width=330;width<=2400;width+=17) {
        for (int count=1;count<=32;++count) {
            std::vector<TabId> tabs;
            for (int i=1;i<=count;++i) tabs.push_back(TabId{static_cast<std::uint64_t>(i)});
            const auto geometry=layout_chrome(width,720,tabs,tabs.back());
            require(geometry.minimize.x>=0 && geometry.close.x+geometry.close.width==width, "caption region remains in client width");
            require(geometry.drag.width>=48, "overflow never consumes deliberate drag gap");
            require(geometry.new_tab.x+geometry.new_tab.width<=geometry.palette.x, "new-tab and palette don't overlap");
            for (const auto& tab:geometry.tabs) {
                require(tab.bounds.x>=geometry.menu.x+geometry.menu.width, "menu and tab don't overlap");
                require(tab.bounds.x+tab.bounds.width<=geometry.new_tab.x, "tab and new-tab don't overlap");
                require(chrome_hit_test(geometry,{tab.bounds.x+tab.bounds.width/2,20},{static_cast<double>(width),720},6,false)==NativeHit::client,"every visible tab excludes drag");
            }
        }
    }
    chrome = layout_chrome(330,240,{{1},{2}},{1});
    require(chrome.new_tab.width == 0, "compact hides new-tab before sacrificing drag/caption");
    chrome = layout_chrome(1100,720,{{1}},{1},46,0);
    require(chrome.band.height==0 && chrome.tabs.front().bounds.height==0, "fullscreen chrome has no hit area");
    ModalFocus modal;
    require(modal.activate()==ModalDecision::cancel,"modal starts with safe Cancel");
    modal.next(); require(modal.activate()==ModalDecision::confirm,"Tab explicitly focuses destructive choice");
    modal.next(); require(modal.activate()==ModalDecision::cancel,"modal focus cycles without escaping");
    modal.select(ModalDecision::confirm); modal.reset();
    require(modal.activate()==ModalDecision::cancel,"new decision resets to Cancel");
    for (int width=330;width<=2200;width+=37) for (int height=180;height<=1000;height+=53) {
        const auto box=layout_modal(width,height);
        require(box.panel.x>=0 && box.panel.y>=48,"modal fits below integrated crown");
        require(box.panel.x+box.panel.width<=width && box.panel.y+box.panel.height<=height,"modal fits resized client");
        require(box.cancel.x+box.cancel.width<=box.confirm.x,"modal actions never overlap");
        require(box.body.y+box.body.height<=box.cancel.y && box.body.height>=18,"compact modal retains warning above actions");
    }
    ChromeTransition transition;
    require(!transition.active(100) && transition.value(100)==0, "resting transition has no scheduler work");
    transition.target(1,100,80);
    require(transition.value(100)==0 && transition.active(100), "transition starts from current presentation");
    require(transition.value(140)>0 && transition.value(140)<1, "finite hover interpolation");
    const auto midway=transition.value(140); transition.target(0,140,80);
    require(transition.value(140)==midway, "interrupted hover continues without jump");
    require(!transition.active(220) && transition.value(220)==0, "transition stops exactly at deadline");
    transition.target(1,300,0);
    require(!transition.active(300) && transition.value(300)==1, "reduced motion commits immediately without timer");
    transition.target(0,400,120); transition.finish();
    require(!transition.active(401) && transition.value(401)==0, "minimize/accessibility finishes timer work");
    Utf16Input input;
    require(input.feed(0xd83d).empty(), "hold high surrogate");
    require(input.feed(0xde80) == "\xf0\x9f\x9a\x80", "supplementary scalar");
    require(input.feed(0xdc00) == "\xef\xbf\xbd", "orphan low surrogate");
    input.feed(0xd800);
    require(input.feed(u'A') == "\xef\xbf\xbd" "A", "malformed pair recovery");
    input.feed(0xd800); input.reset();
    require(input.feed(u'B') == "B", "focus cancels pending scalar");
    require(input.feed(0xd800).empty(), "second surrogate start");
    require(input.feed(0xd83d)=="\xef\xbf\xbd", "consecutive high surrogate recovery");
    require(input.feed(0xde80)=="\xf0\x9f\x9a\x80", "new high surrogate still pairs");
    std::cout << "window geometry and UTF-16 tests passed\n";
}
