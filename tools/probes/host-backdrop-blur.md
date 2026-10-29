# Experimental host-backdrop additional-blur probe

This standalone executable is **not StormTTY's renderer**, is not linked into the
application, and has no production settings/configuration change. It qualifies a
possible backend before any renderer migration. Native compilation, lifecycle checks and physical blur quality are separate
qualification steps. The normal application build does not enable this experiment. The attempted
Zig syntax check stops in dispatcherqueue.h because windows.system.h is absent;
this is an unsupported-toolchain blocker, not a native compile pass.

## Contract and limits

The bottom SpriteVisual receives a CompositionEffectBrush describing a Gaussian
whose input is `CreateHostBackdropBrush()`. The top SpriteVisual contains a
premultiplied D3D11/D2D swapchain, with opaque DirectWrite text drawn after the
background-only alpha clear. These are separate siblings: no text blur or global
window alpha. No desktop capture/readback, private APIs, Win2D runtime, WinUI, XAML,
Windows App SDK bootstrapper, or extra runtime package is involved.

The host source already has OS blur. `sigma=0` means **zero additional blur**, not
an unblurred desktop. This experiment cannot establish a continuously adjustable
sharp-to-blurred desktop radius. Effect/host creation success is not proof that DWM
actually displays the expected content. Use another moving window behind this HWND
for physical verification; do not substitute a bitmap inside the probe.

An ordinary redirection HWND is the default in this isolated harness: its topmost
DesktopWindowTarget covers the backing surface, and GDI remains available if
composition fails. This differs from production's WS_EX_NOREDIRECTIONBITMAP host;
passing ordinary mode is NOT enough to migrate that host. `--no-redirection`
repeats the probe with the production HWND flag. In that mode a graphics failure
closes with a failure code: this probe does not pretend GDI can recover a
no-redirection HWND. Production opaque-child recovery remains a separate gate. WM_NCACTIVATE is left to DefWindowProc. No frame/material hacks are used.

## Build (MSVC x64, SDK 10.0.22621+)

Runtime host-backdrop support begins at Windows 11 build 22000. SDK 22621+ is the
probe's compilation baseline because it also names DWMWA_SYSTEMBACKDROP_TYPE.
C++/WinRT and all runtime components come from Windows/its SDK.

From an x64 developer PowerShell in the repository, this command builds without
adding any project dependency or requiring libghostty:

```powershell
New-Item -ItemType Directory -Force out/host-backdrop-probe | Out-Null
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE tools/probes/host-backdrop-blur.cpp /Fe:out/host-backdrop-probe/host-backdrop-blur.exe /Fo:out/host-backdrop-probe/host-backdrop-blur.obj /link windowsapp.lib CoreMessaging.lib d3d11.lib dxgi.lib d2d1.lib dxguid.lib dwrite.lib dwmapi.lib user32.lib gdi32.lib advapi32.lib ole32.lib
if ($LASTEXITCODE -ne 0) { throw 'Probe compilation failed' }
```

The optional CMake target is OFF by default, is not installed, and is never linked
into `stormtty`. For an existing MSVC build directory:

```powershell
cmake -S . -B build-msvc -DSTORMTTY_EXPERIMENTAL_HOST_BACKDROP_PROBE=ON
cmake --build build-msvc --target stormtty_host_backdrop_probe
```

A GNU cross build with the option OFF must not parse/include this file. Existing
production dependencies, packaging and tests remain unchanged.

## Automated and physical qualification

```powershell
# Real HWND + D3D/D2D WARP + owned-surface Gaussian graph construction,
# opaque-policy + recreate/resize + queue teardown. No host-effect/blur proof.
./out/host-backdrop-probe/host-backdrop-blur.exe --warp --smoke
if ($LASTEXITCODE -ne 0) { throw 'Opaque WARP smoke failed' }
# Hardware Windows 11 session: requires active host graph outside forced fallback.
# A success still only establishes API/lifecycle, NOT physical blur quality.
./out/host-backdrop-probe/host-backdrop-blur.exe --smoke --require-host
if ($LASTEXITCODE -ne 0) { throw 'Host graph unavailable or lifecycle failed' }
# Interactive physical verification:
./out/host-backdrop-probe/host-backdrop-blur.exe
```

