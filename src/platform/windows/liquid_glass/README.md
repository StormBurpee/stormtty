# Experimental desktop Liquid Glass

Windows native MSVC/SDK builds expose **Appearance → Experimental Liquid Glass**.
It is OFF by default and session-only, not Config state. Start requires a fresh,
default-No confirmation naming the display containing this window. No startup,
preview, config reload or test automatically starts capture. Existing Blur and
background-opacity settings are unchanged. Lower opacity exposes the optical base.

This is a bounded custom Windows lens, not Apple's private implementation. Real
underlying-source freshness, exclusion behavior and native integrated appearance
remain **unverified until an interactive Windows test**. CPU optics and standalone
shader compilation evidence do not establish them. macOS has no experimental
capture action; native AppKit glass is a separate future implementation.

## Boundaries

- `consent`: selected display warning and identity checks; host additionally rejects
  consent across Settings lifetime or display/policy generation changes.
- `CaptureSource`: checked own-window exclusion, exact SDR/same-adapter display,
  two-frame WGC pool and a retained GPU snapshot. Geometry changes recrop the latest
  frame even if WGC has no new frame; frame timestamp/count only advance on delivery.
- `Session`: capture/material lifetime, bounded crop/output, first-frame timeout,
  error/status reporting and affinity restoration. Stops before renderer replacement.
- `MaterialPass`: immutable source, distinct GPU intermediates, validated material
  constants, strict runtime HLSL compilation, full D3D context-state restoration.
  Opaque SDR encoded BGRA is decoded via sRGB SRV, shaded linearly, encoded via sRGB
  RTV; this is not a contract for arbitrary fractional-alpha captured content.
- Renderer: optical background is restored into each default-background/tinted
  region before its tint, avoiding double desktop composition. ANSI backgrounds,
  glyphs, selection, cursor, IME and chrome foreground are never optical input.
  All CPU canvas readback/alpha diagnostics are blocked while active **and after
  Stop until a successful full ordinary repaint**. Start cancels pending diagnostics.
- Host: session Start/Stop and visible Settings status, 33 ms active-only source
  polling. Only new source/geometry/state or ordinary terminal/UI work invalidates
  painting; the timer does not force permanent idle repaint.

Native capture indicator remains enabled. Full selected-display pixels remain local
GPU resources; no CPU pixel mapping, recording or upload. Exclusion can hide the
window from supported screenshots/shares while enabled. Protected content is not
bypassed. Prior affinity is restored on stop/failure/close; restoration failures
remain visible and prohibit restart until restoration succeeds/window is closed.

## Conservative limits and fallback

One SDR display, same rendering adapter, Windows 10 2004+ capture contract, hardware
D3D11 FL11.0; no WARP, HDR or spanning monitors. Display maximum 8,294,400 pixels;
client maximum 4,194,304 pixels, 4096 per axis. Source memory: two WGC buffers plus
one full snapshot (up to 96 MiB), crop (16 MiB); material pair (32 MiB steady, up to
64 MiB during resize allocation); session output (16 MiB), plus existing renderer
canvas/swapchain and driver overhead. These are bounded allocations, not measured
GPU residency/performance claims. Display validation runs before source updates.

Minimize, resize, DPI/display/settings change, loss of transparency/accessibility
eligibility, source/device errors and shutdown stop capture and restore ordinary
material. Restart always needs explicit Start/confirmation; no silent monitor switch.
The first version uses a native consent dialog and fixed restrained optics, no motion
or continuous optical tuning UI. High contrast/transparency policy disables Start;
smoke tests disable capture. The first integration additionally requires the ordinary
translucent-backdrop path to succeed; unsupported system-material fallback disables
Start rather than capturing invisibly behind forced opaque tint. Native display
notification behavior remains a test item.

## Required interactive evidence

Use a non-sensitive SDR test desktop and authored moving checkerboard from the
standalone source probe. Follow `tools/probes/desktop_glass/README.md` source gates,
then test in the actual composition-swapchain app:

1. Hidden pattern numbers remain live/correct; no black/stale/recursive terminal.
2. Move over a static grid with no delivered new frame: crop tracks position;
   source timestamp does not falsely advance. Leaving display stops, no switch.
3. At low opacity, edge detail displaces, center remains stable; terminal glyphs,
   cursor, selection, IME and opaque ANSI backgrounds stay sharp/unwarped.
4. Off restores original blur/opacity exactly after repaint. Readback is denied
   before that clean repaint, including Stop/error paths and pending alpha probes.
5. Cancel, repeated Start/Stop, minimize, resize/DPI, settings/HDR/topology changes,
   device loss and close release resources/restore affinity or show persistent error.
6. Confirm system indicator and independently authorized screen-share exclusion;
   test display color/linear blending, D2D state restoration and GPU memory/time.

Never run desktop capture in CI. Portable authored-texture math tests and native
compile/link/shader checks are separate, non-capture evidence.
