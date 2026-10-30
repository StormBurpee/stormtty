# Architecture and ownership

StormTTY is a native terminal: portable C++20 workspace and terminal models,
with Win32/DirectWrite/Direct2D on Windows and AppKit/CoreText/Metal on macOS.
This guide describes the current ownership boundaries, not a completed refactor
or a claim of platform parity. The detailed contracts remain in
[AGENTS.md](../AGENTS.md) and the component documents linked below.

## From input to a frame

```text
NativeWindow → Session commands → terminal actor → TerminalCore
                                  ↕
                              Process / PTY
                                  ↓
                         owned immutable RenderFrame
                                  ↓
NativeWindow builds WindowScene → Renderer → canvas → presentation
```

`Session` is the thread-safe host interface (`include/stormtty/session.hpp`,
`src/session/session.cpp`). Its actor owns `TerminalCore`, including construction
and destruction. The PTY reader copies bytes into the bounded actor queue; it
never calls the VT engine or touches a window. UI actions enqueue commands rather
than reading or mutating terminal internals.

`Process` owns asynchronous transport and child lifetime. Its implementations are
`src/platform/windows/conpty_process.cpp` and `src/platform/posix/pty_process.cpp`.
Change native launch, transport, resize or reaping here; change terminal protocol
behavior in `src/terminal/terminal.cpp`. Neither belongs in a painter or window
input handler.

`FramePtr` is `shared_ptr<const RenderFrame>` (`include/stormtty/frame.hpp`).
Frames own their data: never retain borrowed Ghostty pointers across mutation or
threads. A frame can outlive its actor. Process exit, retained read-only document
lifetime, and final actor disposal are separate states; `process_finished()` is
not `closed()`.

Queues, parsing work, scrollback and image decoding must remain bounded. PTY
backpressure waits rather than discarding output. Transport input enqueue is all-or-none;
retain and retry bytes on `would_block`. Terminal titles, paths, links, clipboard
requests and image payloads are untrusted data, not authority to execute anything.
See [the terminal adapter](../src/terminal/README.md) and
[retained documents](retained-terminal-documents.md).

## Application lifetime versus a view

The Windows `NativeApplication`, `NativeWindow`, `ActorLifetimeRegistry` and
`ApplicationActors` currently live in `src/platform/windows/window.cpp`. These
are real ownership boundaries, but the host is not yet separated into focused
implementation files.

**`NativeApplication` owns application lifetime:** native view ownership and the
message pump, `ApplicationWorkspaces`, actor registration, and the
`ApplicationWorkspacePersistence` owner. **`NativeWindow` owns one view:** HWND, renderer, input/focus and
layout state, editor presentation, and a `WindowWorkspaceView` scoped to its
`WindowId`. Change placement rules in `application_workspaces.hpp` and
`src/core/application_workspaces.cpp`, not by giving a view another catalog.
Reacquire model references after mutation; placement changes can invalidate them.

Creating a view is a transaction. An unpublished model candidate and exact actor
creation ledger permit rollback without removing a survivor's actors. Retiring
unused actors checks the whole application catalog, including hidden and parked
sessions. Closing a placement is not permission to close unrelated processes;
close previews revalidate ownership and session associations before removal.

Native resources use RAII; exceptions must not cross Win32, COM or C callbacks.
Do not join actors or wait for process exit in a window callback. Renderer-ready
waits are canceled and drained before resize, recovery or view destruction.
Actor callbacks retain routes with weak endpoints, not raw view pointers: removing
registry discoverability must not lose final completion delivery. See
[application windows](application-windows.md).

### Launch policy, identity and attention

- **Shell/profile selection:** `WindowsSessionLauncher`
  (`include/stormtty/windows_session_launcher.hpp`,
  `src/platform/windows/session_launcher.cpp`) owns immutable launch capabilities
  and prepares independent `ProcessOptions`/`TerminalOptions` values from current
  configuration. It does not start/register actors, retain the caller, save config
  or mutate the ambient environment. The host still performs the launch transaction.
