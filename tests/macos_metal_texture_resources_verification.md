# macOS Metal texture-residency extraction

## Boundary and unchanged paths

MetalTextureResources owns the ordered row/image texture maps, resident byte
counts, upload/hit counters and shared 64 MiB upload allowance. It does not retain
a device, font, encoder, pane, renderer, layer, drawable or completion callback.
Renderer owns geometry/placement, serial advancement, scene comparison,
device/pipelines/layer, submission and presentation observations. CPU glyphs,
fallback, ligatures, decorations and BGRA rasterization remain in
CoreTextRowRasterizer/CoreTextFontResources.

The facade combines texture statistics when queried, including after an
exception. Successful uploads/hits are cumulative; clearing residency never
resets them. The dirty_rectangles counter remains the existing successful row
upload count, not a newly introduced damage tracker.

Source comparison against fe67c7ef establishes unchanged shader, presentation
types, resize/draw/clipping, callbacks/submission, readback and device facade.
Device initialization differs only in calls replacing upload/reset/clear
bookkeeping. There is no Windows-style pending-present or final GPU drain added.

## Ordering contracts

- Rows: pane, both row-ID words, row generation, font generation, dimensions,
  cursor variant and effective blink form identity. Nonblinking rows ignore pane
  blink state. Lookup precedes raster checks. On miss, check remaining allowance,
  rasterize CPU pixels using an allowance snapshot, upload, increment successful
  row counters, then trim and optionally retain. Oversized uncached rows still
  trim old resident rows after successful upload.
- Images: pane, generation, ID and dimensions form identity. PixelFormat is
  intentionally absent, matching existing macOS behavior. Dimensions/total
  budget checks precede lookup; lookup precedes payload validation. A miss
  validates channels, checks remaining allowance, converts premultiplied BGRA,
  evicts, uploads, inserts and increments upload count. Failure does not restore
  evicted images.
- Both maps use frame serial recency. Equal serials evict first in ordered key
  traversal, not Windows unordered-map iteration.
- Limits remain row 64 MiB/16384 entries, image 128 MiB/1024 entries and image
  dimension 8192. Configured budgets are clamped, not rejected. Zero image
  dimension/budget disables image residency; zero row budget permits uncached
  row uploads.
- All row/image/white/chrome uploads consume one 64 MiB allowance. Reservation
  precedes dimension/native allocation checks, with no rollback. The private
  upload API now takes a bounded span and additionally rejects a mismatched
  payload after reservation; every production caller supplies exact-sized BGRA.
- Only the two original sites begin a batch: start of device creation and after
  drawable acquisition/serial advancement. Busy, unchanged and occluded returns
  do not reset it. Chrome retains its existing skip-on-insufficient-allowance
  policy; row/image misses still throw.
- Font reset, including same-font reset, and DPI changes clear only rows.
  Same-DPI resize and opacity changes retain both caches. Successful device
  setup clears both at its original late position, after white/layer creation.
  A failed partial setup does not gain a new early cache clear.

## ARC and asynchronous lifetime

Cached texture fields and caller locals are strong ARC references. Default
retained-resource command buffers retain textures encoded into them through
completion, independently of cache eviction/destruction. The new owner is never
captured in a block. Existing shared completion-generation objects and old-device
callback behavior are unchanged; no waits or new callback ownership are added.

The native lifetime test uses only a 1x1 authored image, an ordinary command
buffer and a blit into an app-owned buffer. A shared event holds the submitted
work until its local texture is cleared, the cache is evicted/cleared/destroyed,
and a nested autorelease pool has drained. Only then is the event signaled and
the copied pixel compared. An RAII event guard unblocks pending work on assertion
failure. This proves submitted-resource retention, not GUI presentation.

## Required native gates

The macos_metal_texture_resources_tests target covers:

- All row/image identity fields, omitted image format, hit-before-validation,
  effective blink and exact uploaded bytes against CPU raster output.
- RGB/RGBA/gray/gray-alpha premultiplication and rounding.
- Byte budgets, zero limits, maximum dimension clamp, real entry caps.
- Serial hit refresh and ordered ties; opposite row/image eviction ordering.
- Deterministic allocation failure using a nil borrowed device, preserving
  reservation, cache mutation and successful counters. This is synthetic failure
  characterization, not proof of real driver exhaustion behavior.
- Shared allowance across white/row/image, exhausted-budget hits versus misses,
  dimension failure after reservation, and independent clear/batch lifetimes.
- Submitted texture lifetime through eviction, owner destruction and pool drain.
- Offscreen Renderer size/opacity retention, same-font/DPI row invalidation,
  device invalidation of both maps, and cumulative facade statistics.

Retain all existing macOS font/row original-pixel comparisons, renderer and PNG
tests, plus native onscreen window smoke as a distinct presentation gate. A
passing offscreen Metal test does not establish physical UI correctness.
Windows/core gates must remain unaffected by this APPLE-only extraction.

No Apple SDK, native build, native execution, desktop capture or source capture
was performed in the Linux editing workspace. Native compilation/runtime results
must be recorded against the exact published commit/tree; source review and
diff checks are not substitutes.
