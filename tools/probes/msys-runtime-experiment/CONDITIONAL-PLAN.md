# Private stock versus conditional-retirement native experiment

This is a bounded hypothesis test, not a production runtime fix or a promotion.
Root owns review and the one intentional workflow publication push. This branch changes no
production runtime pins/defaults, application assertions or raw-probe code.

## Exact probe and base

Remote PR7 commit `74c0df464012d586032d3a9479380d099cb1788f` has tree
`5aedc1a9c752c9ca2b30b9620172d25ecc976940`. Its verified local equivalent is
`2f8569230eccec454357fd2588cfb375c4c3af5d`, the base of this experiment.
`tests/paste_input_probe.cpp` remains byte-for-byte SHA256
`251477c3587c25b498bf1bf099e355d47a2cf3e1b8f1dcd7649e406aca39beae`.
The build and runner check that hash; Git attributes prevent Windows CRLF drift.
The standalone CPR producer classifier and child-byte comparison are unchanged.
The same compiled probe and production smoke executable serve every trial.

## Two source builds only

Both builds use the existing official complete `msys2-runtime-3.6.11-4` source
archive, SHA256 `4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204`,
128907415 bytes, and all its official packaging patches. The unchanged historical
compiler bootstrap verifies official signatures and hashes into a private prefix:
GCC 15.3.0, GNU++17, `__cplusplus=201703L`. Neither system downgrade nor signature
bypass is permitted. Both apply identical utility-only const compatibility.

- **stock**: official prepared runtime source, no cursor/retirement/trace patch.
- **conditional**: stock plus exact reviewed `0005-conditional-retirement.patch`,
  SHA256 `788f01ace57334125ef99c1358699e4ebd3aa0f1d888d8202a1511c416c21039`.

No previous 0001/0003/0004 behavior patch is applied. The untouched binary package
is only an integrity/staging anchor, not a third executed or compiled variant.
Both copies are marked qualification-only, not default-activatable or releasable.
Provenance preserves original archive locally, source before/after, recipe,
packaging/experimental diff, compiler/package inventory and DLL hashes.
Non-DLL bundle content must remain identical across original and both variants.

Exact prepared `console.cc` stock SHA256:
`535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33`.
Conditional console:
`706028a7474f421a43667e55d437a2656b3a9f42d487be130b86229296721a86`.
Conditional `fhandler.h`:
`ef5f8cd8db11a874807d37661a87ac0a65a9e5bad04c6f37039bd9b814a5f3d4`.
Stock header and both termios sources remain official bytes; build/staging checks
all these hashes before accepting either runtime.

## Patch caveat: NOT fully safe

The conditional patch retires the peek prefix only inside the existing
changed-dimensions branch, before its existing unlock/signal, and returns the
original `input_winch` result without a second discard. Unchanged-size resize
and mouse call paths retain their original control flow. This avoids the older
unconditional early-return patch's demonstrated new loss at unchanged resize.

**An earlier mouse-triggered unlock may already have made the peek prefix stale.**
Retiring that stale count before notifying can consume the wrong records or
stall in discard before SIGWINCH notification. A second resize during that gap
is not proven safe. `discard_key_events` returns void and can partially fail;
signal-handler escape and read/select callers remain native regression concerns.
The reviewed patch comment is not a proof of prefix ownership. There is no claim
of unconditional losslessness, notification liveness or production suitability.
Eight simplified differential model tests are included unchanged; they do not
cover all native schedules or resolve the earlier-mouse/second-resize caveat.

## Prespecified matrix and stopping condition

PR10 is dirty against main, so GitHub suppresses pull-request workflows. The
exact experiment head remains preserved; no native trial started by that route.
The workflow therefore runs only on **push** to the exact
`experiment/msys-conditional-retirement` branch when this workflow file changes,
with an explicit push-event/exact-ref job guard. It checks out `github.sha`.
Root's one intentional workflow publication commit starts the fixed matrix
without default-branch registration or a clean synthetic PR merge. Source-only
pushes, PR events and unrelated branches do not start it. **Do not push workflow
edits to retry failures or rerun until green.** Stop after the matrix and review
all outcomes; any further run needs a separately authorized plan. Normal
`native.yml` and source/app/probe/variant semantics remain unchanged.
For **each** inbox/packaged provider, run three fresh trials in this order:
stock→conditional, conditional→stock, stock→conditional.

1. Exact unchanged PR7 raw probe: 12 total invocations. Each retains its own
   native VT-off, native VT-on and MSYS readers, four activity conditions and
   20 rounds per condition (up to 2880 rounds overall). First strict mismatch
   still fails that case; later independent cases/trials still run. No retries,
   resends, polling changes, filtering of child CPR bytes or discarded failures.
2. Actual unchanged `--smoke-settings` application: 12 fresh launches, same order,
   explicit experimental shell runtime and capture, inbox default or explicit
   qualified packaged provider. Production settings assertions and internal
   deadlines are unchanged. The settings test may itself exercise backend
   switching as before; provider label describes the initial launch selection.

Settings cases run even if raw cases fail. Evidence directories must be absent;
no overwrite or resume-until-green exists. Stop after these 24 launches. If stock
fails and conditional passes with comparable actual resize activity, that supports
only the targeted hypothesis. If stock does not reproduce, the comparison is
inconclusive. A candidate regression remains a failure and blocks promotion.
No outcome alone authorizes changing defaults.

## Bounds, ownership, evidence

Two full source builds have 40-minute limits each. Raw invocations retain the
existing 180-second outer watchdog; settings uses 30 seconds. The owned runner
waits on its exact process, never pipe EOF; kill/reap has an additional 10-second
bound. Raw and settings steps allow 42 and 9 minutes; entire job 240 minutes.
The runner does not enumerate unrelated processes or claim descendant cleanup.
Normal fixture/Session destructors retain their existing cleanup behavior;
timeout/exit/reaped/cleanup status is preserved in per-case results.

Artifacts retain every synthetic raw log, actual resize requested/applied/call
counts, completed writer bytes, exact child bytes and CPR writer records, per-case
results and executable/DLL/manifest hashes. Failed or truncated evidence is never
called success. VT-off control failures remain strict and separately reported;
they alone are not evidence of a production input defect. Build provenance and
logs are preserved. DLLs/packages, raw fixture HOME/history, private keyrings and
screenshots are excluded from uploads. These synthetic smoke logs are not user
terminal transcripts.

Local script/model/staging checks and cross-build checks are not native evidence.
No native source build or runtime execution is implied by preparing this branch.
