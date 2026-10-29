#define _DEFAULT_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
static void interrupted(int sig) { (void)sig; (void)write(1, "INT\n", 4); _exit(42); }
static int send_all(const void *data, size_t size) {
    const char *p = data;
    while (size) { ssize_t n = write(1, p, size); if (n > 0) { p += n; size -= (size_t)n; } else if (errno != EINTR) return 1; }
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 2 || !isatty(0) || !isatty(1) || tcgetpgrp(0) != getpgrp()) return 90;
    struct termios t; if (tcgetattr(0, &t)) return 91;
    if (!strcmp(argv[1], "signal") || !strcmp(argv[1], "eof")) {
        t.c_lflag &= ~ECHO; t.c_oflag &= ~OPOST;
        if (tcsetattr(0, TCSANOW, &t)) return 92;
        signal(SIGINT, interrupted); send_all("READY\n", 6);
        char c; ssize_t n = read(0, &c, 1);
        if (!n) return send_all("EOF\n", 4);
        return 93;
    }
    cfmakeraw(&t); if (tcsetattr(0, TCSANOW, &t)) return 92;
    if (!strcmp(argv[1], "bytes")) {
        unsigned char data[256]; for (unsigned i = 0; i < 256; ++i) data[i] = (unsigned char)i;
        return send_all(data, sizeof data);
    }
    send_all("READY\n", 6);
    if (!strcmp(argv[1], "resize")) {
        char c; if (read(0, &c, 1) != 1) return 94;
        struct winsize w; if (ioctl(0, TIOCGWINSZ, &w)) return 95;
        char text[64]; int n = snprintf(text, sizeof text, "%u,%u\n", w.ws_col, w.ws_row);
        return send_all(text, (size_t)n);
    }
    if (!strcmp(argv[1], "raw")) {
        /* Prefix is 4-byte LE length, then opaque payload, maximum 1 MiB. */
        unsigned char hdr[4]; size_t got = 0;
        while (got < 4) { ssize_t n = read(0, hdr + got, 4 - got); if (n <= 0) return 96; got += (size_t)n; }
        size_t left = hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16) | ((size_t)hdr[3] << 24);
        if (left > 1024 * 1024) return 97;
        unsigned char b[4096];
        while (left) { ssize_t n = read(0, b, left < sizeof b ? left : sizeof b); if (n < 0 && errno == EINTR) continue; if (n <= 0) return 98; if (send_all(b, (size_t)n)) return 99; left -= (size_t)n; }
        return 0;
    }
    if (!strcmp(argv[1], "flood")) { char b[4096]; memset(b, 'X', sizeof b); for (;;) if (send_all(b, sizeof b)) return 0; }
    return 100;
}
