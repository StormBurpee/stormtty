# Windows chrome painting boundary

`windows::ChromePainter` rasterizes the supplied stable chrome order. It owns no
scene, GPU resource, cache, device, HWND or presentation state. Its synchronous
`const TextResources&` access creates owning chrome layouts; it does not configure
fonts or shape terminal rows. Measurement and wrapping remain TextResources APIs.

`PaintSurface` borrows the current context, solid brush and optional resolved
replacement-background brush only during drawing. It exposes draw mechanics,
not raw context/factory getters, transaction controls or material policy. Clip
and opacity scopes are noncopyable and unwind in native stack order. Neither
surface nor painter stores callbacks or a Renderer implementation pointer.

All geometry is physical pixels. Renderer still configures D2D at 96 DPI;
TextResources converts DIP font sizes using the active DPI. Clipping, rounded
coverage, line minimum width, label validation, ellipsis and color-font opacity
retain the original behavior. Replacement fills restore SOURCE_OVER after COPY;
a supplied background brush retains the existing background-then-tint path.

Renderer still resolves opaque/translucent material policy, computes paint order,
and owns damage snapping, canvas clear/background, BeginDraw/EndDraw, exception
handling, scene commit and presentation. Terminal algorithms remain in Renderer
for the next separately reviewed slice; they reuse only the extracted clip,
color/rectangle conversion and rectangle-fill mechanics. Cache accounting stays
in TextResources/ImageCache, including partial progress before an exception.
No new painter statistics or transactional counter merging is introduced.

## Required evidence

`paint_surface_tests` is native Windows WARP/D2D coverage using a 4x2 app-owned
synthetic bitmap. It checks replacement alpha, SOURCE_OVER restoration, the
optional replacement-brush path without capture, and partial drawing plus clip/
opacity cleanup across a test-local exception. No production fault callback or
public test switch is introduced. The source also checks scopes cannot be copied.

Existing renderer smoke remains the integration oracle: chrome layer/viewport
raster tests at 100/150/200% DPI, label opacity/ellipsis/alignment, material alpha,
terminal pixel identity, image/text lifetime, device reconstruction and unchanged
frame handling. Run image cache, text resources and PNG tests too, followed by
the final native teardown matrix (12/12 cases) for the exact resulting commit.

Direct Windows cc1 syntax checks are compilation evidence only. Native tests
must be run for this extraction before claiming runtime equivalence. Physical
DWM blur, hardware behavior and user-visible font quality remain separate gates.
Ordinary blur is unchanged; Liquid Glass/HDR/AI/Leyline remain paused. No macOS
resource or drawing abstraction is changed by this Windows-only slice.
