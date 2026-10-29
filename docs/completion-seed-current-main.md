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
