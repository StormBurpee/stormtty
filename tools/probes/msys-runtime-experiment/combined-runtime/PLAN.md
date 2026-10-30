# Copied-prefix + retirement: fixture/observer review gate

**Status: local preparation only. No native build, workflow, staging, binary,
runtime/default/pin change, or publication is authorized by this package.**
A source-only stage-1 runner/collector and component adjudicator are now
present below. They require independent source and linked-binary review before
execution; the full matrix adjudicator remains incomplete. Nothing here is
a production runtime qualification.

## Fixed comparison and retained negative evidence

A (`copied`) = pinned stock + unchanged 0007. B (`combined`) = the same source,
toolchain, utility-only compatibility, and 0007, plus unchanged 0006. The
experimental source identity files do **not** replace production pins. Preserve
the existing GCC/G++ 15.3.0 package hashes, GNU++17, source archive
`4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204`, upstream commit
`00eb8435b1c498e9211c7a7cee01584d76bd5f21`, and untouched termios source. Current
upstream/package-version reconciliation is root-owned; the parallel source
review reports no relevant newer fix and does not justify changing these pins.

- 0006: `1afc3483da5dcc8848f336619f858c0ffcfbef608d1446464e6566135b2aff66`.
- 0007: `2a827b8e656975d8f34511152a664783fe48c1f6199ecd5506825af92bd7b508`.
- A console: `cf4eba2150f2b99dfe7c55c8d73ac0f578d7675e649b8468a364bfb8fe2efdb2`.
- B console: `4d32b6219c1241a9fea998909ea8451799bf0c4f57d0eb004f88b9b9e9a57e66`.
- B header remains the 0006 `send_winch_maybe(size_t = 0)` interface. Applying
  0006 and 0007 in either order must yield byte-identical console and header.

Run 40948783314 remains failed: resize-only leading ESC loss in all 18 raw
launches, managed 24/24 bounded samples passing, Settings 70/78. Run
40971233209 remains failed for both timed and direct one-byte readers under
resize; all 80 no-resize rounds passed. Neither experiment identifies the
internal loss path. The old passive test changed geometry *before* mouse
injection and actually observed a head resize, not the intended head mouse.

## Diagnostic scheduling contract (explicitly NOT passive)

The generated observer is applied **after** the A/B behavior patches. It changes
no input content, input mode, signal decision, retirement count, source result,
or lock ownership. Its one deliberate scheduling perturbation is:

1. In `process_input_message`, dispatch a selected mouse (`i=0` or `i=1`).
2. With input mutex still held and the prior peek's attach mutex already
   released, signal an inherited unnamed auto-reset `arrived` event.
3. Wait at most **1500 ms** on a distinct inherited auto-reset `resume` event.
4. The native controller, which neither acquires an MSYS mutex nor reads the
   trace, changes the private console window width by the prescribed -1/+1 (or
   unchanged 0), verifies the OS geometry, then signals `resume`.
5. Only then does the original `send_winch_maybe` query run. Trace must show
   the selected dispatch, successful gate, changed geometry, and signal edge.

No master suspension, signal suppression, queue flush/rewrite, added input-mutex
round trip, reader delay/retry, or input timing workaround. The bounded gate
counter is **diagnostic scheduling control only**, never a proposed runtime
progress fix. Gates apply only to a returning reader with a live read ID, never
select/master. At most eight selected visits; a failed/timed-out gate permanently
disables later gates and makes the case inconclusive. Original processing still
continues. All observer paths preserve both errno and GetLastError. The
controller releases an arrived gate even on geometry failure, then must terminate
and reap its owned job. Its 2000 ms wait does not turn a late runtime timeout green.

There is no new lock cycle: reader holds input, not attach; controller uses only
Win32 console geometry and event APIs, not runtime locks; master may naturally
wait on the existing input mutex. This is a source argument, not native deadlock
proof. Native timeout/reap/cleanup observations are mandatory.

## Required component matrix (both A/B; fixed two orders, no rerun-to-green)