Apply a timeout around automated launches (e.g. CTest TIMEOUT 30). The smoke timer
closes after four steps, except a genuine hung compositor/driver remains a process
failure requiring the external timeout. Run hardware smoke only in an interactive
supported session, not as a claim about headless CI.

Acceptance checklist (record OS build, SDK, adapter/driver, display/DPI and result):

- Keep opacity at .25, move a high-contrast window behind the probe; sweep extra
  sigma 0, 8, 24, 48, 64. Left/right change by 2; Home/End select 0/64. Verify
  *spatial* edge spread changes independently of tint and live motion is visible.
- Up/down change background opacity across **0..1**. Verify text remains sharp and
  opaque throughout. This harness caps only the exploratory extra sigma at 64;
  that is not a proposed product limit.
- Test focus/unfocus, minimize/restore, snap/maximize, interactive resize, monitor
  DPI transitions, R (recreate), repeated launches and close while moving/resizing.
- F forces opaque policy; actual Windows transparency-off and high contrast must
  also suppress the host brush. High contrast uses system foreground/background.
  WARP never enables the host graph. Restore settings and confirm reactivation.
- Effect failure leaves an opaque composition brush; device/composition failure
  falls back to GDI. Actual device-removed/reset errors get one immediate recreation
  attempt; a failed recreation settles to GDI. R can explicitly retry later.
- Measure GPU/DWM cost at maximized/high-DPI size with motion behind the probe.
  One shared effect is used, not one per pane. No performance claim exists yet.

## Primary sources

