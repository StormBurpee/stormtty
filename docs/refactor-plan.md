# Completing the responsibility refactor

The goal is a codebase whose ownership and change locations are clear, including
the renderer—not merely a smaller window file. This is a finite pass over the
existing application, not a rolling feature roadmap. The [architecture guide](architecture.md)
records current owners; this plan identifies the remaining responsibility groups
and the evidence needed to close them.

## Remaining groups

### 1. Windows application and view transactions

Three private boundaries are implemented: `application_actors.hpp/.cpp` owns
registry/disposal coordination; `native_wake.hpp/.cpp` owns notification mailboxes
and renderer readiness waits; `view_creation.hpp/.cpp` owns the unpublished
candidate's model transaction, exact actor ledger and deferred dirty state.
Creation rollback follows native candidate destruction; catalog commit retains
the publication guard until the host transfers native ownership. Persistence
notification remains outside the creation catch.

Group 1 is still open. `NativeApplication` and `NativeWindow` in
`src/platform/windows/window.cpp` retain these responsibilities:

- Native creation/admission, raw view registration, reserved publication and
  resource destruction. The creation owner does not own or expose the view map.
- Retirement outside WndProc, after persistence completion draining. Preserve
  pending Settings-work deferral and completion reporting before detachment,
  plus the existing persistence and last-window gates.
- Close authorization and catalog revalidation, exact actor disposal with final
  completion-route rebinding/wake delivery, view destruction, and final persistence
  flush ordering. Preserve readiness-wait cancellation/draining before native
  resource replacement or teardown.

Continue separating coherent responsibilities through narrow dependencies, not
shared contexts or map-access facades. Keep the transaction's explicit failure
order and survivor edits/actors intact. This work does not enable New Window or
grouped persistence.

For the creation slice at `7542710`, portable model coverage passed 1,811 checks;
strict Windows syntax checks cover the owner, lifetime tests and host. The native
Windows gate passed 70/70, including execution of both captured legacy and owned
creation fixtures. The legacy implementation was captured before extraction but
was **not executed natively before extraction**.