| Case | Authored input / control | Required evidence, not just success bytes |
|---|---|---|
| Stable head mouse off/on, read and select | `[mouse,key]`; unchanged geometry | Head dispatch i=0, actual mouse mode, unchanged successful query, normal retirement and exact key/report ordering; no changed-geometry signal |
| Focus off/on, read and select | `[focus,key]` | Head focus dispatch, mode configuration evidence, exact report/key order or no report, retirement and empty native queue |
| Unchanged head resize, read and select | `[unchanged-size,key]` | Head resize dispatch, unchanged query, progression/retirement and exact key |
| Changed head mouse off/on | `[mouse,key]`; one i=0 gate | Changed query, **zero retirement before signal unlock**, paired internal handler, B returns existing status, abandons peek; fresh peek redispatch/consumption, exact report/key once |
| Positive prefix off/on | `[key,mouse,key]`; one i=1 gate | First key traversed, i=1 changed mouse, **exactly one record** retired once before unlock; no stale-peek continuation in B; fresh head mouse and tail consumption with exact bytes once |
| Finite storm off/on | `[mouse,key]`; three i=0 gates with -1/+1/-1, then stability | Three changed revisits and three successful rendezvous, no retirement before each unlock; fourth stable fresh peek consumes head and tail; no fourth geometry change |
| Copied-positive input_winch | first `a`, VMIN=2, request=2 **and** 64; resize then `b`, no SA_RESTART | Copy1 + READ_MORE; actual input_winch ingress and restart=false; SIGNAL_COPIED returns1/error0; later exact `b` and empty queue |
| Copied-positive WAIT_SIGNALED | same read sizes; external SIGUSR1 after wait notice, then `b` | Actual cygwait result WAIT_SIGNALED and WAIT_SIGNAL ingress; token-correlated external handler in same read; SIGNAL_COPIED returns1/error0, exact tail |
| Zero-copy input_winch / WAIT_SIGNALED | no key before each independent signal; then `a` | Distinct ingress, zero copied, no restart, SIGNAL_EMPTY returns -1/EINTR; later exact `a` |
| SA_RESTART input_winch, zero and positive copy | VMIN2; resize before any key, or after `a`; finish with exact `ab` | input_winch ingress, restart=true, same read continues and returns exact `ab`, no signal-return reason |
| SA_RESTART external, zero and positive copy | VMIN2; SIGUSR1 before any key, or after `a`; finish with exact `ab` | External handler pair inside one wait begin/end; same read returns exact `ab`; **do not require WAIT_SIGNALED** |
| Ordinary raw/canonical/select | VMIN1/request1 and larger normal cases retained | Correct bytes/newline/VMIN/readiness and `len=0` select evidence; no diagnostic mouse gate in select |
| Mouse ordering/surrogates | existing report/key and split-surrogate cases retained | Exact event/report/key order, UTF-8 output and native queue residue, including ordinary no-resize controls |

The native runner must inject all specified records as one verified complete
write per batch, log requested/accepted counts, collect exact synthetic bytes,
configure modes explicitly, and prove no extra authored records. Queue=0 alone
does not prove preservation. Dispatch type/index alone does not uniquely identify
a native record: controlled input inventory, exact output and retirement totals
are also required. A control that never reaches its selected path is
**INCONCLUSIVE**, never pass. A valid witnessed path with incorrect bytes remains
a behavioral failure. No case may redefine the older passive evidence's verdict.

A is not expected to satisfy B's pre-unlock retirement/abandonment assertions.
For A, preserve and report the actual continued-peek/late-retirement baseline
path rather than changing its behavior or pretending the B contract was met.
The intended dispatch/gate/geometry exposure must be established independently
in both variants; mechanism difference is separate from byte correctness.

**Finite storm recovery is not unconditional liveness.** Geometry changes at
every head-mouse revisit remain a source counterexample for B. These tests do
not claim to solve it, and do not add retry counters/delays to the runtime fix.

## Ingress, handler and return distinctions

