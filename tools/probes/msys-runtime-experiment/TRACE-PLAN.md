# Private resize observation experiment — prepared, not run

This is instrumentation, **not a runtime fix**. It does not include
`0001-retire-resize-before-signal.patch`. Neither generated DLL belongs in an
active installation or release. Root owns publication and CI launch.

## Fixed comparison

- Base: commit `6a97434` (the same source changes as remotely verified `f48ef83b`).
- Original packaged runtime; unchanged source-built control; source-built
  instrumented runtime. All non-runtime bundle files must match original.
- Official complete archive `msys2-runtime-3.6.11-4.src.tar.zst`, SHA256
  `4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204`.
- Apply all official packaging patches; retain identical utility-only const
  compatibility, private GCC 15.3.0 and GNU++17. Do not downgrade host compiler.
- `build-trace.sh ARCHIVE NEW_OUTPUT control|instrumented` builds two isolated
  outputs; instrumented alone adds `-DSTORMTTY_MSYS_RESIZE_TRACE=1` and patch
  `0002-resize-observation-trace.patch`. No install or active-DLL replacement.
- `stage-trace.py BASE BUILD NEW_DEST` requires exact before/after hashes for
  console.cc, termios.cc and fhandler.h, exact compiler-mode bytes, symlink/reparse-free bundle and provenance trees, unchanged
  package inventory and matching patch/source/DLL hashes. Failed behavioral
  patch variant is rejected. Source/provenance and notices remain private.
- One immutable newly built probe and fixture hash for all three variants.
  `compare-trace.py --workload idle --rounds 1` first; then `--workload repeated --rounds 1` with the
  existing 50-phase assertions and prespecified round order. Each subprocess
  keeps the existing 180-second deadline; failure is never retried away.
- No forced signals, sleeps, resends, alternate success criteria, or post-failure
  diagnostics before the host captures the trace boundary.

## Opt-in trace and interpretation

Both build-time macro and `STORMTTY_MSYS_RESIZE_TRACE_DIR` are required. The
latter is set only in the disposable child environment by the opt-in probe.
It must be an existing, drive-absolute DRIVE_FIXED local directory with no ambiguous path
components or reparse ancestors. No automatic directory or terminal logging.
Each process appends `resize-<WindowsPID>.log`; at most 8192 records, each <256
bytes. Existing hard-linked or reparse-point log files are rejected. No inherited file handle is retained. Counter saturates; fork resets
NO_COPY storage. Failed writes do not change control flow, errno or LastError.
The host independently rejects unsafe paths, invalid records, oversized sets,
truncated records and exhausted budgets. Records never contain keys, commands,
terminal output, screen content or paths.

Twelve space-separated decimal integers per LF record:

`seq winPID winTID caller event unit oldX oldY newX newY targetPOSIXPID targetPGID`

`caller` identifies a source callsite, not a memory address. Source line numbers
are fixed against the uninstrumented source where recorded. Unit `-1` is unknown
(e.g. a dev_console helper or generic tty group routing). Target IDs are POSIX
IDs and must not be confused with the Windows emitter PID. Event11 maps the
Windows emitter PID to its POSIX identity once per process; the first reservation
atomically accounts for both this identity line and the triggering observation.
If process identity is not initialized yet, tracing returns without emitting.

| Event | Meaning |
|---|---|
| 1 | fillin changed cached dimensions, with callsite |
| 2 | master thread consumed WINDOW_BUFFER_SIZE_EVENT |
| 3 | reader/select path observed WINDOW_BUFFER_SIZE_EVENT |
| 4 | resize signal path compared old/current cached dimensions |
| 5 | tty group SIGWINCH request and requested foreground pgid |
| 6 | matching tty/foreground group target selected |
| 7 | matching target excluded because it is native/non-MSYS |
| 8 | sig_send about to be called (attempt, NOT delivery proof) |
| 9 | reserved, not emitted |
| 10 | same tty process excluded for different process group; actual pgid |
| 11 | first record (seq1): Windows emitter PID maps to POSIX pid/pgid in target fields |

All fillin callsites are labelled, including read's set_cursor_maybe, ioctl,
output/cursor and master paths. Both event consumers and signal routing are
observed. Do not preselect read's set_cursor_maybe as the cause: it runs before
blocking and select can independently consume events. Post-failure stty ioctl
is not evidence for the initial failure.

Predictions to distinguish:

1. A non-notification fillin changes the cache; later resize consumer compares
   equal dimensions, emits no group request: baseline clobber is supported.
2. No resize event in a complete trace: investigate event generation/consumption;
   this alone does not prove upstream event absence.
3. Group request but wrong-group/native exclusions or no eligible target:
   investigate foreground routing.
4. Correct target and send attempt but stale shell: this trace cannot distinguish
   runtime delivery from Zsh masking/queueing. A separate instrumented Zsh
   handler/adjustwinsize observation would then be required, not a guessed fix.

Native-command recovery alone is not proof of deferred SIGWINCH: pinned Zsh
calls adjustwinsize(0) when reclaiming a foreground tty (signals.c:371 and
jobs.c:582). The stty diagnostic runs in command substitution, unlike the
foreground native helper.

## Important observation limits

Synchronous native file I/O and additional shared-state reads perturb timing.
There is no added sleep/retry, but ordinary storage I/O can block. A passing
instrumented case is not fix qualification. Sequence numbers order trace-call
entry within a process, not a global cross-process order; concurrent writes may
appear out of sequence. Shared cache old/new snapshots are not an atomic history
of every writer. No claim of exact causal ordering across threads/processes is
permitted from this trace alone. The unchanged control is mandatory.

## Local validation and proposed cost

`trace-tests.py --source PREPARED_SOURCE` verifies exact source pins, deterministic
patch application and staged source hashes, and compiles a mocked Win32 sink
with GCC C++17 -Wall -Wextra -Werror. It exercises absent opt-in, relative path,
traversal, alias, reparse/mapped-network-drive/hardlink rejection, open failure, error preservation, numeric
output and concurrent saturation at 8192 records. `stage-trace-tests.py` covers
17 staging integrity/nonmutation cases, including rejection of the failed patch
variant. These tests do NOT verify the native MSYS ABI or Windows filesystem.
Native build and execution remain unperformed.

Exact proposed work: **two full MSYS source builds**, one control and one
instrumented, one immutable probe build, then six idle cases (three variants ×
two providers) and six repeated cases (one round × variants × providers).
No original-runtime compilation is required. The last measured builds took
14m09s + 13m43s = 27m52s; total prior run was 36m17s including setup/comparison.
Those are observations, not guaranteed cost. Allow roughly 35–45 minutes for a
healthy same-runner experiment; native instrumentation compilation may fail.
At the unchanged 180s case limit, all 12 cases alone have a 36-minute upper
budget, plus source builds/setup; do not impose a job timeout below that combined
budget. Each source invocation requires at least 6 GiB free and cleans only its
own generated tree after provenance is secured. No remote action has been taken.
