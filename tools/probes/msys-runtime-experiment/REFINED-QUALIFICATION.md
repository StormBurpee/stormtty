# Fixed refined-prefix native qualification plan

This source-only candidate does not change production runtime pins, defaults,
backend selection or public release status. Root owns publication. New push-only
workflow: `experiment/msys-refined-retirement`, triggered only by its own workflow
file, exact pushed commit checkout, no PR merge or dispatch/retry-until-green.
Production `native.yml` remains untouched. The original conditional experiment
and successful alternative-PTY prototype remain preserved independently.

## Why this candidate, not the previous conditional patch

The exact refined patch is `0006-refined-prefix.patch`, SHA-256
`ceb70e9c04be478e051f2be891409e4d7e4ad30c8654141c86f520f165b70693`.
`REFINED-SOURCE-REVIEW.md` preserves the source-control-flow review and remaining
caveats; `refined-model-tests.py` contains its 13 simplified tests, not proof of
native behavior. The patch retires only already-traversed records (`i`, not the
current event) on changed-size mouse/resize, before unlock/signal, and abandons
that borrowed peek. Zero prefix is guarded because discard(0) has other meaning.
Mouse returns existing `stat`, never newly introduces `input_winch`/EINTR after
raw bytes were already copied. Resize keeps its existing `input_winch` result.

The prior conditional variant still allowed an earlier mouse-triggered unlock
to stale the later peek count. Refined addresses that particular gap but still
has inherited partial-discard/competing-consumer risks and a new head-mouse
liveness assumption: eventual stable geometry. Short-read batching may change.
Do not label it unconditionally equivalent, safe, or production qualified.

Directly re-inspected original run40817519570 preserved evidence: 12 raw cases,
stock target VT-on failures6/6 vs conditional0/6; VT-off controls fail4 per case
for both; all12 settings-close launches pass. No timeout/unreaped raw/settings
case. The refined candidate is different and cannot inherit that pass.

## Exact source/build isolation

Two builds only: stock and refined. Official complete runtime3.6.11-4 archive:
SHA-256 `4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204`,
128907415 bytes. Same official packaging patches and common utility-only const
compatibility; no cursor/trace/other behavioral patch. Same official signed,
hash-pinned GCC15.3.0 private prefix, GNU++17, strict warnings and makepkg
integrity checks. Build pre/post package inventory must match for both variants.

Exact source identities are checked independently by build, staging and runner:
stock console `535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33`;
refined console `c6ca2d2d9e72c38f6c698c7c5b3288f935ef16a8d7a52d79391ba54339a52b8d`.
Full header/termios hashes remain in stage-refined.py. Private runtime copies
retain all identical non-DLL bundle bytes; no active DLL replaced. Runtime and
source manifests mark qualification-only and forbid default activation.

## Prespecified matrix; failures never suppress independent later workloads

1. **External native edge diagnostics:** 6 scenarios × 2 paired orders × 2
   variants =24 fresh launches. raw VMIN=2, select, canonical, mouse reporting,
   surrogate pair and guarded longjmp. Exact scenarios/limits in
   `refined-native/PLAN.md`. Reader compiled once with verified GCC15; injector
   compiled once with MSVC. Reader imports must be MSYS and explicit Windows
   system DLL allowlist only; no unpinned library injection.
2. **Full shipped managed shell through real Session/ConPTY:** inbox and packaged,
   each with stock→refined then refined→stock:8 invocations. Each invocation
   runs one fresh and one warm session in private native-validated state. Actual
   unchanged shipped loader/addons/prompt, additive ZLE observer, UTF8 helper,
   Unicode/4096-character bracketed paste, edit/resize, native console child
  132x43→148x47, Ctrl-C130, shell exit37 and completion-cache reuse are required.
   Real Session owns Ghostty feedback; no synthesized DSR replies or fabricated
   CPR counters. This is a fixed synthetic native child, not arbitrary native
   program/SSH/editor interoperability qualification. No real user commands.
