#include "stormtty/pane_context.hpp"
#include <cstdlib>
#include <iostream>
using namespace stormtty;
void check(bool ok,const char* name) {if(!ok){std::cerr<<name<<'\n';std::exit(1);}}
int main() {
    PaneContextInput in;
    check(make_pane_context(in).primary=="Terminal","absent fallback");
    in.profile_label="Shell";in.configured_directory="/start";
    auto p=make_pane_context(in);
    check(p.directory_source==PaneDirectorySource::configured&&pane_context_secondary(p)=="Start: /start","configured is not observed");
    in.observed_directory="/old";in.integrated_shell=true;
    p=make_pane_context(in);
    check(p.directory.empty()&&p.detail.find("Observed directory: Unavailable")!=std::string::npos,"cleared context hides stale cwd");
    ShellContext context{"opaque-not-for-display","/one/project","/one/project","feature/日本語",GitHeadKind::branch};
    in.shell_context=&context;in.title="My explicit title";
    p=make_pane_context(in);
    check(p.primary==in.title&&p.directory==context.display_directory&&p.head==context.head,"explicit title and Unicode");
    in.title="vim README.md";
    check(make_pane_context(in).primary==in.title,"application title preserved");
    in.title="/usr/bin/zsh";
    check(make_pane_context(in).primary==in.title,"no title provenance heuristic");
    context.git=GitHeadKind::detached;context.head="a1b2c3d";
    p=make_pane_context(in);
    check(pane_context_secondary(p)=="/one/project · HEAD a1b2c3d"&&p.detail.find("Detached HEAD: a1b2c3d")!=std::string::npos,"detached HEAD");
    check(pane_context_secondary(p,PaneContextDensity::directory)=="/one/project"&&pane_context_secondary(p,PaneContextDensity::title_only).empty(),"narrow priorities");
    const auto first=p;
    context.display_directory="/two/project";
    p=make_pane_context(in);
    check(p.directory!=first.directory&&p!=first,"same basename different roots and metadata updates");
    check(p==make_pane_context(in),"stable snapshot no animation");
    in.retained_exited=true;
    p=make_pane_context(in);
    check(p.directory=="/two/project"&&p.retained_exited&&p.detail.find("retained output")!=std::string::npos,"retained exited context");
    context.display_directory="/日本語/"+std::string(1200,'x');
    p=make_pane_context(in);
    check(p.directory==context.display_directory&&p.detail.find(context.display_directory)!=std::string::npos,"full long path retained");
    in.title="bad\n\033\t\xE2\x80\xAE\xFF";
    p=make_pane_context(in);
    check(p.primary=="bad[U+000A][U+001B][U+0009][U+202E][byte:FF]","visible control bidi and invalid UTF8 escaping");
    check(p.detail.find(context.directory_key)==std::string::npos,"opaque key never displayed");
    in.shell_context=nullptr;in.integrated_shell=false;
    p=make_pane_context(in);
    check(p.directory=="/old"&&p.head.empty()&&p.repository_root.empty()&&!p.detached,"removed context clears all git fields");
    context.git=GitHeadKind::none;in.shell_context=&context;
    check(make_pane_context(in).head.empty(),"none ignores stale head");

    PaneContextLineCache cache;unsigned layouts=0;
    // Match the terminal callback's 64 KiB label bound; hostile invalid bytes
    // exercise worst-case escaping without introducing thousands of layouts.
    const auto large=pane_context_text(std::string(65536,static_cast<char>(0xff)));
    const std::string unicode="café e\xcc\x81 日本語 😀";
    const auto wrap=[&](std::string_view text,double,double){++layouts;return std::vector<std::string>{std::string(text)};};
    check(cache.lines(large,400,1,wrap).front()==large&&layouts==1,"one wrap for maximum escaped label");
    for(unsigned n=0;n<1000;++n)check(cache.lines(large,400,1,wrap).front()==large,"cached full label preserved");
    check(layouts==1,"repaint/filter/paging performs no native layouts");
    cache.lines(large,180,1,wrap);check(layouts==2,"one wrap after width change");
    cache.lines(large,180,1.5,wrap);check(layouts==3,"one wrap after DPI change");
    check(cache.lines(unicode,180,1.5,wrap).front()==unicode&&layouts==4,"Unicode passes intact to native shaping");
    std::cout<<"pane context tests passed\n";
}