- **Actor lifetime:** `ActorLifetimeRegistry` owns registered `Session` handles,
  identity policy and wake-route discovery. Its borrowed lookups are UI-thread-only
  and cannot survive erasure or reentrant host work; use explicit retained ownership
  for queued work. `SessionIdentities` (`session_identities.hpp`) is the portable
  counter/incarnation policy, not another registry. Session IDs never recycle;
  placement and process state do not change an actor incarnation.
- **Watch/Activity behavior:** `SessionAttention` (`session_attention.hpp`) owns
  watch and parked-attention policy, not actors. Validate actions against current
  identities. `SessionObservationCache` (`session_observation_cache.hpp`) separately
  owns the application-shared last-observed state/label values used for invalidation.
  It does not become authoritative Session state or format labels.
  `ApplicationActors::dispose` coordinates registry removal, cache cleanup and
  `attention.forget`; do not create a second disposal path.

See [actor lifetime](actor-lifetime-registry.md) and
[session attention](session-attention.md) for ordering and stale-action contracts.
[Observation caches](session-observations.md) records the inherited shared-history
and first-observation rules.

### Terminal Find lifecycle

`TerminalFindController` (`include/stormtty/terminal_find_controller.hpp`,
`src/session/terminal_find_controller.cpp`) owns the UI-thread bounded query editor,
bound session ID, request ID, owned search snapshot, initial-selection intent and
error. Editing does not submit implicitly. The Windows host retains the actor for
each synchronous `Session&` borrow; the controller retains no actor, registry,
window or callback. Retained exited documents remain searchable.

Keep snapshot adoption distinct from selection: after changed-result adoption,
the host invalidates chrome and records diagnostics before consuming/submitting
the initial token. Consumption is one attempt per exact token, including rejected
submission. Explicit navigation cancels automatic intent before actor lookup and
uses a fresh actor snapshot's token, not cached positions. Submission clears the
old selection before queuing replacement search or empty-query cancellation.

The host retains input precedence, IME, focus, clipboard, pointer/geometry,
timers, rendering and old-actor cancellation. Both focus-loss reset and explicit
close preserve query text while unbinding and dropping request/result/selection
intent; only explicit close clears the transient error, and focus loss must not
acquire explicit-close scheduling effects. See
[Find controller verification](../tests/terminal_find_controller_verification.md)
for lifecycle and no-terminal-input-leakage gates; this is not macOS integration
or physical IME/UI qualification.

### Workspace Navigator interaction

`WorkspaceNavigator` (`include/stormtty/workspace_navigator.hpp`,
`src/core/workspace_navigator.cpp`) owns its value catalog and visible interaction
lifetime: background filtering, frozen Activity project scope, bounded query editor,
control focus and switch-button gesture identity. Catalog initialization is not
overlay visibility. `begin` resets the interaction; close preserves catalog/query/
focus/filter while dropping visibility, pressed eligibility and Activity scope.

The host still samples and sorts catalog values, admits overlays, performs live
identity/placement validation and activates/restores the selected target. Ordinary
catalog replacement follows the existing selection-based open policy; Activity
refresh preserves stale selection instead of silently retargeting it. Both reapply
the editor query. Preview never activates a project, launches a dormant shell or
acknowledges a watch.

Switch release consumes one-shot eligibility by comparing the captured optional
target with the current one. Two null targets intentionally remain eligible for
the host's stale-selection notice; this is not permission to activate a stale
actor. Host capture/IME/input routing, timers, rendering and project/actor authority
remain outside the owner. See [navigator interaction verification](../tests/navigator_interaction_verification.md)
for refresh, cancellation and native progression gates; this does not establish
physical input or macOS interaction parity.

## Settings and persistence

`SettingsPanel` (`settings_panel.hpp`, `src/core/settings_panel.cpp`) owns the
editor draft and dirty baseline, and emits preview/cancel intents for the host to apply.
`SettingsDocumentSession` (`settings_document_session.hpp`,
`src/core/settings_document_session.cpp`) owns serialized document I/O, open intent,
operation/generation checks and the exact same-read save token. It retains no live
Config, renderer, window or actor. Workers receive values and a weak wake endpoint;
completion returns outcomes for the UI owner to apply.

