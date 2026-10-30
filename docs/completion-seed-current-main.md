# Completion-seed integration on current application main

Base: local `a93930c` (prepared remote `560eefd`, tree
`13438098e91eb26ceba3158a3ad8bc07594d8c14`), incorporating the native-qualified theme gallery/compact Settings
from run `40888253285`, workspace navigator, overlay/input ownership, paste
handling and capture-smoke child helpers. The first integration used `af58593`;
it was refreshed before current-app native qualification rather than publishing
a stale application base.
The integrated seed source is the native-qualified diagnostic through `a222254`
(remote qualification `1ea8c4f`, run `40883576212`), plus its retained evidence.
No runtime pin, public redistribution/license gate or default activation changes.
`STORMTTY_EXPERIMENTAL_COMPLETION_SEED` remains **OFF**.

## Narrow application integration

The current `window.cpp` is preserved. Its entire change is a guarded include and
one guarded initialization assignment in the existing managed-session factory,
after private-state validation. Every ON managed launch participates, including
warm/custom state; non-managed profiles and OFF launches do not attach a planner.
The seed is regenerated from the exact final native runtime before compilation,
and its identity is compiled into the application. No old anchor or executable
seed is copied across source trees.

## OFF-path review

OFF is not described as byte-identical source or binary behavior:

- `ProcessOptions` gains an optional Windows initialization object and process
  diagnostics gain a counter. With no object, preparation/release are skipped and
  the controller still waits on the same child/control handles, without polling.
- The managed launcher clears two reserved initialization environment values, so
  ambient values cannot activate the gate. The preset consumes them; absent an
  event, it starts no additional readiness helper. Ordinary audited compinit stays
  selected when no-dump is not requested. Module order, user configuration,
  history and custom completion ownership stay unchanged.
- Native private-state journal code is compiled but not called by the OFF factory.
  Seed packaging/generation and trust-header compilation are absent from OFF.
- The final staged runtime is now created with a protected concrete
  user/SYSTEM/Administrators DACL, replacing inherited Python OWNER RIGHTS on the
  newly owned output only. This intentional build-staging change passed native
  renamed-root/child/collision security tests. No existing source/ancestor ACL is
  changed or repaired. Ordinary OFF installation still uses its existing source
  layout; no seed package is installed.
- The console helper adds a strictly parsed completion-ready mode; its no-argument
  UTF-8 console setup remains unchanged. No system-wide console changes occur.

## Exact native gate before main publication

The root-owned diagnostic branch route is unchanged: intentional exact-branch
source publication only, no main/PR/dispatch trigger or blind retry.

1. Stage current pinned sources/provider once; run packaging and native ACL
   regressions. Restore/build the exact pinned engine once.
2. Configure a fresh `build-default-off` **without specifying the seed option**.
   Build all default targets, including current child helpers, and run the full
   native CTest suite with its existing tests and timeouts.
3. The owned OFF driver verifies `CMakeCache` says OFF and that no seed package or
   generated trust directory exists. Exercise actual app layout/modal/projects/
   settings smokes (four independently reaped 120-second bounds), then actual
   managed-state/history/custom-config/editing tests on inbox and packaged
   providers (two independently reaped 180-second bounds). Preserve authored
   screenshots plus numeric status only. A 900-second outer bound prevents hangs.
4. Configure a distinct ON build against the same current runtime/engine. Generate
   a new seed/compiled anchor, prove no-op/header repair, relocate/install, check
   full identity and execute the unchanged crash/journal/ACL lifecycle gate,
   installed-app smoke, eager first-Tab/module checks and all five bounded timing
   conditions with required numeric phase rows.
5. Always retain evidence on failure. An OFF failure blocks ON qualification;
   neither previous branch results nor local cross-compilation substitute for
   success of this exact integrated tree. Default enablement remains a separate
   decision, including warm tradeoff and retained-uncertainty recovery policy.

## Local evidence and limits

The complete Linux build succeeded. Its first CTest invocation ran while the
workspace filesystem was full: four tests reported explicit NoSpaceLeft. After
removing only this integration's partial Windows output and freeing space, the
complete suite passed **26/26**. Original failed and successful logs are retained
separately. This is an environment recovery, not a hidden test fix or native pass.
Current `window.cpp` cross-compiles with both OFF and ON under strict Windows GNU
warnings; native execution remains pending. Final combined cross/test results are
recorded in the integration handoff, not inferred from older app binaries.

