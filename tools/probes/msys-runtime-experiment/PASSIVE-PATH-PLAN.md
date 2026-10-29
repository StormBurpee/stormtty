# Passive internal-path observation proposal — not implemented

This is a design-only next gate. No diagnostic hooks, runtime semantic changes,
workflow changes, application changes, or default selection are authorized by
this document. External-reader byte results, when executed, remain **INCONCLUSIVE for exact
internal-path coverage**, even if synthetic bytes are conserved. Run40881120249
executed zero external cases because its launcher failed before the first child;
that is missing coverage, not a passing or executed-inconclusive case. A later
instrumented run would prove only its observed execution, not production safety.

## Source identity and symmetric instrumentation

Anchor locations below to preserved `console.cc.before`, SHA256
`535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33`.
Verified local evidence:
`stormtty-evidence/msys-trace/run40746143302/artifact/control-build/provenance/console.cc.before`.
Do not anchor to the mutable shared `source/prepared` tree: its inspected resize
branch already contained the older unconditional `discard_key_events(i + 1)`.
Offsets change after applying the refined patch; match functions and statements,
not line numbers alone. Record exact stock/refined source, diagnostic patch,
compiler, DLL and fixture hashes separately in cold-path provenance.

A future diagnostic build must be a compile-time opt-in, absent from ordinary
binaries. Use identical observation sites/schema and recorder implementation in
stock/refined; preserve their original branch, return, queue, locking and signal
operations. Candidate-only retirement naturally produces additional records;
stock must not execute a fictitious discard merely to match it. No hook may
repair failures, change statuses, flush native input or manufacture feedback.

## Proposed format v1

A bounded private mapping has a 64-byte numeric header and exactly 16,384 slots
of 128 bytes (2 MiB payload); no wrap or overwrite. A separate cold-path artifact
binds numeric variant/build IDs to provenance. Header fields: magic, version=1,
header/record byte sizes, capacity, session generation, reserved-slot count,
overflow count, initialization/finalization state. Unknown version is rejected.

Each record has this fixed wire layout, with explicit little-endian encoding:

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 4 | committed: zero until complete, one on publication |
| 4 | 2 | schema version=1 |
| 6 | 2 | event code |
| 8 | 8 | reservation sequence |
| 16 | 8 | fixture process generation |
| 24 | 4 | Win32 thread ID (numeric only) |
| 28 | 2 | thread role: reader/master/signal/other |
| 30 | 2 | flags, including valid context and incomplete correlation |
| 32 | 8 | read ID, zero if not a read |
| 40 | 8 | peek ID, zero if not a peek |
| 48 | 8 | operation ID: discard, signal request or lock operation |
| 56 | 8 | parent operation ID |
| 64 | 64 | eight event-specific 64-bit numeric fields |

Signed values use fixed two's-complement representation; sentinel validity is
explicit, not inferred from zero. No character values, terminal bytes, paths,
addresses, environment values or raw native handles are recorded. Event types
and counts suffice; permitted geometry is only the fixed synthetic fixture's
sizes. Byte correctness stays in the existing bounded synthetic reader evidence.
Do not add key hashes: even hashes unnecessarily encode user input.

Suggested event codes/payloads:

- `READ_BEGIN`: requested length, ICANON, VMIN, VTIME, nonblocking, copied=0.
- `COPY`: previous copied, actual copy delta, resulting copied, RA length,
  input_ready, remaining request; separate `READ_MORE` for the actual branch.
- `PEEK`: native API success/error, total_read, remaining requested length,
  caller read/select, ready/RA state. Allocate a fresh peek ID for every attempt.
- `DISPATCH`: event class, index i, total_read, stat, mouse mode, RA length,
  copied count if a validated active read context exists.
- `GEOMETRY`: calling event/index, old/new columns/rows, changed, query success.
  Record already-computed values; do not add another console query.
- `RETIRE_BEGIN`: explicit n, effective n (including stock n=0 default),
  num_input_events_processed, con.num_processed, reason/callsite.
- `RETIRE_API`: requested native count, BOOL result, actual returned count,
  error-valid flag/error, aggregate successful discarded count.