Keep parsed config and exact file bytes/identity/timestamp from the same read.
Never refresh a token silently to make stale edits save. Failed saves retain the
draft; only a successful guarded save advances the baseline and allows full draft
application. Preview/Cancel do not write. Closed views discard abandoned editor
results; pending file work must not block surviving views. Normal worker reap
requires a published completion; destruction/join happens outside native callbacks.
The native adapter is `src/platform/windows/settings_document_services.cpp`; guarded
file operations are in `settings_services.cpp`. See [settings services](settings-services.md).

Workspace persistence is a different, application-owned writer, not one writer per
view. `ApplicationWorkspacePersistence` (`application_workspace_persistence.hpp`,
`src/core/application_workspace_persistence.cpp`) owns the document, exact save token,
serialized worker and completion mailbox. The host retains native timers, snapshot
capture, one-window admission and error presentation. No live model or window is
retained by the writer; workers receive copied values and a weak wake endpoint.

The host's 1500 ms save timer starts at the first edit, not after every edit. A busy
save defers before snapshot capture and coalesces to one pending request; the next
capture uses the current model. Successful completion advances only the file token,
not the live document or final-close history. Failed writes invalidate the token
before fallible UI error presentation. Final-close history is recorded before the
shutdown drain; all joins remain outside WndProc. The inherited final-capture-failure edge
remains: an older successful completion can reinstall its token.

The native adapter in `src/platform/windows/workspace_persistence_services.cpp`
loads and saves the bounded application-layout envelope through
`load_application_layout_snapshot` and `save_application_layout`
(`application_layout_services.hpp`, implemented in Windows `settings_services.cpp`).
Legacy v1 libraries are imported without writing on read; migration on save requires
an exact, permission-preserving `.v1-backup` before guarded atomic replacement.
The host supports one persistent window: running checkpoints and clean individual
close are integrated, but grouped startup, explicit application-wide Quit history,
normal New Window and second-executable ownership remain gated. Unsupported selected
multi-window recovery records are refused, not flattened or overwritten. See
[application-layout storage](application-layout-storage.md) and
[workspace-persistence verification](../tests/application_workspace_persistence_verification.md).

## Rendering: scene policy is not a device

`WindowScene` (`include/stormtty/renderer.hpp`) carries owned frame references and
value-based chrome. The host owns geometry, interaction, animation deadlines and
identity; stable pane IDs must not become array indexes.

`RibbonRenderer` (`ribbon_renderer.hpp`, `src/core/ribbon_renderer.cpp`) is stateless
Ribbon paint composition: it appends scene primitives/text from supplied layout,
content and style. It does not measure native text, inspect actors or own graphics
resources. Change Ribbon appearance here; change hit testing or actions in the host.

`WindowHeaderRenderer` (`window_header_renderer.hpp`,
`src/core/window_header_renderer.cpp`) composes the native Windows crown, caption
buttons and tabs into that scene. Its synchronous view borrows layout and a span
of tab presentations with owned labels; resolved animation values carry no clock,
model or transition authority. It appends base-layer primitives/text without
clearing existing scene content or introducing clips. The host retains fullscreen
admission, the backdrop alpha change used by later surfaces, stable tab identity,
transition insertion/sampling, hover lookups, timers, hit testing and actions.
Change header appearance here; change host state or input routing in the host.
The independent emission oracle verifies resolved-value equivalence, not identical
wall-clock hover sampling or physical frame pacing. See
[header verification](../tests/window_header_renderer_verification.md).

`SceneDamageTracker` (`scene_damage.hpp`, `src/core/scene_damage.cpp`) owns the scene
represented by the persistent canvas, on the renderer thread. It retains immutable
frames, not GPU resources or presentation state. Commit after successful `EndDraw`,
before `Present`; a busy/occluded presentation does not undo a completed canvas draw.
Surface/font/material/device changes and draw-body exceptions require a repaint;
the last committed scene remains owned until the next successful commit.

