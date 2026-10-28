# Windows renderer verification boundary

`renderer_smoke.cpp` and `renderer_png.cpp` are **native Windows runtime tests**.
A Windows-targeted object or EXE produced by Linux/Zig is compilation evidence,
not a test pass. No Wine result substitutes for native DWrite/D2D/DXGI/WIC.

## Implemented path

- One D3D11 BGRA device/context per renderer, hardware with explicit WARP fallback.
  One two-buffer flip-sequential swapchain per top-level window; maximum frame
  latency one and a borrowed readiness HANDLE for the shell's event-driven wait.
- DirectComposition + premultiplied alpha when supported; ordinary opaque HWND
  swapchain fallback. Only the default canvas is attenuated. Explicit TUI cell
  backgrounds, selection, inverse backgrounds and glyph colors remain opaque.
  The shell owns supported DWM material, high-contrast and transparency policy.
- A persistent BGRA canvas accumulates row/cursor damage. Each presented flip
  buffer receives a full canvas copy: this deliberately does not assume flip
  buffers retain pixels, nor claim dirty-rectangle presentation optimization.
- DirectWrite shapes a contextual row, resolves system fallback, and reports real
  glyph clusters through `IDWriteTextRenderer`. UTF-16 cluster maps are associated
  with owned VT cell spans. Adjacent compatible LTR clusters are batched with
  padding **at cluster boundaries**, not by stretching or independently drawing
  Unicode scalars. Baseline and intra-cluster glyph offsets survive capture.
- Row shape cache keys include pane identity, stable row ID, content generation,
  font/DPI generation and cell metrics. Cursor changes reuse shapes. Image keys
  include pane, image identity, animation generation, dimensions and format.
- Kitty placements use adapter-resolved source crop, pixel extent, offsets,
  clipping and layer/z order, including fixed, relative and placeholder kinds.
  Unicode placeholder marker cells suppress their marker glyph. Owned straight
  pixels are validated and premultiplied for D2D upload within a separate budget.
- Runtime-probed DWrite4/D2D4 color translation handles COLR outline layers,
  SVG and bitmap glyphs, with an explicit monochrome fallback on older APIs.
- WIC's PNG decoder is selected explicitly, never by arbitrary codec or filename.
  Signature/IHDR, dimensions and RGBA byte budget are checked before decoder
  initialization and destination allocation; actual decoded size is rechecked.
  The callback owns COM initialization on the actor thread and catches exceptions.

## Automated native checks

`renderer_smoke`: offscreen WARP renders actual text ink and checks readback for
explicit/inverse/selection backgrounds, image crop and animation generation,
unchanged-frame skipping, one-row damage/cache invalidation, cursor erasure, DPI
remeasurement, GPU resource reconstruction, zero-size suspend/restore, alpha
versus explicit same-RGB background, and oversized-image rejection. Assertions
throw in Release too. The Unicode corpus enters the real shaping path but is not
a font-specific visual golden test.

`renderer_png`: checks one real semitransparent PNG produces straight RGBA,
decoded-byte and dimension limits, truncated/non-PNG headers, and decoding from
an actor thread without prior COM initialization.

## Explicit limits and required physical verification

- Full terminal bidi reordering is intentionally not implemented. DWrite's script
  shaping is used, but output clusters are returned to VT logical/visual cell
  positions. Arabic joining, Indic reordering and fallback fragmentation need
  Windows font-specific golden screenshots before any complex-script claim.
- A fallback font that cannot shape a ZWJ grapheme may produce multiple glyphs.
  The renderer preserves their relative placement, not a false one-glyph promise.
  Horizontal ink overhang is limited to half a cell around the drawn run and
  clipped vertically to its row; difficult italic/combining overhang needs review.
- Pathological cell sequences over 4096 UTF-16 units or a bounded row text budget
  use U+FFFD. This protects shaping work; it is not full arbitrary-length combining
  support. Frame/VT ownership is not altered by the visual fallback.
- Explicit image texture bytes and shape allocation capacities are bounded;
  D2D internal texture/color-glyph caches have separate 32/16 MiB soft purge
  thresholds. Driver allocations and transient per-frame cache growth are not
  included in the explicit-cache counters.
- WIC does not provide a hard wall-clock cancellation deadline here. Encoded input
  is capped at 64 MiB and dimensions at 8192, with a decoded-byte budget. A future
  isolated decoder worker is needed for a strict time-limit security boundary.
- Device reconstruction tests release/recreate real WARP resources, not physical
  adapter removal, RDP transition, suspend/resume or driver fault injection.
