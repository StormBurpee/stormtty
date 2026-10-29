/* Controlled native child for existing Session/ConPTY qualification only.
 * No subprocesses, file writes, DSR fabrication or production integration.
 * Actual console API success and observed sizes are required, not inferred.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

struct input_read {
    HANDLE input;
    WCHAR text[64];
    DWORD length;
    DWORD error;
    BOOL success;
};

static DWORD WINAPI read_line(void *context) {
    struct input_read *result = (struct input_read *)context;
    result->success = ReadConsoleW(result->input, result->text,
                                  (DWORD)(sizeof result->text / sizeof result->text[0]),
                                  &result->length, NULL);
    if (!result->success) result->error = GetLastError();
    return 0;
}

static int output(HANDLE handle, const WCHAR *text) {
    size_t length = wcslen(text);
    DWORD written = 0;
    return length <= MAXDWORD && WriteConsoleW(handle, text, (DWORD)length, &written, NULL)
           && written == (DWORD)length;
}

static int dimensions(HANDLE handle, unsigned *columns, unsigned *rows) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(handle, &info)) return 0;
    *columns = (unsigned)(info.srWindow.Right - info.srWindow.Left + 1);
    *rows = (unsigned)(info.srWindow.Bottom - info.srWindow.Top + 1);
    return *columns > 0 && *rows > 0;
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) return 60;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD input_mode = 0, output_mode = 0;
    if (!GetConsoleMode(input, &input_mode) || !GetConsoleMode(out, &output_mode)) {
        fprintf(stderr, "BRIDGE_NATIVE_ERROR stage=console-modes win32=%lu\n", (unsigned long)GetLastError());
        return 61;
    }
    unsigned columns = 0, rows = 0;
    if (!dimensions(out, &columns, &rows)) return 62;
    /* A wrong initial size is evidence, never silently corrected by this child. */
    if (columns != 132 || rows != 43) {
        fprintf(stderr, "BRIDGE_NATIVE_ERROR stage=initial-size columns=%u rows=%u\n", columns, rows);
        return 63;
    }
    /* Cooked ReadConsoleW is part of the test. Disable echo to isolate markers.
       Preserve all original modes and restore after the owned reader exits. */
    DWORD desired = (input_mode | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT) & ~(DWORD)ENABLE_ECHO_INPUT;
    if (!SetConsoleMode(input, desired)) return 64;
    struct input_read read_result = {0};
    read_result.input = input;
    HANDLE reader = CreateThread(NULL, 0, read_line, &read_result, 0, NULL);
    if (!reader) { SetConsoleMode(input, input_mode); return 65; }
    WCHAR marker[160];
    int count = swprintf(marker, sizeof marker / sizeof marker[0],
                        L"__BRIDGE_NATIVE_MODES:%lu:%lu__\r\n__BRIDGE_NATIVE_READY:%u:%u__\r\n",
                        (unsigned long)input_mode, (unsigned long)output_mode, columns, rows);
    int failure = 0;
    if (count < 0 || !output(out, marker)) failure = 66;
    /* Wait on our reader, not merely the console event: a resize/key-down may
       signal console input while a complete cooked line is still unavailable. */
    DWORD wait = failure ? WAIT_TIMEOUT : WaitForSingleObject(reader, 5000);
    if (wait != WAIT_OBJECT_0) {
        CancelSynchronousIo(reader);
        if (WaitForSingleObject(reader, 1000) != WAIT_OBJECT_0) {
            /* Only this fixture process is ended; host Job owns its lifetime.
               Do not unwind read_result while the reader might still use it. */
            ExitProcess(67);
        }
        if (!failure) failure = wait == WAIT_TIMEOUT ? 68 : 69;
    }
    CloseHandle(reader);
    if (!SetConsoleMode(input, input_mode) && !failure) failure = 70;
    if (failure) return failure;
    if (!read_result.success) {
        fprintf(stderr, "BRIDGE_NATIVE_ERROR stage=read-console win32=%lu\n", (unsigned long)read_result.error);
        return 71;
    }
    const WCHAR expected[] = L"native-ok\r\n";
    const WCHAR expected_lf[] = L"native-ok\n";
    if (!((read_result.length == 11 && !wmemcmp(read_result.text, expected, 11)) ||
          (read_result.length == 10 && !wmemcmp(read_result.text, expected_lf, 10)))) return 72;
    if (!dimensions(out, &columns, &rows)) return 73;
    if (columns != 148 || rows != 47) {
        fprintf(stderr, "BRIDGE_NATIVE_ERROR stage=resized-size columns=%u rows=%u\n", columns, rows);
        return 74;
    }
    /* UTF-16 codepoints for 中🙂, intentionally through WriteConsoleW. */
    if (!output(out, L"__BRIDGE_NATIVE_UNICODE:\x4e2d\xd83d\xde42__\r\n")) return 75;
    count = swprintf(marker, sizeof marker / sizeof marker[0],
                     L"__BRIDGE_NATIVE_DONE:%u:%u__\r\n", columns, rows);
    return count >= 0 && output(out, marker) ? 0 : 76;
}