`TextResources` (`src/platform/windows/text_resources.hpp` and `.cpp`) owns
Windows device-independent DirectWrite resources: font configuration and metrics,
bounded row shaping, chrome text layout/measurement and color-glyph enumeration.
It is renderer-thread-confined and retains no window, GPU cache, painter or scene.
Its COM apartment must outlive it and returned COM objects. Returned row shapes
are borrowed only until the next shape/configuration call; uncached results use
caller-owned scratch. Consume them synchronously, never retain them in frames or
painters. A font/DPI remeasurement requires the caller to invalidate the scene.
GPU device recreation preserves this CPU cache; font/DPI remeasurement clears it.

`ImageCache` (`src/platform/windows/image_cache.hpp` and `.cpp`) owns
device-dependent bitmap residency and upload policy. It borrows the D2D context
only for lookup/upload, and returns a bitmap for immediate synchronous painting.
Placement, cropping and layer order stay with the painter. Ordinary resize, font
and DPI changes preserve these images; device replacement clears them before D2D
resources are released. Residency is decoded cache-byte accounting, not a claim
about total driver memory. See [image-cache qualification](../tests/image_cache_verification.md).

On macOS, `CoreTextFontResources` (`src/platform/macos/core_text_font_resources.hpp`
and `.mm`) owns the CoreText font reference, validation and measured cell metrics.
Rasterization borrows the font synchronously. The renderer still owns font options,
DPI, row generation and invalidation; Metal resources and asynchronous presentation
are separate. Its row cache contains GPU raster textures, not Windows-style CPU
shaped rows, so the backends do not share cache-lifetime assumptions.

`CoreTextRowRasterizer` (`src/platform/macos/core_text_row_rasterizer.hpp` and `.mm`)
owns synchronous CPU row shaping and rasterization. It borrows immutable cells,
`CTFontRef`, locale and explicit geometry/style options only for one call, returning
owned premultiplied sRGB BGRA bytes. It retains no font, pane, host, Metal object or
cache. Trait fonts, attributed text and the CoreText line stay scoped to CPU work;
the line survives decoration drawing, and the bitmap context releases its backing
store reference before owned pixels return to the renderer.

`MetalTextureResources` (`src/platform/macos/metal_texture_resources.hpp` and `.mm`)
owns GPU row/image residency, identity keys, serial-based eviction, resident-byte
accounting and cumulative upload/hit counters. It borrows device, font and pixels
only during calls; CPU shaping/rasterization remains in `CoreTextRowRasterizer`.
Row keys include blink/cursor variants; row hits avoid rasterization. Rows evict
only after successful upload, including oversized uncached rows. Images evict
before upload, so a failed upload does not restore evicted images. Equal-recency
ties follow ordered key traversal. Do not substitute Windows cache policy here.

Row, image, white and chrome textures share one 64 MiB upload allowance.
Rasterization checks an allowance snapshot; upload reserves bytes before dimension
and native-allocation checks, without rollback. A batch starts at device creation
and after drawable acquisition, not on busy, unchanged or occluded returns.
Clearing residency neither starts a batch nor resets cumulative counters. The
CPU rasterizer's 4,096-cell guard retains its existing failure order after
bitmap/context/attributed-string creation.

The renderer requests row invalidation on font reset (even the same font) and DPI
changes. Same-DPI resize and opacity changes preserve both caches. Successful
device reconstruction clears both at its existing late boundary after white/layer
creation; partial setup failure does not gain an early cache clear. The renderer
retains font/DPI geometry, scene comparison, paint policy and public counters.
`MetalCompositor` now owns serial advancement, device/queue/pipelines/layer,
composition target, readback and shared completion/presentation generations—not
texture cache or scene policy.

Cached textures and encoding locals are strong ARC references. Ordinary retained-
resource command buffers retain encoded textures through GPU completion even if
the cache is evicted, cleared or destroyed. Completion blocks must not borrow the
texture owner; existing shared completion-generation state stays separate. These
are resource-lifetime contracts, not proof of onscreen or physical presentation.
See [Metal texture verification](../tests/macos_metal_texture_resources_verification.md).

