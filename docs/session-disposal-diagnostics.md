# Session disposal diagnostic (opt-in, not a fix)

The Windows 2025 MSVC baseline for PR20, run `41040682622`, head
`ab91ee9e49151244ac9404570a72b342a7c7ffb1`, failed `session_tests` with
`default final-frame-only child failed to dispose` (22.3441 seconds total).
Its original assertion combines a 10-second actor wait and process-result flag;
the existing log cannot distinguish output, transport teardown, final-frame work,
or actor disposal. The prior passing total duration is not a per-child baseline.

The final-only child has no wake callback or queued owner-capturing command.
The settings owner implementation is outside this test's link dependencies.
Neither observation proves the failure harmless or fixes it. In particular,
Windows transport exit notification follows reader/controller teardown; a child
exit wake alone does not mean the output reader or actor has finished.

## Build/run

Configure the normal supported native build with
`-DSTORMTTY_SESSION_DISPOSAL_DIAGNOSTICS=ON`, build
`session_disposal_diagnostics`, then run that executable once from the build
folder. This target is intentionally **not** registered as a CTest replacement.
`session_tests` remains linked to the production session library. The diagnostic
Process library, Session library and executable share one PUBLIC compile definition;
none links either production Process or Session library. Only one Process
implementation is linked. The option remains OFF by default. Nothing diagnostic is installed or enabled by default.

Only failures of the first bounded-retained process wait or the final-frame-only
disposal assertion emit a report, before unwind/cleanup. The first failure still
throws immediately: the diagnostic does not continue past it to force the second
site to run.
The existing synthetic child, 30,000-line output, acceptance conditions, 10-second
wait, failure exit and cleanup are unchanged. No retries, larger deadlines or
production Session behavior changes are introduced. The report records counters,
phase names and presence of fixed READY/FINAL markers, never frame payload, PID,
user path, arguments or environment. It does not expose error/provider strings.

## Interpretation and limits

Observations are sampled independently, not an atomic barrier or proof of a hang.

- `site=bounded-retained` uses the original `wait_for_process_exit(10s)` result;
  `site=final-frame-only` uses the original `wait_closed(10s)` and process-finished
  check. `wait_satisfied=0` preserves the timeout even if later samples advance.
- Process states: absent=-1, starting=0, running=1, close_requested=2,
  draining=3, exited=4, failed=5. Session states: starting=0, running=1,
  closing=2, exited=3, failed=4.
- `child_exit_wakes>0`, process draining, `exit_callback=0`: native teardown
  has observed child exit but has not delivered the post-reader/reap callback.
- `received_bytes`, `fed_bytes`, queued output and backpressure waits distinguish
  accepted output backlog from bytes already fed. A difference can include one
  in-flight chunk; counts can advance between samples. Transport byte counts need
  not equal fixture source bytes because ConPTY transforms output.
- `feed-terminal`, `finish-output`, `final-snapshot`, `publish-process-result`,
  `destroy-terminal` and `publish-actor-completion` identify actor stage.
- `exit_callback=1` with pending bytes means the actor has not yet consumed all
  delivered output. `process_finished=1` with actor not closed narrows the problem
  to post-result actor/document cleanup (or a sample race).
- FINAL marker presence alone does not prove terminal EOF/finalization: a held
  frame may become visible before process cleanup.

A successful diagnostic run is an observation, not a reproduction, native gate
qualification, or fix. Preserve the original failed logs and investigate the
reported phase before changing behavior. Instrumentation may perturb scheduling.

## One-shot Windows recipe

The branch-scoped `session-disposal-diagnostic.yml` workflow uses Windows 2025,
MSVC and the existing pinned Ghostty cache/bootstrap contract. It builds only the
isolated executable and invokes the existing owned-process runner once, retaining
the normal test's 90-second outer limit and the fixture's 10-second wait. It saves
head/run provenance, bounded fixture output and process/reap/timeout metadata even
when the test fails. Truncation or cleanup failure cannot yield a green result.
There is no matrix, repetition, automatic retry or normal-gate change. Root owns
publication. The initial qualification is triggered by one publication to the exact
`diagnostic/session-final-disposal` branch, filtered to the diagnostic workflow,
CMake target, session implementation/header and fixture paths. There is no main or
pull-request trigger. Manual dispatch remains optional. Do not repeatedly push
unchanged code or dispatch again to chase a failure; inspect the terminal result
and preserve its evidence before deciding the next authorized change.

## Producer and native snapshot (v2, Windows only)

The fixture gives each of the two bounded children its own private named mapping
via a diagnostic-only environment override. No extra output, flush, input,
thread, reader, wait loop, retry or deadline is introduced. Mapping handles/views
are fixture-owned RAII; no mapping name or environment value is printed. Setup
errors leave diagnostics unavailable, not a failed transport. Stage and completed
line count use Windows interlocked operations and are sampled independently:

- `producer_stage=-1`: local metadata unavailable; `0`: mapping exists but no
  child publication observed (child attach may have failed; not proof of a stall).
- `1`: inserting history; `producer_lines` counts completed ostream insertions,
  **not** bytes written to the console or accepted by ConPTY.
- `2`: READY insertion/flush; `3`: FINAL insertion/flush; `4`: returning from
  the fixture child function. Stage 4 does not prove CRT flush/destructors or OS
  process exit finished. The stage is set before the corresponding insertion.

The isolated Windows Process implementation duplicates the suspended child handle
with only SYNCHRONIZE access before ResumeThread. It retains this handle for the
Impl lifetime, independently of the controller's handle cleanup. Only a failure
snapshot queries it with zero timeout: `os_process_signaled=-1` means unavailable
or failed query, `0` means nonsignaled, `1` means signaled. No exit-code access or
extra process-control rights are requested. The extra handle retains a process
object, not a running process, but instrumentation may still perturb timing.

`read_bytes` counts successful nonempty ReadFile bytes; `callback_returned_bytes`
counts bytes whose output callback returned normally. The difference can include
an in-flight callback, exceptions or independently sampled progress. Reader phase
`read-file` identifies a read attempt (not proof it is blocked); `output-callback`
identifies delivery; `done` plus `reader_done` identify pump completion. Controller
phases distinguish wait, writer join, console close, process reap, reader drain,
reader join, exit callback and done. They are passive labels, not new predicates.
If Session already released Process, `transport_snapshot=unavailable` is expected.
No producer stage, process signaled sample, marker or phase alone proves a hang.
A single failure report can separate unfinished production from signaled-child
controller/reader drain or actor cleanup without claiming a production fix.
