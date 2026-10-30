# Windows native resource graph ownership

`RenderDevice` owns the native device, canvas, diagnostics and presentation graph.
It retains neither scenes nor painters nor TextResources/ImageCache. Renderer
keeps damage, tick, scene commit, pending presentation, DPI/text updates and
public statistics. PaintSurface escapes only as a synchronous CanvasDraw borrow;
no native device/context/factory getters or renderer callbacks were introduced.

## Release order: normal versus failed construction

This extraction is based on TerminalPainter tree at `7bbd0d9`. Its declaration
order, not just successful shutdown traces, defines failed-construction cleanup.

Normal release remains:

1. Stop dormant material; final release waits on the existing bounded completion
   query while all GPU owners are alive.
2. SetTarget(null); alpha staging, canvas bitmap, canvas texture, solid brush.
3. Clear external ImageCache at its existing trace bracket (uploads survive).
4. Context4, context, D2D device.
5. Surface-release stop; HostBackdrop detach; composition visual/target/device.
6. Latency handle close; swapchain2 then swapchain.
7. Replacement-only ClearState/Flush; opaque child HWND.
8. Immediate context then D3D device.
9. Later member destruction releases text, then D2D factory, then material
   subobjects, before the caller's COM apartment.

The exact replacement predicate remains `!final_shutdown || (window &&
IsWindow(window))`. Prepared live-HWND shutdown drains; already-destroyed/no-HWND
final shutdown does not. Prepared teardown followed by destruction remains safe.
Shell readiness cancellation still precedes render/recovery and HWND teardown;
its exported latency HANDLE is borrowed and invalid after replacement.

A throwing Renderer::Impl constructor does NOT run its destructor. The explicit
catch therefore restores the former implicit reverse order:

- ImageCache reset;
- brush, canvas, alpha staging, canvas texture slots reset WITHOUT SetTarget,
  completion query, ClearState or Flush;
- TextResources reset;
- native owner implicit reverse destruction: context4, context, D2D device,
  D2D factory, composition visual/target/device, latency handle, swapchain2,
  swapchain, immediate context, D3D device, opaque child HWND;
- glass brush, glass bitmap, glass Session, HostBackdrop;
- COM apartment last.

Native fields retain that relative declaration order inside the owner. Member
slots reset early are empty during later implicit destruction. The native owner
constructor creates no GPU objects; initialize() is called only after the same
font/cache/opacity validation and text/image-owner allocation as before.

## Transaction and privacy contracts

SurfaceChanges reports repaint and successful canvas installation separately.
Renderer applies changes on success AND exceptions; failed replacement does not
claim a new canvas or clear pending presentation. A successful canvas installation
resets pending presentation as before. Size-zero resize does not gain a new
bounded device-recovery catch path.

CanvasDraw finishes once. On a paint-body exception Renderer ends drawing,
invalidates and rethrows the original error. Normal EndDraw checking remains
outside that catch; its existing GraphicsError recovery behavior is unchanged.
Canvas commit precedes Present1; busy/occluded preserve committed canvas/pending
presentation. Full canvas copies, offscreen Flush and one bounded recovery remain.

Active/stale glass canvas rejects full readback and alpha probes. Stop alone,
partial repaint and aborted drawing do not clear taint. A successful complete
non-glass draw clears it; taint is set before any sensitive draw can begin.
Readback remains straight RGBA; alpha probes remain bounded/nonblocking.
Ordinary blur and dormant capture feature gates are unchanged. No test starts
capture; synthetic taint tests contain only app-owned solid pixels.

## Characterization and evidence

`render_device_tests` recompiles renderer sources into an isolated test executable
with `STORMTTY_RENDER_DEVICE_TESTS`. The production library has neither exception
checkpoints nor synthetic taint access. Glass is explicitly disabled in this test
build regardless of the optional production build flag. There are no injected
callbacks, production test-policy fields or altered native acceptance timeouts.

The native WARP suite checks:

- actual constructor exceptions after D2D factory, D3D device, D2D device,
  context, brush, and completed text configuration;
- actual traced COM release ordering around TextResources, with no new failure
  shutdown wait/unbind/drain and apartment last;
- failed texture/bitmap canvas acquisition, invalidation and clean replacement;
- scoped drawing, exception unwind, no double EndDraw, offscreen presentation;
- stale-taint probe/readback rejection, Stop/partial/aborted repaint, full recovery;
- normal canvas/brush/ImageCache/D2D/device/factory release ordering.

Existing renderer smoke, image/text-owner tests, PaintSurface tests, three-DPI
terminal/chrome full-versus-incremental raster comparisons, PNG and 12-case native
renderer teardown remain required. Shell/application marker readiness failures
must be reported separately rather than treated as graphics success or failure.

Direct Windows-target cc1 checks are compilation evidence only. Native runtime
results must be recorded for the exact candidate; no physical driver-loss,
hardware latency, desktop capture or visual DWM claims follow from WARP tests.