Final local checks: **26/26 Linux/core tests passed** after disk recovery; full
Windows GNU default-OFF application and test-target build completed **131/131**,
including final app linkage. The current app window also compiled separately with
ON for guarded-factory coverage (not seed generation or execution). Package,
staging, parser/route and POSIX completion suites passed **59 checks** with the
Windows-only ACL reader skipped; pinned-module managed-shell tests passed
**42/42**, and shell-bootstrap tests passed **22/22**. No ON native timing or OFF
native UI claim is made for this new integration before its workflow runs.

The earlier failed cross-build was explicitly NoSpaceLeft, preserved in its own
log. The successful build used a fresh owned build directory and existing shared
Zig compiler cache, without reconfiguring another worker's build. After validation,
only its reproducible object files were removed to recover shared disk space;
source, logs, linked executables and shared engine/compiler cache were retained.


## Current gallery refresh

The refresh to `a93930c` merged without conflicts. Compared with that base,
`window.cpp` still differs only by the guarded seed include and factory assignment.
Gallery/compact Settings, workspace-navigator geometry and their test sources are
byte-identical to the new main. No native-craft workflow, UI assets, capture or
paste source changes were introduced by the seed integration. The changed
Settings and geometry tests were rerun locally, and the refreshed native window
was cross-compiled with both OFF and ON. The earlier full 26-test Linux suite and
131-target Windows cross-build belong to the preceding `af58593` integration;
they are not relabeled as full runs of the refreshed tree. The forthcoming exact
native OFF/ON workflow is the comprehensive integrated gate.

Refresh checks passed: Settings panel **37,693 assertions**, window geometry/input/managed-profile tests, and all **13** seed parser/workflow checks. Both refreshed-window Windows OFF/ON translation units compiled with strict warnings.

## Current-app OFF qualification failure (2026-10-29)

Run `40890835384` on published `923ca8abb8c3fa6d682f6b7b32449bb517c2bbd3`
(tree `1c88b08fc7758c015999be1e72631a4a39d58911`, local `1941729`)
completed **failure**, not a qualification pass. Native default-OFF all-target
build and all 43 CTests passed. Four subsequent UI smoke processes exited zero
and were reaped; layout/projects/settings exceeded the new driver's 8 KiB
capture limit and were correctly rejected as truncated. The driver now uses
its existing ON counterpart's bounded 64 KiB limit, still rejecting truncation.

Inbox managed state passed phase 11, 508 checks, cold 5307 ms/warm 741 ms.
Packaged managed state exited 1; its emitted typing counters were zero. The
initial driver incorrectly filtered out the numeric failure phase/check count;
the owned runner's temporary raw capture was closed, so that information is
not recoverable from this artifact. No provider failure cause is asserted.
The sanitizer now preserves exact numeric failure and contract records, without
retaining shell/config/history contents. This is a diagnostic correction, not
a claimed packaged-provider fix. ON build/lifecycle/timings were skipped and
remain unqualified on the current integration. Do not retry the unchanged head.

Artifact `12904644835` SHA256
`e57946800d75e82cdb125c5ea9f819a2edf8776a46a1365afc4875e933d7b63c`
is preserved locally under `/workspace/shared/seed-evidence-40890835384`.
Default remains OFF; root controls any corrected-head publication.

## Corrected current-app qualification passed (2026-10-29)

Run `40893899530` completed **success** on published
`50125b8f39fb5f0ca83910e005c7c87945fa2fcb`, exact tree
`3d70b2219425fffd0daf04310b144b2dbc0040a1` (local `4db1525`).
This qualifies the gallery-era current-app integration with default OFF, not
later main footer/README changes and not activation by default. The prior
packaged failure remains unexplained; a later pass is not a diagnosis.

Default-OFF all-target build, 43 native CTests, four current UI smoke captures,
and managed inbox/packaged state tests passed. Both providers passed all ten
state contracts and typing checks (529/542 checks, respectively). Explicit ON
build, no-op repair/install, installed app, lifecycle, journal sole-handle
process-crash persistence, retained prior journal, contention, config
preservation, native ack, abandoned-owner no-dump, public runtime ACL rejection
and all five timing conditions passed. All nine fresh candidate samples
adopted; all nine warm samples skipped adoption.

