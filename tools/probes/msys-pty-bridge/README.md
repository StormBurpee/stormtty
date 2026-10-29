# Independent MSYS PTY bridge — isolated hypothesis

**Not a production backend, default, package, or release-qualified binary.**
No existing experiment is replaced. Base: StormTTY `87ac3dd`. This directory is
not wired into CMake, shell selection, packaging, or production CI. A separate
push-only stage-1 workflow is limited to `prototype/msys-pty-bridge`. Native Windows profiles
must retain their existing ConPTY backend.

## Finding / intended topology

```
Native StormTTY Process worker (owns kill-on-close Windows Job)
    stdin/out anonymous byte pipes + separate control pipe
MSYS-native single-threaded broker (fork before application threads)
    forkpty -> genuine MSYS POSIX PTY
Managed Zsh -> Unix programs and optional native Windows children
```

This bypasses the **outer** ConPTY and Zsh's console-stdin path. It does not prove
that native children avoid ConPTY: pinned MSYS source
`winsup/cygwin/fhandler/pty.cc:4411` (`setup_for_non_cygwin_app`) conditionally calls
`setup_pseudoconsole`, whose implementation calls `CreatePseudoConsole` at 3590.
This native-child path also uses cursor-position requests (`CSI 6 n`). The eventual
qualification host must use the real VT adapter and response channel, including
cursor reports, rather than a passive stdout capture. Preserve raw bytes; do not
strip these requests or fabricate responses unrelated to the terminal state.

Evidence inspected on 2026-10-29:

* Existing verified source archive `msys2-runtime-3.6.11-4.src.tar.zst`, SHA-256
  `4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204`, prepared tree
  at `/workspace/shared/stormtty-evidence/msys-runtime-experiment/source/prepared`.
  Git base `867eb7748e6e5a0e8430ce9f46af27b4b1e8f204`; packaging patches are also
  applied, so this commit alone is NOT a complete source identity. Existing
  runtime experiments retain their provenance.
* `winsup/cygwin/libc/bsdlib.cc:98–160` implements openpty via `/dev/ptmx`, then
  forkpty via fork/login_tty. `login_tty` establishes the child session/TTY.
