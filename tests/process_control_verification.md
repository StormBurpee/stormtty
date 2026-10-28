# Windows event-driven process control

The running ConPTY controller waits indefinitely on the owned child-process
handle and one auto-reset control event. Child exit wins when both are ready.
Only close/failure and accepted resize requests signal the control event; input
still wakes the existing writer condition variable. No VT/output ordering or
ConPTY provider policy changed. The separate teardown cancellation and final
output-drain deadlines are unchanged.

## Ownership and signal ordering

The control event is created before the controller thread starts and is held by
the shared `Process::Impl` until all worker/caller references have gone away.
It is never replaced or explicitly reset. Producers publish `closing` or the
latest resize under the existing mutex, unlock, and then call `SetEvent`.
The controller consumes the event in `WaitForMultipleObjects`, takes pending
state under that same mutex, and calls `ResizePseudoConsole` without the lock.

- Requests published before a wait remain represented by both pending state and
  the signalled event. Coalescing several event signals is safe because resize
  has the same latest-value semantics.
- A request published while the controller applies an older resize signals the
  event for the next wait. Updating applied diagnostics never clears pending work.
- If the controller consumes newer pending state before that producer signals,
  the late signal can cause an empty wake, not lost work. No `ResetEvent` can
  erase a producer's notification. Close is a persistent predicate and cannot
  be overwritten by a resize.
- The caller never waits for resize or runs stop-token callbacks. Win32 control
  event operations, ConPTY calls and callbacks run outside the state mutex.

The event handle is private, valid and unchanged throughout every signal/wait;
normal child exit does not close it. This is important when callers race a final
resize or repeated close against process completion.

## Diagnostic meaning

`ProcessDiagnostics::windows_control` is present only on Windows. POSIX returns
`nullopt` and retains its existing transport implementation. The diagnostic
contains only numeric counts, generations and grids; it never captures terminal
bytes, environment values or paths.

Generation zero denotes the launch grid. `applied_grid` remains empty until
`CreatePseudoConsole` succeeds. Every accepted resize advances the requested
generation; the sole controller advances the applied generation/grid only when
that specific `ResizePseudoConsole` call succeeds. Coalescing may skip generations.
A rejected request does not advance state. A native resize failure increments
`resize_failures`, preserves the previous applied state, and enters the existing
failed-session cleanup. Generation exhaustion rejects further requests rather
than wrapping. Wake/call/failure counters saturate at `UINT64_MAX`.

`wait_calls`, `control_wakes` and `child_exit_wakes` describe only the running
controller wait, not reader/writer activity or teardown grace-period waits.
They are event counts, not latency, CPU utilization or performance measurements.

**Applied is not a child acknowledgment or output-stream barrier.** It does not
prove MSYS/Zsh updated its dimensions, that old-width bytes have drained, or that
the known compact alternate-screen Ink race is fixed. No bytes are dropped,
no clear-screen sequence is injected, and no extra signal is sent to the child.

## Tests and evidence boundary

The normal `process_tests` executable includes:

- Portable bookkeeping checks: failed resize leaves applied state unchanged,
  success can advance a coalesced generation, and counters saturate.
- Windows-only real ConPTY checks: a quiet child produces no controller wakes
  over a bounded idle observation; launch-grid state is correct; invalid requests
  do not advance generations; 32 request/apply/repark transitions and four
  concurrent producers converge on one final requested/applied generation.
- A separate child console-API query observes the final dimensions. Native-call
  completion is never substituted for that child evidence.
- Natural exit after resizing retains final output and observes child exit;
  repeated resize/close races run against both a quiet and flooding child.
- Existing no-input exit, callback cancellation, slow-reader input backpressure,
  trailing UTF-8 output, immediate close and destructor tests remain enabled.
- POSIX explicitly rejects fabricated Windows control diagnostics.

These are bounded stress/observation tests, not a deterministic proof of every
thread interleaving. Native failure bookkeeping is tested with an injected
*result* in the pure internal helper, not by invoking a failing native API.
There is no public test-failure switch, invalid-handle exposure or resource-
exhaustion test. A real provider failure requires separately observed native
evidence; failed-call bookkeeping tests must not be called such evidence.

Run the existing `process_tests` CTest target on native Windows/MSVC. For a focused
native rerun use `process_tests.exe --event-control-tests`; non-Windows execution
of that selector fails explicitly rather than silently skipping the native tests.
Linux
execution validates only portable bookkeeping and unchanged POSIX behavior;
Windows GNU cross-compilation validates compilation, not Win32 execution.
Physical terminal/input behavior and CPU/latency measurements remain separate.