- `RETIRE_END`: aggregate discarded, remaining request, con.num_processed
  before/after. Failed API output counts must not be treated as valid success.
- `LOCK_BEGIN`/`LOCK_END`: input/attach role, acquire/release, native result,
  error-valid/error. Observe existing calls only; never add a lock operation.
- `SIGNAL_REQUEST_BEGIN`/`END`: SIGWINCH, source mouse/resize/master, geometry
  operation ID. Separate handler entry/return observations below.
- `PROCESS_RETURN`: actual stat, peek/index, RA/input_ready, active copied count.
- `READ_RETURN`: count or error, copied count, actual return reason.
- `MASTER_QUEUE`: existing native read/writeback request/result/actual counts,
  total_read, con.num_processed; `MASTER_EVENT`: index/class and resize-change
  decision, removal decision. No queue contents.

Publication is release/acquire with a completed-slot check; reservation order is
not execution completion order. A record after ReleaseMutex cannot prove the
master did not run between that release and its publication. Treat begin/end as
intervals and use source-order/operation IDs, never timestamp ordering alone.

## Context, storage and reentrancy gates

The first qualification scope is one fixed synthetic MSYS reader process with
its actual console master thread. Parent injector owns a private mapping and
owned-job lifetime; initialize/map it before measured reads. Prefer explicit
handle inheritance restricted to approved handles, with no public names,
listeners or user-profile storage. Recorder failure must disable recording and
make the result inconclusive, not change terminal results or block the runtime.

No hot-path allocation, formatting, I/O, event signaling, waits, additional
locks, or lazy TLS initialization. Reserve/publish only bounded numeric memory
records. Save/restore errno and GetLastError without replacing the original API
error capture. Saturation sets a bounded overflow flag/counter and stops
recording; it must not spin or wait for the collector. Do not drain live slots
back into reuse. The controller can export the mapping after owned processes
finish, avoiding runtime teardown logging or partial dump dependencies.

Do NOT assume C/C++ atomics are lock-free or interprocess-safe. Before selecting
a primitive, verify the exact Windows/MSYS target's generated instructions,
alignment, platform-supported shared-memory semantics and lack of libatomic,
TLS, allocator or hidden lock calls. Inspect optimized disassembly and imports;
compile-time lock-free assertions alone are insufficient for cross-process
publication. Abort the diagnostic design if these properties cannot be shown.

Read/peek/operation IDs require nesting-safe identity, not one global
`current_read`. A future implementation must establish fixed preinitialized
per-thread contexts for the reader and master, with role and process generation.
Nested/reentrant reads need distinct contexts and bounded depth; exhaustion or
unexpected threads invalidates coverage. A mapping slot reserved before signal
entry must not be reused by a nested handler. Never retain stack pointers in
records or assume that a suspended context will unwind normally.

Fork/exec is deliberately outside the first fixture: inherited mappings/TLS and
copied ID state can collide across processes or survive exec unexpectedly. A
future fork-aware version needs explicit child generation/reset/detach rules
validated against MSYS fork lifecycle. Unexpected fork/exec makes this v1 trace
inconclusive; do not claim it covers managed Zsh process trees.

The ordinary returning handler may increment its existing sig_atomic_t counter.
Exact handler chronology needs separately reviewed, async-signal-safe observation
using the same proven bounded lock-free primitives, with no standard logging.
A request record is NOT handler entry and kill_pgrp return is NOT handler return.
Do not force an ambiguous handler to inherit a current operation ID; unresolved
multiple pending requests make signal attribution inconclusive. Longjmp can
abandon reserved slots and TLS contexts: unfinished pairs or stale identity must
invalidate the affected trace. Do not install cleanup, suppress longjmp, mask
signals or change SA_RESTART to make traces easier to interpret.

## Concrete source sites

Preserved-stock `fhandler/console.cc`:

- read entry1300–1314; process call/result1365–1385; copy1416–1421;
  read-more1425–1427; final returns1431–1440 (also early-return paths).
- peek1459–1463; dispatch1492; mouse1648–1649; resize1834–1838;
  line_edit result1845–1864; process return1867–1876.