`MetalFrame` is a noncopyable, synchronous paint borrow of the compositor and
texture owner. Concrete fill/row/image/BGRA calls upload and encode immediately;
no raw device getter or general callback context is exposed. Admission retains
the suspended/bounds/error/occluded/unchanged/busy ordering, two in-flight commands
per generation and monotonic serials across nontransactional reconstruction.
Callbacks capture only shared generation state; the view remains weak. An early
optional compositor slot plus explicit owner-generation release preserves the
external texture/font owners' teardown position before the native graph.

Explicit submit ends encoding and commits; renderer history follows submission.
A separately disclosed robustness correction also ends an active encoder during
CPU-exception abandonment or failed construction, exactly once. Cleanup never
submits, presents, waits, increments in-flight or rolls back charged upload bytes.
Native tests exercise actual subsequent-frame recovery, not merely process-exit
acceptance. Readback retains aligned shared/managed storage and its own wait;
onscreen readback still requires positive `presentedTime`. These are not Windows
pending-present semantics. See [compositor verification](../tests/macos_metal_compositor_verification.md)
for the diagnostic failure and later native recovery evidence.

Native CPU characterization compares all decoration styles at 96/192 DPI against
the pre-extraction row algorithm, separately from Metal renderer integration tests.
The original dotted zero-length-dash path can be invisible at 96 DPI on the tested
CoreGraphics runtime; exact-pixel parity records that existing limitation rather
than repairing it in an ownership extraction. This is not physical UI or onscreen
presentation qualification; see the [macOS limits](../src/platform/macos/README.md).

`ChromePainter` (`src/platform/windows/chrome_painter.hpp` and `.cpp`) executes
the ordered native chrome drawing, separate from domain scene composition such
as `RibbonRenderer`. `PaintSurface` is a synchronous drawing borrow with scoped
clip/opacity state; it owns no GPU resources and exposes no resize, presentation
or frame-transaction control. `Renderer` coordinates the draw transaction, damage
commits and presentation; `RenderDevice` supplies the scoped native draw borrow.

`TerminalPainter` (`src/platform/windows/terminal_painter.hpp` and `.cpp`) owns
terminal paint policy: selection and colors, glyph runs and decorations, image
placement/cropping/layers and cursor drawing. A per-call pass borrows only
`TextResources`, `ImageCache`, `PaintSurface` and resolved DPI/opacity/tick values.
Rows, color-glyph layers and image bitmaps are consumed synchronously; none is
retained by the painter. `PaintSurface` submits images immediately after cache
lookup and exposes narrow glyph operations, not a raw context getter.

Terminal background/image/text/decorations/cursor ordering remains below chrome.
Physical-pixel geometry, clipping and DPI rounding are unchanged. Cache owners
retain their cumulative counters, including progress before a later exception;
no painter-owned counters or successful-frame-only statistics are introduced.
The shared Windows HRESULT classifier preserves bounded device-loss recovery and
unsupported color-glyph fallback. See [terminal-painter verification](../tests/terminal_painter_verification.md).

`RenderDevice` (`src/platform/windows/render_device.hpp` and `.cpp`) owns the
renderer-thread native resource graph: D3D/D2D devices and contexts, canvas and
brushes, alpha-readback staging, swapchains and latency handle, DirectComposition,
opaque presentation child window, HostBackdrop and liquid-glass resources. It
retains no scene, damage history, painter, text owner or image-cache policy. The
caller's COM apartment outlives this graph and `TextResources`.

`CanvasDraw` is a noncopyable synchronous borrow of that canvas and its
`PaintSurface`. The device/canvas must outlive drawing and clip scopes; no resize,
reset or presentation is valid during the borrow. `finish()` ends drawing once;
unwinding ends an unfinished draw without replacing the original paint exception.
`Renderer` retains scene/damage coordination, DPI/tick, public statistics and
`pending_present`: commit follows successful `EndDraw`, and busy/occluded Present
keeps the completed canvas pending. `SurfaceChanges` distinguishes repaint from
successful canvas installation; apply it on success and exceptions, clearing
pending presentation only when a new canvas was actually installed.

