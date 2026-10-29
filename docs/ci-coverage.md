# CI baseline and explicit qualification

The automatic **Native build and runtime evidence** workflow runs on pushes to
`main` and pull requests, plus manual dispatch. Experimental branch pushes no
longer start it a second time. It has no `continue-on-error` steps: build,
bootstrap, baseline test and packaging failures remain failures. Cancellation of
an obsolete run only supersedes it with the newer revision's run.

## Blocking automatic coverage

- **Windows 2025 / MSVC:** actual native application and all test executables;
  terminal, process/session transport, portable workspace/configuration/command
  tests, cursor motion and context-menu tests, Windows settings services,
  renderer PNG/smoke, startup notice, console-backend and all five app smoke
  modes. New feel-native tests remain blocking, not qualification exclusions.
- **macOS 15 / LLVM 20:** actual AppKit application, portable core/POSIX PTY,
  toolchain, input, PNG and offscreen Metal renderer tests. Other Ink fixtures
  remain automatic; compact-alt resize and the onscreen presentation timestamp
  gate are explicit qualifications.
- **Ubuntu 24.04:** portable core and real POSIX process/session tests, including
  all registered Ink cases. There is no Linux desktop application target.
- Bootstrap boundary and negative-control Ink tests remain required. Windows
  additionally checks bundled-console cached repair, portable installation and
  installed settings launch; packaged-console app smoke; shell staging safety,
  managed HOME/drive paths; explicit and app-owned Zsh app smoke; and repeated
  owned settings teardown. These are not substitutes for resize qualification.

CTest uses `-LE qualification --no-tests=error`. Only the five Windows Ink cases,
macOS `ink_compact_alt`, and `native_macos_window_smoke` receive that label. Plain local `ctest` still runs
all registered tests; nothing is disabled or made an expected success.

## Explicit, strict qualification

Run **Explicit compatibility and stress qualification** (`qualification.yml`)
with `workflow_dispatch` on the revision to qualify. Its Windows and macOS jobs
build real native binaries and retain the prior strict assertions and nonzero
failure exits:

- Windows Ink primary, fullscreen, compact-alt, host-close and static-margin
  cases; raw transport capture and independent replay.
- Packaged-console comparison, including missing-host rejection, static margin,
  all Ink cases, raw transport and provider stress.
- Bundled Zsh native sessions against inbox and packaged providers, each with
  six repeated resizes, plus staging and app/teardown verification.
- macOS compact-alt resize, plus the onscreen smoke gate and genuine presentation timestamp
  requirement. Zero timestamps and offscreen rendering are not a pass.
- Isolated additional-blur WARP lifecycle experiment, not physical blur proof.

A green automatic baseline **does not qualify** Windows Ink/resize stress,
bundled-Zsh repeated resize, macOS compact-alt resize, hosted macOS onscreen
presentation, physical input,
IME, accessibility, display latency or visual quality. Known compatibility and
hosted-display gaps remain open until the corresponding strict run produces
passing evidence. Moving a gate does not fix its underlying behavior.

Both workflows retain test XML/logs, configure logs and available screenshots
and traces as artifacts even on failure. Artifact names distinguish baseline
`evidence-*` from `qualification-evidence-*`.

The six historical MSYS source/cursor/resize, ConPTY width and macOS presentation
diagnostic workflows are manual-only. Existing source-branch guards remain for
experiments that require those branches: select the matching branch explicitly.
They do not change production defaults. No account notification settings are
changed; genuine automatic baseline failures still notify normally.


### Renderer final teardown

The clear-blur qualification at `d957bed` passed all six focused native suites
and completed the real settings scenario, but the process hit its 90-second owned
launch deadline after entering `ID3D11DeviceContext::ClearState` during final
renderer destruction. This is not a clean process-exit pass.

