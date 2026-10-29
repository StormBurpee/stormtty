#include "stormtty/agent_provider_settings.hpp"
#include "stormtty/agent_display_text.hpp"
#include <algorithm>
namespace stormtty {
std::string validate_agent_provider_settings(const AgentProviderSettings& s) {
    if(s.executable.empty()&&s.account_reference.empty())return {};
    const auto& path=s.executable;
    if(path.size()<7||path.size()>32768||!((path[0]>='A'&&path[0]<='Z')||(path[0]>='a'&&path[0]<='z'))||path[1]!=':'||(path[2]!='\\'&&path[2]!='/'))
        return "Choose an absolute local executable path, such as C:\\Tools\\codex.exe.";
    if(agent_display_text(path,65536)!=path||path.find_first_of("\r\n\t\"<>|?*")!=std::string::npos||path.find(':',2)!=std::string::npos)
        return "Executable path contains unsupported control characters, quotes or path syntax.";
    std::string extension=path.substr(path.size()-4);for(auto& c:extension)if(c>='A'&&c<='Z')c+=32;
    if(extension!=".exe")return "Select the installed native .exe, not a script or shell command.";
    std::size_t begin=3;for(std::size_t i=3;i<=path.size();++i)if(i==path.size()||path[i]=='/'||path[i]=='\\'){
        const auto part=path.substr(begin,i-begin);
        if(part.empty()||part=="."||part==".."||part.back()=='.'||part.back()==' ')return "Use a complete path without empty, relative or trailing-dot components.";
        begin=i+1;
    }
    const auto& account=s.account_reference;
    if(!account.starts_with("chatgpt:")||account.size()>256)return "Account reference must be chatgpt: followed by the signed-in account email. Never enter a token.";
    const auto email=account.substr(8);const auto at=email.find('@');
    if(at==std::string::npos||at==0||at+1==email.size()||email.find('@',at+1)!=std::string::npos||
       std::any_of(email.begin(),email.end(),[](unsigned char c){return c<=32||c>=127||c=='"'||c=='\\'||c==':'||c=='<'||c=='>';}))
        return "Enter the signed-in account email after chatgpt:, not a password or API key.";
    return {};
}
std::string serialize_agent_provider_settings(const AgentProviderSettings& s) {
    if(!validate_agent_provider_settings(s).empty())return {};
    return "stormtty-agent-settings:1\nexecutable="+s.executable+"\naccount="+s.account_reference+"\n";
}
std::optional<AgentProviderSettings> parse_agent_provider_settings(std::string_view bytes,std::string& error) {
    error.clear();constexpr std::string_view prefix="stormtty-agent-settings:1\nexecutable=";
    if(bytes.size()>34000||!bytes.starts_with(prefix)){error="Invalid local agent settings format.";return {};}
    bytes.remove_prefix(prefix.size());const auto end=bytes.find('\n');
    if(end==std::string_view::npos){error="Incomplete local agent settings.";return {};}
    AgentProviderSettings s;s.executable=bytes.substr(0,end);bytes.remove_prefix(end+1);
    if(!bytes.starts_with("account=")){error="Missing local account reference.";return {};}
    bytes.remove_prefix(8);
    if(bytes.empty()||bytes.back()!='\n'||bytes.find('\n')!=bytes.size()-1){error="Unexpected local agent settings fields.";return {};}
    s.account_reference=bytes.substr(0,bytes.size()-1);error=validate_agent_provider_settings(s);
    if(!error.empty())return {};
    return s;
}
}
