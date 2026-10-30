# Completing the responsibility refactor

The goal is a codebase whose ownership and change locations are clear, including
the renderer—not merely a smaller window file. This is a finite pass over the
existing application, not a rolling feature roadmap. The [architecture guide](architecture.md)
records current owners; this plan identifies the remaining responsibility groups
and the evidence needed to close them.

## Remaining groups

### 1. Windows application and view transactions

`src/platform/windows/window.cpp` still contains `NativeApplication`,
`NativeWindow`, `ApplicationActors`, `ActorLifetimeRegistry` and `RendererReadyWake`.
Move existing application lifetime and view-creation/retirement machinery into
private modules with explicit dependencies. Preserve unpublished-candidate
rollback, exact actor ledgers, close authorization, final wake delivery and
readiness-wait draining. A view must not acquire application-wide authority through
a shared context or a map-access facade. This work does not enable New Window or
grouped persistence.

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

`NativeWindow::scene` still assembles workspace rail, footer, pane-frame and overlay
values. Separate these domain compositions from host interaction and native drawing,
using explicit content/layout/style inputs. `RibbonRenderer` and
`WindowHeaderRenderer` already demonstrate this boundary; do not rebuild them.
Preserve chrome order, clipping, stable pane identity and fractional-DPI geometry.
A composition helper should not look up actors, measure through a whole renderer
context or own native resources.

### 4. macOS painters and host

`MetalCompositor` and scoped `MetalFrame` implement the device/submission boundary;
the separate exceptional-encoder cleanup correction passed the native macOS gate.
See [compositor verification](../tests/macos_metal_compositor_verification.md) for
exact source/run evidence and limits; this does not qualify physical presentation.
Separate remaining terminal/chrome paint policy in
`src/platform/macos/renderer.mm` from device submission and presentation. Keep
`CoreTextFontResources`, `CoreTextRowRasterizer` and `MetalTextureResources` as their
existing owners. In `src/platform/macos/window.mm`, separate application/session,
view and input responsibilities within `Host` without imposing Windows lifecycle
symmetry. ARC resource retention, completion-generation state and AppKit close/Quit
semantics require native characterization, not mechanical translation.

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
