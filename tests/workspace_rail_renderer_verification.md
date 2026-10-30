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

## Remaining qualification and scope

Windows runtime qualification remains required for this source: the native
project/focus/watch rail scenarios, collapsed/expanded/minimum workspace-frame
captures, Settings cog/theme checks, renderer/cache/PNG and teardown gates. No
native runtime, full-suite, macOS UI, mixed-monitor physical DPI, presentation
smoothness or performance result is claimed here. Existing shell qualification
failures remain separate and must not be relabeled as success.

This completes the rail composition boundary only. Header/Ribbon owners are not
rebuilt; pane-frame acquisition, typing observation, find/palette/navigator/modal
composition and host orchestration remain separate work. No ordinary blur policy
or paused AI, Liquid Glass/HDR or Leyline feature is changed or activated.
