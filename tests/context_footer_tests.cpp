#include "core_test.hpp"
#include "stormtty/context_footer.hpp"
#include <limits>
using namespace stormtty;
int main(){return run_core_tests("context footer",[]{
    for(double width:{0.,20.,64.,320.,400.,1120.})for(double height:{0.,26.,100.,720.}){
        const auto f=layout_context_footer(width,height,38);
        for(auto r:{f.bounds,f.text,f.inspect,f.terminal}){CHECK(r.x>=0);CHECK(r.y>=0);CHECK(r.width>=0);CHECK(r.height>=0);CHECK(r.x+r.width<=width);CHECK(r.y+r.height<=height);}
        CHECK(f.text.x+f.text.width<=f.inspect.x);CHECK(f.terminal.y+f.terminal.height<=f.bounds.y);
    }
    const auto f=layout_context_footer(1120,720,38);CHECK(f.bounds.height==26);CHECK(f.inspect.width==64);CHECK(f.terminal.y==52);CHECK(f.terminal.height==634);
    CHECK(layout_context_footer(400,720,38).bounds.height==26);
    CHECK(layout_context_footer(std::numeric_limits<double>::quiet_NaN(),720,38).bounds.width==0);
    ContextFooterPresentation p;p.context=make_pane_context({"Terminal","Zsh",{},"/configured"});p.status="Terminal open";
    auto measure=[](const std::string& s){return double(s.size());};
    CHECK(p.label(1000,measure)=="Start: /configured · Terminal open");
    CHECK(p.label(20,measure)=="Start: /configured");
    CHECK(p.detail().find("Observed directory: Unavailable")!=std::string::npos);
    ShellContext shell;shell.display_directory="/actual";shell.repository_root="/repo";shell.git=GitHeadKind::branch;shell.head="feature";
    p.context=make_pane_context({"Terminal","Zsh","/stale","/configured",&shell});
    CHECK(p.label(1000,measure)=="/actual · feature · Terminal open");CHECK(p.label(30,measure)=="/actual · Terminal open");
    CHECK(p.label(10,measure)=="/actual");CHECK(p.detail().find("/stale")==std::string::npos);
    p.context.retained_exited=true;p.status="Terminal exited · exit 123";
    CHECK(p.label(20,measure)=="Terminal ended");CHECK(p.label(30,measure)==p.status);
    p.failed=true;CHECK(p.label(1,measure)=="Terminal failed");p.failed=false;
    p.notice="Input queue full";CHECK(p.label(100,measure)==p.notice);CHECK(p.detail().find("Notice: Input queue full")!=std::string::npos);
    p.notice.clear();p.context=make_pane_context({});p.status="Terminal open";CHECK(p.label(1,measure)=="Directory unavailable");
});}
