# Ribbon domain extraction verification

This is the first renderer-domain slice, not a completed renderer refactor.
`RibbonRenderer` emits base-layer chrome into an existing `WindowScene` from
resolved layout, content, activity and style values. It owns no native graphics,
actor identity, hit testing, configuration, timer or workspace mutation.

The Windows host still resolves the active session and activity summary, applies
feature/layout gates, chooses system/theme/material colors and measures the final
activity label using DirectWrite. A one-physical-pixel seam and the original
footer alpha are preserved. Ordinary blur is unchanged. AI, Liquid Glass/HDR and
Leyline work is not activated by this extraction.

## Portable characterization

`ribbon_renderer_tests` compares every primitive and text field, insertion order
and shared chrome paint order against the pre-extraction footer emission from
`eb319418`. The oracle substitutes resolved host values for host lookups; it does
not call the new painter or its label-geometry helper. Coverage comprises 14,336
cases: four DPI scales, seven widths (including zero and marker/inset boundaries)
and 512 state combinations. It includes absent bindings, empty/nonempty Unicode
Git labels, detached/subdued/failure states, clock/agent admission, activity
attention/running indicators, hover and resolved material/contrast surfaces.
Existing scene contents and metadata must survive append unchanged.

The activity label's text is deliberately supplied rather than measured by this
portable test. Its independently computed available width is checked exactly.
Native font metrics and host input behavior require the native gates below.

Small standalone reproduction, without Ghostty linkage or a native renderer:

```sh
mkdir -p .ribbon-check
TMPDIR="$PWD/.ribbon-check" c++ -std=c++20 -O1 \
  -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests \
  tests/ribbon_renderer_tests.cpp src/core/ribbon_renderer.cpp \
  -o .ribbon-check/ribbon_renderer_tests
.ribbon-check/ribbon_renderer_tests
```

Expected output: `ribbon renderer: 597248 checks passed`.
The target is also registered in CMake/CTest as `ribbon_renderer_tests` and links
`stormtty_workspace`, alongside the existing geometry and semantic-color tests.

## Local result (2026-10-30)

- Standalone C++20 `-O1 -Wall -Wextra -Wpedantic -Werror`: passed,
  597,248 checks.
- CMake Release with `CMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG`, application off,
  existing pinned `/workspace/shared/build-cache/ghostty-linux-out`: built
  `ribbon_renderer_tests`, `window_geometry_tests`, `ui_colors_tests` without
  warnings. CTest selecting exactly these targets: **3/3 passed**.
- `git diff --check`: passed.
- Native Windows/macOS build/runtime and physical UI checks: **not run here**.
  These portable checks are not a whole-application or native qualification.

Private reproducible build outputs are not part of the deliverable.

## Native acceptance still required

- Windows application compile and native ribbon/window smoke: clock, Git,
  directory and status; narrow windows; activity/watch indicators; pointer hit
  targets unchanged; Settings preview/cancel; contrast and fractional DPI.
- Native `renderer_smoke`, `renderer_png` and `renderer_final_teardown` provide
  backend regression evidence, not portable-test substitutes.
- Physical DWM/ordinary blur appearance and mixed-monitor rendering remain
  physical acceptance gates. No portable assertion proves these.
- No macOS ribbon UI is introduced; its AppKit controls and Metal backend remain
  unchanged. macOS native compilation/runtime remain separate qualification.

No device resources, native renderer, release ordering, image cache, shaping or
presentation scheduling are changed here. Those are separately reviewable future
ownership slices, after their native baseline and lifetime gates are established.