- send_winch_maybe1227–1243: existing fillin/resulting dimensions; changed
  branch; original unlock, kill_pgrp, relock. Refined retirement remains before
  these unchanged operations, with its explicit zero guard.
- discard_key_events1880–1902: original/effective count, every existing
  ReadConsoleInputW result, and actual con.num_processed update.
- master native reads480–493, legacy geometry discovery508–521, event
  loop523–579 (resize553–567), and existing writeback starting580.
- input mutex wrappers4886–4909 in the currently inspected prepared file;
  resolve by function name in the pinned stock. Their returned native results
  are required; invoking a wrapper is not proof the acquisition succeeded.

Any geometry-query return currently ignored cannot become a new behavioral
branch: it is only diagnostic validity information. Do not claim absence of
competing queue consumers from these sites alone. The controlled fixture must
exclude external readers, and native queue mutation outside this runtime remains
outside the ownership guarantee.

## Acceptance predicates: keep distinct cases distinct

1. **Exact partial-copy/status case:** raw VMIN2, VTIME0, requested read length2;
   one read copies one synthetic byte and takes read_more. A later peek dispatches
   MOUSE_EVENT at i=0, discovers changed dimensions with mouse reporting off,
   and a non-SA_RESTART handler returns. Attribute the signal to this mouse
   operation, not an intervening master/resize discovery. Refined skips retirement
   at zero, returns its existing stat and retains copied_chars across retry.
   Validate exact per-read count/bytes and all EINTR results, not aggregate bytes.
2. **Positive-prefix case:** separately arrange VMIN2/readlen64, initial partial
   byte, then a peek containing [synthetic key, mouse, synthetic key]. Require
   actual mouse i=1, changed geometry and refined requested/actual retirement1
   before successful unlock. Require fresh peek after return and exact final
   bytes. This is not the i=0 trace. Existing reader uses readlen64, unlike the
   REVIEW's precise readlen2 counterexample.
3. **Ordinary resize control:** require actual WINDOW_BUFFER_SIZE_EVENT dispatch
   with changed geometry and correlated signal, not merely a window API call or
   SIGWINCH count. Observe stock/refined input_winch and read EINTR separately;
   do not relabel inherited resize semantics as a mouse-path result.

All cases require valid source/binary identity, bounded complete records, no
unexpected threads/processes/context overflow, valid lock/API outcomes, and
unambiguous chronology. API retirement failure/partial retirement is a separate
observation, not successful prefix ownership. Overflow, abandoned longjmp
context, missing handler attribution or missed scheduling predicate yields
**INCONCLUSIVE**, even when output matches. Observed byte failure remains a
failure; inconclusive coverage must never erase it.

## Bounded future validation and limits

Before native execution: schema parser tests for truncation, unknown versions,
invalid IDs, incomplete publication, overflow and unpaired operations; compile
both variants and inspect recorder disassembly/imports; static check that no
original branch/return/mutex/signal operation changed. First native recorder
self-tests exercise concurrent master/reader publication, nested handler and
capacity exhaustion under an owned-job timeout. No retries hidden as success.
Then a predeclared finite paired stock/refined case set, retaining every attempt,
with fixed fixture deadlines and independent controller/job cleanup. Preserve
uninstrumented controls; measured timing in instrumented runs is not a baseline.

Passive recording cannot force the required schedule. Queue-empty polling does
not prove copied_chars; the master may temporarily drain/write back records.
A genuine native resize may be handled before a queued mouse. If the fixed
attempt set never observes the predicates, report inconclusive rather than
adding an implicit retry loop or claiming exact coverage.

A future pause/rendezvous at mouse dispatch could create a controlled schedule,
but extends input-mutex hold time and may alter signal-delivery opportunities.
It is NOT part of this passive plan. Never add unlock/relock, suspend the master,
flush/reorder the queue, or suppress signals. Any later rendezvous proposal needs
separate review of conhost/controller dependencies, deadlock risk and deadlines;
it cannot be described as an unperturbed observation. Even successful passive
traces remain subject to recorder scheduling perturbation and do not prove all
interleavings, unconditional liveness, or release qualification.
