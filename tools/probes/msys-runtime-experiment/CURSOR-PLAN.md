# Private modern cursor candidate — not production-qualified

## Hypothesis and exact change

Run 40746143302 at head 6388fcb4b2a5ae719b140a6c60d0bea8dca338c0 captured
both providers' final instrumented prefixes: set_cursor_maybe's fillin changed
the shared cached size 111×35→100×30, then reader resize events compared the
already-current baseline and emitted no new group request. Earlier successful
phases had changed comparisons and mapped self-target send attempts. This
supports a narrow baseline-clobber hypothesis; it does not establish every
failure's exact assertion or signal delivery. Instrumentation perturbs timing.

Candidate 0003 moves only the existing set_cursor_maybe con.fillin call below
its existing modern-console no-op early return. The predicate remains exactly
`wincap.has_con_24bit_colors () && !con_is_legacy`. Other callsites, event
consumption, signal routing and legacy cursor behavior are untouched. This
avoids refreshing the resize baseline on that no-op path, but native execution
must test whether other consumers rely on the incidental cache refresh.

Both control and candidate are UNINSTRUMENTED. Neither applies failed
0001-retire-resize-before-signal.patch or 0002-resize-observation-trace.patch.
No production runtime pin, active bundle, release or source redistribution
approval changes. Private candidate packages/DLLs stay ephemeral.

## Fixed provenance

Exact complete official msys2-runtime-3.6.11-4.src.tar.zst SHA256
`4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204`;
all official packaging patches; identical common utility-only const patch;
private GCC 15.3.0 and explicit GNU++17 with no trace macro. Host compiler is
not downgraded. Build/staging safety, compiler/package inventories, exact
source hashes and DLL identities follow the prior diagnostic contract.

Control console SHA256
`535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33`;
candidate console SHA256
`25a3fb9355235614819f4c56e2a60c8ecb3b4bd4cd49a6befa7ff56ab03cc0b3`.
Termios and fhandler header remain exact control bytes. Generator rejects any
other source input; tests verify the patch changes nothing outside one block.

## Independent workloads

One immutable probe/fixture across variants and inbox/packaged providers:

1. Existing one-shot initial idle resize.
2. Existing normal strict 50-phase qualification, unchanged default semantics.
3. Explicit builtin-only one-shot 50-phase workload, no external/native jobs
   before or between resizes, no stale-size polling or resize resends.

Normal strict qualification already polls completed stale size responses via
fresh observation commands (10s deadline, 20ms intercheck delay); lost/altered
commands fail rather than being retried. Do not describe this old workload as
one-shot. Native helper follows, never substitutes for shell size evidence.
The additional workload is explicitly stricter and separate, not a weakening
or retrospective replacement. No forced SIGWINCH, retry-until-green run,
artificial foreground job or added sleep is a proposed runtime fix.

Bounded whitelisted numeric outcome metadata records code, phase, expected
and observed grids before diagnostics, without input commands or screen text.
Missing/duplicate/invalid summary must fail evidence validation. Workloads
remain independent so one failure does not suppress the others.

## Verification and authorization boundary

Local deterministic patch/source checks, 20 staging safety tests and a portable
C++ model covering 8 modern/legacy/moved-cursor combinations are not native
ABI or Windows qualification. Cross-compilation is not runtime evidence.
Root owns review, remote publication and native CI launch. Two source builds
previously cost roughly 28 minutes; no native run is authorized by this file.
A pass is candidate evidence, not permission to replace a release runtime.
