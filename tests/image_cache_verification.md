# Windows GPU image ownership

`windows::ImageCache` owns only the D2D bitmap map, immutable cache limits,
residency and cumulative upload count. It retains no device context, renderer,
frame, placement or source pixel payload. Its returned bitmap is borrowed for
immediate use, not retained across another lookup or device replacement.

## Preserved contracts

- Keys contain pane ID, pixel generation, image ID, dimensions and pixel format.
  Lookup precedes payload validation: unchanged identity means immutable pixels.
- Misses validate dimensions, source length and decoded BGRA budget. Conversion
  uses the existing rounded premultiplied BGRA math and 96-DPI D2D bitmaps.
- Recency uses renderer frame ticks. Same-tick ties retain unordered-map behavior.
  Eviction precedes conversion/upload and is not rolled back if upload fails.
- Residency is decoded bytes, not actual driver memory. Clear resets residency,
  not cumulative uploads. Existing D2D internal cache limits remain separate.
- Renderer options are validated before GPU allocation. The owner additionally
  validates its own limits when directly constructed.
- Renderer clears images at the same traced point before D2D context/device
  release. Device recreation clears images; font/DPI changes and ordinary
  surface/swapchain replacement do not. TextResources remains independent.
- Placement, layer ordering, crop, opacity, clipping and damage remain in the
  renderer. Ordinary blur, glass/readback policy and presentation are unchanged.

## Tests and evidence boundary

`image_cache_tests` requires native Windows WARP/D2D. It uses only app-owned
synthetic pixels, copying one pixel into a CPU-readable D2D bitmap; no window,
desktop capture or external file is involved. It checks key separation,
lookup-before-validation, all input formats and rounding, native bitmap format
and DPI, malformed dimensions/lengths, exact and zero budgets, decoded residency,
eviction/touch accounting, unspecified ties, clear and new-device reuse.

`renderer_smoke` adds image lifetime checks across force repaint, surface resize,
DPI/font changes and device recreation, including independent retained text.
Existing image crop/animation, clipping, layer/material, PNG and final teardown
checks remain required. The final teardown matrix must still pass 12/12 cases.

Cross-target syntax/object checks are compilation evidence only. Native runtime
results must be reported separately for the exact candidate. Pre-upload failure
ordering is retained structurally; these tests do not inject GPU upload failure,
physical device loss or driver faults. Hardware and physical UI qualification
remain separate from WARP tests.