- Hosted WARP checks cannot establish visible DirectComposition/DWM material,
  perceptual beauty, mixed-monitor DPI/Snap, actual hardware latency, screen-reader
  behavior or font rasterization on user hardware. Those remain physical Windows
  acceptance gates. No performance numbers are asserted by these tests.

## Official API references used

- [DirectWrite cluster descriptions](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/ns-dwrite-dwrite_glyph_run_description)
- [Color-font rendering](https://learn.microsoft.com/en-us/windows/desktop/DirectWrite/color-fonts)
- [D2D device contexts](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-quickstart-with-device-context)
- [Composition swapchains](https://learn.microsoft.com/en-us/windows/desktop/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition)
- [Frame latency wait handle and ownership](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)
- [Supported Windows 11 system backdrops](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type)

## Private Cascadia Mono packaging and verification

The Windows renderer now tries a **private DirectWrite collection**, without any
`AddFontResource`, registry write, installer, or global font registration. Only
these fixed executable-relative assets are considered:

```text
<directory containing stormtty.exe>/assets/fonts/CascadiaMono.ttf
<directory containing stormtty.exe>/assets/fonts/CascadiaMonoItalic.ttf
<directory containing stormtty.exe>/assets/fonts/Cascadia-LICENSE.txt
```

`GetModuleFileNameW` is grown with a 32K path bound; Unicode, long paths and a
changed shell/process working directory do not change the lookup root. A maximum
8 MiB per font, supported OpenType-file analysis, bounded font counts and actual
normal/italic faces without synthetic simulation are checked. Both fonts must
load, or the existing system-font fallback remains available. Failed optional
asset loading is attempted once per renderer lifetime, not every frame.

`IDWriteFactory5` is runtime-probed. `IDWriteFontSetBuilder1::AddFontFile` includes
all variable-font instances; the resulting collection is private and used only
when the requested family is `Cascadia Mono` (case-insensitive). A configured
system family such as Consolas still uses the system collection. Missing user
families retain the prior Consolas/Courier New fallback policy. DirectWrite's
system fallback list is retained for CJK, emoji and other missing glyphs; the
private base family is not a replacement fallback list. UI fonts are unchanged.
The private collection survives D3D device recreation; font/DPI changes invalidate
shapes as before.

Distribution uses the **unmodified Microsoft Cascadia v2407.24** variable TTFs:

| Asset | Bytes | SHA-256 |
|---|---:|---|
| `CascadiaMono.ttf` | 714804 | `08026f617eea315caca59898784e999f6cdb2d0c34d2040deeb042bcdc359fa2` |
| `CascadiaMonoItalic.ttf` | 524380 | `d9d23dfb03c6844448a24cf0337a657ac99965262874d2e6fb5b82ac0ac08c4d` |
| `Cascadia-LICENSE.txt` | 4489 | `51882cd3cdba4e16f220f44ddb08a635c38c44ea6e0975db2574f4be6f958238` |

Keep the SIL Open Font License notice alongside the files. Do not package the
150 MB release archive or concept-only static font. Packaging verifies the
pinned asset hashes; the renderer bounds/parses local assets but does not
implement a separate runtime cryptographic integrity check.

Root-owned CMake should copy the three assets after building **both `stormtty`
and `renderer_smoke`**, and retain that directory in portable artifacts/install:

```cmake
function(stormtty_copy_bundled_fonts target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_FILE_DIR:${target}>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${PROJECT_SOURCE_DIR}/assets/fonts/CascadiaMono.ttf"
      "${PROJECT_SOURCE_DIR}/assets/fonts/CascadiaMonoItalic.ttf"
      "${PROJECT_SOURCE_DIR}/assets/fonts/Cascadia-LICENSE.txt"
      "$<TARGET_FILE_DIR:${target}>/assets/fonts"
    VERBATIM)
endfunction()
```

`RendererStats::bundled_font` reports whether the current terminal format
actually selected the private collection. The default Windows WARP smoke now
requires it, changes process CWD to the temporary directory, and tests a system
font override, switching back to Cascadia, and an unknown-family fallback. This
prevents missing packaging from passing silently through a system-installed font.
The earlier WARP/PNG tests passed native Windows CI (job `122576227915`, reported
by root); that result **predates this private-font change**. This change requires
a new native smoke run and visual font/weight/italic/fallback inspection before
claiming it verified on Windows.

API basis: [Microsoft custom font sets](https://learn.microsoft.com/en-us/windows/win32/directwrite/custom-font-sets-win10)
and [default system fallback behavior](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_2/nf-dwrite_2-idwritetextlayout2-setfontfallback).