The [full Windows run](https://github.com/StormBurpee/stormtty/actions/runs/41098888113)
failed: packaged `smoke-layout` reached its stage 0 deadline, bundled
`smoke-settings` reached its stage 8 deadline, and the repeat summary reported
1/10 with return code 2. All 13 repeat receipts reported `reaped=true` and
`timed_out=false`; successful process reaping is not scenario success. Failure
classification remains under review, so these results do not establish a cause
or a fully green shell run. They also do not qualify physical UI or macOS parity.
See [creation verification](../tests/view_creation_verification.md) for coverage
and source-characterization limits.

### 2. Windows input and interaction

Separate input delivery/backpressure, IME and gesture lifetime from view orchestration,
and give remaining palette/modal interaction coherent homes. Keep keyboard, text,
paste, pointer capture and focus transitions ordered; rejected input must not be
lost or leak to the terminal beneath an overlay. `TerminalFindController` is already
separate, and `WorkspaceNavigator` now owns its visible interaction lifecycle.
Its host retains live catalog sampling, activation, native input and scheduling;
those are not moved into the navigator. Continue with adjacent interaction state
without reopening these established owners. Host-native routing must remain
distinct from portable policy.

### 3. Remaining scene composition

`NativeWindow::scene` still samples footer content and assembles pane-frame and
overlay values. `WorkspaceRailRenderer` now owns rail visual composition from
explicit geometry, resolved workspace rows and style; the host retains live
catalog/watch sampling, transitions, input and layout. Its frozen oracle and
actual-host-fragment trace provide portable equivalence, not native UI evidence.
See [rail verification](../tests/workspace_rail_renderer_verification.md).
Separate the remaining domain compositions from host interaction and native drawing,
using explicit content/layout/style inputs. `RibbonRenderer` and
`WindowHeaderRenderer` already demonstrate this boundary; do not rebuild them.
Preserve chrome order, clipping, stable pane identity and fractional-DPI geometry.
A composition helper should not look up actors, measure through a whole renderer
context or own native resources.

### 4. macOS painters and host

The macOS renderer now coordinates rather than implementing both paint domains:
`ChromePainter` owns per-item chrome primitives/text, and `TerminalPainter` owns
pane backgrounds/selection, image layers/crops, row dispatch and cursors. Both
borrow an admitted `MetalFrame` synchronously. Scene admission, canvas/pane order,
viewport reset, chrome traversal, submission and success-only scene-history/
statistics updates remain with the renderer.

`CoreTextFontResources`, `CoreTextRowRasterizer`, `MetalTextureResources` and
`MetalCompositor` remain the font, CPU row, texture/accounting and native device/
submission owners. Preserve fresh upload allowances, charged failures, cursor
clip lifetime, mixed terminal/chrome alpha and exceptional-frame recovery.

Terminal painter native qualification passed 60/60, including the new fixture;
see [terminal painter verification](../tests/macos_terminal_painter_verification.md)
and [the native run](https://github.com/StormBurpee/stormtty/actions/runs/41105869939).
The original algorithm was frozen before extraction but not executed natively
before extraction. Linux and Windows baseline 70/70 passed. The Windows full run
failed packaged layout (actor 5 stale suffix at 33×13, exit 0/marker 0), packaged
Settings and repeats `explicit2`/`explicit6` (malformed bracketed paste). All 10
bundled/developer cases passed; 29 compact receipts recorded 25 successes/four
failures, all reaped without outer timeout. No shell fix or full-green result is
claimed; physical UI and performance remain unproven. Earlier chrome/compositor results
and limits remain in their [chrome](../tests/macos_chrome_painter_verification.md)
and [compositor](../tests/macos_metal_compositor_verification.md) verification notes.

Group 4 remains open for `Host` in `src/platform/macos/window.mm`: separate
application/session, view and input responsibilities, including AppKit close/Quit
semantics, without imposing Windows lifecycle symmetry. ARC resource retention
and completion-generation state require native characterization, not mechanical
translation. Do not reopen cohesive painter/resource owners merely to reduce file
size. Neither extraction nor offscreen success activates held presentation work
or paused features.

### 5. Terminal search and frame materialization

`src/terminal/terminal.cpp` combines the VT adapter with incremental search
allocation/handles/results and frame/image snapshot materialization. Give those
algorithms focused collaborators while `TerminalCore` remains the single mutation
and actor-thread authority. Preserve search budgets and stale-token rejection,
owned immutable frames, callback failure behavior and borrowed Ghostty lifetimes.
Do not introduce another terminal model, actor, lock or unbounded snapshot copy.

### 6. Windows document-service modules

`src/platform/windows/settings_services.cpp` combines bounded shell discovery,
guarded atomic file transactions and document-specific adapters. Separate these
responsibilities while retaining one security/commit-lock authority. Exact same-read
save tokens, path/link checks, DACL preservation, legacy backup verification and
atomic replacement ordering must not diverge between document types. Existing
Settings and workspace worker owners remain separate from the synchronous file
service. The current single-window envelope integration is not a missing v1-to-envelope
migration; see [application-layout storage](application-layout-storage.md).

### 7. Trusted managed-shell modules

`tools/shell-preset.zsh` combines private state/completion initialization with
context/Git protocol and prompt hooks. Split trusted packaged modules along those
boundaries while preserving source order, initialization acknowledgement, validation,
user opt-in sourcing and package inventory. No startup network calls, new plugin
manager or silent sourcing of unrelated user files. Bootstrap, addon/tool packaging
and notice scripts already have coherent purposes; this is not a rewrite of them.

## Keep cohesive owners cohesive

Extraction is not a class-count target. Windows resource owners, painters and the
Renderer frame coordinator are established boundaries. So are the Session actor's
queue/backpressure/retained-document ordering and the ConPTY/POSIX transports;
workspace/catalog/application models; identity, attention and observation owners;
Settings and workspace workers; and bounded config/codecs and PNG adapters.

`SettingsPanel` and `ContextMenu` may own layout and paint within their UI domain
without becoming generic host contexts. `WindowsSessionLauncher`, shell-state
security, startup dialogs and pure geometry helpers do not need another wrapper
merely because they have several methods. Embedded qualification code may move to
make production files readable, but it is not another production domain.

AI implementation, Liquid Glass/HDR work and all Leyline work remain paused. Preserve existing
contracts and regression coverage without using this refactor to activate new
capabilities. Vendored code and diagnostic-only branches are outside this ownership
pass. Known shell qualification failures—malformed `[200~` paste input, stale layout
markers and `history.new` failures—remain separately diagnosed issues, not reasons
to weaken a refactor gate or hide a failure behind longer timeouts.

## What closes the pass

For each group, record a small responsibility inventory: the owner, its callers,
its resources, and whether it is cohesive, still pending or intentionally paused.
A group closes when:

- Lifetime, thread, borrowing, cancellation and failure-order contracts are explicit
  and enforced by the actual boundary. Dependencies are narrow values or specific
  capabilities, not an accessor-only facade over the former host.
- Characterization protects existing behavior before movement; focused tests and
  affected native integration gates pass for the exact resulting source. Failures
  and unrelated diagnostics stay visible rather than being relabeled as success.
- Names explain the domain, touched code has readable spacing and logical grouping,
  and a maintainer can locate a behavior change without tracing a whole host.
- The architecture guide reflects the implemented boundary and remaining limitations.

Portable tests, cross-compilation, native runtime and GUI/physical verification are
different evidence. Offscreen graphics tests do not qualify onscreen presentation,
IME, accessibility or hardware behavior. Follow [CI coverage](ci-coverage.md) and
[acceptance gates](acceptance-gates.md); do not claim platform parity from shared
models or test success on the other platform.

Closing these groups completes this responsibility pass. It does not mean every
bug is fixed or every desired feature exists. New Window, broader platform parity,
new graphics features and unrelated cleanup require their own scope rather than
keeping this refactor permanently open.
