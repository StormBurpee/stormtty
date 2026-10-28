# Private cursor-only versus cursor-plus-retirement comparison

## Evidence and hypothesis, not a proven fix

Run40753733764/head d3378614 passed both candidate builtin-only one-shot50
cases, whereas original/control failed all four. Candidate normal strict50
still failed command/prompt scope code3 at inbox phase30 and packaged phase28;
unknown observed grid. That code included pre-resize journal-reset and
post-request size-query operations. It did NOT establish lost input, a stale
resize event, a native-helper cause or a candidate regression. No raw command
logs are uploaded. The previous retirement-only candidate also failed stale
shell-size observations; combining patches is an experiment, not an inference
that either observation establishes the other mechanism.

The baseline here is explicitly the UNINSTRUMENTED cursor-only candidate,
not untouched upstream. The combined candidate adds retirement of the peek
prefix before send_winch_maybe can release input_mutex, then returns without
a second discard. No other event path or signal routing change. Both retain
the modern cursor early-return change. Production pins, active installation,
app settings, release qualification and distribution approval are untouched.

## Exact isolated builds

Both full source builds use official complete msys2-runtime-3.6.11-4 archive
SHA2564b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204,
all official packaging patches, identical utility-only const compatibility,
isolated verified GCC15.3.0/GNU++17 and no instrumentation macro. Both apply
0003-modern-cursor-fill.patch. Combined alone applies
0004-retirement-after-cursor.patch (the same retirement mechanics as the older
experiment, with a corrected narrow comment). Do not apply old0001 or trace0002.

Official prepared console hash:
535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33
Cursor-only baseline:
25a3fb9355235614819f4c56e2a60c8ecb3b4bd4cd49a6befa7ff56ab03cc0b3
Combined:
da2efe83f28c324d4d3a9085240ccfc0da38c180f94bab78cb7b6f3612a7edab

Header and termios source remain official bytes. Staging/comparison checks
both patches, exact before/after source, compiler mode and package inventory.
Untouched bundle is only the staging and non-runtime identity anchor; it is
NOT an executed baseline or a third compiled variant in this experiment.

## Retirement caveats and regression scope

The source's mouse-event path can call send_winch_maybe earlier in the same
peek loop and release input_mutex. Consequently the later resize branch's
peek index may already be stale. This patch does NOT fix that mouse path or
assert all prefix indices remain protected. discard_key_events returns void
and can stop on error/zero read, so retirement completeness is not guaranteed.
Unchanged-size resize records now return early, requiring callers to process
remaining queued keys on another invocation. Signal-handler escape can skip
normal return paths. Prefix keys may already have entered line-edit buffers;
read and select callers have different readiness predicates. These are native
regression obligations, not resolved by the deterministic model tests.

Model tests cover buffered prefix keys, ready-prefix return, unchanged-size
early return, keys arriving during unlock, signal-handler escape, read/select
caller abstractions, partial/zero discard and an explicit unfixed mouse-unlock
negative case. They are not MSYS execution or proof of observed code3 cause.
The existing termios signal path already retires before releasing its mutex;
that supports an ordering discipline, not correctness of this new branch.
Cursor-only's removed incidental con.b/dwEnd refresh and unmeasured native mode
flags remain independent limitations. No native legacy-mode claim is made.

## Prespecified qualification

One immutable probe/fixture for both providers (inbox and packaged) and both
variants. Initial idle one-shot: one round, four cases. Normal strict50 and
builtin-only one-shot50: three rounds each, twelve cases per workload,24 stress
cases total. Within each provider order alternates baseline/combined,
combined/baseline,baseline/combined. All28 cases are retained. No failed trial
is replaced or retried until green; later workloads run despite earlier
assertion failure. Default normal qualification polling/strict assertions and
job-control gates remain unchanged. Builtin-only mode has one size command
per resize and no native job before/between phases. No forced SIGWINCH,
resize resends or corrective sleeps are introduced.

Individual outer watchdog remains180s. Each12-case workload allows42min,
not the old22min six-case cap. Two source builds each allow40min; job300min
covers builds, up to84min for28×180s and setup overhead. Prior source build
wall time near28min is a reference only: total cost is not guaranteed35min.
No appSettings30s gate changes.

## Diagnostic contract

Opt-in one-line numeric summary expands original code/phase/grids with
operation(substage), protocol_step, fixedreason, write_result, expected prompt
sequence, resize_accepted, prompt-status known/value, child-exit known/value.
Stages distinguish journal-reset before resize from size-query after request,
native query and journal flush. Protocol steps are set inside send/ready:
prior readiness, text enqueue, Enter enqueue, next readiness. Readiness
failure is classified only after the unchanged deadline. Reason3 means that
deadline expired with a nonexpected numeric status for the exact expected
prompt sequence, not an early failure. Child exit status is distinct from
prompt status. Failure inspects only matching-sequence status in the final
predicate frame; older-sequence status is ignored;
no frame/command content is retained. Unknown numeric fields use explicitknown
bits and zero sentinel. Status/readiness observation does not prove transport
or shell-internal causality. No extra shell commands, retries or deadline
changes; non-opt-in uses the original path. Summary is emitted once before
post-failure diagnostics, and strict parsers reject missing/duplicate/malformed
or incoherent numeric records. Full specification remains in code/tests.

Root owns final review, remote publication and native launch. Local model,
parser, staging and cross-compilation results are not native qualification.
Do not activate a production runtime merely because a comparison improves.
