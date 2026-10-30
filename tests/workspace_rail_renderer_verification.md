# Workspace rail scene composition

`WorkspaceRailRenderer` composes only the workspace rail. It owns project icon
paths, focus/hover washes, selected/unread markers, expanded labels, toggle,
New workspace and Settings visuals. It is not native rasterization, navigation
interaction, a catalog or an animation owner.

## Boundary and ordering

The caller snapshots visible catalog rows in layout order, preserving stable
project IDs and owned strings. It resolves accent parsing, material alpha and OS
contrast before append. The renderer borrows a row span and rail-only DIP geometry
synchronously; it retains no references, clocks, model, native handles, callbacks
or resources. It appends owned base-layer primitive/text values and does not
mutate existing entries, pane frames or scene metadata.

The host retains the rail-width admission, missing-project skip, active lookup,
hover sampling, focus-map insertion and unread lookup in their original order.
Only unselected admitted rows query unread state. New-workspace hover is sampled
even when its glyph has no height; Settings hover remains height-guarded. Hidden
rail performs none of those lookups. Composition still precedes pane frame
acquisition and all later chrome. Geometry remains the layout used for hit tests,
hover targets, context menus and tooltips. No new hit geometry is returned.

DIP-to-pixel conversion retains double arithmetic followed by float casts. The
one-physical-pixel seam retains its original rail-width/caption-height formula;
no origin adjustment, clamping or viewport clip was introduced. The owned Settings
cog retains its existing float-scale conversion and stroke width. Background
replacement, material alpha 240, text DIP sizes, Segoe UI Variable Text, icon
fallbacks and primitive/text insertion order are unchanged.

Snapshot assembly shifts elapsed work between hover-clock lookups even though
lookup ordering is preserved. Exact output at resolved animation samples is not
proof of wall-clock sampling equivalence or physical frame pacing. Snapshot
allocation and append may throw as other scene-value assembly can; this does not
introduce a transactional or no-throw guarantee for scene composition.

## Original-reference evidence

`tests/reference/workspace_rail_original.inc` contains the literal original
`window.cpp:3596–3659` block from
`7aab01489e15d0a40d411d928a7ec7df5c663902`. It was committed at `317fba8` before
production movement. The oracle was **not executed before extraction**. Its test
fixture supplies the old host environment and independent emission arithmetic;
it does not call the new renderer's private helpers. It continues to call the
pre-existing shared Settings icon routine, which was not changed.

The standalone C++20 matrix has 12,288 cases and 1,809,437 checks. It covers four
DPI scales (100/125/150/200%), six widths, eight heights including control-admission
thresholds, expanded/compact/hidden rails, scrolled/reordered stable IDs, missing
catalog entries, all project icon branches and fallback, Unicode/empty names,
valid/invalid accent fallback, selected/unselected unread states, intermediate
focus/hover, distinct contrast-background/navigation colors, light/dark inputs and material
alpha. Synthetic non-origin rails and direct seam/surface assertions retain the
legacy seam origin even when it differs from the rail origin. Every primitive
and text field, stable paint order and preserved clipped/modal entries, metadata
and owned frame reference are compared.

`workspace_rail_host_characterization.py` extracts the **actual current host
sampling block** from `window.cpp`, compiles it into the same traced test
environment and compares it with the frozen original block. It verifies output
and catalog/active/hover/focus/watch lookup sequence across the matrix, plus
explicit missing-project, initially absent focus-transition, selected-unread,
zero-height-control and hidden-rail cases: 3,598,531 checks. This runs real source
from the host block, but not `NativeWindow`, its real catalog, Win32 or native
callbacks. The fixture's animation/attention values are deterministic.

Reproduce without a full application build:

```sh
g++ -std=c++20 -O0 -Wall -Wextra -Werror -Iinclude \
  tests/workspace_rail_renderer_tests.cpp src/core/workspace_rail_renderer.cpp \
  -o /existing/workspace-cache/rail-tests
/existing/workspace-cache/rail-tests
python3 tests/workspace_rail_host_characterization.py \
  --output-dir /existing/workspace-cache/rail-host
```

The ordinary renderer test is registered with CTest. The host-fragment test is a
standalone compiler harness, not a native integration test; `--compiler` selects
an existing GNU-compatible C++20 compiler.

Strict existing Windows GNU Clang cc1 `-Werror -fsyntax-only` checks passed for
the new renderer, fixture and full changed Windows host. Sparse-checkout missing
headers initially prevented the fixture/host syntax checks; materializing those
existing tracked headers resolved them. These checks are not Windows execution.

## Native qualification and remaining limits

PR #44 head `b606156a` passed the Windows baseline **71/71** in
[native run 41115202828](https://github.com/StormBurpee/stormtty/actions/runs/41115202828).
The retained Windows baseline artifact is `13004838852`. Its JUnit result records
71 tests, zero failures and zero skipped tests; `workspace_rail_renderer_tests`
executed natively and passed the 12,288-case / 1,809,437-check emission matrix.
This executes both frozen original and extracted emission after extraction; it
does **not** establish original native execution before extraction.

The same baseline passed the existing native workspace-frame, focus,
collapsed/single/narrow/high-contrast, watch/activity, navigator and
previous-terminal scenarios. Retained native logs record the controlled-time
pane-focus checks and Settings owned-monochrome-cog/shared-hit-geometry checks.
Renderer/resource/cache/PNG and final teardown gates also passed. These are
existing native scenario contracts, not new physical UI or timing measurements.
Linux and macOS jobs passed in the same run; they do not establish macOS rail UI
parity or Windows physical behavior.

The completed Windows tail passed **15/15 application cases**: all five packaged,
all five bundled-zsh and all five developer-zsh scenarios. Settings repeats passed
**8/10 explicit and 3/3 default** cases. Explicit cases **9 and 10 failed** with
return code 2: retained terminal rows show malformed `[200~` paste input, a
trailing `~` and `zsh: = not found`, followed by the internal phase-2 smoke
deadline. Both failures were reaped, with `timed_out=false` and no cleanup error;
that is successful cleanup, not successful scenarios. All 13 repeat receipts
recorded reaping without an outer timeout or cleanup error.

The Windows job therefore failed despite its green baseline and application
cases. This records the observed shell failure, not a shell fix or a common-root-
cause determination. No fully green Windows run, mixed-monitor physical DPI,
wall-clock sampling equivalence, presentation smoothness, physical frame pacing
or performance result is claimed.

This completes the rail composition boundary only. Header/Ribbon owners are not
rebuilt; pane-frame acquisition, typing observation, find/palette/navigator/modal
composition and host orchestration remain separate work. No ordinary blur policy
or paused AI, Liquid Glass/HDR or Leyline feature is changed or activated.
