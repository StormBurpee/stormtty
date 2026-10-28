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
session. Normal UI callbacks and normal close never wait for active process or file I/O; a completed settings worker is reaped only after it publishes its result. Failure exits
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
  rename, reordering, compact renderer-backed tab picker, pane focus/move/swap/zoom,
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
- Retained-terminal Find UI remains disabled; its backend is not yet surfaced.
  Settings editing is implemented below; external-file changes require restart
  before editing rather than silently replacing a stale baseline.
- No physical validation of Snap hover, mixed-DPI monitors, high contrast,
  keyboard layouts, Narrator/NVDA, RDP, device loss, or GPU latency/idle budgets.
- Finite presentation-only hover (80 ms), selected-tab settle/split-label reveal
  (120 ms), and palette fade/3-DIP translation (100 ms) now have deterministic
  portable transition tests. Focus/hit testing/PTY geometry change immediately;
  animation timers stop at their deadline and are disabled by reduced motion.
  Their visual quality still requires physical Windows verification.

## Graphite refinement (source/cross-build evidence, not desktop visual proof)

- 48-DIP single integrated crown; fixed 32-DIP tabs at y8 with 4-DIP gaps,
  188-DIP preferred width, compact overflow and at least 48 DIP for dragging.
- Graphite defaults, opaque text/explicit ANSI backgrounds; documented Windows
  11 Desktop Acrylic/Mica preferences, exact-alpha crown material replacement,
  DWM rounded-corner hint, and opaque high-contrast/transparency-off/WARP fallback.
  DWM may keep maximized/snapped/unsupported windows square. No layered-window
  global opacity or undocumented accent API is used.
- 28-DIP quiet pane headers; 20×16-DIP terminal content insets, 1-physical-pixel
  idle seams and unchanged 7-DIP divider hit targets. Full title native tooltips
  preserve executable paths while exact shell-path display labels use profiles.
  DirectWrite ellipsis, not byte truncation, handles constrained labels.
- `StormTTY --smoke-layout --capture layout.png` is an explicit bounded CI
  scenario: five actual controlled cmd processes, three real tabs, three visible
  panes in a 60/40 asymmetric split, real output markers and successful exits,
  then a settled WARP framebuffer capture. Existing `--smoke` stays one-pane.
  Neither framebuffer proves desktop blur, outer corners, Snap, IME, or physical
  text quality. Normal launch never runs this test or its commands.

## Styled modal and caption ownership regression

- Close pane/tab/window and unsafe-paste confirmations are renderer-backed,
  asynchronous in-window state. No MessageBox, secondary HWND or nested dialog
  loop is involved in these decisions. Cancel is the default; Escape cancels,
  Tab/arrows cycle only the modal actions, and Enter/Space activate the selected
  action. Terminal key/text/mouse/wheel/IME input is intercepted while modal;
  process output continues. A paste action retains the original actor target.
- The HWND retains thick-frame, system-menu, minimize/maximize styles but omits
  WS_CAPTION. Because overlapped-window creation can normalize caption styles,
  the host explicitly enforces and reads back this mask after CreateWindowEx,
  recalculates the frame, and prints exact masks on smoke/failure. NCCALCSIZE/NCPAINT/NCACTIVATE are application-owned before DWM;
  caption commands are explicitly dispatched on mouse-up. HTMAXBUTTON remains
  available for documented Snap integration. This source change and style tests
  are not proof of physical inactive-window, corner, shadow or Snap behavior.
- `--smoke-modal --capture modal.png` uses actual HWND/renderer/ConPTY sessions,
  tests synthetic modal key routing and focus ownership, captures a presented
  styled confirmation, accepts close for a disposable real tab, then requires
  the surviving shell's output marker and clean exit. A framebuffer cannot show
  a stale underlying DWM redirection bitmap or prove desktop activation behavior.
- Startup failures/help now use a separate dependency-light Graphite GDI notice
  window, so reporting a GPU startup failure does not require a working renderer
  or launch a shell. No stock MessageBox remains in the Windows entry/shell path.
  UIA semantics for the renderer-backed runtime modal are not yet exposed.

## Real project navigation (separate follow-up)

- Each displayed workspace is an actual catalog entry with a real launch folder,
  tabs, split tree and opaque session associations. Startup uses the configured
  profile folder or actual process current directory; no illustrative projects,
  agent panels, Git branches or remote-connection claims are created.
