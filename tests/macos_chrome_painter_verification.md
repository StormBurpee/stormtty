# macOS chrome painter extraction

`ChromePainter` borrows an admitted `MetalFrame` synchronously and stores only
that reference plus viewport/DPI values. It owns no native graph, cache, font,
scene, ordering or callback state. Construction allocates nothing and introduces
no autorelease pool. Renderer retains the full-target clip, construction and
iteration of `chrome_paint_order`, fresh remaining-upload snapshot for every item,
explicit submit, public statistics and previous-scene commit. Terminal pane policy
is unchanged.

Each item resets its clip before validation. Primitive direct-fill fast paths do
not require upload allowance; raster paths retain padding, signed line endpoints,
rounded-path arithmetic and replacement behavior. Invalid geometry/label fields,
the 4 MiB item limit and the current aggregate allowance reject before bitmap
allocation. The original 64 MiB shared row/image/white/chrome budget remains owned
by `MetalTextureResources`; successful and failed native uploads retain their
original charge ordering, without rollback.

Bitmap context/backing bytes and CTLine stay alive through synchronous upload.
The font family/system fallback, semibold conversion, UTF-8 replacement, baseline,
alignment and opaque CPU glyph color followed by alpha tint are unchanged. The
`wrap` flag remains ignored; this extraction does not add multiline behavior or
repair visual quality. Transparent raster padding is not normalized into direct
replacement-fill semantics.

## Native gates

`macos_chrome_painter_tests` compares extracted output against a frozen test-only
copy of the original chrome loop/helpers from local main `1378612d`. Both paths
use real Metal composition and straight-RGBA readback in the same run; no reference
calls the new painter. It compares full pixel buffers and remaining upload bytes.
Coverage includes:

- fractional rectangles, rounded replacement fills, strokes, signed lines and
  overlapping layer/depth text at 96/144/192 DPI;
- text alignment, combining/CJK/emoji input, invalid UTF-8, missing/empty font
  family, semibold, low alpha and unchanged ignored wrapping;
- scroll clipping followed by fixed items, invalid/empty clips and skipped items;
- zero/just-short/exact allowance, consecutive item charges, direct fill with no
  allowance, exact 4 MiB item limit and over-limit rejection;
- an earlier successful upload followed by a too-wide texture failure: exact
  exception and charged progress, then actual next-frame rendering after unwind.

Budget precharge tests deliberately use the existing upload validation order to
charge an oversized-dimension upload before it throws, without allocating a
64 MiB texture. They introduce no production test hooks or policy changes.
Guard-before-allocation and lifetime ordering additionally require source review;
pixel equality alone does not prove allocation or exception equivalence.

Retain `macos_renderer_tests` (chrome layers/viewports/Settings, alignment and mixed
terminal/chrome alpha), compositor exceptional-recovery fixtures, font/CPU row,
texture-residency and PNG tests.

[PR #42 native qualification](https://github.com/StormBurpee/stormtty/actions/runs/41102242173)
passed macOS 59/59 for PR head `d7b4c150` (tree `7a5c62b3`), tested through checkout
`d3440505`. Linux passed. Windows baseline passed 70/70 and all 15 application
cases passed. The full run failed only repeated Settings cases `explicit5`,
`explicit7` and `explicit8`, each with verified malformed bracketed paste: 10/13
repeats passed, all 13 processes were reaped, and none had an outer timeout or
cleanup error. These are disclosed failures, not a full-green Windows result.

The native original/extracted comparison ran at 96/144/192 DPI and exercised
allowances of 0, 1020, 1024, 2048, 2300 and 2304 bytes. The partial-failure case
required the dimensions error and retained charges of `1024 + 80000` bytes; it
then asserted a fresh next-frame budget and green readback before the final PASS.
These checks establish the tested chrome extraction and recovery behavior, not
full renderer parity.

Linux source checks are not Apple SDK compilation or Metal evidence. Native
offscreen checks do not establish physical UI, onscreen presentation,
IME/accessibility, Intel/AMD parity or performance. Held presentation work and
paused features remain untouched.