3. **Unchanged raw probe:** 3 paired trials per provider,12 invocations, same
   PR7 source SHA-256
   `251477c3587c25b498bf1bf099e355d47a2cf3e1b8f1dcd7649e406aca39beae`.
   Preserve every VT-off control failure; no filtering/resend/classifier edits.
4. **Actual unchanged settings-close application:** same12 paired invocations,
   same production app executable and internal assertions. Initial provider
   label is not a promise that existing settings smoke never switches provider.

Stock→refined, refined→stock order is fixed before outcomes, not randomized or
chosen after failures. All logs/results are retained. A failed case is not
rerun. If stock does not reproduce, targeted improvement is inconclusive. Any
new candidate regression blocks promotion. Expected VT-off failures still make
the raw step fail: do not turn this experiment green by weakening its predicate.

## Honest internal-path and fixture limitations

External queue-empty observations/markers do NOT prove `copied_chars=1`, which
mouse/resize callsite sent SIGWINCH, exact peek indexes or retirement success.
Every native edge result therefore keeps `exact_path_verdict=inconclusive` even
when exact bytes match. There are no diagnostic runtime hooks in this candidate.
A future separately reviewed paired hook experiment would be needed to close
those internal coverage gaps. The longjmp guard includes small before/after-call
windows; signal escape there can affect the fixture itself. Mutable termios state
uses static storage across siglongjmp, and the limitation is documented rather
than claiming a pure runtime regression from an ambiguous failure.

## Bounds and publication

Source builds40min each, overall job300min. External reader8s/injector10s/
outer20s plus explicit5s reap,12min step for24 launches. Managed host50s work plus bounded8s session close; outer90s/reap10s;
8 invocations fit16min step. Raw outer180s/reap10s and42min step, settings30s/
reap10s and9min step. Finite ownership/timeout/error metadata remains strict.
The standalone outer runner's process reap is not alone descendant-cleanup proof;
Session and injector own their respective Windows Jobs. Managed scratch cleanup
is checked before successful aggregate output. No private HOME/history/screens,
keyrings, DLLs, executable/object files or source archive binaries are uploaded.
Preserve exact source/patch/provenance, numeric diagnostics and fixed fixture logs.

No source-license obligation is waived by these tests. Before any modified DLL
publication, regenerate the existing matching source/notice packet with complete
original source, exact experimental patch, recipe/compiler/link provenance and
modification notice. Preserve runtime LGPL/linking exception, applicable utility
GPL/Newlib/other notices and user's replacement rights. Do not relicense the
original native app or claim release legal qualification. A passing matrix only
supports its observed cases; production promotion remains a separate decision.

## Local validation (not native execution)

Windows-GNU cross-link passed for the actual managed Session target, unchanged
raw paste probe, external injector and native console fixture. Reader passes
Linux C syntax and config passes Zsh syntax. Sixty-nine portable tests pass: 9 fixed
raw/settings orchestration, 2 identity, 20 staging, 13 control-flow model,
11 external-log/owned-completion checks, 7 workflow/managed negative-outcome/import
tests and 7 owned-process tests.
Workflow YAML and embedded Bash parse, and git diff whitespace checks pass.
No native MSYS runtime source build, MSVC fixture execution or Windows matrix
has run for this refined candidate. Those remain the purpose of the new CI run.

### Pre-publication bounds correction

The inherited native edge runner used subprocess.run(timeout=20), whose timeout
cleanup can wait without an explicit reap deadline. It now uses the existing
owned runner with20s execution plus5s reap and64KiB bounded output, preserving
timeout/reap/truncation/cleanup metadata and failing incomplete outcomes. Its
scoped child environment is passed directly to Popen, never installed into the
parent environment. The24-case matrix and runtime patch are unchanged. The
step budget is12min, not10min, to include all24 worst-case25s invocations plus
setup. No internal-path qualification claim changes.
