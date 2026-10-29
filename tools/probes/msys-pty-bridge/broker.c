/* Isolated qualification prototype, not a production Process backend.
 * Host owns a Windows kill-on-close Job covering broker and descendants.
 * No shell interpolation; argv after -- is passed directly to execv.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __CYGWIN__
#include <windows.h>
#include <io.h>
#endif
#ifdef __CYGWIN__
static HANDLE control_handle;
#endif
#define CAP 65536
struct queue { unsigned char bytes[CAP]; size_t len; };
static int nonblock(int fd) {
    int f = fcntl(fd, F_GETFL);
    return f < 0 ? -1 : fcntl(fd, F_SETFL, f | O_NONBLOCK);
}
static int cloexec(int fd) { return fcntl(fd, F_SETFD, FD_CLOEXEC); }
static long long now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int pending_error(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static int fill(int fd, struct queue *q) {
    ssize_t n = read(fd, q->bytes + q->len, CAP - q->len);
    if (n > 0) { q->len += (size_t)n; return 1; }
    if (n == 0) return 0;
    return pending_error() ? 1 : -1;
}
static int flush(int fd, struct queue *q) {
    ssize_t n = write(fd, q->bytes, q->len);
    if (n > 0) { q->len -= (size_t)n; memmove(q->bytes, q->bytes + n, q->len); return 0; }
    return n < 0 && pending_error() ? 0 : -1;
}
static int number(const char *s, unsigned long long *v) {
    if (!*s || *s == '-' || *s == '+') return -1;
    char *end; errno = 0; *v = strtoull(s, &end, 10);
    return errno || *end ? -1 : 0;
}
int main(int argc, char **argv) {
    /* Fixed 30-second test lifetime; host must impose its own finite deadline. */
    if (argc < 5 || strcmp(argv[3], "--") || argv[4][0] != '/') return 125;
    unsigned long long value;
    if (number(argv[2], &value)) return 125;
    int control = -1;
#ifdef __CYGWIN__
    if (!strcmp(argv[1], "--control-handle") && value && value <= UINTPTR_MAX) {
        HANDLE h = (HANDLE)(uintptr_t)value;
        if (GetFileType(h) != FILE_TYPE_PIPE) return 125;
        if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0)) return 125;
        control_handle = h;
    }
    setmode(0, O_BINARY); setmode(1, O_BINARY); setmode(2, O_BINARY);
#else
    if (!strcmp(argv[1], "--control-fd") && value > 2 && value <= INT_MAX) control = (int)value;
#endif
#ifdef __CYGWIN__
    if (!control_handle) return 125;
#else
    if (control < 3 || cloexec(control)) return 125;
