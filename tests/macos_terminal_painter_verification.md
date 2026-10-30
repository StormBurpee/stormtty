# TerminalPainter characterization

Baseline: `31c01ec895ba931f42af90c5ab0ed85b68a3f61d` renderer.mm.
The test-only reference freezes both helpers and the pane loop verbatim before
production extraction. The original was **not executed natively before extraction**;
this is a frozen-source oracle, not an executed pre-extraction native baseline.
Native fixtures compare this algorithm with the new
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

[Native qualification](https://github.com/StormBurpee/stormtty/actions/runs/41105869939)
passed macOS 60/60 for PR #43 head `8179685b`, through native checkout
`9d32d702254f724c537e38467a2b6a5daff21f8f` (tree
`c0f8bb59ebac75f829ebc1e9acb1f0b1b228d63d`, matching the reviewed source).
The new `macos_terminal_painter_tests` fixture compiled, linked and passed in
1.34 seconds, comparing frozen-original and extracted paths
on real offscreen Metal frames at 96/144/192 DPI. Linux passed and Windows baseline
passed 70/70, but the Windows full run failed: packaged layout actor 5 retained a
stale suffix at 33×13 (`exit=0`, `marker=0`); packaged Settings and repeats
`explicit2`/`explicit6` showed malformed bracketed paste. All 10 bundled/developer
cases passed. Across 29 compact receipts, 25 succeeded and four failed; all
processes were reaped with no outer timeout. No full-green result or shell fix is
claimed. Linux source checks alone are not native compile, pixel, CoreText or GPU
evidence.

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
These source checks passed in the extraction environment; native compilation and
fixture execution subsequently passed in the run linked above. The renderer's
later readability pass preserves its coordinator responsibilities. Physical UI,
onscreen presentation, performance and broader hardware parity remain unproven;
the focused comparisons do not establish full renderer parity.

Cache eviction remains the unchanged `MetalTextureResources` dependency's test
responsibility; these new painter fixtures exercise reuse/identity, not eviction.
