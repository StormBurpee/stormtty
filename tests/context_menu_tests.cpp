#include "core_test.hpp"
#include "stormtty/context_menu.hpp"
#include <algorithm>
#include <limits>
#include <set>

using namespace stormtty;
namespace {
ContextMenuItem item(std::string id,std::string label) {ContextMenuItem result;result.id=std::move(id);result.label=std::move(label);return result;}
std::vector<ContextMenuItem> model() {
    auto split=item("pane.split_right","Split right");split.shortcut="Ctrl+Shift+D";
    auto unavailable=item("pane.move_tab","Move pane to tab");unavailable.enabled=false;unavailable.disabled_reason="Create another tab first";
    auto swap=item("pane.swap","Swap panes");swap.children={item("pane.swap:21","Command Prompt"),item("pane.swap:22","PowerShell")};
    auto close=item("pane.close","Close pane");close.destructive=true;
    return {split,item("pane.split_down","Split below"),ContextMenuItem::separator(),unavailable,swap,ContextMenuItem::separator(),close};
}
Point center(const Rect& r) {return {r.x+r.width/2,r.y+r.height/2};}
const ContextMenuRow& row(const ContextMenu& menu,std::string_view id,std::size_t depth=0) {
    CHECK(depth<menu.layout().panels.size());
    const auto& rows=menu.layout().panels[depth].rows;
    const auto found=std::find_if(rows.begin(),rows.end(),[&](const auto& r){return r.id==id;});
    CHECK(found!=rows.end());return *found;
}
void click(ContextMenu& menu,std::string_view id,std::size_t depth=0) {
    const auto& target=row(menu,id,depth);CHECK(target.visible);const auto p=center(target.bounds);
    CHECK(menu.pointer_down(p));CHECK(menu.pointer_up(p));
}
void key(ContextMenu& menu,const char* key) {CHECK(menu.key({{key}}));}
void check_scene(const WindowScene& scene,Size viewport,float scale) {
    auto valid=[&](PixelRect r) {
        CHECK(std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) && std::isfinite(r.height));
        CHECK(r.x>=0 && r.y>=0 && r.width>0 && r.height>0);
        CHECK(r.x+r.width<=viewport.width*scale+0.03 && r.y+r.height<=viewport.height*scale+0.03);
    };
    for (const auto& primitive:scene.primitives) {valid(primitive.rect);CHECK(std::isfinite(primitive.radius));CHECK(primitive.radius>=0);}
    for (const auto& text:scene.texts) {valid(text.rect);CHECK(text.size_dip>0 && text.size_dip<=256);}
}
void check_layout(const ContextMenu& menu,Size viewport) {
    CHECK(menu.layout().panels.size()<=ContextMenu::maximum_depth);
    for (const auto& panel:menu.layout().panels) {
        CHECK(panel.bounds.x>=0 && panel.bounds.y>=0 && panel.bounds.width>=0 && panel.bounds.height>=0);
        CHECK(panel.bounds.x+panel.bounds.width<=viewport.width+0.001 && panel.bounds.y+panel.bounds.height<=viewport.height+0.001);
        for (const auto& r:panel.rows) if (r.visible) {
            CHECK(r.bounds.x>=panel.content.x && r.bounds.y>=panel.content.y-0.01);
            CHECK(r.bounds.x+r.bounds.width<=panel.content.x+panel.content.width+0.01);
            CHECK(r.bounds.y+r.bounds.height<=panel.content.y+panel.content.height+0.01);
        }
    }
}
}
int main() {return run_core_tests("context menu",[] {
    ContextMenu menu;auto items=model();
    CHECK(!menu.is_open());CHECK(!menu.key({{"DOWN"}}));CHECK(!menu.pointer_down({0,0}));CHECK(!menu.text("c",0));CHECK(!menu.wheel({0,0},1));
    CHECK(menu.open(items,{100,80}));CHECK(menu.is_open());CHECK(menu.focused_id().empty());CHECK(!menu.take_action());
    CHECK(menu.pointer_up(center(row(menu,"pane.close").bounds)));CHECK(menu.is_open()); // Opening release is never an action.
    key(menu,"DOWN");CHECK(menu.focused_id()=="pane.split_right");
    key(menu,"DOWN");CHECK(menu.focused_id()=="pane.split_down");
    key(menu,"DOWN");CHECK(menu.focused_id()=="pane.swap"); // Skip disabled/separator.
    key(menu,"END");CHECK(menu.focused_id()=="pane.close");key(menu,"DOWN");CHECK(menu.focused_id()=="pane.split_right");
    key(menu,"UP");CHECK(menu.focused_id()=="pane.close");key(menu,"HOME");CHECK(menu.focused_id()=="pane.split_right");
    CHECK(menu.key({{"ENTER"},false,false,false}));CHECK(menu.is_open());
    CHECK(menu.key({{"ENTER"},true,false,true}));CHECK(menu.is_open());
    CHECK(menu.key({{"ENTER"},false,true,true}));CHECK(menu.is_open());
    CHECK(menu.key({{"ENTER",CommandModifier::control}}));CHECK(menu.is_open());
    key(menu,"ENTER");CHECK(!menu.is_open());auto action=menu.take_action();CHECK(action && action->kind==ContextMenuActionKind::selected && action->id=="pane.split_right");CHECK(!menu.take_action());
    CHECK(menu.open(items,{20,20},true));CHECK(menu.focused_id()=="pane.split_right");
    click(menu,"pane.move_tab");CHECK(menu.is_open());CHECK(!menu.take_action());
    key(menu,"ENTER");CHECK(menu.is_open());CHECK(!menu.take_action()); // Disabled hover cannot execute.
    WindowScene disabled_scene;menu.append_scene(disabled_scene);CHECK(std::any_of(disabled_scene.texts.begin(),disabled_scene.texts.end(),[](const auto& t){return t.text=="Create another tab first";}));
    CHECK(std::any_of(disabled_scene.texts.begin(),disabled_scene.texts.end(),[](const auto& t){return t.text=="Ctrl+Shift+D";}));
    CHECK(std::any_of(disabled_scene.texts.begin(),disabled_scene.texts.end(),[](const auto& t){return t.text=="Close pane" && t.color.r>t.color.g;}));
    click(menu,"pane.swap");CHECK(menu.layout().panels.size()==2);CHECK(!menu.take_action());
    key(menu,"DOWN");CHECK(menu.focused_id()=="pane.swap:21");key(menu,"END");CHECK(menu.focused_id()=="pane.swap:22");
    key(menu,"LEFT");CHECK(menu.layout().panels.size()==1 && menu.focused_id()=="pane.swap");
    key(menu,"RIGHT");CHECK(menu.layout().panels.size()==2 && menu.focused_id()=="pane.swap:21");
    key(menu,"ENTER");action=menu.take_action();CHECK(action && action->id=="pane.swap:21");

    // Escape/Tab/outside dismissal are terminal actions, not host commands.
    for (const auto* dismiss:{"ESCAPE","TAB"}) {
        CHECK(menu.open(items,{10,10},true));key(menu,dismiss);CHECK(!menu.is_open());
        action=menu.take_action();CHECK(action && action->kind==ContextMenuActionKind::cancel && action->id.empty());CHECK(!menu.take_action());
    }
    CHECK(menu.open(items,{100,80}));CHECK(menu.pointer_down({0,0}));CHECK(!menu.is_open());CHECK(menu.take_action()->kind==ContextMenuActionKind::cancel);
    CHECK(menu.open(items,{100,80}));const auto first=center(row(menu,"pane.split_right").bounds),second=center(row(menu,"pane.split_down").bounds);
    CHECK(menu.pointer_down(first));CHECK(menu.pointer_up(second));CHECK(menu.is_open());CHECK(!menu.take_action());
    CHECK(menu.pointer_down(first));menu.pointer_cancel();CHECK(menu.pointer_up(first));CHECK(menu.is_open());
    CHECK(menu.pointer_down(first));CHECK(menu.pointer_move(second));CHECK(menu.pointer_move(first));CHECK(menu.pointer_up(first));CHECK(menu.is_open());
    CHECK(menu.pointer_down(first));CHECK(menu.open(items,{100,80}));CHECK(menu.pointer_up(first));CHECK(menu.is_open());CHECK(!menu.take_action()); // Reopen cannot reuse a press.
    menu.cancel();CHECK(menu.open(items,{100,80}));CHECK(!menu.take_action()); // No stale cancel on a new menu.
    click(menu,"pane.close");action=menu.take_action();CHECK(action && action->id=="pane.close");
    menu.cancel();CHECK(!menu.take_action());

    // Owned snapshots and stable IDs survive source mutation/destruction.
    CHECK(menu.open(items,{20,20},true));items[0].id="changed";items[0].label="Changed";
    key(menu,"ENTER");CHECK(menu.take_action()->id=="pane.split_right");items=model();
    CHECK(menu.open(items,{20,20},true));CHECK(menu.text("s",1000));CHECK(menu.focused_id()=="pane.split_down");
    CHECK(menu.text("s",1100));CHECK(menu.focused_id()=="pane.swap");CHECK(menu.text("s",1200));CHECK(menu.focused_id()=="pane.split_right");
    CHECK(menu.text("w",1300));CHECK(menu.focused_id()=="pane.swap");
    CHECK(menu.text("cl",3000));CHECK(menu.focused_id()=="pane.close");
    CHECK(menu.text("\n",3100));CHECK(menu.focused_id()=="pane.close");CHECK(menu.text(std::string(1000,'a'),3200));
    CHECK(menu.text("sp",0));CHECK(menu.focused_id()=="pane.split_right"); // Clock rollback resets timeout.
    key(menu,"DOWN");key(menu,"DOWN");CHECK(menu.focused_id()=="pane.swap");key(menu,"RIGHT");
    CHECK(menu.text("p",100));CHECK(menu.focused_id()=="pane.swap:22"); // Submenu starts a fresh prefix.
    auto unicode=std::vector{item("one","日本語"),item("two","PowerShell")};CHECK(menu.open(unicode,{0,0}));CHECK(menu.text("日",1000));CHECK(menu.focused_id()=="one");menu.cancel();

    // Scroll, clamp and submenu direction remain safe at fractional DPI and
    // narrow/minimized viewports. All scene coordinates remain physical pixels.
    std::vector<ContextMenuItem> many;
    for (unsigned i=0;i<100;++i) many.push_back(item("item:"+std::to_string(i),"Action "+std::to_string(i)));
    for (const auto viewport:std::vector<Size>{{1280,800},{500,320},{320,260},{180,120},{40,40},{1,1},{0,0}}) {
        menu.resize(viewport);CHECK(menu.open(items,{viewport.width+400,viewport.height+400},true));
        key(menu,"END");key(menu,"UP");key(menu,"RIGHT");CHECK(menu.layout().panels.size()==2);
        check_layout(menu,viewport);
        for (const auto scale:{1.0f,1.25f,1.5f,2.0f,3.0f}) {WindowScene scene;menu.append_scene(scene,scale);check_scene(scene,viewport,scale);}
        key(menu,"ESCAPE");CHECK(menu.take_action()->kind==ContextMenuActionKind::cancel);
        CHECK(menu.open(many,{0,0},true));key(menu,"END");CHECK(menu.focused_id()=="item:99");
        if (menu.layout().panels.front().content.height>=32) CHECK(row(menu,"item:99").visible);
        key(menu,"HOME");CHECK(menu.focused_id()=="item:0");
        if (menu.layout().panels.front().content.height>=32) CHECK(row(menu,"item:0").visible);
        CHECK(menu.wheel(center(menu.layout().panels.front().bounds),-50));check_layout(menu,viewport);
        WindowScene scene;menu.append_scene(scene,1.25f);check_scene(scene,viewport,1.25f);
        menu.cancel();
    }
    menu.resize({1024,768});CHECK(menu.open(items,{0,0},true));
    const auto old=menu.layout().panels.front().bounds;menu.resize({std::numeric_limits<double>::infinity(),500});CHECK(menu.layout().panels.front().bounds==old);
    CHECK(menu.pointer_down({std::numeric_limits<double>::quiet_NaN(),0}));CHECK(menu.pointer_up({0,0}));CHECK(menu.is_open());
    CHECK(menu.pointer_move({0,std::numeric_limits<double>::infinity()}));CHECK(menu.wheel({0,0},std::numeric_limits<double>::quiet_NaN()));
    for (const auto bad_scale:{0.0f,-1.0f,9.0f,std::numeric_limits<float>::infinity()}) {WindowScene scene;menu.append_scene(scene,bad_scale);CHECK(scene.texts.empty() && scene.primitives.empty());}
    WindowScene covered;const auto bounds=menu.layout().panels.front().bounds;
    covered.texts.push_back({{float(bounds.x+20),float(bounds.y+10),80,20},"must not leak"});
    covered.texts.push_back({{900,700,100,20},"outside"});menu.append_scene(covered);
    CHECK(std::none_of(covered.texts.begin(),covered.texts.end(),[](const auto& t){return t.text=="must not leak";}));
    CHECK(covered.texts.front().text=="outside" && covered.texts.front().color.a==255);
    CHECK(std::any_of(covered.primitives.begin(),covered.primitives.end(),[](const auto& p){return p.background_replace && p.fill==Color{24,29,36,255};}));
    menu.set_high_contrast(true,{250,249,248,0},{12,13,14,1},{1,2,3,4},{4,5,6,7},{90,91,92,8});
    const auto contrast_generation=menu.generation();
    menu.set_high_contrast(true,{250,249,248,0},{12,13,14,1},{1,2,3,4},{4,5,6,7},{90,91,92,8});CHECK(menu.generation()==contrast_generation);
    WindowScene contrast;menu.append_scene(contrast);
    CHECK(std::any_of(contrast.primitives.begin(),contrast.primitives.end(),[](const auto& p){return p.background_replace && p.fill==Color{250,249,248,255};}));
    CHECK(std::all_of(contrast.primitives.begin(),contrast.primitives.end(),[](const auto& p){return p.fill.a==255;}));
    CHECK(std::any_of(contrast.texts.begin(),contrast.texts.end(),[](const auto& t){return t.text=="Split right" && t.color==Color{4,5,6,255};}));
    CHECK(std::any_of(contrast.texts.begin(),contrast.texts.end(),[](const auto& t){return t.text=="Close pane" && t.color==Color{12,13,14,255};}));
    CHECK(std::any_of(contrast.texts.begin(),contrast.texts.end(),[](const auto& t){return t.text=="Create another tab first" && t.color==Color{90,91,92,255};}));
    menu.set_high_contrast(false);CHECK(menu.generation()>contrast_generation);

    // Defensive bounds: empty/separator-only, duplicate/oversized IDs, huge
    // trees and excessive depth must not degrade into executable leaf items.
    CHECK(!menu.open({},{}));CHECK(!menu.is_open());CHECK(!menu.take_action());
    CHECK(!menu.open(std::vector{ContextMenuItem::separator()},{}));
    CHECK(menu.open(std::vector{ContextMenuItem::separator(),ContextMenuItem::separator(),item("only","Only"),
        ContextMenuItem::separator(),ContextMenuItem::separator()},{}));CHECK(menu.layout().panels.front().rows.size()==1);
    CHECK(!menu.open(std::vector{item("same","One"),item("same","Two")},{}));
    CHECK(!menu.open(std::vector{item(std::string(129,'a'),"Oversized")},{}));
    CHECK(!menu.open(items,{std::numeric_limits<double>::quiet_NaN(),1}));
    auto nested=item("level:0","Level 0");auto* leaf=&nested;
    for (std::size_t i=1;i<ContextMenu::maximum_depth;++i) {leaf->children.push_back(item("level:"+std::to_string(i),"Level"));leaf=&leaf->children.back();}
    CHECK(menu.open(std::vector{nested},{0,0},true));
    for (std::size_t i=1;i<ContextMenu::maximum_depth;++i) key(menu,"RIGHT");
    CHECK(menu.layout().panels.size()==ContextMenu::maximum_depth);key(menu,"ENTER");CHECK(menu.take_action()->id=="level:7");
    leaf->children.push_back(item("too-deep","Too deep"));CHECK(!menu.open(std::vector{nested},{}));
    std::vector<ContextMenuItem> maximum;
    for (std::size_t i=0;i<ContextMenu::maximum_items;++i) maximum.push_back(item("bounded:"+std::to_string(i),"Bounded"));
    CHECK(menu.open(maximum,{0,0},true));key(menu,"END");CHECK(menu.focused_id()=="bounded:511");
    maximum.push_back(item("one-too-many","Too many"));CHECK(!menu.open(maximum,{}));
    auto bad_label=item("sanitized","Line\n\x1b[31m");bad_label.disabled_reason=std::string(10000,'x');bad_label.enabled=false;
    CHECK(menu.open(std::vector{bad_label},{0,0},true));CHECK(menu.focused_id().empty());key(menu,"DOWN");key(menu,"ENTER");CHECK(menu.is_open());
    WindowScene safe;menu.append_scene(safe);
    for (const auto& t:safe.texts) {CHECK(t.text.size()<=256);CHECK(std::none_of(t.text.begin(),t.text.end(),[](unsigned char c){return c<32;}));}
    menu.cancel();menu.take_action();

    // Exercise interruption and model replacement with no stale indices or
    // accidental dispatch. This is portable input-model testing, not native QA.
    for (unsigned iteration=0;iteration<200;++iteration) {
        menu.resize({double(180+iteration%8*53),double(120+iteration%9*37)});
        CHECK(menu.open(iteration%2?items:many,{double(iteration*13),double(iteration*11)},iteration%2));
        key(menu,"END");key(menu,"UP");key(menu,"RIGHT");
        CHECK(menu.pointer_move({double(iteration*7%600),double(iteration*17%500)}));
        CHECK(menu.wheel({double(iteration%200),double(iteration%200)},-3));
        menu.pointer_cancel();key(menu,"LEFT");key(menu,"HOME");key(menu,"ESCAPE");
        CHECK(!menu.is_open());CHECK(menu.take_action()->kind==ContextMenuActionKind::cancel);CHECK(!menu.take_action());
    }
    WindowScene closed;menu.append_scene(closed);CHECK(closed.texts.empty() && closed.primitives.empty());
});}