New wire schema `STCMB002`/version2 adds events21–28: original read wait begin/end,
ingress (WAIT_SIGNAL / INPUT_WINCH / INPUT_SIGNAL), actual SA_RESTART branch,
rendezvous begin/end, one-shot wait notice result, and external signal arm.
DISPATCH lane7 now captures actual `con.use_focus` alongside the existing actual
mouse-mode lane; a successful configuration write alone is not mode coverage.
READ_RETURN lane3 identifies the exact source return site, independent of ingress:
background, timeout-positive/empty, nonblock-positive/empty, complete, error,
signal-positive, signal-empty. The generator fail-closes if the nine sites drift.

Internal SIGWINCH handlers retain same-thread signal-operation/read/peek
correlation. External SIGUSR1 has an explicit one-time token armed before read;
handler snapshots use only atomic reader IDs, never the interrupted mutable
Context table. Unmatched/outside-read signals emit CONTEXT_ERROR and invalidate full coverage.
The decoder additionally rejects reused one-shot tokens, overlapping handlers or
external/internal signal sources, and arm/handler/read thread mismatches. Such
ambiguity may still be recorded by the bounded observer, but can never produce
a structurally complete trace. No handler-side allocation, lock or new API is
introduced for these checks. External sender must be a separate, owned MSYS helper
using `kill(reader_posix_pid,SIGUSR1)`, launched by the controller only after the
one-shot wait notice. No thread suspension or synthesized runtime signal path.
The wait notice is not evidence that cygwait is already blocked; **actual trace
result determines coverage**. No signal retry if the path was missed.

Pinned `cygwait.cc` SHA256
`921b698deb9c2fa02f5b2894e78d570e1bd35c0245458c7d9bb561ee7cd5b95f` and header
`22a6f8b292570ef76c17613f91f30ea0ea1be2bf9da521ca011f78df45b0d75d` show the
default `cw_cancel|cw_sig` call loops internally on restart. Thus external
SA_RESTART continuation is distinct from observed WAIT_SIGNALED nonrestart.
No cygwait source change is proposed.

## Pre-build / pre-execution gates and follow-on scope

1. Review this exact diff, two generated patches, source contracts, malformed
   trace rejection and portable mocked-API tests. These are **portable only**.
2. Review the staged native runner/collector/adjudicator against its explicit
   scope, complete the pending matrix cases, and verify native timeout and
   child/job cleanup behavior. Portable fixed-order, mutation and CRLF checks
   cannot replace those native observations.
   No runtime build until root clears source/upstream reconciliation and review.
3. A later authorized native build must use the unchanged exact archive/compiler
   pins, capture source hashes and disassembly, and build both behavior variants
   with an identical observer. No execution based on cross-compile alone.
4. Before executing reader code, independently check actual linked DLL handler,
   emitter, TLS errno accessor and resolved OS imports, plus reader's real PE
   imports and callback call graph. Existing v1 linked hashes **cannot** qualify
   this v2 observer. `linked-gate.py` requires a new variant-specific independent
   review, exact DLL/reader/object/fixture identities, reviewed reader handler
   bytes and exact named PE imports; there is intentionally no approved review
   checked in. It reuses the tested PE/relocation verifier, not the v1 approval.
   Permit only reviewed leaf TLS/accessor paths in handlers;
   reject logging, allocation, locking/waiting, lazy import resolution and unknown
   helpers. Rendezvous SetEvent/WaitForSingleObject must be unreachable from the
   handler; cold GetProcAddress/MapViewOfFile must remain cold.
5. Native evidence requires complete/finalized/unoverflowed traces, no context
   errors, paired waits/gates/handlers/locks, exact source/toolchain/fixture
   identities, byte truth and owned-process cleanup. Missing linked or runtime
   gates stay pending/inconclusive. Only logs/source/provenance artifacts may be
   exported; never binaries, toolchains, HOME, tokens, handles or user payloads.
6. Only after component evidence review may root consider **uninstrumented** A/B
   raw/readiness comparison; later current-app managed/Settings qualification
   requires a separate justification. AI/Leyline remain paused.

## Local verification command