Final destruction after the HWND is already gone now releases all D2D,
composition, swapchain, context and device owners without an unnecessary
synchronous replacement drain. A still-live HWND, swapchain replacement and device
recovery retain ClearState/Flush; they can need the documented deferred flip-chain
destruction barrier. See [Microsoft's Flush contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush).
No resource owners are leaked or detached to bypass cleanup.

`renderer_final_teardown` runs a fixed 12-case native matrix: opaque/composition,
HWND-first/renderer-first destruction, and 1/16/64 frames with readback, resize and
font changes. The whole matrix has a 30-second external timeout; no retry or
extended deadline converts a hang into success. This covers the app's actual
HWND-first ordering, absent from the earlier renderer lifetime fixture. Passing
it and the real settings-owned process gate qualifies the changed cleanup path;
it does not establish the underlying driver's lock mechanism or hardware blur.

The next teardown qualification records noninvasive native thread stacks from
one additional synthetic `renderer_smoke --final-teardown` owned child when the
existing strict gates fail. `tools/diagnose-renderer-teardown.py` uses the installed
Windows SDK CDB, samples after 10 seconds, bounds debugger work at 20 seconds,
and kills/reaps only its retained fixture/debugger handles. It neither retries a
passing gate nor extends the app or CTest timeout; diagnostic success is not
renderer success. No real user terminal or desktop process is inspected.
The previous final-drain candidate moved the app stall to `ID3D11Device::Release`;
the fixed matrix also stalled there with composition disabled, after
`ClearState` and `Flush` returned. This disproves a ClearState-only explanation.

The numbered sanitized stack sample from run 40861831891 records the main
thread waiting inside D3D10Warp from final `ID3D11Device::Release`, with another
WARP worker waiting and the remaining workers idle. It does not show an HWND
SendMessage cycle. The next candidate places a D3D11_QUERY_EVENT before any
final GPU-owner destruction, explicitly submits and polls completion for at
most two seconds, and then performs normal RAII release even on timeout/error.
This distinguishes asynchronous pending-work cleanup from a driver shutdown
failure; the stack alone does not establish the precise internal WARP cause.
No graphics resources are abandoned, no window messages dispatched during
member teardown, and external test deadlines are unchanged. Native results
remain pending; do not call this a verified fix until the full gate passes.

Run 40865086237 narrowed the lifetime bug further: completion queries and device
release succeeded for renderer-first 1/16/64-frame cases, but the HWND-first
16-frame query could not finish after the presentation window was destroyed.
The next repair explicitly completes/releases GPU and presentation ownership
before every NativeWindow DestroyWindow call, guarded against nested renderer
callbacks; the host dispatcher still shuts down only after HWND/userdata teardown.
The matrix retains both object-destruction orders, with the HWND-first owner now
using the required explicit prepare_window_destruction lifecycle. This method is
irreversible: no renderer calls are valid afterward except final destruction.
The previous candidate is not promoted; native qualification of this ordering
repair is required. This is not a promise that arbitrary external destruction of
a renderer's HWND can be made safe after the presentation queue has stopped.

**Verified pre-close ordering repair:** native run
[40868955721](https://github.com/StormBurpee/stormtty/actions/runs/40868955721)
at `20358d8d7d7db480b52151b019f27a92159e0d3e` passed all seven suites.
The final teardown matrix completed all 12 cases in 6.56 seconds, with twelve
completed GPU event queries and no incomplete query. The actual settings app
exited 0 in 1.188 seconds, was reaped without timeout/truncation/cleanup error,
and produced all four captures. Its log confirms pre-HWND GPU completion and
final application-local destruction. The production host helper independently
completed three owned-graph cycles and dispatcher shutdown. This qualifies the
repaired native lifecycle, not physical desktop blur/clear quality. Artifact
SHA-256: tests `4ec6635142a99d52453a8ef2755e8ed8cdafe0dfcccc85c06d84fbbedb00b12c`;
settings `dfc6d7c13f64603de0a0e531924ba67b77c75cf7d166f659a78f67d9a0306fa5`.
