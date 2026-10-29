# Uninstrumented current-app comparison — root publication only

## Question and immutable app baseline

Does **stock plus0007 only**, without passive recorder instrumentation, preserve
normal current-app raw paste/resize, managed-shell/native-child and Settings
lifecycle behavior? This experiment neither promotes a runtime nor attributes
Settings leading-ESC loss solely from the earlier instrumented component pass.
Broader WAIT_SIGNALED/SA_RESTART/copied>1/error/cancellation gaps remain explicit.

App baseline: local `aaec0c4ec6357b9395b6b8c478ce8585f21b32f7`, remote
`4d65a9fa1af839fa5ac5e878700a7557850a74b2`. `app-plan.json` pins exact src/include/
assets/cmake/tests Git trees, key source hashes, unchanged PR7 raw probe, and
unchanged managed fixture. Only an OFF-by-default explicit qualification target
is added to CMake; no application code or assertions change. One current app
build is reused identically across all variants. Selected CMake cache fields and
the actual Windows app-main Ninja compile block are retained and checked for
STORMTTY_HAS_BUNDLED_SHELL_RUNTIME=1 and official-only configured source, with normal bundled-shell
support ON, seed feature OFF, normal input-evidence isolation checked.
The same build contract runs immediately after the native app build, before
either runtime source build, and again during staging. Ninja object/source paths
accept native MSVC backslashes and GNU slashes without changing or normalizing
the hashed raw bytes. Exactly one current-app source/object/compiler rule and
one enabled discovery define are required; ambiguity and instrumentation fail
closed. A bounded relevant-rule/selected-cache report is retained on success or
failure; full Ninja/cache/environment contents are not uploaded.

## Three runtime variants; two console providers

- `original`: untouched official pinned runtime, real reference where scheduled.
- `stock`: freshly compiled stock with official packaging and utility-only
  compatibility patch, GCC/G++15.3.0, GNU++17, strict warnings.
- `copied`: identical build plus only0007; copied-positive signal-exit behavior.

Official archive SHA2564b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204.
Stock console535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33;
candidate consolecf4eba2150f2b99dfe7c55c8d73ac0f578d7675e649b8468a364bfb8fe2efdb2.
Headers/termios remain unchanged. No0006 retirement patch, diagnostic macro,
observer patch, runtime exports or recorder code. Bounded PE/whole-DLL checks
reject stormtty_path diagnostics before staging/use; actual native DLL hashes,
source/diff/compiler identities and export reports are retained. No binary from
prior instrumented runs is reused.

Both providers: Windows inbox and independently signature/hash-verified pinned
packaged ConPTY. Provider binaries are also rehashed before every invocation.
Official original is never modified; staged variants and app copies have exact
runtime identities, identical non-runtime content, and explicit private receipts.

## Private default-discovery test is NOT delivery eligibility

CMake's production runtime validator receives **only the untouched official
original**. It is not weakened and no experimental receipt is submitted to it.
After compilation, new job-private qualification-only executable directories get
copies of the identical app/helper/engine binaries and variant runtimes/zsh.
The `default` rows omit --shell-runtime specifically to exercise current app
executable-relative discovery and Settings UI; all copies remain experimental,
not production installations or ordinary stager-eligible bundles. Presence-only
application discovery does not constitute identity/release validation. Receipts
explicitly forbid default activation/public redistribution. No production
lock/pin/default, normal validation rule, release/source-notice gate, user
configuration, public binary upload or candidate binary cache is changed.

## Fixed matrix — no retries or outcome-dependent changes

All three workloads continue independently even when another fails:

| Workload | Fixed launches | Existing checks / per-process bound |
|---|---:|---|
| Raw paste/resize | 18 | 3 fixed orders ×3 variants ×2 providers; unchanged probe's12 cases, up to20 rounds each;180s |
| Managed shell/native child | 12 | 2 opposing orders ×3 ×2; fresh+warm samples =24;90s |
| Current Settings lifecycle | 78 | explicit10 +default3 ×3 ×2; exact --smoke-settings path;30s |

The raw orders rotate original/stock/copied deterministically, never retry a
failure. Managed second order reverses the first. Settings rotates the same
three variants on each of the10 explicit and3 default indices. No application
sleep, resize/input queue behavior, paste byte/assertion, phase deadline or
watchdog is weakened. Reap10s and output cap1MiB remain bounded. Step envelopes
raw60min, managed24min, Settings55min cover the declared counts including failed
process waits; they do not change per-process budgets. Job330min bounds all
source builds/setup/workloads. Every planned row and original failure is kept.

A pass requires exit0, reaped process, no timeout/truncation/cleanup error and
existing exact final markers. Raw summary requires matching provider/status and
retains VT-off and VT-reader failure counts separately; expected-control failure
is still failure. Managed fresh/warm marker remains unchanged. Settings additionally
requires current success and final native-application teardown markers, not just
an early success log or exit alone. App, provider and runtime identities are
checked before each fresh invocation and after each matrix.

Uninstrumented results cannot assert internal-path coverage. No hypothetical
EINTR, mouse/master or Settings cause is promoted from a successful output row.

## Evidence, privacy and publication

New workflow `.github/workflows/msys-partial-app.yml`, exact push-only branch
`experiment/msys-partial-app`, exact github.sha checkout, contents read-only.
Root reviews and publishes; no API dispatch, PR trigger, retry or release action.
Artifacts use a bounded exact allowlist of108 log paths,3 results files, current
app/binary/source plans and source/compiler/runtime provenance. Screenshots stay
private on the runner; no DLL/executable/package/keyring/HOME/history is uploaded.
Missing files, over-cap content, incomplete matrices or identity drift fail the
collection/report; failed outcome rows themselves remain retained. Process
reaping is owned-handle evidence, not a claim of arbitrary descendant-tree
cleanup; the app/Session's existing child/job checks remain unchanged.

Local portable tests and exact source gates are not Windows execution. Native
compilation and all108 launches remain unexecuted until the new run completes.
