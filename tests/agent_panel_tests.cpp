#include "core_test.hpp"
#include "stormtty/agent_panel.hpp"
#include "stormtty/agent_display_text.hpp"
#include <algorithm>
using namespace stormtty;
namespace {
void click(AgentPanel& p,std::string_view id) {for(const auto& c:p.controls())if(c.id==id){Point point{c.bounds.x+1,c.bounds.y+1};p.pointer_down(point);p.pointer_up(point);return;}CHECK(false);}
AgentPanelView view(std::string id="workspace-a") {AgentPanelView v;v.workspace_id=id;v.workspace_name="StormTTY";v.directory="C:/project";v.execution_scope="Read-only sandbox; untrusted commands require approval";return v;}
}
int main() {
    AgentPanel p; p.open(view());p.text("Build tests");click(p,"start");CHECK(!p.take_action());CHECK(p.status_label()=="Unavailable");
    p.close();p.open(view("workspace-b"));CHECK(p.draft().empty());p.text("Other task");p.close();p.open(view());CHECK(p.draft()=="Build tests");CHECK(!p.take_action());
    auto ready=view();ready.state=AgentPanelState::ready;ready.provider_generation=5;p.update(ready);
    click(p,"start");CHECK(!p.take_action()); // Review is not start consent.
    p.key({{"ESCAPE"}});CHECK(p.is_open());click(p,"start");click(p,"start");
    auto action=p.take_action();CHECK(action);CHECK(action->kind==AgentPanelActionKind::start);CHECK(action->prompt=="Build tests");CHECK(action->workspace_id=="workspace-a");CHECK(action->provider_generation==5);CHECK(!p.take_action());
    p.close();p.open(ready);CHECK(!p.take_action()); // Restore/reopen never replay.
    click(p,"start");ready.provider_generation=6;p.update(ready);click(p,"start");CHECK(!p.take_action()); // Reconnect invalidates review.
    ready.state=AgentPanelState::running;ready.approvals.push_back({{6,"thread","turn","item","1"},"Run command","echo hello",false});p.update(ready);
    CHECK(p.status_label()=="Needs approval");click(p,"allow");CHECK(!p.take_action());click(p,"decline");action=p.take_action();CHECK(action&&action->kind==AgentPanelActionKind::decline);CHECK(action->approval_key==ready.approvals[0].key);
    p.pointer_down({p.controls()[0].bounds.x+1,p.controls()[0].bounds.y+1});p.pointer_cancel();p.pointer_up({p.controls()[0].bounds.x+1,p.controls()[0].bounds.y+1});CHECK(p.is_open());
    WindowScene scene;p.append_scene(scene,Theme{},1.5f);CHECK(!scene.texts.empty());CHECK(!scene.primitives.empty());
    AgentSnapshot snapshot;snapshot.association.workspace_persistent_id="saved";snapshot.connection=AgentConnection::ready;snapshot.conversation=AgentConversation::running;
    CHECK(agent_panel_view(snapshot,"Saved","/saved",false).state==AgentPanelState::unavailable);
    CHECK(agent_panel_view(snapshot,"Saved","/saved",true).state==AgentPanelState::running);
    p.key({{"ESCAPE"}});CHECK(!p.is_open());CHECK(!p.key({{"ENTER"}}));
    auto disconnected=view();disconnected.state=AgentPanelState::disconnected;disconnected.connection_scope="/fixture/codex · account-reference";
    p.open(disconnected);click(p,"connect");CHECK(!p.take_action());click(p,"connect");action=p.take_action();CHECK(action&&action->kind==AgentPanelActionKind::connect);
    p.close();p.open(disconnected);CHECK(!p.take_action());
    p.update(ready);const auto allow=std::find_if(p.controls().begin(),p.controls().end(),[](auto& c){return c.id=="decline";})->bounds;
    p.pointer_down({allow.x+1,allow.y+1});ready.approvals[0].key.request_id="new";p.update(ready);p.pointer_up({allow.x+1,allow.y+1});CHECK(!p.take_action());
    p.pointer_down({allow.x+1,allow.y+1});ready.approvals[0].scope="changed scope";p.update(ready);p.pointer_up({allow.x+1,allow.y+1});CHECK(!p.take_action());
    // Provider evidence is displayed visibly, but raw scope and canonical keys
    // remain untouched for the permission broker.
    const std::string bidi="\xE2\x80\xAE"; // RIGHT-TO-LEFT OVERRIDE U+202E
    const std::string isolate="\xE2\x81\xA6"; // LEFT-TO-RIGHT ISOLATE U+2066
    const std::string normal="caf\xC3\xA9 / \xE6\x96\x87\xE4\xBB\xB6 / \xF0\x9F\x98\x80";
    const std::string crafted="rm C:/safe/"+bidi+"txt.exe "+isolate+"--force";
    CHECK(agent_display_text(crafted)=="rm C:/safe/[U+202E]txt.exe [U+2066]--force");
    CHECK(agent_display_text(normal)==normal);
    CHECK(agent_display_text("a\t\r\x1b" "b")=="a[U+0009][U+000D][U+001B]b");
    CHECK(agent_display_text("\xD8\x9C\xE2\x80\x8F\xE2\x81\xA9")=="[U+061C][U+200F][U+2069]");
    CHECK(agent_display_text(std::string("\xF0\x80\x80\xAF",4))=="[byte:F0][byte:80][byte:80][byte:AF]");
    CHECK(agent_display_text(std::string(8191,'x')+"\xF0\x9F\x98\x80")==std::string(8191,'x')+" [truncated]");
    for(std::size_t width=1;width<12;++width) {
        const auto lines=agent_display_lines(normal+crafted,width);std::string joined;
        for(const auto& line:lines) {CHECK(agent_display_text(line)==line);joined+=line;}
        CHECK(joined==normal+agent_display_text(crafted));
        CHECK(std::any_of(lines.begin(),lines.end(),[](const auto& line){return line.find("[U+202E]")!=std::string::npos;}));
    }
    ready.approvals[0].title="Inspect "+bidi+"path";ready.approvals[0].scope=crafted+"\n"+normal;
    p.update(ready);scene={};p.append_scene(scene,Theme{});
    std::string rendered;for(const auto& item:scene.texts){rendered+=item.text;CHECK(item.text.find(bidi)==std::string::npos);CHECK(item.text.find(isolate)==std::string::npos);}
    CHECK(rendered.find("[U+202E]")!=std::string::npos);CHECK(rendered.find("[U+2066]")!=std::string::npos);
    CHECK(p.view().approvals[0].scope==ready.approvals[0].scope);CHECK(p.view().approvals[0].key==ready.approvals[0].key);
    // Setup edits remain drafts until explicit Save; Cancel and reopen restore
    // the saved metadata, never create a Connect/Start action.
    AgentPanel setup;auto local=view();local.setup_available=true;local.setup_reload_available=true;
    local.provider_settings={"C:\\Tools\\codex.exe","chatgpt:saved@example.com"};setup.open(local);
    click(setup,"settings.open");CHECK(setup.editing_settings());setup.text("relative.exe");click(setup,"settings.save");CHECK(!setup.take_action());
    click(setup,"settings.cancel");CHECK(!setup.editing_settings());CHECK(!setup.take_action());
    click(setup,"settings.open");click(setup,"settings.executable");setup.text("C:\\Installed\\codex.exe");
    click(setup,"settings.account");setup.text("chatgpt:new@example.com");click(setup,"settings.save");
    auto saved=setup.take_action();CHECK(saved&&saved->kind==AgentPanelActionKind::save_settings);
    CHECK(saved->provider_settings.executable=="C:\\Installed\\codex.exe");CHECK(saved->provider_settings.account_reference=="chatgpt:new@example.com");
    local.setup_busy=true;setup.update(local);setup.key({{"ESCAPE"}});CHECK(setup.editing_settings());CHECK(!setup.take_action());
    local.setup_busy=false;local.provider_settings=saved->provider_settings;setup.update(local);setup.settings_result(true);CHECK(!setup.editing_settings());CHECK(!setup.take_action());
    setup.close();setup.open(local);click(setup,"settings.open");scene={};setup.append_scene(scene,Theme{});
    CHECK(std::any_of(scene.texts.begin(),scene.texts.end(),[](auto& t){return t.text=="chatgpt:new@example.com";}));
    setup.key({{"ESCAPE"}});CHECK(setup.is_open()&&!setup.editing_settings());
    setup.resize({480,320});click(setup,"settings.open");for(const auto& c:setup.controls()){CHECK(c.bounds.x>=0&&c.bounds.y>=0);CHECK(c.bounds.x+c.bounds.width<=480);CHECK(c.bounds.y+c.bounds.height<=320);}
    click(setup,"settings.executable");setup.text("C:\\Unsaved\\codex.exe");click(setup,"settings.reload");CHECK(!setup.take_action());
    click(setup,"settings.cancel");CHECK(setup.editing_settings());click(setup,"settings.save");
    auto kept=setup.take_action();CHECK(kept&&kept->provider_settings.executable=="C:\\Unsaved\\codex.exe");
    setup.settings_result(false,"Local settings changed outside StormTTY.");
    click(setup,"settings.reload");CHECK(!setup.take_action());click(setup,"settings.reload");auto reload=setup.take_action();CHECK(reload&&reload->kind==AgentPanelActionKind::reload_settings);
    AgentProviderSettings external{"C:\\External\\codex.exe","chatgpt:external@example.com"};setup.settings_reloaded(external);scene={};setup.append_scene(scene,Theme{});
    CHECK(std::any_of(scene.texts.begin(),scene.texts.end(),[](const auto& t){return t.text=="chatgpt:external@example.com";}));CHECK(!setup.take_action());
    setup.close();local.setup_available=false;local.setup_status="Corrupt local settings";setup.open(local);click(setup,"settings.open");CHECK(setup.editing_settings());
    click(setup,"settings.save");CHECK(!setup.take_action());click(setup,"settings.reload");CHECK(setup.take_action()->kind==AgentPanelActionKind::reload_settings);
    const std::string scope_line="Read-only fixture scope: src/platform/windows/window.cpp";
    const auto fallback=agent_display_word_lines(scope_line,600,12);CHECK(fallback.size()==1);CHECK(fallback.front()==scope_line);
    const auto words=agent_display_word_lines("alpha beta gamma delta",70,12);
    std::string word_joined;for(const auto& line:words){word_joined+=line;CHECK(line.ends_with(" ")||line.ends_with("delta"));}CHECK(word_joined=="alpha beta gamma delta");
    AgentPanel measured;auto measured_view=view();measured_view.approvals={{{1,"t","u","i","1"},"Read source files",scope_line,true}};measured.open(measured_view);
    unsigned measures=0;AgentPanel::TextWrapper wrap=[&](const ChromeText& text){
        ++measures;CHECK(text.wrap);CHECK(text.size_dip==12);CHECK(text.rect.width>500);
        std::vector<std::string> lines;std::size_t start=0;for(std::size_t i=0;i<=text.text.size();++i)if(i==text.text.size()||text.text[i]=='\n'){lines.push_back(text.text.substr(start,i-start));start=i+1;}return lines;
    };
    scene={};measured.append_scene(scene,Theme{},1,wrap);CHECK(measures==1);
    CHECK(std::any_of(scene.texts.begin(),scene.texts.end(),[&](const auto& text){return text.text==scope_line&&!text.wrap;}));
    scene={};measured.append_scene(scene,Theme{},1,wrap);CHECK(measures==1);
    measured.resize({1100,800});scene={};measured.append_scene(scene,Theme{},1.25f,wrap);CHECK(measures==2);
    CHECK(std::any_of(scene.texts.begin(),scene.texts.end(),[](const auto& text){return text.text=="Allow once"&&text.alignment==ChromeTextAlignment::center;}));
    return 0;
}
