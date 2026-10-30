# Window header scene composition

`WindowHeaderRenderer` composes only the crown/caption controls and tab strip.
It is a portable scene emitter, not `windows::ChromePainter` rasterization or a
native window/device owner. Geometry, hit testing, actor/model identity, watch
lookup, material policy, animation state and timers remain in `NativeWindow`.

## Immutable synchronous presentation

The host builds `HeaderTabPresentation` values from the current `ChromeTab`
geometry and stable tab IDs. Each value owns its label and copies watch/selected
state plus sampled selection/hover amounts. `WindowHeaderView` borrows only the
portable `ChromeLayout` and a span of those values for one append call. It stores
no model, session, renderer, HWND, callback or clock. Neither layout nor tab/span
references escape the call; outputs copy their own text and geometry.

Host sampling retains the `tab_transitions_[id]` insertion and normal sampled
hover lookup order. The composer never advances or creates a transition and has
no timer or invalidation authority. `hover_amount` still samples the host clock
per lookup; moving emission after snapshot construction changes elapsed work
between lookups. The oracle establishes exact emission for resolved samples, not
wall-clock sampling/physical frame-pacing equivalence. Native animation checks
remain required. Fullscreen admission remains in the host.
The original local `chrome.a = 230` backdrop decision remains inside the host's
non-fullscreen block because later scene surfaces reuse that local color.

## Emission and geometry contract

- Input rectangles are DIPs; conversion to physical pixels occurs once on append.
  One-/two-physical-pixel seams/selection strips retain the original formulas.
- Emit crown/tabs background replacement, seams, title, button hover, zoom return,
  tab visuals/labels, menu/new-tab/palette/focus marks and caption buttons in their
  existing order. Primitive/text insertion and stable shared paint order matter.
- Header output stays on the base layer. The old header added **no viewport clips**;
  this extraction adds none. Do not introduce tab/header clipping or normalize
  zero-width geometry merely because overlaps appear unusual. Native target and
  scene damage clipping remain backend responsibilities.
- Header geometry is derived from the same layout used by native hit testing;
  `layout_tab_label` and `layout_window_title` remain the existing shared helpers.
- Existing scene primitives, labels, panes, frame references and metadata survive
  append unchanged. Closing overlays, typing observation/timers and other scene
  mutations remain in their original host functions.

## Characterization and qualification

`window_header_renderer_tests` contains the original `aea9609` crown/tab emission
as an independent oracle with only host lookups replaced by resolved values.
It does not call the new renderer's private helpers. It compares every primitive
and text field, stable paint order and preservation of existing clipped/modal
scene entries, pane state and an owned immutable frame reference.

The matrix has 28,672 cases: four DPI scales (100/125/150/200%), seven widths,
four tab-count inputs and 256 presentation combinations. It covers compact and
ordinary labels, zero/narrow geometry, reordered stable IDs, Unicode/empty labels,
watch states, residual/intermediate hover and selection, zoom return, focus mode,
maximized glyphs, high contrast and resolved material alpha.

Standalone portable C++20 tests and strict Windows cc1 checks are not native UI
execution. Require the Windows build and header/tab/hierarchy/watch/focus/Settings
preview scenarios, plus renderer/cache/scope/PNG and final renderer teardown gates.
Physical Snap/DWM/ordinary blur and mixed-monitor inspection remain separate.
No macOS header UI or paused AI/Liquid Glass/HDR/Leyline feature is introduced.
