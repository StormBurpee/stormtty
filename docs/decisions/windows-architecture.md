# StormTTY: Windows-native shell and renderer recommendation

**Council position · 2026-10-28 · architecture proposal, not a performance result**

## Decision

Build the first Windows product in **C++20, with a direct Win32 HWND shell, DirectWrite text analysis/fallback, a D3D11/DXGI compositor, and Direct2D for the initial text renderer and correctness/fallback path**. Put the exact-pinned `libghostty-vt` C API behind a small StormTTY-owned adapter. Use one swapchain per top-level window, compositing every visible pane and the custom chrome into it. Do not create a swapchain per pane, a XAML element per cell, or a browser terminal.

This choice is about minimizing Windows integration uncertainty, **not** a claim that C++ is inherently faster than Rust or that .NET is inherently slow. Rust with `windows-rs` and these same native APIs is a credible alternative. The critical choices are the terminal state boundary, native text stack, ownership, batching, frame scheduler, and Windows verification—not language branding.

The first supported product target should be **Windows 11 x64**, with ARM64 next. Treat Windows 10 compatibility as a separate product decision, not a reason to depend on preview APIs. The essential APIs here predate Windows 11; optional backdrop, corner, or newer color-font features must be runtime-probed. Do not choose SDK/runtime versions from memory: pin a supported Windows SDK/MSVC combination when implementation is authorized.

**Scope now:** research and this council note only. No application source changed, no Codex task created, no workflow published, no toolchain installed by this worker, and no user computer accessed.

## Why this stack, and the honest alternatives

| Candidate | Real strengths | Principal costs / risks | Council verdict |
|---|---|---|---|
| C++20 + Win32 + DirectWrite + D2D/D3D11 | Direct access to all HWND, COM, TSF, UIA, ConPTY, DWM, and DXGI behavior; straightforward C ABI; minimal runtime dependencies; one controlled compositor | Custom accessible controls, IME, focus, layout, and lifetime rules are ours; C++ memory safety must be engineered | **Recommended Windows-first route** |
| Rust + direct `windows-rs` + same graphics stack | Same native integration capabilities; safer owned models/channels/config parsing; first-party Windows bindings | TSF/UIA COM implementations and unsafe Win32 callbacks still need specialist work; mixing Rust/Zig/native SDK adds build surfaces | **Strong alternative**, especially with a Rust-experienced maintainer |
| Rust + winit + wgpu + COSMIC Text / glyph atlas | Good cross-platform foundation; safe application models; GPU abstraction; existing shaping/rasterization ecosystem | Correct Windows caption hit tests, UIA text ranges, TSF, and direct presentation control still need platform-specific integration; generic paragraph layout is not terminal layout; no automatic ClearType/Windows font parity | Choose if near-term macOS/Linux is a first-class requirement, not merely to call the stack modern |
| WinUI 3 / C# .NET shell + native terminal surface | Native controls, UI composition, styling, ordinary-control accessibility, settings forms; team productivity | Runtime/deployment servicing; managed/native boundaries; custom terminal still needs text/IME/UIA; extra compositor/layout coupling; transparency limitations on SwapChainPanel | **Viable**, never classify as Electron; reconsider if rich settings/control development dominates |
| WinUI 3 / C++/WinRT shell + native surface | WinUI control benefits without a managed hot path; natural COM/native integration | Still takes WinUI runtime/build/deployment and surface constraints; same custom terminal accessibility | Credible second shell, not required to prove the first slice |
| Qt 6 Quick + C++ native terminal item | Mature native cross-platform toolkit, animation/layout/accessibility support; Qt Quick uses D3D11 by default on Windows | Dependency/package size; Qt licensing review; QML/scene-graph boundary; terminal text provider and Windows frame integration remain custom | Competitive if cross-platform UI is a product goal; not the leanest Windows-only start |
| Electron / WebView2 / Tauri HTML terminal | Fast web-UI iteration; mature web terminal ecosystem | Browser runtime, web layout and bridge costs; native goal explicitly displaced | **Not recommended** for this request |

