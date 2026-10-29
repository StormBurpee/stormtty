/* Fixed synthetic MSYS console reader, not an application or runtime hook.
   Exact internal coverage is decided by parser.py, never by this fixture. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

typedef int (*init_fn)(uintptr_t, uint64_t);
typedef void (*handler_fn)(int, int, unsigned);
static handler_fn trace_handler;
static volatile sig_atomic_t signals_seen;
static void winch(int signal_number) {
  int saved = errno;
  if (signals_seen < SIG_ATOMIC_MAX) ++signals_seen;
  if (trace_handler) {
    trace_handler(1, signal_number, (unsigned)signals_seen);
    trace_handler(0, signal_number, (unsigned)signals_seen);
  }
  errno = saved;
}
static void timeout_exit(int signal_number) { (void)signal_number; _exit(124); }
static int decimal(const char *s, uint64_t *value) {
  uint64_t n = 0;
  if (!s || !*s) return 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9') return 0;
    unsigned digit = (unsigned)(*s-'0');
    if (n > (UINT64_MAX-digit)/10) return 0;
    n = n*10+digit;
  }
  if (!n) return 0;
  *value = n;
  return 1;
}
int main(int argc, char **argv) {
  /* CASE LOG MARKER MAPPING_HANDLE_DECIMAL GENERATION_DECIMAL */
  if (argc != 6) return 2;
  int positive = !strcmp(argv[1], "positive-prefix");
  int zero = !strcmp(argv[1], "zero-copy");
  if (!positive && !zero && strcmp(argv[1], "partial-zero")) return 2;
  uint64_t handle, generation;
  if (!decimal(argv[4], &handle) || handle > UINTPTR_MAX ||
      !decimal(argv[5], &generation)) return 2;
  if (!setlocale(LC_ALL, "C.UTF-8")) return 3;
  FILE *log = fopen(argv[2], "wx");
  if (!log) return 4;
  if (setvbuf(log, NULL, _IONBF, 0)) { fclose(log); return 4; }
  int result = 1, configured = 0;
  struct termios saved, raw;
  size_t attempt_count = 0;
  unsigned char collected[256]; size_t count = 0;
  size_t expected = zero ? 1 : (positive ? 3 : 2);
  HMODULE runtime = GetModuleHandleW(L"msys-2.0.dll");
  if (!runtime) { result = 5; goto cleanup; }
  FARPROC init_address = GetProcAddress(runtime, "stormtty_path_init_v1");
  FARPROC handler_address = GetProcAddress(runtime, "stormtty_path_handler_v1");
  if (!init_address || !handler_address) { result = 6; goto cleanup; }
  /* Copy representation instead of incompatible function-pointer casts.
     Exact native x86_64 ABI/export verification remains a build gate. */
  init_fn initialize;
  _Static_assert(sizeof initialize == sizeof init_address, "function pointer ABI");
  _Static_assert(sizeof trace_handler == sizeof handler_address, "function pointer ABI");
  memcpy(&initialize, &init_address, sizeof initialize);
  memcpy(&trace_handler, &handler_address, sizeof trace_handler);
  if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved)) { result = 7; goto cleanup; }
  raw = saved; cfmakeraw(&raw); raw.c_cc[VMIN] = 2; raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw)) { result = 8; goto cleanup; }
  configured = 1;
  struct sigaction action;
  memset(&action, 0, sizeof action); sigemptyset(&action.sa_mask);
  action.sa_handler = winch; /* deliberately no SA_RESTART */
  if (sigaction(SIGWINCH, &action, NULL)) { result = 9; goto cleanup; }
  action.sa_handler = timeout_exit;
  if (sigaction(SIGALRM, &action, NULL)) { result = 10; goto cleanup; }
  alarm(8);
  if (initialize((uintptr_t)handle, generation)) { result = 11; goto cleanup; }
  FILE *marker = fopen(argv[3], "wx");
  if (!marker) { result = 12; goto cleanup; }
  int marker_ok = fputs("configured-not-proof-of-read-entry\n", marker) >= 0;
  if (fclose(marker)) marker_ok = 0;
  if (!marker_ok) { result = 12; goto cleanup; }
  while (count < expected && attempt_count < 8) {
    unsigned char bytes[64];
    size_t requested = attempt_count == 0 && !positive ? 2 : sizeof bytes;
    errno = 0;
    ssize_t n = read(STDIN_FILENO, bytes, requested);
    int error = n < 0 ? errno : 0;
    /* Fixture-only numeric I/O after a returning read preserves failure
       evidence before alarm exit. It perturbs scheduling between reads, never
       records user bytes or performs I/O in the runtime recorder's hot path. */
    fprintf(log, "{\"attempt\":%zu,\"requested\":%zu,\"count\":%ld,\"errno\":%d,\"signals\":%u}\n",
            attempt_count++, requested, (long)n, error, (unsigned)signals_seen);
    if (ferror(log)) { result = 17; goto cleanup; }
    if (n > 0) {
      if ((size_t)n > sizeof collected-count) { result = 13; goto cleanup; }
      memcpy(collected+count, bytes, (size_t)n); count += (size_t)n;
    } else if (n == 0 || error != EINTR) break;
    /* After the first returning read, drain a possible single tail byte.
       EINTR cannot establish that a preceding partially copied byte survived. */
    raw.c_cc[VMIN] = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw)) { result = 14; goto cleanup; }
  }
  result = count == expected && !memcmp(collected, zero ? "b" : (positive ? "abc" : "ab"), expected) ? 0 : 15;
cleanup:
  if (configured && tcsetattr(STDIN_FILENO, TCSANOW, &saved) && result == 0) result = 16;
  alarm(0);
  fprintf(log, "{\"result\":%d,\"bytes\":%zu,\"signals\":%u,\"synthetic_hex\":\"", result, count, (unsigned)signals_seen);
  for (size_t i = 0; i < count; ++i) fprintf(log, "%02x", collected[i]);
  fputs("\",\"exact_path\":\"requires-parser\"}\n", log);
  if (ferror(log)) result = 17;
  if (fclose(log)) result = 17;
  return result;
}
