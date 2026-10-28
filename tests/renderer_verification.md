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

## Material-band alpha, chrome trimming and finite transitions

`ChromePrimitive::background_replace` marks a **material background fill**, not a
foreground opacity effect. Direct2D uses COPY for that fill and immediately
restores SOURCE_OVER for the stroke, tabs, overlays and text. Consequently a
crown fill with alpha 204 remains alpha 204 over a canvas at alpha 230; the two
backgrounds do not compound to near-opacity. Rounded corners preserve their
coverage. A real HWND backed by WARP or an opaque swapchain uses opaque default
and replacement backgrounds; offscreen `composition=false` exercises that
fallback. Offscreen composition surfaces still allow WARP to test alpha math.
The shell owns supported DWM Desktop Acrylic selection and OS/high-contrast
policy; renderer alpha alone does not verify the visible material on a desktop.

Single-line `ChromeText` now uses DirectWrite's actual ellipsis inline object and
character/cluster-aware trimming. Its UTF-8 text is not manually shortened.
Explicit text alpha for finite palette transitions is applied to a bounded D2D
layer, including color-font glyph layers; ordinary chrome text and all cached
terminal rows remain opaque. No time-based animation is started by the renderer.

Chrome generation changes compare old/new primitive and label content and damage
only changed bounds. A crown-only alpha transition neither reshapes nor redraws
terminal rows beneath the content region. Finite overlay transitions still
recompose the rows they actually cover, using cached shapes; a settled scene
returns `unchanged`. Geometry changes, device recovery and explicit force redraw
still legitimately repaint the surface.

Added native WARP assertions check exact material alpha, opaque rounded tabs,
terminal pixel identity and zero row-cache activity on a crown-only transition,
opaque fallback, long-label equivalence to a native ellipsis reference, bounded
text opacity, erasure after opacity reaches zero, and no extra presentation when
settled. GNU Windows compilation/linking is not execution of these new checks;
a fresh native Windows CI run is required for this delta.

API basis: [DirectWrite ellipsis trimming sign](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritefactory-createellipsistrimmingsign)
and [Direct2D layer opacity](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-layers-overview).

## DirectComposition HWND backing-surface ownership

The physical Windows screenshots from 2026-10-28 showed the stock native title,
icon and caption buttons **under** the translucent custom crown, including while
the stock close-confirmation dialog had deactivated the window. Offscreen canvas
readback cannot detect that second desktop layer.

The renderer now enables `WS_EX_NOREDIRECTIONBITMAP` only after creating and
committing its actual DirectComposition swapchain/visual target. A checked style
failure discards that target and selects the opaque HWND fallback. Immediately
before `CreateSwapChainForHwnd`, the bit must be cleared: the ordinary HWND path
requires its redirection surface. This applies even when the caller originally
supplied the bit; teardown restores that original value.

Only the one renderer-owned bit is changed. Other extended styles are read fresh
and preserved, including changes made by the shell during the renderer lifetime.
Zero-valued `Get/SetWindowLongPtrW` results use `GetLastError` correctly; changes
are refreshed using checked `SetWindowPos(SWP_FRAMECHANGED)` without moving,
resizing, activating or reordering the window. Readback also rejects a style
silently altered by `WM_STYLECHANGING`. Surface-creation failure and device
reconstruction release the swapchain and restore the bit before proceeding;
failure to restore blocks recovery instead of creating an incorrectly styled
fallback. Destruction is nonthrowing and emits a debugger diagnostic if a live
HWND refuses restoration. An already-destroyed HWND needs no restoration.
The shell must defer nested layout/render work while these synchronous style
notifications run; the Windows shell owns that renderer-call reentrancy guard.

`renderer_smoke` now creates hidden real HWNDs and requires both native WARP
DirectComposition and opaque HWND presentation to initialize, rather than treating
an unavailable path as a pass. Assertions cover initial zero/nonzero EXSTYLE,
caller-preexisting no-redirection, unrelated-style preservation, reconstruction,
DComp-to-opaque-to-DComp recovery, and original-bit restoration on destruction.
A test window's `WM_STYLECHANGING` handler deliberately refuses each direction
of the bit change: rejected DComp hosting must fall back, while rejected HWND
redirection must throw and roll back; removing the rejection must permit retry.
No production fault-injection option or shared renderer API was added.

These assertions require a **new native Windows run**. GNU Windows object/EXE
compilation is not execution. Even a successful hidden-HWND smoke cannot verify
the visible DWM layering, inactive crown, custom-alert layout, or physical
hardware behavior; focused/blurred windows and all close/cancel/paste flows still
require physical acceptance screenshots. The shell's separate native-caption
and custom-alert changes are complementary, not proven by these renderer tests.

Microsoft's [DirectComposition target layering contract](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-createtargetforhwnd)
places the visual above ordinary HWND content. The
[extended-style reference](https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles)
defines redirection suppression, and Microsoft's
[DirectComposition layering walkthrough](https://learn.microsoft.com/en-us/archive/msdn-magazine/2014/june/windows-with-c-high-performance-window-layering-using-the-windows-composition-engine)
explicitly distinguishes composition swapchains from `CreateSwapChainForHwnd`
when that surface is absent. Checked updates follow
[SetWindowLongPtrW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowlongptrw)
and [SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos).

The first native run of these new host checks
([Windows job 122609459062](https://github.com/StormBurpee/stormtty/actions/runs/40625544947/job/122609459062))
failed at the requested-versus-actual composition-path assertion. This is **not**
a native pass for backing-surface ownership; the original one-line failure did
not identify which DirectComposition API failed. Fallback now logs the exact API
stage, HRESULT, WARP choice, EXSTYLE/read error, window visibility and dimensions,
and the test prints each requested/actual path. These diagnostics contain no
terminal text, paths or user payload. The strict assertion is retained pending
that evidence. Only on that assertion's failure, four independent direct-API
probes compare ordinary versus statically no-redirection HWNDs, waitable versus
non-waitable WARP composition swapchains, and a hardware-device control. Each
uses a separate hidden HWND/device, logs exact failure stage and HRESULT plus
adapter description/flags/vendor/device, requested driver, feature level and
process session, then releases its resources. They do not call Renderer or its
style-changing guard and do not suppress the original failure.
WARP alone is not sufficient reason to skip it: Microsoft's
[composition sample](https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/blob/master/cpp/ScreenCaptureforHWND/ScreenCaptureforHWND/d3dHelpers.h)
supports WARP device fallback followed by a composition swapchain.

The subsequent native diagnostic run
([Windows job 122626442595](https://github.com/StormBurpee/stormtty/actions/runs/40630798275/job/122626442595))
established that all four independent DirectComposition probes successfully
committed, including forced WARP with the waitable flag. The production path
failed only at dynamic no-redirection enable (`0x80070057`, unchanged EXSTYLE0).
This rules out unavailable DComp/WARP capability for that runner; it does not
establish whether a particular Win32 call, binding order, or a creation-time-only
style caused the rejection. Diagnostics now name the exact style operation and
compare raw enable/disable transitions before any DComp binding and after a
successful Commit. The strict composition assertion remains unchanged.