`windows-rs` exposes Windows APIs to Rust; winit supplies Windows message hooks, but a hook is not a turnkey Windows terminal shell. wgpu exposes a *desired* frame-latency hint and presentation modes: it should not be dismissed as incapable of low latency, but backend behavior must be measured. COSMIC Text supplies shaping/fallback/rasterization, not a VT cell-placement contract. [Rust for Windows](https://github.com/microsoft/windows-rs), [winit Windows event-loop extension](https://docs.rs/winit/latest/winit/platform/windows/trait.EventLoopBuilderExtWindows.html), [wgpu surface configuration](https://docs.rs/wgpu/latest/wgpu/type.SurfaceConfiguration.html), [COSMIC Text](https://github.com/pop-os/cosmic-text).

WinUI can host native swapchains. Its documentation warns against many simultaneously updating swapchains and identifies transparency/composition limitations: one workspace surface is the sensible design even if WinUI is selected later. Do not infer that all rendering must execute on the UI thread merely from swapchain attachment requirements. [WinUI SwapChainPanel](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.controls.swapchainpanel), [native swapchain attachment](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/win32/microsoft.ui.xaml.media.dxinterop/nf-microsoft-ui-xaml-media-dxinterop-iswapchainpanelnative-setswapchain). Qt has a threaded scene graph and native custom-render integration. [Qt Windows graphics](https://doc.qt.io/qt-6.8/windows-graphics.html), [Qt scene graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html).

## Evidence from actual source

Read-only Ghostty checkout verified at `46e9dc9a927b2e7e7cba79e6902c176127dbdf2c`:

- `src/renderer.zig` selects Metal or OpenGL, not a ready Windows D3D backend.
- `src/font/backend.zig` does have a `freetype_windows` path; do not misstate that every Ghostty subsystem lacks Windows awareness. This does **not** make the public VT library a complete Windows surface.
- The separate council C-API audit found native Windows build examples/CI and a substantial VT/render-state/graphics API. Use its detailed audit as the authoritative boundary and version/build record.
- Crucial ownership finding from that audit: `GhosttyRenderState` is updated in place. Its row/cell views are not an immutable, cross-thread frame. Graphics pixels are terminal-owned and must be copied while their lifetime is protected. Raw-cell bit positions are not a stable ABI; use public getters or generated type metadata.

Windows Terminal provides relevant precedent, not code that can simply be pasted into this architecture: its Atlas renderer has Direct2D and custom D3D glyph-cache backends, and separates work inside/outside the console lock. Its own README identifies unnecessary intermediate graphics abstractions as a cost. StormTTY should render directly from its owned cell snapshot, not imitate that historical layering. [Atlas architecture](https://github.com/microsoft/terminal/blob/main/src/renderer/atlas/README.md), [Atlas native backend selection](https://github.com/microsoft/terminal/blob/main/src/renderer/atlas/AtlasEngine.r.cpp).

## Component boundaries

```text
Application / Window registry
  ├─ HWND + input + custom controls + TSF + UIA (main STA thread)
  ├─ Workspace: tabs -> split tree -> pane views -> SessionId
  ├─ Command registry + validated configuration snapshot
  └─ Render scheduler -> D3D11/D2D compositor (render owner)
                              ↑ owned FrameSnapshot / ImageUpdate
Session registry              |
  ├─ ConPTY read pump -> bounded output mailbox
  ├─ single-writer terminal actor -> libghostty-vt adapter
  ├─ ConPTY write pump <- input / reply byte queue
  └─ process and pseudoconsole lifetime controller
```

A **session** owns the child process, ConPTY, terminal state, scrollback, and exit state. A **pane view** owns viewport/selection/focus/zoom and references a session. A **window** owns a swapchain and a workspace view. Moving a pane or tab never respawns a shell. Detaching a tab transfers the view, not borrowed pointers or OS process ownership. Closing a window requests session closure according to user policy; it must not rely on destruction of the HWND to accidentally clean up children.

Suggested interfaces, deliberately independent of renderer implementation:

- `SessionCommand`: input key/text/paste, viewport resize, focus, mouse, scroll, select, close.
- `TerminalAdapter`: feed bytes, encode input, collect replies/actions, resize, snapshot dirty content. Pin ABI checks and all foreign-pointer handling here.
- `FrameSnapshot`: session + frame + layout generations, viewport dimensions, stable row identities/content generations, owned grapheme/style/cursor data, palette, damage, graphics placements, references to **StormTTY-owned** image objects.
- `TextShaper`: immutable cell run + font/locale/features/metrics -> glyph IDs, font faces, cluster-to-cell mapping, advances/offsets and ink extents.
- `GlyphRasterizer`: shaped glyph + rendering parameters -> monochrome/grayscale or color atlas entries.
- `Compositor`: rects, glyph batches, image quads, clipping, cursor, selection, chrome, overlays -> present.
- `AccessibilitySnapshot`: stable text/range identifiers and geometry, not a view into live terminal memory.

Do not prematurely design a generic cross-platform widget toolkit. Implement a small retained tree of titlebar buttons, tab controls, split handles, palette/search controls, and terminal surfaces with shared layout/hit-test/accessibility identities.

## Ownership, threads, and shutdown

1. **Main STA:** creates/destroys HWNDs and COM input services; handles messages, focus, clipboard, drag/drop, command routing and UIA entry points. It does not read pipes, parse unbounded output, wait on the GPU, or join worker threads inside a window callback.
2. **Two synchronous I/O pumps per active session initially:** one reader, one writer. Microsoft's ConPTY guidance explicitly recommends independent channels/threads because blocking read/write/teardown on one thread can deadlock. Keep reads draining during close; final output can arrive during pseudoconsole closure. [ConPTY lifecycle](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session).
3. **Terminal actors:** serialize all mutation of each VT instance. Start with simple dedicated workers or a small fair executor with per-session serialization; do not pretend a blocking reader can also service input commands without a wake mechanism. Input encoding and VT replies pass through the same state owner.
4. **Render owner:** owns the D3D immediate context and GPU resource operations. D3D11 device contexts are not freely concurrent; a single render owner is simpler than locks around every draw. No GPU work while holding terminal-state ownership. [D3D11 threading](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro).
5. **Snapshot publication:** copy dirty rows and changed image payloads under the precise library lifetime rules; publish owned immutable data. Use a two-slot/latest-frame mailbox. Drop obsolete *render snapshots*, never PTY bytes or state transitions. When using library two-phase snapshot APIs, respect begin/end lifetime requirements rather than assuming the object is immutable.
6. **Shutdown state machine:** Running -> CloseRequested -> Draining/Closing -> Exited -> Reaped. Disable new user input, schedule HPCON close off UI, keep output drain alive, observe process exit/EOF, cancel blocked operations through documented mechanisms if necessary, then release handle owners. Test close-under-flood and creation failure at every acquired-resource boundary. A timeout reports and escalates the session cleanup; it must not free live callback state.

Use RAII for handles/COM; explicit move-only ownership; `std::jthread`/stop coordination where appropriate; no owning raw pointers and no thrown exception crossing Win32/COM/C callbacks. Keep FFI narrow and fuzz it. Per-session Job Objects can aid cleanup but need explicit policy/testing for child processes and WSL; never shut down an entire WSL distribution to close one tab.

### Backpressure and fairness

Proposed starting limits (tunable, not benchmark claims): 1–4 MiB output queue/session, 64 KiB parse chunks, approximately 1–2 ms actor quantum before checking commands/other sessions, and a bounded input queue with explicit large-paste flow control. Full output queues naturally backpressure the child; never discard arbitrary escape bytes. Keep protocol replies/normal input responsive, preserve ordering, and do not interleave unrelated input inside a bracketed-paste transaction. If shutdown intentionally discards output, make that a separate terminal lifecycle state.

Coalesce resize requests by generation, then apply VT and ConPTY sizes consistently. During divider dragging, update visual layout immediately but cap session resize emission to presentation cadence and always send the final size. Hidden tabs continue parsing and retaining bounded scrollback; they do not render or animate.

## The difficult part: text correctness in a fixed grid

**DirectWrite is not a terminal layout engine.** Ghostty's cell/grapheme widths remain authoritative. Never feed a whole screen to paragraph layout and accept its wrapping, nor independently center each Unicode scalar.

Pipeline:

1. Obtain cell graphemes, cell spans, style/font identity and stable row generation from the adapter.
2. Form contextual runs, respecting row, font/style and other required boundaries. Resolve fallback for actual script/locale, not one hardcoded emoji or CJK face. DirectWrite font fallback maps character ranges; its text analyzer exposes many-to-many cluster mapping. [Font fallback mapping](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_2/nf-dwrite_2-idwritefontfallback-mapcharacters), [GetGlyphs](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextanalyzer-getglyphs).
3. Shape with script/locale/features; retain within-cluster glyph offsets and contextual relationships. Assign each cluster an advance equal to its occupied terminal cells; adjust at cluster boundaries, not by stretching individual glyphs. A font fallback must not change the terminal's column count. Bound pathological combining sequences and retry insufficient glyph buffers safely.
4. Position in physical pixels from a consistent per-pane cell metric. Keep backgrounds and selection cell-exact. Define controlled glyph overhang, clipping and fallback-scale policy; naive per-cell clipping cuts italics/combining marks, while unlimited overhang corrupts neighbors.
5. Default to the VT engine's visual cell order. Complex-script shaping does not itself authorize paragraph-wide bidi reordering. Any bidi mode needs explicit mapping for cursor, selection, mouse reporting and copy. Test Arabic/Indic contextual shaping without claiming unsupported full bidi semantics.
6. Use `TranslateColorGlyphRun` and format-aware drawing/rasterization for color emoji. Plain `DrawGlyphRun` is insufficient. Runtime-probe supported glyph formats and provide an explicit monochrome/tofu fallback; do not require preview DirectWrite interfaces. [Color-font rendering](https://learn.microsoft.com/en-us/windows/desktop/DirectWrite/color-fonts).

The first renderer should draw correctly shaped glyph runs through D2D on the DXGI surface. Only then add an atlas-backed D3D11 text path under the **same** shaping/snapshot interfaces, using the D2D path as a visual oracle. This avoids debugging custom rasterization, state extraction, frame scheduling and ConPTY simultaneously.

### Caches and invalidation

- Shape-cache key: text/grapheme contents + cell widths + font/fallback/locale/features/style identity + font collection and metric generation. A screen row number is not identity.
- Row draw-cache key: stable row/content generation + viewport origin/clip + palette/style/selection effects. Separate layout/shaping from paint colors when possible.
- Glyph-atlas key: physical font face/glyph ID/size, variations, raster mode, DPI/subpixel phase as applicable. Version atlas entries; never reuse UVs while submitted frames can still refer to old content.
- DPI/font/zoom change invalidates metrics/shapes/atlases as required; palette changes repaint without necessarily reshaping; cursor blink should not reshape text; scrolling/reflow must move or invalidate stable row mappings correctly.
- Image key: session + image identity + generation/frame. Copy/upload changed pixels, update placements separately, respect clipping/z-order/alpha and animation deadlines. Cap CPU decoded bytes and GPU residency independently.
- Prefer row/run batches and instanced quads, not one allocation or draw call per cell. Fullscreen redraw can still be inexpensive if shaping and uploads are damage-driven; optimize dirty-rectangle presentation only after measurement.

## Premium chrome without breaking Windows

An application-drawn titlebar is compatible with normal Windows behavior. Keep overlapped-window capabilities and implement the non-client contract; do not replace Windows move/resize with a homegrown polling loop. Microsoft's custom-frame guidance describes `WM_NCCALCSIZE`, DWM delegation and hit testing; Snap guidance specifically calls for `HTMAXBUTTON` over a custom maximize/restore region. [DWM custom frame](https://learn.microsoft.com/en-us/windows/win32/dwm/customframe), [Snap layouts](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-snap-layout-menu).

Required checklist:

- Correct drag, resize edges/corners, caption-button hit regions and client exclusions for tabs/buttons; signed coordinates on negative-origin monitors.
- Maximize/restore, double-click titlebar, Win+arrow, Win+Z/Snap hover, Alt+Space system menu, Alt+F4, activation state, taskbar, keyboard traversal and tooltip/accessibility names.
- Per-monitor-v2 awareness before window creation; handle `WM_DPICHANGED`, suggested rect, monitor work areas, minimized/zero-sized surfaces, fractional scale and maximum bounds. DPI awareness alone does not rescale custom-drawn contents. [Windows DPI model](https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows).
- Provide accessible button/tab/split-handle providers and keyboard pane movement/resizing. “Native tabs” means native application controls and semantics here, not a browser DOM and not a mandatory old-looking system tab widget.
- Respect high contrast, reduced animation, system text scaling where applicable, and meaningful visible focus. System shadow/rounded corners are compatible with custom artwork; backdrop blur is optional, not an architecture dependency.

Use opaque near-black terminal content initially. Transparency is not free visual polish: ClearType on transparent targets is problematic, and D2D changes antialiasing behavior for alpha targets. Prefer grayscale text for translucent surfaces; add backdrop effects only after readability/power measurements. [D2D alpha/text behavior](https://learn.microsoft.com/en-us/windows/win32/direct2d/supported-pixel-formats-and-alpha-modes).

## IME, accessibility, input and Ink

**IME:** implement a TSF text store representing the active composition, with read-only surrounding context if appropriate—not an editable model of all terminal output. Main-thread focus, caret rectangle, composition updates/underlines, candidate positioning, cancel/commit and DPI moves are mandatory. Commit text to PTY exactly once and prevent duplicate key/text paths. Preserve dead keys, AltGr, keyboard-layout changes and emoji input; command bindings must not steal composition navigation. TSF has document-lock/reentrancy rules separate from the terminal actor lock. [TSF text stores](https://learn.microsoft.com/en-us/windows/win32/tsf/text-stores).

**UIA:** provide the terminal as a real text-bearing provider with text ranges, selection, visible text, cursor/bounds and focus. UIA TextPattern and TSF solve different problems. Serve bounded, stable text snapshots without requiring a synchronous call into a busy parser/render thread; emit coalesced text/selection notifications and invalidate stale range IDs predictably on scrollback eviction. Check Narrator and NVDA, not just an accessibility inspector. [Text/TextRange providers](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-implementingtextandtextrange), [TextPattern versus TSF](https://learn.microsoft.com/en-us/dotnet/framework/ui-automation/ui-automation-textpattern-overview).

**Command system:** one registry of command IDs, typed arguments, descriptions, enablement/context and shortcuts. Palette/menu/keybinding use the same dispatch. Versioned declarative configuration with schema, validation, useful errors and atomic hot reload; no executable JavaScript/Lua config initially. Context precedence: OS-reserved behavior -> IME/text service -> explicit application bindings (with composition-aware exceptions) -> terminal input encoding. Chords must time out/cancel visibly and fall through predictably. Never silently reinterpret Ctrl+C as copy when a TUI expects it.

**Ink:** treat it as a compatibility workload, not a special renderer. Run pinned real Node/Ink applications under both Windows Node and WSL: frequent rerenders/spinners, wide/combining/emoji content, alternate screen where used, cursor hide/show, focus/mouse modes, resizing, input/raw mode, Ctrl+C, bracketed paste and color capability negotiation. Also run a byte-exact deterministic VT fixture suite. Keep terminal cells/cursor updates atomic at snapshot boundaries; honor synchronized-output semantics if exposed by the engine, with a bounded recovery policy for an app that never ends a synchronized update. Do not advertise an unsupported terminal capability merely to make a demo colorful.

## Frame scheduling and measurable budgets

Use a two-buffer flip-model swapchain and a frame-latency waitable object; initially vsync-on, at most one queued frame per swapchain. Wait on **work + presentation readiness + next animation deadline**, rather than render continuously or sleep a fixed 16 ms. Own all swapchains through the render scheduler, share device/atlases, and revisit per-device versus per-swapchain throttling when multiple windows are active. [DXGI flip-model guidance](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model), [multiple-swapchain caveats](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-multiple-swap-chains).

Suspend rendering when minimized/occluded/unchanged; cursor blink and images schedule their next actual change. Use time-based 120–180 ms chrome transitions, not animated terminal reflow. Recover device removal by recreating GPU state from owned CPU snapshots; preserve sessions. Provide WARP/D2D fallback and test RDP. Avoid D3D12 until evidence justifies its synchronization/resource-management complexity.

**Provisional acceptance targets**, measured in Release on a declared integrated-GPU Windows 11 laptop at 1920×1080, 120×40 cells, then repeated at 4K/150% and 4 panes. These are goals, not established results:

| Metric | Initial target / measurement |
|---|---|
| App-to-first usable frame | warm p95 ≤150 ms; cold p95 ≤350 ms; measure shell-ready separately |
| Local synthetic input-to-present | p95 ≤1 refresh interval + 4 ms; report OS input→enqueue, actor, render and presentation separately; shell latency excluded |
| Renderer at steady 120 Hz | CPU prepare+submit p95 ≤2 ms; GPU p95 ≤2 ms for defined one-pane workload; full frame ≤8.33 ms |
| Idle | no periodic rendering except scheduled cursor blink; p95 CPU <0.5% of one core with static visible screen |
| UI responsiveness under output flood | input/command dispatch p95 <10 ms; no unbounded queues; UI remains movable/resizable |
| Memory | initial one-pane private bytes target ≤100 MiB excluding shell; explicit bounded scrollback/image/cache budgets; measure committed + resident + GPU separately |
| Throughput | record parser-only and ConPTY end-to-end MiB/s on identical corpora; no superiority claim before comparison with Windows Terminal/other native terminals |
| Stability | close/flood/resize/DPI/device-loss loops show no growing handles, orphaned sessions, deadlocks, or stale snapshots |

Instrument with ETW/QPC spans and PresentMon, CPU/GPU tools, queue depths, cache hit rates, frame age and memory counters. Compare traces, not FPS alone. A terminal can display 120 FPS while falling seconds behind output.

## Minimal runnable vertical slice and stop/go gates

**Slice A: prove native window + actual terminal.** One HWND, custom titlebar, D3D11/D2D surface, one ConPTY `cmd.exe` session, pinned VT adapter, true Unicode/selection/clipboard, resize, basic input, clean close, and event-driven rendering. Use a real frame snapshot immediately; no toy handwritten ANSI parser. Add PowerShell and `wsl.exe` launch profiles with an explicit executable, working directory and argument model (no command-string concatenation).

**Slice B: prove the expensive correctness risks before decorative polish.** Japanese/Chinese/Korean IME, dead keys/AltGr, UIA text and focus, emoji/fallback/ligature/combining/wide-character corpus, high-contrast and multi-DPI Snap/resize. A successful ASCII shell window is not a proof of the product architecture.

**Slice C: prove the product structure.** Two tabs, two panes, divider drag, keyboard focus/move/resize, pane relocation without restarting a shell, hidden-tab output, one animated image using the audited graphics API, and a real Ink workload. Keep one compositor surface. Then optimize D3D atlas rendering against the D2D reference and measured budgets.

**Go/no-go:** do not build a large settings UI or theme catalog before A+B pass on real Windows. If text mapping/IME/UIA are disproportionately costly, evaluate WinUI for controls or a maintained native terminal-control substrate; do not silently substitute a web terminal. If native GPU control is not actually needed, retain the D2D path rather than overengineering an atlas prematurely.

## Build, CI, packaging and verification ownership

- CMake + Ninja/MSVC (or clang-cl as a secondary compiler), Windows SDK, C++20 and narrow dependencies. Use exact hashes/versions and notices for the VT library/toolchain; follow the audit's Zig requirement, do not silently track upstream HEAD.
- Build `libghostty-vt` for the matching Windows architecture as a separate reproducible step; link through its public C API. Verify enum sizes, architecture, calling convention, runtime compatibility, and adapter version assumptions in CI. Do not require shipping the full Ghostty application or its platform renderer.
- Linux can run adapter/model/config/pane-tree tests where portable, parser fuzz/replay and static checks; an eventual cross-compiled `.exe` is not native verification. No claim that Wine validates ConPTY, TSF, UIA, DWM or GPU drivers.
- Native Windows CI: x64 build/unit/ConPTY smoke/replay/exit tests and artifact creation. Add ARM64 build/smoke separately. Public CI can verify compiler/ABI and many process tests; hosted runners cannot certify interactive IME, real mixed-DPI monitors, Snap UI, GPU performance, RDP/device loss, or screen-reader usability.
- The user/parent still needs a Windows hardware/VM verification path for the interactive matrix. Root reports planned native CI availability; no such workflow was created by this worker. No new Codex task may be started without the user's approval.
- Package an unpackaged portable build for early testers; signed per-user MSIX as a release path, with installer/portable alternative only when needed. MSIX can package a classic full-trust desktop app; it does not magically sandbox a terminal that launches arbitrary user programs. Keep signing credentials outside ordinary build jobs; release publication requires authorization. [MSIX packaging overview](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/packaging/).
- If WinUI is chosen later, explicitly budget framework-dependent versus self-contained runtime deployment and servicing rather than treating that dependency as free. [Windows App SDK deployment](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/deploy-overview).

## Security boundaries

The shell is intentionally powerful; StormTTY is not a sandbox. Treat all terminal output, fonts/images/config files and shell-integration markers as untrusted data, not instructions or authority.

- VT OSC/DCS/actions must enter a policy gateway: no automatic arbitrary command execution, file writes, downloads, external URL launches, or clipboard reads from terminal output. Clipboard writes need visible/user-configurable policy; hyperlink activation requires a user gesture and scheme validation.
- Limit encoded and decoded image sizes, dimensions, animation frames, total pixels, decode time, scrollback bytes, escape payloads and retained metadata. Do not assume a graphics protocol's filename transport authorizes arbitrary local file reads. Disable or tightly broker file/shared-memory transport until audited.
- Spawn only explicit trusted/discovered executable paths and correctly escaped argument lists; do not interpolate shell titles, URLs or profile labels into commands. WSL launch parameters are data, not a command shell template.
- No elevation by default. Isolate elevated sessions explicitly if later implemented. ACL any future IPC to the same user/session; do not open unauthenticated local control endpoints.
- No executable plugins/config or automatic network fetches initially. Logs/crash reports must not include terminal contents or secrets by default; telemetry is opt-in/minimal.
- C++ guardrails: fuzz adapter input, bounds-check copies and integer dimensions, ASan in supported builds, control-flow protection and hardened release builds, static analysis, dependency/SBOM review. Memory safety is a genuine Rust advantage, not erased by RAII.

## What remains unverified

No Windows application was built or run in this research worker. The architecture recommendation is evidence-based but **ConPTY behavior, TSF/UIA integration, custom caption behavior, visual fidelity, device recovery and every numerical budget remain native acceptance tests**. The highest-risk open loop is fixed-cell shaping + IME/UIA, not whether D3D can draw a fast rectangle. The next authorized implementation should prove those contracts before scaling up the product.
