#include "core_test.hpp"
#include "../src/platform/windows/theme_picker.hpp"
#include <fstream>
#include <iterator>
using namespace stormtty;
namespace {
std::string read(const std::filesystem::path& path) {std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
void write(const std::filesystem::path& path,const std::string& text) {std::ofstream file(path,std::ios::binary);file<<text;CHECK(file.good());}
}
int main(){return run_core_tests("native theme files",[]{
    const auto root=std::filesystem::temp_directory_path()/("stormtty-theme-owned-"+std::to_string(GetCurrentProcessId()));
    CHECK(std::filesystem::create_directory(root));
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{root};
    const auto seed=*find_builtin_theme("daylight");auto expected=*find_builtin_theme("ember");expected.name="custom";
    const auto target=root/L"palette.stormtheme";
    auto saved=exchange_theme_file(target,false,ThemeFormat::stormtty,expected);CHECK(saved.error.empty());CHECK(!saved.notice.empty());
    auto loaded=exchange_theme_file(target,true,ThemeFormat::stormtty,seed);CHECK(loaded.error.empty());CHECK(loaded.imported && *loaded.imported==expected);
    const auto initial=read(target);CHECK(initial==*export_theme(expected,ThemeFormat::stormtty));
    CHECK(!exchange_theme_file(target,true,ThemeFormat::ghostty,seed).error.empty());CHECK(read(target)==initial);
    // Existing file replacement is atomic; invalid export and sharing failures
    // must preserve it, and failed exports must leave no temporary files behind.
    auto invalid=seed;invalid.accent="bad";
    CHECK(!exchange_theme_file(target,false,ThemeFormat::stormtty,invalid).error.empty());CHECK(read(target)==initial);
    HANDLE locked=CreateFileW(target.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);CHECK(locked!=INVALID_HANDLE_VALUE);
    const auto denied=exchange_theme_file(target,false,ThemeFormat::stormtty,seed);CloseHandle(locked);
    CHECK(!denied.error.empty());CHECK(read(target)==initial);
    // Permit metadata access but deny delete: the temporary write succeeds and
    // the replacement fails, exercising cleanup after a completed file write.
    locked=CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);CHECK(locked!=INVALID_HANDLE_VALUE);
    const auto replace_denied=exchange_theme_file(target,false,ThemeFormat::stormtty,seed);CloseHandle(locked);
    CHECK(!replace_denied.error.empty());CHECK(read(target)==initial);
    std::size_t files=0;for(const auto& entry:std::filesystem::directory_iterator(root)){(void)entry;++files;}CHECK(files==1);
    CHECK(exchange_theme_file(target,false,ThemeFormat::stormtty,seed).error.empty());
    auto seeded=seed;seeded.name="custom";CHECK(*exchange_theme_file(target,true,ThemeFormat::stormtty,expected).imported==seeded);
    write(target,"background=#123456\ncursor-color=#FFFFFF\n");
    loaded=exchange_theme_file(target,true,ThemeFormat::ghostty,seed);CHECK(loaded.imported);CHECK(!loaded.notice.empty());
    CHECK(loaded.imported->chrome==seed.chrome);CHECK(loaded.imported->background=="#123456");
    write(target,"background=#123456\ncommand=sh\n");loaded=exchange_theme_file(target,true,ThemeFormat::ghostty,seed);CHECK(!loaded.imported);CHECK(!loaded.error.empty());
    write(target,std::string(max_theme_bytes+1,'x'));CHECK(!exchange_theme_file(target,true,ThemeFormat::ghostty,seed).error.empty());
    CHECK(!exchange_theme_file(root/L"missing",true,ThemeFormat::stormtty,seed).error.empty());
    CHECK(!exchange_theme_file(root,false,ThemeFormat::stormtty,seed).error.empty());
    for(const auto& path:{std::filesystem::path(L"relative"),std::filesystem::path(L"\\\\invalid-server\\share\\theme"),std::filesystem::path(target.wstring()+L":stream")}) {
        CHECK(!exchange_theme_file(path,true,ThemeFormat::stormtty,seed).error.empty());
        CHECK(!exchange_theme_file(path,false,ThemeFormat::stormtty,seed).error.empty());
    }
    CHECK(!exchange_theme_file(target,false,static_cast<ThemeFormat>(99),seed).error.empty());
    for(int n=0;n<20;++n){CHECK(exchange_theme_file(target,false,ThemeFormat::ghostty,expected).error.empty());CHECK(exchange_theme_file(target,true,ThemeFormat::ghostty,seed).imported);}
    files=0;for(const auto& entry:std::filesystem::directory_iterator(root)){(void)entry;++files;}CHECK(files==1);
});}
