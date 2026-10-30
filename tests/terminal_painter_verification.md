# Windows terminal paint ownership

`TerminalPainter` owns the selection/foreground/background, row glyph painting,
decorations, image placement and cursor drawing policy. Its implementation uses
one synchronous per-pane pass containing only borrowed `PaintSurface`,
`TextResources`, `ImageCache` references and resolved DPI/background opacity/tick.
It owns no GPU resource, factory, scene, previous frame, counter or callback.

Renderer keeps BeginDraw/EndDraw, canvas clear/background, damage snapping,
commit/pending-present, device reconstruction, teardown and all material policy.
PaintSurface exposes narrow glyph/image submission, never a raw context getter.
Its image operation consumes the cache bitmap immediately after lookup; no borrow
survives another lookup. The optional D2D4 pointer is a synchronous draw borrow,
not a new owner. Shaped rows stay local to draw_text, including the uncached
scratch case. Color-run enumerator pointers are consumed before MoveNext.

## Preserved details

- Pane default background, below-background images, explicit/inverse/selection
  cell backgrounds, below-text images, per-row text then decorations, above-text
  images and cursor remain in exactly that order. Chrome follows all panes.
- Geometry remains physical pixels; D2D stays at 96 DPI. Decoration thickness
  floors DPI/96; cursor thickness rounds it. Original outline/underline endpoint
  formulas are kept, rather than algebraically regrouping fractional coordinates.
- Row/pane/half-cell overhang clips, grouped foreground selection paints, blink
  and invisible behavior, block-cursor text inversion and wide-tail handling stay
  intact. Image crops use the original integer-add-then-float source endpoints.
- TextResources/ImageCache own cumulative counters and residency. No successful-
  frame delta aggregation hides partial cache progress when later painting fails.
- The shared `device_lost` helper has precisely the existing four HRESULTs.
  Successful color translation followed by an enumeration failure still stops
  without monochrome repaint; only classified translation device loss throws.
  Unsupported translation still falls back monochrome. GraphicsError is unchanged.
- No device, release, readback, capture or ordinary blur ownership moves here.
  Liquid Glass/HDR/AI/Leyline remain paused; macOS is unchanged.

## Tests and evidence boundary

`renderer_smoke::terminal_painter_incremental_test` compares exact full versus
incremental synthetic pixels at 96/144/192 DPI: styled/selected/inverse/faint text,
combining and wide graphemes, all underline styles, strike/overline/blink/invisible,
three image layers with clipping, immutable image generation/crop/removal, cursor
bar/underline/block/wide-tail/reveal, inactive hollow cursor and cursor erasure.
Each changed frame, row and pixel payload is newly owned before publication; no
previously published object is mutated. Settled scenes must return unchanged.
Shared HRESULT classification is asserted in the existing error-identity fixture.

Retain paint_surface_tests, text_resources_tests, image_cache_tests, PNG, full
renderer smoke and the 12/12 renderer teardown matrix for the exact new commit.
The previous ChromePainter native gate is not execution evidence for this slice.
Cross-target cc1 checks establish syntax only; native pixel/recovery/teardown
checks and physical font/material acceptance remain separate.