Installed totals (three fresh and three warm samples per condition):

| Provider | Fresh baseline range | Fresh seeded range | Warm baseline median | Warm seeded median |
| --- | ---: | ---: | ---: | ---: |
| Inbox | 4942–4963 ms | 2027–2073 ms | 707 ms | 826 ms |
| Packaged | 4925–4975 ms | 1976–2072 ms | 703 ms | 802 ms |

Installed warm preparation medians were 7.07/6.96 ms (inbox/packaged),
journal create/write 0.275/0.273 ms, flush 5.21/5.14 ms and ready cleanup
0.336/0.324 ms. End-preparation-to-native-ready medians 731.8/730.5 ms include
child creation, shell and helper dispatch; they do not isolate helper cost.
These same-run samples demonstrate fresh benefit and warm overhead, not that
journal durability causes that overhead. Preserve durability and compaudit;
default activation requires root's explicit decision and recovery tradeoff.

Artifact `12907920670` SHA256
`cbcd0710cc382cf1668593938a8c686823d8cff86954c2b8c4b46c8a7c5e8cb0`
is retained under `/workspace/shared/seed-evidence-40893899530`.
Build/install identity assertions passed, including:
- application: `e424dd653f732a314d96e04a4c3a6363504904dc622a1dc200a0a321a5de8df3`
- runtime manifest: `2b28817c1bb43cbfffbfee16a4162f636728682b1578678e2dd42ce157fd1854`
- seed identity: `4385058824c2b053d86e8bd8b94ce6b4bd071fd3a0257d6f01263f7d73ffcbba`
- seed text: `361846b3c44f1db8e82799fb28fa3bf0ae90971621a7ed6a5fd06ef3fa0d7ec7`


## Current-main requalification candidate (2026-10-30)

Prepared against reviewed palette-scroll commit
`f721db87aa51dd2e4fc0a10e6678947a8fd4f015`, tree
`45275e9e595ebd46a57ab96820b758c021bd5294`, on local main
`1bebd53dbe3e6064e1b5197caf465addbf89e8cf`. Both upcoming native OFF and ON
builds include that exact finite Settings/palette scrolling and clipped pixel
viewport implementation; it is not replaced with the older gallery UI.
The gallery-era run above does not qualify this palette change or later workspace
ribbon, pane context, renderer, or native test changes. Relative to the reviewed
palette base, this refresh changes only workflow source-tree evidence, workflow
regression contracts, and documentation. Production code, pins, seed policy, default OFF,
security checks, sample counts, deadlines, and all existing native gates remain
unchanged. No seed or trust header from the historical run is reused.

The existing exact `diagnostic/completion-seed-shipping` push route is ready for
root-controlled publication of the reviewed tree. It stages the pinned runtime
and provider once; the OFF and ON builds share compiler/build configuration and
engine/runtime inputs. The timing matrix deliberately retains one identical
ON-built measurement executable for baseline (planner absent) and candidate
(planner attached), removing a binary/build confound. The separate fresh
unspecified-option build proves the actual default remains OFF and exercises
full native CTest, current UI, state, history, customization and typing behavior.

Warm state still participates in the planner. Skipping seed adoption is not
permission to bypass the lease/journal/shared-cache concurrency protocol. A
~100 ms historical warm penalty is a reviewable tradeoff, not justification to
remove durability, compaudit, eager first-Tab or plugin coverage.

Before a separate default-activation decision, require a successful run on the
exact published commit/tree with complete OFF regressions, current ON installed
identity/no-op repair, all lifecycle/ACL/journal/crash/contention checks, all five
timing conditions and numeric phase rows, eager completion/module checks,
fresh adoption and warm non-adoption. Compare all three fresh and warm samples
per condition/provider, including preparation in total time; report distributions
and the warm penalty rather than selecting the best sample. Root must explicitly
accept the measured tradeoff and retained-uncertainty recovery policy. A failed
or missing gate blocks qualification; no repeated unchanged runs until green.
Any later production-source change needs its own exact-tree native evidence.
None of these CI fixtures measures the user's installed executable or machine.

Native results for this refresh are **pending**. Local portable checks are not
native performance, native ACL/lifecycle, or physical UI qualification.
