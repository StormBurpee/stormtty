# Stock versus positive signal-return diagnostic — root review pending

New workflow: `.github/workflows/msys-partial-read.yml`. Push-only exact branch
`experiment/msys-partial-read`, exact commit checkout, read-only permissions.
No dispatch, PR trigger, production/default change, release or automatic retry.
The workflow is authored but has not been run. Root alone publishes it.

## Separate identity and behavior

Stock is byte-identical to the previously observed stock source/observer.
Candidate applies **only** ../0007-preserve-copied-on-signal.patch to stock,
then the same generator's observational instrumentation. Both variants retain
original termios.cc/fhandler.h. No refined-prefix patch is applied. Observer
header and stock observer patch are byte-identical to previous passive-path.
New identities.json pins both source/observer outputs; the old identities are
not modified. Copied instrumented console SHA256:
`65518094c0c99fd54aa350ea1bb4fa284bad3245a89ab436578d363126d287ab`.

Build/toolchain, exact-source verification, staging, DLL identity, strict fixture
compilation, ownership and evidence collection retain the previous bounds. The
native helpers' actual object code must match previously reviewed **stock**
helper bodies. Linked gate then checks actual errno/handler/emitter code and
resolved accessors in the NEW DLL. New source hashes are checked against this
manifest, separately from historical helper evidence. This is an expectation
that identical helper source compiles identically, not a fabricated historical
review of a future object. Any helper-code drift blocks fixtures for review.

## Fixed twelve trials and three independent axes

`trial-plan.json` locks reader/controller/recorder/observer source identities,
case/order matrix, expected hex and timing bounds. Each case runs stock/copied
in order1 and copied/stock in order2, with no retries:

- partial-zero: unchanged prior positive-copy resize fixture; expected `ab`.
- positive-prefix: unchanged prior positive-copy resize fixture; expected `abc`.
- zero-copy: same fixture, except no first key injected; expected `b` after the
  initial zero-copy EINTR. No extra sleep, release, input retry, signal gate or
  scheduling change. Missed zero-copy/resize observation stays inconclusive.

Read readiness marker still is NOT proof of read entry. Reader alarm8s; owned
controller/runner bounds unchanged. Original eight scenarios remain unchanged.

1. `byte_verdict` requires actual exact fixture hex and complete successful owned
   lifecycle. Stock positive-prefix/partial-zero failures are retained as FAIL,
   not converted to pass because expected. Overall run therefore may/should
   remain nonzero even when the candidate improves the measured component.
2. `coverage` retains original passive mouse/master predicates unchanged. They
   are retained informationally, NOT used as the new resize acceptance gate,
   claimed as resize coverage, or silently relaxed. Unreturned reads,
   missing output and unsupported candidate/case names remain INCONCLUSIVE.
3. `resize_observation` uses a separate strict numeric predicate for the actual
   resize→signal→read-end chain. It is observation, not byte/native qualification;
   reader alarm may coexist with an earlier complete numeric observation.
   `component_verdict` requires byte PASS plus RESIZE_OBSERVED and matching
   expected return. Byte failure remains FAIL; missing/mismatched resize evidence
   is INCONCLUSIVE. After byte failures (exit1), missing component evidence exits2.

A future partial-read pass cannot attribute Settings leading-ESC uniquely,
qualify the old refined-retirement behavior, or establish all signal/restart/
error/cancellation semantics. The additional semantic cases in QUALIFICATION.md
remain required before any wider promotion decision.

## Collection and local boundary

Bounded allowlist captures both build sources/provenance, trial-plan, linked
code evidence, all12 traces and logs, and results. No DLL/object/package/user
history is uploaded. Missing required files, aliases or size overflow fail
collection; no trace is silently truncated. Full source archives/private binaries
remain only in the isolated runner, with original licenses preserved.

Portable suites cover pinned patch application, source/hash and fixture timing
contracts, fake-PE linked gates, stage isolation, outcome fail-closed behavior,
strict resize predicate and collection. Local reader syntax check uses only a
Windows API stub: not native MSYS compilation. No native result exists yet.