- New workspace opens the Windows filesystem folder picker and launches the
  selected profile in that directory. The picker does not change the host's cwd.
  The rail is 44 DIP collapsed, 184 DIP expanded on sufficiently wide windows;
  scrolling handles the bounded project list. Focus mode hides navigation and
  context without restarting sessions. Task tabs retain their rename command.
- Context explicitly distinguishes observed **Shell directory** metadata from
  declared **Launch folder**. Observed terminal metadata is sanitized/displayed
  only, never executed or used to change directories automatically.
- Hidden projects retain their original actors, output and model metadata.
  Renderer pane identity uses host-global SessionId; local pane/tab/node caches
  reset at project activation. Close confirmations capture qualified project/
  local IDs and original session associations. Last-project close exits the
  window deliberately; it never spawns an unsolicited replacement process.
- Palette/native menu expose new/next/previous/close/navigation/focus commands.
  Hosts explicitly advertise capability; unsupported hosts show disabled reasons.
  No new default shortcuts take terminal chords. No on-disk persistence is claimed.
  Pane/tab moves are within the active workspace; cross-project and cross-window
  transfers are not implemented or represented as successful.
- `--smoke-projects --capture projects.png` uses two actual local directories and
  real shells. It checks live actor identity across activation/retirement, hidden
  terminal output, and removal of only one project. It does not prove interactive
  folder-picker behavior or physical visual/accessibility quality.


## Settings, shared menus and input-feel regression

- `Ctrl+,` opens renderer-backed Graphite Settings. Appearance controls preview
  font/size/ligatures, material/opacity and motion reversibly; Escape/Cancel restores
  the original presentation. Shell selection merges only an explicitly chosen
  discovered candidate into existing profiles. Git Bash is discovered as an actual
  executable plus discrete arguments; opening Settings never starts a shell.
- The exact absolute startup config path is retained. One background job performs
  bounded snapshot/discovery or explicit atomic Save; no filesystem probing runs
  in a WndProc. The same-read parsed Config must match the running baseline, and
  Save checks the original snapshot for conflicts. Failed Save retains the draft;
  pending Save traps Cancel/close until its result. Profiles/bindings apply only
  after successful persistence, and existing Session actors are never restarted.
  Worker results own data and hold no NativeWindow pointer. Active I/O is not
  joined while a window is visible; shutdown joins only after DestroyWindow.
- `Alt+Enter` on a palette action opens its shortcut-capture editor without
  dispatching the action. Duplicate, OS-reserved and disallowed terminal chords
  are rejected by shared binding validation. Explicit Save persists accepted
  bindings. The host traps keyboard, text, IME and pointer channels in Settings.
- Main/pane menus, compact task-tab picker, pane destinations and Alt+Space now
  use shared renderer-backed ContextMenu, including submenus/disabled reasons,
  Unicode committed-text typeahead and bounded scrolling. There are no remaining
  HMENU/TrackPopupMenu calls in the owned-window host. Callbacks capture and
  revalidate project/pane/session identities. A dismissing outside click cannot
  begin a terminal selection, and consumed menu keys bypass TranslateMessage.
  The actual Windows folder picker is still the system file-selection UI.
- Settings and menus accept OS high-contrast colors; every panel surface is opaque
  in that mode, including HighlightText/disabled text, rather than retaining a
  low-contrast Graphite palette. They do not yet expose UIA semantics.
- The caret uses a tested deadline clock: accepted terminal text/key activity
  restarts a full visible interval; queued old timer messages cannot immediately
  hide it. Output does not reset the deadline. Reduced motion suppresses both
  caret and VT text blink, without changing the application's cursor shape.
  No cursor timer remains when blinking is disabled or an overlay owns focus.
- `--smoke-settings --capture settings.png` is an opt-in actual HWND/WARP/ConPTY
  scenario. A GUID-namespaced temporary config avoids user files. It verifies
  asynchronous load, styled Settings pixels, input trapping, preview rollback,
  custom system-menu cancellation/outside-click routing, shortcut capture and
  explicit atomic Save, retained actor identity, then real shell marker/exit.
  The temporary file/directory are cleaned after UI shutdown. This is synthetic
  routing plus actual renderer/process/file-service integration, not physical
  keyboard-layout, IME, accessibility, DWM blur or visual-quality evidence.
