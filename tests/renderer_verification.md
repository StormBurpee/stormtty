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

The physical Windows screenshots from 2026-10-28 showed stock native title/icon/
caption pixels **under** the translucent custom crown. Offscreen canvas readback
cannot detect that second desktop layer.

### Native diagnosis: the style is a creation-time choice

The initial dynamic `WS_EX_NOREDIRECTIONBITMAP` guard was wrong, despite passing
cross-compilation. Native [job 122626442595](https://github.com/StormBurpee/stormtty/actions/runs/40630798275/job/122626442595)
showed all independent DComp baselines committing successfully, including WARP
with a waitable swapchain, while the renderer's dynamic style step failed.
[Job 122640299995](https://github.com/StormBurpee/stormtty/actions/runs/40635151790/job/122640299995)
then identified `SetWindowLongPtrW` error 87 for both enable and disable, before
any DComp binding **and** after Commit. The style remained unchanged. This was
not unavailable WARP/DComp capability and was not solved by relaxing assertions.
Microsoft's WinUI maintainer identifies the same
[creation-time constraint](https://github.com/microsoft/microsoft-ui-xaml/issues/11523#issuecomment-5470068383);
Windows Terminal likewise [supplies the flag to CreateWindowEx](https://github.com/microsoft/terminal/blob/main/src/cascadia/WindowsTerminal/IslandWindow.cpp).

### Immutable host and ordinary child fallback

The shell now creates its top-level window with `WS_EX_NOREDIRECTIONBITMAP`.
The renderer **never changes parent extended styles** on setup, failure, recovery,
or teardown. All shell-owned flags and the parent HWND identity remain intact.

- A host created with no-redirection, with composition enabled, uses the real
  premultiplied DirectComposition swapchain and visual target.
- An ordinary caller-supplied host cannot be converted into that host at runtime;
  it safely uses an opaque HWND swapchain instead of blending translucent pixels
  over a hidden redirection surface.
- If DComp fails, or composition is explicitly disabled, a no-redirection parent
  gets a renderer-owned **ordinary child HWND** for `CreateSwapChainForHwnd`.
  The swapchain target itself never has `WS_EX_NOREDIRECTIONBITMAP`. No top-level
  recreation, session replacement, or undocumented composition API is needed.
- The fallback child covers the physical viewport at parent-client origin, uses
  the parent's DPI context, and is raised within the parent's child z-order on
  resize. It is disabled, returns `HTTRANSPARENT` and refuses mouse activation;
  chrome, terminal input, focus, IME and accessibility ownership stay in the shell.
  Custom modals, menus and settings remain primitives in the same composited
  scene, not separate controls hidden behind the fallback child.
- Swapchain references are released/flushed before destroying the owned child.
  Surface-creation failure, renderer destruction and device reconstruction remove
  it. Recovery back to DComp leaves no stale child over the composition visual.
  Parent-first destruction is also handled. The private class is process-local.

DComp fallback diagnostics retain exact API stage/HRESULT, WARP choice, host
EXSTYLE/read error, visibility and dimensions, without user text or paths.
Independent static DComp probes remain failure diagnostics; the unsupported raw
style-toggling experiments are no longer part of the active renderer tests.

### Required checks and honest verification boundary

`renderer_smoke` requires the valid **creation-time** DComp path to initialize;
it does not skip that assertion. The HWND matrix verifies ordinary direct HWND,
static DComp and static-parent/ordinary-child fallback, with zero renderer-owned
style changes, shell-added-style preservation, reconstruction and child cleanup.
A separately reserved topmost DComp target forces a genuine `CreateTargetForHwnd`
failure without a production fault-injection option. That fixture requires the
child fallback to present a real visible frame without activation, checks custom
modal pixels, child bounds/DPI/z-order/hit-test policy, resize and zero-size
suspension, opaque-to-opaque reconstruction, then recovery to DComp after the
reservation is released. Parent-first teardown is checked separately. Hardware
adapter removal and a forced same-renderer DComp-to-opaque device-loss transition
are not simulated; the real target-creation failure exercises their common
fallback construction path.

The immutable-host/child-fallback revision has strict GNU Windows compilation and
EXE-link evidence only until a **new native run** executes it. Neither this native
fixture nor renderer canvas screenshots prove the visible physical desktop
material, inactive crown, mixed-monitor DPI/Snap, IME or mouse interaction on user
hardware. Focused/blurred windows, custom close/cancel/paste, settings/menu flows
and hardware recovery still need physical acceptance. No visual approval is
claimed from the earlier successful shell tests.

API basis: [DComp target layering and one-target-per-layer lifetime](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-createtargetforhwnd),
[extended window styles](https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles),
[HWND swapchain target](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd),
[child window/input behavior](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features),
and [composition rather than redirection-surface presentation](https://learn.microsoft.com/en-us/archive/msdn-magazine/2014/june/windows-with-c-high-performance-window-layering-using-the-windows-composition-engine).

## Chrome surface stacking

`ChromePrimitive` and `ChromeText` share a `ChromeZOrder`: a named surface layer
and a nested depth. Both native backends consume `chrome_paint_order()`, painting
all shapes and then labels **within one surface**, before advancing to the next.
The stack is base chrome, transient adornments/IME, toast, palette/rename,
Settings, modal alert, context menu. Submenus increment depth. Callers must bump
`chrome_generation` for z-only changes, just as for other chrome changes.
Terminal panes retain their existing cell/image/cursor composition below chrome.

Covered labels are not deleted, clipped as whole objects, or specially faded.
Opaque higher surfaces cover their pixels; translucent scrims dim the already
composed lower surface. A partially covered label therefore remains visible
outside the overlay. Cached closing-palette shapes and labels preserve their
surface key and deliberate animation alpha on both backends.

`tests/chrome_layer_raster.hpp` runs from `renderer_smoke` and
`macos_renderer_tests`, using each native backend's offscreen readback. It checks
opaque overlays over red terminal/base-label ink at 100/150/200% DPI, readable
overlay labels, partially covered labels, z-only invalidation, removal/restoration,
real overlapping context-menu panels and submenu dismissal, and settled/fading
palette-label alpha. Core `context_menu_tests` separately validates stable layer
ordering and retention of underlying labels. Compiling these native fixtures on
Linux is compile coverage only, never native runtime or desktop visual proof.

### Windows typing settle

Default block, bar and underline cursors use an 85 ms cubic ease-out reveal
**within the destination cell**, not a translated cursor or animated terminal
text. A short printable input must first be accepted by the session; a forward
one/two-cell, same-row cursor change and changed row generation then qualify the
response. This is a conservative presentation heuristic, not proof that specific
PTY bytes echo that input. Terminal text and input delivery are immediate. Large jumps, alternate
screen, selection, focus loss, resize, IME and reduced-motion settings snap. The
finite host timer stops when settled; output without eligible input does not
start it. macOS currently keeps its existing static cursor presentation.

`cursor_motion_tests` is portable policy evidence. Windows `renderer_smoke`
writes seven actual WARP readbacks to `artifacts/typing-*.png` beside the test
executable at 0, 14, 28, 42, 56, 70 and 85 ms. These are deterministic sampled
presentation times, not measured hardware frame cadence. Incremental and full
redraws must match at every sample (including the inverse glyph under a block),
and the settled frame must return `unchanged`. Native CI execution and human
hardware feel remain separate gates from cross-compilation.

The ordinary Windows `--smoke` also exercises the HWND bridge: terminal
click/release, `WM_CHAR`, actual PTY `x` echo/cursor advance, host-generated
settle, timer shutdown, another input and reduced-motion cancellation. It
backspaces the test input before the existing controlled shell-exit command.
When OS/user reduced motion is already active, this explicitly reports the
active-settle path unavailable and verifies echo/no animation instead; it never
overrides OS policy to manufacture a pass. The normal smoke deadline bounds a
missing real echo. These checks require native execution, not just compilation.
