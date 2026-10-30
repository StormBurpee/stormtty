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
