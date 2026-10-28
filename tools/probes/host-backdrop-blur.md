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

An ordinary redirection HWND is intentional in this isolated harness: its topmost
DesktopWindowTarget covers the backing surface, and GDI remains available if
composition fails. This differs from production's WS_EX_NOREDIRECTIONBITMAP host;
passing this probe is NOT enough to migrate that host without a separate integration
test. WM_NCACTIVATE is left to DefWindowProc. No frame/material hacks are used.

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
# Real HWND + D3D/D2D WARP + opaque-policy + recreate/resize + queue teardown.
# A success is NOT evidence of the host effect or visual blur.
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