`python3 tools/probes/msys-runtime-experiment/combined-runtime/tests.py
--source-root /path/to/already-retained/prepared-source -v`

This applies patches only inside temporary workspace directories, compiles a
Linux API-double test plus a C syntax-only fixture surface, and removes its
temporary files. It does not download source/toolchains or build MSYS. Current
result: 37 top-level tests pass, including 14 separately launched API-double
cases and six shared PE/relocation regressions. No native result is implied.

## Structural validation repair

Lock, retirement and signal operation IDs are single-use, thread-owned and
properly nested. Begin records name the active parent; matching ends preserve
kind, read, peek and role, and retirement API observations belong to the current
retirement operation. An unmatched operation, overlapping read, wrong-thread
reader/handler observation, repeated external token or overlapping handler
invalidates `complete`. These checks concern trace integrity only: even a
complete trace remains INCONCLUSIVE until the native matrix adjudicator and
all linked/runtime gates exist and pass.

## Implemented stage 1 (still pre-execution)

The local `native/` fixture, `run.py`, `adjudicate.py`, `matrix.py` and `collect.py`
now form a staged runner for 15 console-record cases. They are **source only**;
no Windows binary, native execution, or linked review is supplied. `matrix.py`
lists the remaining 17 required cases explicitly. Even all 60 stage-1 trials
passing cannot change `full_matrix_verdict` from INCONCLUSIVE or permit runtime
promotion. The missing stages include select record variants, copied/zero-copy
signal ingress and restart cases, and dedicated surrogate/order controls.

The schedule has two opposing A/B orders and no repeated trials. Each reader
stops at the first mismatching observed byte prefix. A witnessed behavioral
failure remains a failure while the scheduled opposing variant can run; any
incomplete exposure, cleanup, trace or identity check stops the remaining
campaign without retry. Raw byte mismatch alone, without valid exposure, cannot
establish a witnessed component or stage failure. No adaptive case ordering,
extra retry, automatic runtime build or automatic review generation exists.

Before staging either variant, the runner applies the existing linked gate to
both exact runtime/reader binaries and requires a fresh independent controller
review with `controller_job_ownership_reviewed`,
`controller_input_schedule_reviewed`, `controller_sha256`, and the exact
`fixture_source_sha256` mapping. The source map includes the linked verifier,
its PE verifier dependency, and the owned-process runner. The controller copy
is checked against the **reviewed** hash, not a newly observed baseline. Source
and private executable/DLL hashes are rechecked before and after each trial.

The controller creates a new private console and injects one exact, fully
accepted authored record batch. It never drains ambient input; any preexisting
queue makes exposure incomplete. It records actual input inventory, modes,
geometry, accepted counts, exact returned bytes, per-read counts/errors, gate
outcomes and lifecycle evidence. Actual MSYS mouse/focus mode comes from trace
DISPATCH fields, not the requested control sequence. Trace/result mappings are
read only after primary reap and confirmed zero remaining job writers.

Adjudication separates bytes, path coverage and B's retirement/abandonment
mechanism. Changed mouse cases require one-to-one dispatch/gate/query chains,
successful original input unlock, paired internal handlers and fresh mouse
redispatch. B's positive-prefix retirement requires exactly one successful
record retirement before unlock; zero-prefix must not retire. Stable controls
reject unexpected geometry changes/signals. Invalid or missing evidence stays
INCONCLUSIVE. Component success never asserts physical UI performance or a
production fix.

`collect.py` exports only bounded result JSON, assessments, launcher logs and
numeric traces from the fixed trial names, with a hash manifest. It excludes
private staged executables/DLLs and rejects symlink evidence. It never copies a
runtime, toolchain, HOME, credentials or arbitrary directory contents.

Portable checks (not Windows execution):

- Existing observer/source/linked regressions: `tests.py --source-root ...`.
- Synthetic evidence/runner/collector regressions: `stage-tests.py`.
- Native C declaration/syntax and ownership-contract checks: `native/tests.py`.

Fresh native ABI/compiler, real lifecycle/timeout and linked-binary review
remain required before any execution. These portable checks are not substitutes.