- [Documented HWND host enablement and OS floor](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)
- [Host source semantics / system policy](https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.compositor.createhostbackdropbrush)
- [Toolkit source: host blur is OS-fixed](https://github.com/CommunityToolkit/WindowsCommunityToolkit/blob/main/Microsoft.Toolkit.Uwp.UI.Media/Brushes/AcrylicBrush.cs)
- [Win32 DesktopWindowTarget hosting](https://learn.microsoft.com/en-us/windows/uwp/composition/using-the-visual-layer-with-win32)
- [Native effect description ABI](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.effects.interop/nn-windows-graphics-effects-interop-igraphicseffectd2d1interop)
- [SpriteVisual Gaussian effect brush](https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.compositioneffectbrush)
- [Do not use LayerVisual.Effect with host backdrop](https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.layervisual.effect)
- [Existing swapchain interop](https://learn.microsoft.com/en-us/windows/win32/api/windows.ui.composition.interop/nf-windows-ui-composition-interop-icompositorinterop-createcompositionsurfaceforswapchain)
- [Queue shutdown requires pumping](https://learn.microsoft.com/en-us/windows/win32/api/dispatcherqueue/nf-dispatcherqueue-createdispatcherqueuecontroller)

macOS remains separate: NSVisualEffectView behindWindow samples behind the window
but exposes no public blur-radius setting. No macOS implementation or parity claim
is part of this Windows experiment.

The Gaussian effect GUID is provided by `dxguid.lib`; see the [Direct2D Gaussian blur library requirements](https://learn.microsoft.com/en-us/windows/win32/direct2d/gaussian-blur#requirements).

## Bounded owned-window experiment

From an **x64 Developer PowerShell**, the runner compiles this standalone file
with MSVC and SDK 10.0.22621+, without Ghostty or production app dependencies:

```powershell
# Both ordinary and production-style HWNDs; approximately one second each.
./tools/probes/run-host-backdrop-blur.ps1 -Mode Warp
# Interactive Windows 11 hardware session: two 12-second runs to observe.
./tools/probes/run-host-backdrop-blur.ps1 -Mode HardwareSweep
```

Use `-SkipBuild` only after the executable has been built. Each owned process has
an external 30-second deadline and local stdout/stderr logs in
`out/host-backdrop-probe`. The runner does not capture, upload, install, or change
system settings. No native binary has been produced by the Linux cross-check:
Zig's headers stop at missing `windows.system.h`; MSVC/native verification remains
required. The optional existing CMake target remains
`stormtty_host_backdrop_probe`, enabled with
`STORMTTY_EXPERIMENTAL_HOST_BACKDROP_PROBE=ON` in a native MSVC build.

`--sweep` creates a **separate owned HWND** containing only a 64-pixel black/white
checkerboard and moving red stripe. The probe opens above it. It holds opacity
at 0.25 while sigma is 0, 8, 24, then 48 for two seconds each; the remaining four
seconds test forced opaque policy, restoration, device recreation, and resize.
Keyboard adjustments are disabled during this controlled sweep (Escape cancels).
It closes both owned windows, then pumps dispatcher shutdown for at most five
seconds. External timeout remains necessary for a blocked driver call.

Direct executable equivalents:

```powershell
./out/host-backdrop-probe/host-backdrop-blur.exe --require-host --sweep
./out/host-backdrop-probe/host-backdrop-blur.exe --require-host --sweep --no-redirection
# Unbounded interactive inspection, still only synthetic source content:
./out/host-backdrop-probe/host-backdrop-blur.exe --synthetic-backdrop --no-redirection
```

The WARP path now always constructs the Gaussian effect factory and effect brush,
binds the **owned swapchain surface**, and sets all four sigma values before
choosing the opaque policy. This validates effect-descriptor ABI/property
construction if it passes. It does not validate blurred pixels, the hardware host
source, or performance; the validation graph is not used to infer host behavior.
Hardware `--require-host` fails when the host graph is unavailable. Both modes
report `physical_blur_verified=0` or equivalent wording even after API success.
Exit 2 means invalid modes or cancellation, not a completed qualification.

For physical acceptance, record whether the checkerboard/stripe edges visibly
spread more at larger sigma **with fixed tint**, whether motion stays live, and
whether foreground glyphs remain sharp. Compare both HWND variants. Record OS,
GPU/driver and DPI separately; no automated claim is inferred from the local log.
The host source's OS blur remains at sigma 0. No production slider should promise
an unblurred desktop at zero based on this experiment.

## Separate branch/PR CI gate

`.github/workflows/host-backdrop-probe.yml` is independent of the app's native
baseline/qualification workflows. Its `pull_request` path filter runs this new
workflow from a PR containing the probe changes; alternatively a push to
`probe/host-backdrop-*` runs the branch workflow. It does not depend on first
merging a dispatch-only workflow into main. Root owns publication.

The native Windows job initializes MSVC, configures the standalone project with
`cmake -S tools/probes -B build/host-backdrop -G Ninja -DCMAKE_CXX_COMPILER=cl`,
and builds the same `stormtty_host_backdrop_probe` target/source without fetching
Ghostty or changing app gates. Compilation errors fail the job. It then runs only
`--warp --smoke`, both HWND variants, using the existing owned-process runner
(30-second deadline, five-second reap bound). It never runs the hardware sweep.

Evidence JSON distinguishes:

- `warp-graph-lifecycle-passed`: exit zero plus explicit owned Gaussian graph and
  software/host-off lifecycle markers; still **not** hardware or physical blur.
- `runtime-unsupported`: exit 3 plus an explicit capability marker. Only
  `E_NOTIMPL`, `REGDB_E_CLASSNOTREG`, or `ERROR_NOT_SUPPORTED` during runtime
  initialization or compositor/owned-graph creation qualify. This emits a warning
  and a summary saying the diagnostic did **not** pass. It is not blur proof.
- `failed`: timeout/unreaped child, cleanup failure, cancellation, missing markers,
  invalid effect metadata (`E_INVALIDARG`), `E_NOINTERFACE`, device errors, generic
  failures, or unexpected exits. These fail the diagnostic step.

The artifact retains the native executable, configure log and bounded diagnostic
logs/JSON for root review. Do not offer the interactive probe to a user before its
native compile has succeeded. The Python classifier's negative cases run on Linux
and Windows; this does not substitute for native compilation or execution.
