# Isolated native qualification recipe — not dispatched

Root owns review, publication and CI. This document does not install a runtime,
modify a workflow, authorize promotion, or silently reuse a prior gate acceptance.

## Before publication

1. Independently review SOURCE-REVIEW.md and the sole hunk against locked stock
   console source. Run `PARTIAL_READ_SOURCE=/path/to/console.cc.before python
   tools/probes/msys-runtime-experiment/partial-read/tests.py`; all tests,
   including the exact patch gate, must pass. Do not accept a skipped source gate.
2. Prepare a NEW private build recipe/identity set: exact archive/GCC15/GNU++17,
   same strict warnings and common utility-only compatibility patch as prior
   qualification. Two behavior variants only: unchanged stock and stock plus0007.
   No0006 refined patch. Record both source hashes, every applied diff, DLL hash,
   compiler/package inventory, fixtures and executable imports. Never overwrite
   original MSYS or the user's bundle; verify original identity before/after.
3. Do not route the new variant through old stock/refined identity checks or
   alter those checks. If retaining passive hooks, generate separately pinned
   observer patches for the new before/after source and independently review
   actual runtime recorder objects and linked errno/handler/emitter gates. The
   previous linked report cannot authorize a differently hashed DLL. Linkage
   drift fails before execution; preserve all failed evidence.

## Fixed primary experiment (one execution; no retries)

Retain the existing eight controller/reader trials, both cases and both orders,
with the same ready marker, queue hint, geometry change, input sequence, alarms,
watchdogs, output caps and cleanup. Replace only the compared behavior candidate:
stock versus stock+0007. Retain observer numeric source placement/accounting.
The fixed matrix can test byte recovery after the now-observed resize dispatch;
it must NOT be relabelled as observed mouse/master coverage. Preserve existing
full-path predicates and INCONCLUSIVE outcomes. Score byte results independently:

- Stock is expected to retain copied1→-1/EINTR and alarm, not "expected-pass".
- Candidate must return positive1 for interrupted first read, then exactly the
  remaining synthetic bytes, with final exact `ab` or `abc`, no duplication,
  residue, alarm, cleanup failure or hidden read. Do not infer bytes from counts.
- All rows are collected even on failure. The prior mouse/master workflow and
  its aggregate remain unchanged. The separately named resize-component workflow
  retains legacy coverage informationally and requires its own strict resize
  predicate plus exact bytes and cleanup; stock byte failures remain exit1 and
  missing component evidence exit2. No changed delay, input retry, timing gate,
  resize suppression or extra bytes to force this expectation.

## Separately predeclared semantic regressions

Before a wider candidate decision, add fixed bounded cases in a new reviewed
fixture (not retrospective relabelling of the above eight):

- Non-SA_RESTART interruption before any copy: -1/EINTR unchanged.
- Positive partial read for requested2/VMIN2 and requested64/VMIN2: short count
  accurately reports existing prefix, subsequent exact tail, no duplication.
- SA_RESTART input signal with zero and positive copy: existing continuation,
  final exact bytes; validate signal delivery, not only process exit.
- WAIT_SIGNALED ingress versus input_winch ingress: require actual trace evidence
  for each; missing scheduling coverage is INCONCLUSIVE, never assumed.
- No-signal success; canonical delimiter and raw VMIN1; existing cancellation and
  unrelated error results. Any unavailable deterministic fault case is explicitly
  untested, not silently counted as passed. All children kill-bounded and reaped.

Only after these reviews can a separately authorized application matrix compare
unchanged production Settings/paste/resize gates. Even a synthetic byte pass
would not establish Settings leading-ESC causality or replace prior refinement
qualification. No actual user command, Codex, SSH, production bundle/default or
release is involved in this proposal. Original failures remain immutable.