* Official [MSYS runtime source](https://github.com/msys2/msys2-runtime/tree/msys2-3.6.11)
  and [PTY implementation](https://github.com/msys2/msys2-runtime/blob/msys2-3.6.11/winsup/cygwin/fhandler/pty.cc).
* Current [Cygwin options documentation](https://www.cygwin.com/cygwin-ug-net/using-cygwinenv.html)
  describes `disable_pcon` as disabling PTY pseudoconsole support, not the default.
  This does not establish the exact MSYS environment-variable behavior.
* Official [MSYS terminal guidance](https://www.msys2.org/docs/terminals/) and
  [Microsoft pseudoconsole architecture](https://learn.microsoft.com/en-us/windows/console/pseudoconsoles).

The existing `Process` contract requires ordered, all-or-none bounded input,
backpressured output, async resize/close, and a single exit callback only after
output stops and ownership cleanup completes. A production backend must maintain
all of these. This standalone program is not a drop-in implementation.
`tools/run-owned-process.py` observes one owned child but explicitly does NOT
clean descendants: it is not sufficient alone. `host-windows.c` therefore creates
an owned Job, assigns its suspended child before resume, restricts inherited handles,
and kills/verifies remaining Job processes at cleanup. No broad PID/name scans.

## Files / wire contract

* `broker.c`: C11, forkpty, no app threads, binary stdin/stdout, two 64-KiB queues,
  nonblocking PTY/pipes, no VT parsing or character conversion. Direct `execv`,
  no shell interpolation. Child exec error pipe is separate from PTY bytes.
* `fixture.c`: synthetic TTY/foreground-group assertion, raw byte roundtrip,
  all-byte output, size observation, canonical EOF and Ctrl-C signal fixture.
* `host-windows.c`: native Win32 launcher, **no outer ConPTY calls**, handle
  allowlist and owned Job, bounded capture and 10-second deadline. Only fixed
  synthetic modes. Pipe writes are tiny (under 64 bytes) and bounded in count.
  Does not launch Zsh, Codex, real user commands, or native interoperability apps.
* `test-linux.py`: Linux-only behavioral scaffold; 13 passing tests, including
  512-KiB simultaneous input/output through 64-KiB queues and shutdown while output
  is backpressured. This is not evidence of Windows or MSYS runtime behavior.
* `build-msys.sh`: use an existing verified native/private MSYS compiler; compile
  into a **new** directory. No package install, download or runtime DLL replacement.

Control frame is exactly 8 bytes: `53 54 01 opcode colsLE16 rowsLE16`.
Opcode 1 resizes (each dimension 1..32767); opcode 2 closes (dimensions zero).
Malformed/version/unknown-opcode/control-EOF failures close the session. Partial
frames are retained only in an 8-byte buffer. One frame is handled per loop for
fairness. MSYS control is an inherited native pipe HANDLE passed as decimal argv;
`PeekNamedPipe`/bounded `ReadFile` avoid relying on the best-effort
`cygwin_attach_handle_to_fd` conversion. Control handle inheritance is disabled
before fork. POSIX scaffold instead passes a CLOEXEC FD.

The 20-ms polling control loop and fixed 30-second broker lifetime are deliberate
**prototype limitations**, not latency targets. Resize has no acknowledgment or
generation yet. The tiny fixture uses a scheduling delay, not a false resize ACK.
Host stdin EOF is a transport failure, **not** terminal Ctrl-D; Ctrl-D is byte 04
interpreted by the slave termios discipline. Explicit close may discard queued
bytes as shutdown; normal operation never evicts bytes. Direct child exit permits
2 seconds of bounded draining; an inherited-open slave fails that deadline rather
than hanging. Broker kills/reaps only its direct child's group; Windows Job is
mandatory for descendants in different process groups/sessions. No HOME writes.

## Reproduce / evidence status

Linux:

```
python3 tools/probes/msys-pty-bridge/test-linux.py
```

Native MSYS, with existing verified compiler, never MinGW:

```
bash tools/probes/msys-pty-bridge/build-msys.sh /absolute/private/gcc-wrapper /new/output
```

Compile `host-windows.c` with a native Windows C compiler and Unicode entrypoint
(e.g. clang/MinGW `-municode`); then run the host from Windows with the absolute
Windows broker path, absolute POSIX fixture path, and one of `bytes`, `raw`,
`resize`, `signal`, `eof`, `flood`. Parent should wrap each invocation with the
existing owned-process runner's finite deadline and artifact tail. Keep DLL lookup
scoped to a private staged runtime and preserve compiler/runtime hash provenance.

Observed locally: Linux 13/13; native Win32 host cross-compiles with Zig's
x86_64-windows-gnu C compiler using `-Wall -Wextra -Werror`. **Not run:** native
MSYS broker compilation, native Windows launcher execution, actual Zsh,
interactive/native Windows children, physical UI, performance. No MSYS compiler
or Windows execution surface is available in this Linux workspace. Cross-compile
is not a runtime pass. No conclusion that the ESC race or 15-ms gap is fixed.

## Qualification and promotion gates

1. Native MSYS compile against exact bundled runtime. Run the six supplied native
   host modes; then port Linux malformed/partial/EOF, 512-KiB, resize storms and
   unread-output tests. Repeat with parent cancellation, failed exec, spawn close
   races, grandchildren retaining the slave and nested Job environments. Verify
   Job active count zero, exactly-once direct reap, FD/handle counts and no zombie
   sessions. Record exit status separately from pipe EOF.
2. Replace prototype poll loop and missing status ACK with bounded event-driven
   control/status and explicit protocol version/generation. Resize ACK means ioctl
   applied only, never shell output synchronized. Validate initial dimensions and
   working directory/environment, strict UTF-8 launch arguments, trusted runtime
   paths, concurrent sessions and DLL search. Add access/inheritance failure tests.
3. Isolated managed Zsh HOME/ZDOTDIR (scratch, no user init), then production-like
   pinned configuration: raw leading ESC across rapid resize; arrows, BS, UTF-8
   fragmentation, bracketed/large paste; ANSI, alternate screen, combining/wide
   Unicode; prompt redraw, Ctrl-C, canonical/noncanonical EOF, foreground jobs,
   pipelines, stopped/resumed jobs, exit and close. Preserve exact input/output and
   compare the existing direct-ConPTY baseline under the same synthetic schedule.
4. Native Windows interoperability is a **hard gate**: synthetic native child
   checks `GetConsoleMode`, `ReadConsoleW`/`WriteConsoleW`, console size and
   `ReadFile`/`WriteFile`, Unicode/code pages, redirected stdin, native child ->
   MSYS grandchild -> native child, Ctrl-C/control handlers, resize while each
   owns foreground, native child exit and restoration to Zsh. Capture whether
   MSYS activated its internal ConPTY and honor real VT responses. Do not toggle
   `disable_pcon` globally or treat passive capture as a terminal emulator.
5. Implement an explicit managed-Zsh-only experimental `Process` factory route,
   never silently change a profile or default. Preserve existing Windows native
   ConPTY profiles, nonblocking UI/close, callback contracts, bounded queues and
   process ownership. Full native/visual regression and latency measurements are
   required before considering promotion.
6. Distribution remains blocked on GPL/runtime source-offer, complete matching
   source/build scripts, notices, compiler/runtime dependency provenance and
   license review. A separate broker is NOT asserted to settle licensing or to
   relicense the existing app. No release/legal qualification is claimed.

**Decision:** promising isolated architectural alternative, not rejected; not
approved or qualified for promotion. The next useful result is the native MSYS
compile plus native six-mode scaffold, followed by native-child terminal-response
qualification. Existing direct-ConPTY source experiments remain independent.

## Isolated native stage-1 workflow

`.github/workflows/msys-pty-bridge.yml` triggers only on a push to
`prototype/msys-pty-bridge`; publication belongs to the root reviewer. It leaves
`native.yml` untouched and grants only `contents: read`. No dispatch/retry loop,
production selection, actual Zsh session, fabricated DSR response, or native
Windows child fixture is introduced.

The temporary runner-local MSYS build installation bootstraps the existing
signature/hash-verified GCC15 private-prefix helper. The compiled C broker uses
static libgcc and is copied beside the **unchanged pinned** packaged runtime DLL
in a new qualification-only copy. The source bundle is never modified. Binary
package lock, source archive hash/URL, runtime BUILDINFO/PKGINFO, compiler public
signature evidence, installed build-package versions and binary hashes are
captured. Setup packages belong only to the disposable CI installation, not the
user's machine or their default runtime.

`qualify-windows.py` verifies the original manifest, creates the private copy,
scopes HOME/ZDOTDIR to scratch, and runs exactly `bytes`, `raw`, `resize`, `signal`,
`eof`, `flood` once each. Each native host owns a non-inheritable kill-on-close Job
and uses a 10-second deadline; the outer existing runner kills/reaps that host at
20 seconds (5 seconds reap allowance). Closing/killing the host closes its Job.
Cleanup also terminates and queries the Job for zero active processes on ordinary
completion. No failed case is retried; every result is persisted, and failure is
an aggregate failure. Explicit flood shutdown accepts a clean write-error return
or SIGHUP/SIGKILL (0/129/137), all valid ways this synthetic infinite writer ends
after the requested close. This does not permit arbitrary failure codes.

Artifacts preserve compile failures and bounded **synthetic-only** case logs plus
JSON identities, not executables, runtime packages, private HOME or trust-store
files. `qualification-tests.py` adds eight portable integrity/isolation checks.
Native execution remains unrun until the authorized isolated branch is published
and its Windows result is inspected. A passing stage 1 is still not a passing
Zsh/latency/native-child-interoperability gate.

## Stage-1 observed evidence; stage-2 candidate

Read-only GitHub verification of run **40836000051**, job **123320810299**, commit
`dda9204328f340ea3877ba9c9bd71397d2e9c77f`, confirms six native Windows passes.
Downloaded artifact **12880707653** matched SHA-256
`94b089c98b2dae498bc6cce71186e56e3be88eb6d0e5fc10a08ae1ff38921dd6`.
Its six results have `exit_code=0`, `reaped=true`, no timeout or cleanup error.
Inner fixture statuses include signal=42 and flood=0 (clean write-error return);
all six host assertions passed. Runtime DLL identity was
`d3b5324eef7fe377c3412bd8d9f5c526ba1136940e34105436b15e5f1ae90717`.
This establishes only the stage-1 fixture contract, not interactive shell feel.

The same isolated push-only workflow now proposes a subsequent **stage 2**:

* Separate subdirectory CMake project links the unchanged pinned real
  `TerminalCore`/Ghostty adapter, not the app, Process factory, or agent code.
* Native host starts the broker with the actual pinned bundled Zsh executable,
  `-d -i`, and private `/bridge-home/.zshrc`. This is controlled configuration,
  **not** qualification of the full production addon/prompt configuration.
* Every terminal byte is fed to `TerminalCore`; every `write_pty` effect is sent
  back unchanged through a bounded writer queue. Native-phase actual cursor
  reports are counted and **required**. No hard-coded DSR response exists.
* ZLE-ready sequence/status markers guard TTY checks, Unicode bracketed paste,
  a 4096-character paste, left/backspace editing concurrent with a resize,
  ten fixed resize requests and shell size observation.
* A synthetic native Windows child must actually pass `GetConsoleMode`,
  `GetConsoleScreenBufferInfo`, `ReadConsoleW` with fixed input, and Unicode
  `WriteConsoleW`. Host checks 132x43 before native input and 148x47 after resize,
  then verifies Zsh restoration and its observed size.
* A foreground fixed Zsh-child-to-sleep exec tests Ctrl-C/status130 without the
  pre-foreground readiness race. Explicit shell exit37 and zero-active-process
  owned Job cleanup are required. Host writer cancellation is finite; host
  lifetime is 25 seconds, broker 30 seconds, outer owner deadline 40 seconds.
* No retry, actual user command, real AI program, network listener, or default
  switch is introduced. Only fixed synthetic fixture screen/stderr is saved on
  failure, bounded to help diagnose the first failed phase.

The 200-ms resize settling schedule is **not** an OS ACK: subsequent real shell
and native API observations must independently match. A PASS reports actual VT
response counts and completed phases; a FAIL retains its first phase and native
outer ownership record. Runtime mutation is checked before/after.

Local evidence: complete Windows GNU cross-link of standalone host against the
pinned engine; native C cross-compile; ten portable staging/isolation tests;
Linux real-Zsh fixture smoke 10/10 for TTY, Unicode, resize, bracketed paste,
Ctrl-C/status and exit. These do not substitute for Windows stage-2 execution.
Native Ctrl-C handlers, Windows-to-MSYS-to-Windows nesting, full managed addons,
physical UI, sustained performance, 15-ms output-gap causality, and broader
resize/input stress remain subsequent gates even if this fixed stage 2 passes.
