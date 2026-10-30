# TerminalPainter characterization

Baseline: `31c01ec895ba931f42af90c5ab0ed85b68a3f61d` renderer.mm.
The test-only reference freezes both helpers and the pane loop verbatim before
production extraction. Native fixtures compare this algorithm with the new
painter, using real offscreen Metal frames and the existing texture owner.

## Observable order and ownership

- Null frames skip before clip; rejected clips skip before row/image counts.
- Transparent replacement clear, opacity-adjusted pane background, below-background
  images, explicit/inverse/selected cell backgrounds, below-text images, rows,
  above-text images, cursor. Image crop bounds precede destination culling and
  upload; UVs use the validated crop. Defaults use full image size, not remainder.
- Ordinary row destinations use ceil-rounded dimensions; cursor row destinations
  retain fractional dimensions while raster dimensions are ceil-rounded.
- Row identity is pane ID/font generation/variant zero; cursor variant is adjusted
  column plus one. Cursor text forces blink on, copies the source row, and changes
  only its adjusted lead cell. Cursor clip intentionally persists until another
  pane or the renderer's viewport reset.
- Remaining upload allowance is read anew for every row. MetalTextureResources
  owns accounting, cache keys, misses and charged failures; painter has no catches,
  pools, cache invalidation, rollback, frame admission or submission responsibility.
- Renderer keeps scene validation, stamps, pane order, canvas, viewport reset,
  chrome ordering, submission and success-only commits.

## Verification scope

Native fixtures must run on macOS with an actual Metal device at 96/144/192 DPI.
Linux source checks are not native compile, pixel, CoreText or GPU evidence.
No native run was performed in the Linux extraction environment.

## Native fixture inventory

`macos_terminal_painter_tests` compares exact pixels, all six texture counters,
remaining upload bytes and exception strings against the frozen original:

- Each image layer, explicit crops and size precedence, partial/full culling,
  malformed pixel data skipped by crop/cull, null image placements; translucent
  pane reset (including overlapping panes) and explicit, inverse and selected cell
  backgrounds.
- Ordinary rows plus wide-tail block cursor; all cursor shapes and focus states,
  fractional/clipped pane edges, persistent cursor clip, null rows, invalid cursor
  bounds, cursor visibility/off, blinking and metric fallback.
- Repeated-frame cache reuse, pane and font identity, cursor-column variants,
  forced cursor blink versus ordinary blink keys at fixed font generation, and
  row-generation/row-ID invalidation.
- Null frame and culled-pane validation precedence, visible row/image count
  rejection, charged oversized native row upload after successful work, next-frame
  budget reset and retained cache progress.
- Shared image → first row → second row → chrome allowance at 256/320/576/1600
  bytes, with recovery after partial-budget failures.

All groups run at 96, 144 and 192 DPI. Native command after a normal macOS build:
`ctest --test-dir <build> -R '^macos_terminal_painter_tests$' --output-on-failure`.

Extraction-environment checks: `git diff --check`; exact substring comparison of
frozen helpers/pane loop with the base commit; reconstruction check confirming
renderer code outside the extraction/include/unused-metrics removal is unchanged.
These source checks passed. Native compilation and fixture execution are **not
run** here; CI remains the gate for Apple framework and GPU behavior. No build,
remote publication, physical UI or performance verification was performed.

Cache eviction remains the unchanged `MetalTextureResources` dependency's test
responsibility; these new painter fixtures exercise reuse/identity, not eviction.
