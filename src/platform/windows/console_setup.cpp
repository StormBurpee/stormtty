// App-owned one-shot console setup. Runs inside the managed shell's existing
// pseudoconsole; never attaches to another process or changes system settings.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string_view>
int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--completion-ready") {
        const std::string_view name(argv[2]);
        constexpr std::string_view prefix="Local\\StormTTY-ZshInit-";
        if (!name.starts_with(prefix) || name.size()!=prefix.size()+32 ||
            name.substr(prefix.size()).find_first_not_of("0123456789abcdef")!=name.npos) return 5;
        HANDLE event=OpenEventA(EVENT_MODIFY_STATE,FALSE,argv[2]);
        if (!event) return 6;
        const bool signaled=SetEvent(event)!=FALSE;CloseHandle(event);
        return signaled ? 0 : 7;
    }
    if (argc != 1) return 8;
    const UINT input = GetConsoleCP(), output = GetConsoleOutputCP();
    if (!input || !output) return 2; // No inherited console: do not attach/create.
    if (SetConsoleCP(CP_UTF8) && SetConsoleOutputCP(CP_UTF8) &&
        GetConsoleCP() == CP_UTF8 && GetConsoleOutputCP() == CP_UTF8) return 0;
    // Do not leave a half-configured console on failure.
    const bool input_restored = SetConsoleCP(input) != FALSE;
    const bool output_restored = SetConsoleOutputCP(output) != FALSE;
    return input_restored && output_restored ? 3 : 4;
}
