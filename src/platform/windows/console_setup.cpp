// App-owned one-shot console setup. Runs inside the managed shell's existing
// pseudoconsole; never attaches to another process or changes system settings.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
int main() {
    const UINT input = GetConsoleCP(), output = GetConsoleOutputCP();
    if (!input || !output) return 2; // No inherited console: do not attach/create.
    if (SetConsoleCP(CP_UTF8) && SetConsoleOutputCP(CP_UTF8) &&
        GetConsoleCP() == CP_UTF8 && GetConsoleOutputCP() == CP_UTF8) return 0;
    // Do not leave a half-configured console on failure.
    const bool input_restored = SetConsoleCP(input) != FALSE;
    const bool output_restored = SetConsoleOutputCP(output) != FALSE;
    return input_restored && output_restored ? 3 : 4;
}