GPU recreation preserves `TextResources`. Normal device release unbinds the D2D
target, releases staging/canvas/brush, explicitly clears the external `ImageCache`,
then releases D2D contexts/device and presentation/D3D resources. Final shutdown's
bounded completion query precedes resource release; replacement draining preserves
the live-HWND distinction. The host must cancel/drain readiness waits before their
borrowed latency handle is replaced or destroyed.

Failed renderer construction is a different path: reset image ownership, release
brush/canvas/staging slots, then destroy text before the remaining native graph
unwinds in declaration order. Do not replace this with normal shutdown: the failure
path deliberately adds no target unbind, completion wait or ClearState/Flush.

Readback privacy belongs to the native canvas owner. Active glass or a canvas still
tainted by glass blocks pixel readback and alpha probes. Stopping glass alone does
not clear that state; only a successful full non-glass repaint clears the taint.
Partial or aborted repaint cannot make stale desktop pixels readable. See
[render-device verification](../tests/render_device_verification.md) for release
ordering and native failure-injection coverage.

## Remaining ownership work

The [finite refactor plan](refactor-plan.md) defines the remaining responsibility
groups and their completion criteria; it is separate from new feature work.

`src/platform/windows/renderer.cpp` remains the frame-transaction coordinator,
not the native resource owner. Further changes must preserve the distinction
between canvas validity, resource lifetime and presentation readiness rather than
recombining those states behind a shared device/window context.
Observation caches are encapsulated, but deliberately remain application-shared;
changing that lifetime requires separate transfer/invalidation characterization.

Host input, persistence integration and embedded qualification harnesses also need
continued separation. Prefer explicit values and narrow capabilities; do not move
the same coupling behind a shared “context” or an accessor-only facade.

macOS has its own `Host` in `src/platform/macos/window.mm` and renderer in
`src/platform/macos/renderer.mm`. Shared models do not imply shared native lifecycle
or feature parity. AppKit ownership and close/Quit behavior need their own work and
native evidence; consult the [macOS host notes](../src/platform/macos/README.md).

## Verify the boundary you change

Use the existing CMake build instructions in [README](../README.md); keep build
and temporary storage workspace-backed. Run focused tests, then affected integration
gates. Useful starting points:

- Placement/lifetime: `application_workspaces_tests`, `session_identities_tests`,
  `session_wake_router_tests`, `session_attention_tests`,
  `session_observation_cache_tests`, `navigator_interaction_tests`, then native ownership,
  project, modal and retirement scenarios.
- Settings/launch: `settings_panel_tests`, `settings_document_session_tests`, then
  Windows `settings_services_tests`, `windows_session_launcher_tests` and
  `native_settings_smoke`. Exercise conflicts, Cancel/reopen and close during I/O.
- Rendering: `ribbon_renderer_tests`, `scene_damage_tests`, then Windows
  `text_resources_tests`, `image_cache_tests`, `paint_surface_tests`,
  `render_device_tests`, `renderer_smoke`/`renderer_final_teardown`, `renderer_png`
  and relevant native layout/material
  scenarios. Include immutable full-versus-incremental terminal pixel comparisons
  at multiple DPIs, resize, font/DPI changes, occlusion and resource recovery.
- macOS typography/rendering: `macos_font_resources_tests`, `macos_row_rasterizer_tests`,
  `macos_metal_texture_resources_tests`, `macos_metal_compositor_tests`,
  `macos_renderer_tests` and `macos_png_tests`; Windows graphics checks do not qualify
  this backend.
- Actor/VT changes: `session_tests`, `terminal_tests`, `terminal_search_tests`,
  plus the affected real transport/provider tests and complete process exit.

Report portable tests, Windows cross-compilation, native runtime and physical UI
verification separately. None substitutes for another. Existing smoke fixtures are
not proof that normal persistent multi-window use, IME, accessibility or platform
parity is ready. [Acceptance gates](acceptance-gates.md) and
[CI coverage](ci-coverage.md) describe the wider evidence requirements.
