# Desktop Liquid Glass: source-feasibility gate

**Experimental standalone diagnostic, Windows-only, OFF by default, never installed.**
Existing StormTTY blur, defaults, AI-disabled behavior and Leyline are untouched.
This is a raw-source probe, **not a finished Liquid Glass implementation**. Its shared source service also supports the explicitly opt-in experimental app mode;
that integration does not establish successful native source/visual behavior.

## Build (Windows, MSVC + Windows SDK 10.0.22621 or newer)

From a Visual Studio developer shell, without building Ghostty/the terminal:

```powershell
cmake -S tools/probes/desktop_glass -B build/desktop-glass -A x64 `
  -DSTORMTTY_EXPERIMENTAL_DESKTOP_GLASS_PROBE=ON
cmake --build build/desktop-glass --config Release --target stormtty_desktop_glass_probe
```

The root CMake also exposes the same option. Neither configuration installs it.
The standalone configuration with the option omitted creates no probe executable.

## Consent and scope

Launch does **not** capture. `S` opens an explicit Yes/No confirmation identifying
exactly the display containing the probe (move the probe to choose another display).
It explains full-display capture, GPU-local use, no saving/uploads, and that capture
exclusion can hide this window from other supported screenshots/screen shares.
Default answer is No. This explicit application selection uses the documented
`IGraphicsCaptureItemInterop::CreateForMonitor` API; it is not the system picker.
The normal system capture indicator is retained. No borderless permission is requested.

`Esc` stops capture, releases the retained crop and restores prior window affinity.
Minimize, display/settings changes, leaving the selected display, source closure,
GPU/capture errors and closing also stop it. No monitor is silently substituted.
After errors the user must explicitly start/consent again. If affinity restoration
fails, the title warns to close the window; destroying the HWND removes its affinity.

Limitations are deliberate: one SDR display on the rendering adapter, at most
8,294,400 display pixels (4K), hardware D3D11, Windows 10 2004+ API contract.
HDR/cross-adapter/multi-monitor spanning are rejected. Protected content stays
protected. The selected **whole display** enters WGC, even though only the window's
client crop is displayed. Two capture buffers, one retained full-display GPU snapshot
(for recropping when the excluded window moves over a static desktop), and one crop
bound retained images;
no CPU pixel mapping/readback, file output, network, recording or automatic capture.

## Responsibilities

- `CaptureSource`: exclusion lifecycle, selected-display validation, WGC lifetime,
  two-buffer pool, newest GPU snapshot, geometry-driven recrop and frame metadata.
  UI-thread-owned; the sole async callback captures a shared atomic closed flag,
  never a raw object/window pointer. All failure paths close/release before retry.
- `PreviewRenderer`: hardware D3D11/D2D swapchain and crisp diagnostic text. No optics.
- `ProbeWindow`: consent, keyboard controls and lifecycle. Polling/presenting runs
  only during capture; no background capture or timer starts at launch.
- `--pattern`: separate authored animated checkerboard source; no capture calls.

## Required native evidence (not satisfied by a successful compile)

Run two instances manually, on a non-sensitive test display:

```powershell
.\build\desktop-glass\Release\stormtty_desktop_glass_probe.exe --pattern
.\build\desktop-glass\Release\stormtty_desktop_glass_probe.exe
```

1. **No auto-capture:** launch probe, cancel `S`, press Esc repeatedly. No system
   capture indicator, no frames, no affinity change, no new files/network activity.
2. **Live hidden content:** put the pattern window behind the probe, leaving a strip
   visible. Press `S`, verify the named display, explicitly choose Yes. The lower
   preview must show the **same currently advancing numbered checkerboard** as the
   visible strip, in the correct screen position. Cover/uncover the pattern, then
   repeat with it fully occluded. Compare numbers, not merely frame counters.
   **Fail:** black/empty area, frozen numbers, probe text/edges recursively appearing,
   a delayed trail accumulating, or wallpaper substituted for the pattern.
3. **Coordinates:** move/resize the probe within the selected display. Grid alignment
   must remain correct (allow measured capture latency, not persistent offsets).
   Check 100/125/150/200% DPI, negative monitor origins and snap/maximize separately.
   Leaving the selected display must stop instead of capturing another display.
4. **Exclusion tradeoff:** with a separately authorized OS screenshot/share test,
   verify this window is omitted while On and returns after Esc. No screenshots
   are generated automatically. Verify the capture indicator disappears on Stop.
5. **Lifecycle:** Start/Stop/Cancel repeatedly; minimize; close while capturing;
   unplug/reconfigure the selected display; change system settings; simulate capture
   loss. Each must release capture, restore affinity or visibly report restoration
   failure, avoid crashes and require fresh explicit Start after stopping.
6. **Reject unsupported:** HDR display, rendering-adapter mismatch, oversized display,
   unavailable WGC, unsupported exclusion. No capture should remain active on failure.
7. **Resource/performance:** inspect frame age, GPU time, peak allocation and process
   memory over repeated cycles. There must be no retained-image growth. Record the
   OS build, GPU/driver, SDR mode, scale, monitor topology, source freshness result,
   exclusion result, CPU/GPU load and observed latency. Counters/timestamps alone
   are NOT proof of fresh underlying pixels or acceptable performance.

Gates 1–7 remain required before claiming the experimental app integration works.
The app integration is intentionally available for opt-in fix-forward testing before
those runtime gates are established. This probe uses an HWND swapchain; a pass does
**not** establish that the app's different composition topology works.
Keep any user capture evidence private; do not commit screenshots or desktop pixels.

## Source contracts

- [Capture API and frame lifetime](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture)
- [Exact monitor source](https://learn.microsoft.com/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createformonitor)
- [Exclusion vs empty-content affinity](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity)

The affinity documentation says excluded windows do not appear in capture; actual
fresh underlying composition remains a native evidence gate, not an assumed promise.
