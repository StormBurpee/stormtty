/* Minimal public-C-ABI consumer; works with shared or GHOSTTY_STATIC builds. */
#include <ghostty/vt.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    GhosttyTerminal terminal = NULL;
    if (ghostty_terminal_new(NULL, &terminal, 80, 24) != GHOSTTY_SUCCESS) {
        fputs("terminal creation failed\n", stderr);
        return 1;
    }
    const char input[] = "\x1b[31mStormTTY\x1b[0m";
    ghostty_terminal_vt_write(terminal, (const uint8_t *)input, sizeof input - 1);
    if (ghostty_terminal_resize(terminal, 100, 30, 8, 16) != GHOSTTY_SUCCESS) {
        ghostty_terminal_free(terminal);
        fputs("terminal resize failed\n", stderr);
        return 2;
    }
    GhosttyFormatterTerminalOptions options =
        GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions);
    options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    options.trim = true;
    GhosttyFormatter formatter = NULL;
    if (ghostty_formatter_terminal_new(NULL, &formatter, terminal, options) != GHOSTTY_SUCCESS) {
        ghostty_terminal_free(terminal);
        return 3;
    }
    uint8_t *text = NULL;
    size_t length = 0;
    int result = 4;
    if (ghostty_formatter_format_alloc(formatter, NULL, &text, &length) == GHOSTTY_SUCCESS) {
        if (length == 8 && memcmp(text, "StormTTY", length) == 0) {
            puts("libghostty-vt C ABI smoke passed");
            result = 0;
        } else {
            fprintf(stderr, "unexpected terminal text (%zu bytes)\n", length);
        }
        ghostty_free(NULL, text, length);
    }
    ghostty_formatter_free(formatter);
    ghostty_terminal_free(terminal);
    return result;
}