#endif
    /* Caller passes only stdin/out/err/control handles. No threads before fork. */
    int status_pipe[2];
    if (pipe(status_pipe)) return 125;
    if (cloexec(status_pipe[0]) || cloexec(status_pipe[1])) return 125;
    struct winsize size = {24, 80, 0, 0};
    int master = -1;
    pid_t child = forkpty(&master, NULL, NULL, &size);
    if (child < 0) return 125;
    if (!child) {
        close(control); close(status_pipe[0]);
        execv(argv[4], argv + 4);
        int e = errno;
        while (write(status_pipe[1], &e, sizeof e) < 0 && errno == EINTR) {}
        _exit(127);
    }
    close(status_pipe[1]);
    /* Parent owns only these FDs; SIGPIPE is ignored AFTER fork, not inherited. */
    signal(SIGPIPE, SIG_IGN);
    int failure = 0, reaped = 0, child_status = 0, master_eof = 0;
    if (cloexec(master) || nonblock(master) || nonblock(0) || nonblock(1) ||
        (control >= 0 && nonblock(control)) || nonblock(status_pipe[0])) failure = 125;
    struct queue input = {{0}, 0}, output = {{0}, 0};
    unsigned char frame[8]; size_t frame_len = 0;
    long long start = now_ms(), exit_at = 0;
    while (!failure) {
        long long t = now_ms();
        if (t < 0 || t - start >= 30000 || (reaped && t - exit_at >= 2000)) { failure = 124; break; }
        if (!reaped) {
            pid_t w = waitpid(child, &child_status, WNOHANG);
            if (w == child) { reaped = 1; exit_at = t; }
            else if (w < 0 && errno != EINTR) { failure = 125; break; }
        }
        if (reaped && master_eof && !output.len) break;
        struct pollfd f[5] = {
            {control, POLLIN, 0},
            {input.len < CAP && !reaped && !master_eof ? 0 : -1, POLLIN, 0},
            {master_eof ? -1 : master, (short)((output.len < CAP ? POLLIN : 0) | (input.len ? POLLOUT : 0)), 0},
            {output.len ? 1 : -1, POLLOUT, 0},
            {status_pipe[0], POLLIN, 0}
        };
        if (poll(f, 5, 20) < 0) { if (errno == EINTR) continue; failure = 125; break; }
        /* One fixed-size frame per iteration: control cannot starve PTY data. */
        int control_ready = f[0].revents != 0;
#ifdef __CYGWIN__
        DWORD available = 0;
        if (!PeekNamedPipe(control_handle, NULL, 0, NULL, &available, NULL)) { failure = 125; break; }
        control_ready = available > 0;
#endif
        if (control_ready) {
#ifdef __CYGWIN__
            DWORD got = 0;
            DWORD want = (DWORD)(sizeof frame - frame_len);
            if (want > available) want = available;
            if (!ReadFile(control_handle, frame + frame_len, want, &got, NULL)) { failure = 125; break; }
            ssize_t n = (ssize_t)got;
#else
            ssize_t n = read(control, frame + frame_len, sizeof frame - frame_len);
#endif
            if (n > 0) frame_len += (size_t)n;
            else if (!n || !pending_error()) { failure = 125; break; }
            if (frame_len == sizeof frame) {
                unsigned cols = frame[4] | ((unsigned)frame[5] << 8);
                unsigned rows = frame[6] | ((unsigned)frame[7] << 8);
                if (frame[0] != 'S' || frame[1] != 'T' || frame[2] != 1) { failure = 125; break; }
                if (frame[3] == 2 && !cols && !rows) { failure = 0; goto close_session; }
                if (frame[3] != 1 || !cols || !rows || cols > 32767 || rows > 32767) { failure = 125; break; }
                size.ws_col = (unsigned short)cols; size.ws_row = (unsigned short)rows;
                if (ioctl(master, TIOCSWINSZ, &size)) { failure = 125; break; }
                frame_len = 0;
            }
        }
        if (f[4].revents) {
            int e; ssize_t n = read(status_pipe[0], &e, sizeof e);
            if (n > 0) { failure = 127; break; }
            if (!n) { close(status_pipe[0]); status_pipe[0] = -1; }
            else if (!pending_error()) { failure = 125; break; }
        }
        if (f[1].revents && fill(0, &input) <= 0) { failure = 125; break; }
        if (f[2].revents & POLLOUT) if (flush(master, &input)) { failure = 125; break; }
        if (f[2].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (output.len < CAP) {
                int r = fill(master, &output);
                if (!r || (r < 0 && errno == EIO)) master_eof = 1;
                else if (r < 0) { failure = 125; break; }
            }
        }
        if (f[3].revents && flush(1, &output)) { failure = 125; break; }
    }
close_session:
    close(master); if (control >= 0) close(control);
#ifdef __CYGWIN__
    CloseHandle(control_handle);
#endif
    if (status_pipe[0] >= 0) close(status_pipe[0]);
    /* Kill only the owned shell group while direct child is unreaped. Windows
       host Job is mandatory for descendants in other groups/sessions. */
    if (!reaped) {
        kill(-child, SIGKILL); kill(child, SIGKILL);
        long long until = now_ms() + 2000;
        for (unsigned attempt = 0; attempt < 200; ++attempt) {
            long long current = now_ms();
            if (current < 0 || current >= until) break;
            pid_t w = waitpid(child, &child_status, WNOHANG);
            if (w == child) { reaped = 1; break; }
            if (w < 0 && errno != EINTR) break;
            struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
        }
    }
    if (!reaped) return 125;
    if (failure) return failure;
    return WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 128 + WTERMSIG(child_status);
}
