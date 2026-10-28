# Windows shell verification

## Evidence available in the Linux implementation environment

- `window_geometry_tests`: passed with GCC C++20, `-Wall -Wextra -Werror`.
  Covers integrated caption/control exclusions, maximize hit region, normal and
  compact layouts, 330–2400 DIP width × 1–32 tabs, resize corners/maximized
  behavior, and UTF-16 surrogate assembly/recovery.
- Both `window.cpp` and `windows_main.cpp` compile to x86-64 Windows GNU objects
  with exact provisioned Zig toolchain, C++20, `-Wall -Wextra -Werror`.
- This is **not Windows runtime, MSVC, IME, Snap, accessibility or visual QA**.

## Native CI entry point

```powershell
StormTTY.exe --smoke --capture artifacts/native-window.png
```

This explicit mode ignores ambient user configuration and rejects `--config`.
It starts the system-directory `cmd.exe /d /q`, uses the actual Session text/key
path to enter a harmless echo marker and `exit /b 0`, and requires all of:

1. An actual HWND, D3D/DirectWrite renderer (WARP for repeatability), and ConPTY.
2. Received child bytes and the marker as a standalone real VT line, not merely
   the entered command echoed in a prompt.
3. Presentation of a frame containing that output.
4. Observed child exit 0 and terminal actor shutdown.

A 20-second native timer fails startup/output/presentation hangs. Smoke-only
post-message-loop cleanup has an additional bounded five-second actor wait per
session. Normal UI callbacks and normal close never join a worker. Failure exits
nonzero and avoids modal dialogs. Output identifies the actual ConPTY provider.

The PNG is actual **renderer framebuffer readback**, encoded with WIC. It is not
a screenshot of the desktop-composited DWM backdrop, and cannot prove Mica,
physical monitor rendering, window interaction, or accessibility correctness.
The parent CI owner records whether this entry point actually ran and passed.

## Implemented integration paths requiring Windows acceptance

- One custom titlebar/tab band, no duplicate stock titlebar; DWM nonclient
  delegation, resize hit regions, `HTMAXBUTTON`, system caption commands,
  Alt+Space, Alt+F4, signed multi-monitor coordinates, PMv2 DPI, maximized work
  bounds, and fullscreen restoration.
- Actual workspace tabs and split trees; click/select, explicit close,
  rename, reordering, compact native tab picker, pane focus/move/swap/zoom,
  keyboard and pointer resize, escape/capture-loss cancellation. Moves retain
  session IDs and do not restart children.
- Native command palette/menu dispatch through the shared command registry;
  layout text, key release/repeat, AltGr, Ctrl chords, copy/paste safety,
  terminal selection, negotiated mouse/focus, and scrollback.
- IMM32 result composition commits exactly once, composition overlay and
  candidate/exclusion positioning; DPI/focus/composition cancellation paths.
- Event-driven paint/work/presentation wait; actual cursor/text-blink deadlines
  only; no ambient animation. High contrast and OS transparency settings force
  conservative opaque rendering; explicit terminal backgrounds/text stay opaque.

## Explicit gaps, not claimed features

- No TSF text store or UIA TextPattern/custom chrome accessibility providers.
  IMM32 implementation is not proof of Japanese/Chinese/Korean IME correctness.
- No cross-window pane/tab tear-off. Out-of-band tab drags cancel and preserve
  ownership; the app does not pretend a process was transferred.
- Retained-terminal find and in-app settings editing are visibly disabled in
  the palette. Declarative `--config` startup loading exists; no live reload UI.
- No physical validation of Snap hover, mixed-DPI monitors, high contrast,
  keyboard layouts, Narrator/NVDA, RDP, device loss, or GPU latency/idle budgets.
- No polished transition animation yet; static immediate feedback is used
  rather than an ambient loop or delayed terminal resize.
