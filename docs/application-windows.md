# Application window ownership foundation

This is portable model infrastructure, **not a shipped native New Window UI**.
The Windows host uses scoped ownership but still creates only one native window;
macOS integration is pending. The portable model adds no process, Session,
filesystem, IPC, renderer, background service or persistent format dependency.

## Ownership

`ApplicationWorkspaces` owns one existing `ProjectWorkspaceCatalog`. Its windows
contain ordered references to the catalog's projects, with an independent active
project per window. Every catalog project belongs to exactly one window. An empty
window is valid. WindowIds are monotonically allocated within this application
model, never reused there, and are not disk identities or OS handles.

Use `window(id)->active`, **not** `catalog().active_id()`, to render a window.
The catalog's legacy active selection remains an implementation detail and keeps
its original standalone semantics. There is no duplicated workspace model.

`create_window()` deliberately creates an empty placement container. The host
must explicitly create its fresh workspace, launch terminals and handle native
creation failure. `create_workspace()` preserves the catalog's default of only
selecting the first project; selecting another is explicit.

`move(project, expected_source, destination)` validates all ownership before
preparing a copy of the window placements and committing by swap. It never
removes or recreates a catalog project. Runtime ProjectWorkspaceId, persistent
metadata identity, local pane/tab/node IDs, layout, SessionIds and parked entries
remain unchanged. The destination selects the moved workspace. If the source's
active workspace moves, the next row (or previous at the end) becomes active.
Moving its final workspace leaves the source window empty, not closed. Moving
to its current owner is an exact no-op. Failed validation leaves state unchanged.
Allocation failure before commit also leaves ownership unchanged; native callback
boundaries must continue catching exceptions as required by AGENTS.md.

The writable `workspace(id)` is the existing catalog escape hatch. Hosts must
allocate SessionIds application-wide and never bind an already-owned ID there.
`validate()` checks all catalog invariants as well as unique/exhaustive placement.
Windows must not retire actors based on only their visible/local project list.
All catalog records/references should be reacquired after mutations; do not retain
pointers to window-vector elements across placement changes either.

## Close is not move

`preview_close(project)` returns its window, placement revision and all current
session associations, including parked entries. After obtaining authorization,
`close_workspace(preview)` rechecks these before explicitly removing the project
and returning `RemovedProjectWorkspace` for host retirement. A move out and back
invalidates the preview; unrelated placement changes conservatively invalidate it
too. Changed session ownership also invalidates it, even without a placement
revision. Ordinary model edits that preserve the exact session set do not invent
new process-close authorization. SessionId reuse is prohibited by the host.

The portable layer never requests process termination. `close_empty_window`
refuses nonempty windows. Full-window/all-app authorization and coordinated close
are intentionally still host work: do not present successive project closures as
an atomic window-close transaction. Capture the window layout before removal,
recheck the aggregate authorized session set, and avoid reentrant partial closure.

## Restoration policy, without a premature file format

`capture_application_layout` uses existing declarative snapshot capture, arranged
by each window's placement/selection. It strips live session bindings and transient
process titles; parked jobs, output and commands are not restorable processes.

`WindowRestoreHistory` is a bounded, in-memory policy helper holding the latest
individual-close, whole-app Quit and crash-checkpoint snapshots. Successful records
get monotonically increasing sequence numbers; invalid records do not consume a
sequence or replace the previous good snapshot. Selection does not consume history.

- Explicit New Window: no restoration candidate; create fresh.
- Launch while already running: no cold restoration candidate; host routes request.
- Cold launch: latest successful individual-close or explicit-Quit record wins.
- Crash recovery: last complete checkpoint only; never infer a close from a crash.

Record an individually closed window only after authorization, using its layout
captured before removal. For Quit, capture and record all open windows **once**;
internal teardown must **not** generate later individual-close history records.
An empty Quit snapshot is valid (for example, quitting a zero-window macOS app).
Checkpointing does not supersede normal close/Quit startup history. The helper
rejects repeated persistent workspace IDs across windows, invalid selections,
live session bindings and application-wide project/window limit violations.

This helper intentionally has no serializer, migration, recent-window stack,
loaded-sequence import, storage token, disk flush or crash marker. Integrating it
into durable startup requires an explicit versioned envelope and one application
writer that retains existing conflict/atomic replacement safeguards. Do not claim
restart restoration merely because these in-memory policy tests pass.

## Next native slice (Windows first)

1. Add an application controller owning this model, the process registry, session
   ID allocator, settings/workspace writers, window registry and message pump.
   Initially preserve the existing one-window behavior.
2. Replace window-local selection/catalog reads with scoped references. Route actor
   wakes through an application dispatcher resolving current ownership; retain
   process/watch/document identity. Keep HWND/renderer/input/IME caches view-local.
3. Move class registration and loop shutdown above NativeWindow. Destroying one
   HWND must not post WM_QUIT or close another window's actors. Aggregate dirty
   rendering/GPU readiness without introducing idle polling.
4. Implement native New Window with prepared-window rollback, fresh workspace and
   an explicit launch. Implement independent close and window focus/discovery.
   Centralize saves before enabling multiple windows; never permit two competing
   snapshots of the same workspaces file.
5. Wire move only after the dispatcher and global retirement checks are verified.
   Commit model ownership then rebind view/input/focus/resize and schedule a fresh
   destination wake. Native creation failure keeps source ownership intact.
6. AppKit gets the same portable model with an application delegate owning window
   hosts; close one NSWindow must not stop NSApp. Cmd-Q is distinct from close.

## Verification

`application_workspaces_tests` covers independent selection, same-window no-op,
invalid/stale destinations, repeated identity-preserving moves, parked placement,
close versus move, move-away-and-back stale authorization, session-set rechecks,
empty windows, capacity, invalid catalog escape-hatch mutation, declarative capture,
and close/Quit/crash sequence policy including invalid snapshot rollback.

These are Linux portable-core checks. They do not prove HWND/AppKit lifecycle,
actor wake routing, GPU scheduling, native persistence, continuous PTY output,
IME/accessibility, or physical multi-window behavior. Those require host integration
and separately reported native evidence.

## Windows application extraction history (single-window adaptations)

The Windows host now has a `NativeApplication` lifetime above `NativeWindow`.
It owns the class registration/message pump, actor registry and ID allocator,
watch identity/state, parked attention and workspace-save backing state. The
existing window methods reference this state temporarily to keep the adaptation
reviewable. Actors capture an application-owned SessionId wake router instead of
capturing a specific view endpoint directly. Registration lifetime now encloses
window lifetime even during exception unwinding.

This is **still single-window only**: the controller currently constructs exactly
one NativeWindow. Catalog selection is still global, file completion is still
posted to that window, and shutdown still closes the whole registry. No New
Window command is exposed. Scoped model views, independent retirement, application
completion routing and durable multi-window storage must land before that gate
can change. The portable model above is not yet connected to the native catalog.

The next adaptation connects native selection to `WindowWorkspaceView`, a zero-copy
adapter over `ApplicationWorkspaces`. Its read/write operations reject foreign
workspace IDs, and window iteration follows only that window's placement order.
`restore_single_window` is an atomic bootstrap-only operation that refuses a second
window or any live bindings. Native closure now requests only its own sessions;
retirement consults the entire application catalog, not this scoped view. The
remaining single-window gates (save completion and pump shutdown) still apply.

The workspace writer is now application-coordinated too: startup loads once,
a thread-level debounce timer schedules one serialized writer, and an owned
mailbox wakes the application pump rather than an HWND. The pump rechecks the
mailbox after dispatch because native modal loops can consume thread messages.
Worker completion retains exact-baseline conflict refusal, queued-save recapture,
and final post-loop flushing. A failed thread creation clears the busy flag.

The on-disk format is still the existing one-window library. Saving explicitly
rejects multiple windows until grouped persistence is implemented; the native
controller still constructs only one. Settings-editor coordination and multi-window
shutdown/renderer scheduling remain separate upcoming boundaries. A Windows-target
object compile is not evidence that native modal/persistence smoke has passed.

## Aggregate close authorization

`preview_close_window` captures the window's complete project order, placement
revision and sorted session associations. `close_window` rejects a stale preview,
then uses `ProjectWorkspaceCatalog::remove_many` to prepare every detached report
before a nonthrowing catalog/placement commit. No process closes here. This is the
aggregate operation for a host's already-authorized independent-window close;
move still never enters this path. Batch failure leaves all catalog ownership
unchanged, including when a valid ID precedes an invalid/duplicate ID. An empty
window can be closed explicitly. Closing one window leaves every surviving
window/project/session association intact.
### Retired actor completion routing

The first native extraction incorrectly erased the only route before a retired
actor's final `done` notification. Native modal smoke stage 2 waits for that actor's
`closed()` state; an idle survivor could therefore receive no wake to continue.
This is an ownership regression, not a backend flake or a reason to add polling.

The correction makes the actor callback own a route whose endpoint remains weak.
The registry holds only a weak route for rebinding/discovery. Removing a registry
entry no longer removes final completion delivery; a moved-then-retired actor
notifies its last endpoint, and an expired endpoint is harmless. Expired failed-
launch registrations are pruned on bind. Live-move integration must still schedule
an explicit destination wake after commit, because a callback may already have
captured the previous endpoint during a concurrent rebind.

`session_wake_router_tests` exercises retirement before the final callback,
move-then-retire, expired endpoints, registry destruction and concurrent rebinds.
Native modal smoke now emits a distinct stage-3 retirement/survivor assertion after
disposal, before sending its final marker. Only a new native run can qualify that
path; portable router tests and cross-compilation do not replace it.

## Registry-driven pump boundary (local, still single-window)

The application dispatches keys and render work through registered native views;
`WM_DESTROY` requests application exit only when no other live view remains. This
boundary still constructs exactly one view and exposes no New Window command.
Dirty renderers arm one-shot threadpool waits which post the existing wake message,
so 64 modeled windows do not exceed the native message-wait handle limit. There is
no periodic idle polling. A view cancels and drains its wait before render/device
recovery, resize, material/swapchain changes and HWND destruction. Size/move modal
rendering performs the same drain even without returning to the application pump.
This requires native renderer/material/modal regression qualification; compilation
alone is insufficient.

Before constructing a second view, application-owned view retirement must leave
survivors alive, creation must roll back model/actors/renderer/HWND failures without
saving an uncommitted candidate, and actor/resource budgets must remain global.
Grouped storage must distinguish authorized individual close, explicit application
Quit, and crash checkpoints. A second executable must route to the existing owner
or refuse shared-storage writes; concurrent silent writers are not shippable.
Controlled two-window smoke must prove independent close and unchanged survivor
actor identity/output. Moving the last workspace also needs explicit empty-window
and zero-workspace command guards before transfer UI can be exposed.

## Unpublished native creation transaction

The application owns view objects and drains them before unregistering the native
window class. Creation uses an explicitly unpublished model token, not a copied
catalog. Only that pending window can receive newly created workspace records;
transfer/close cannot interleave with construction. Abort is allocation-free,
removes only its candidate records, and never reuses window identities. Normal
close authorization cannot be bypassed by passing an already-committed window ID.

The candidate records its own allocated actor IDs before process launch. Failure
clears the candidate HWND/wake target, aborts model placement, and retires only that
ledger. Actors created by a surviving view are not part of the rollback. Renderer,
process or allocation failure never evicts an existing actor. Pending candidate
changes are not saved; a successful commit forwards its deferred autosave signal.
The factory still refuses a second persistent native view until grouped storage is
wired. The 256-project catalog bound and existing project-local pane/tab/parked
bounds are unchanged and shared across windows. There is no additional global
live-session cap; introducing one is separate resource-hardening policy.

## Controlled two-window qualification (not yet native-qualified)

`native_windows_ownership_smoke` uses an authored, temporary, marker-checked local
fixture and controlled Cmd actors, independently of the optional Zsh bundle. It
rejects user config, missing fixtures and ambiguous smoke modes. It does not load
or save application storage. The owned-process wrapper retains bounded logs and
requires both dedicated proof markers and clean app termination; the native app
separately waits for every fixture actor, including the failed candidate.

The fixture constructs two real HWNDs/renderers with distinct actor IDs, injects
pre-HWND and post-renderer/actor candidate failures, checks surviving ownership,
cancels a live close, then closes the original window and retains the second. The
surviving actor must produce new output and exit zero without replacement. Native
provider evidence is checked for both actors that produce output; the aborted
candidate may close before its process starts and is claimed only as disposed.

Retired views are destroyed outside WndProc. Their actor callbacks are rebound to
an application thread-only wake endpoint before registry removal; that endpoint
never retains an HWND. An explicit wake covers callbacks already holding the old
endpoint, and the pump clears notification coalescing after native modal dispatch.
This adapter is gated to nonpersistent fixtures: grouped storage, clean Quit versus
individual close, crash recovery and second-executable ownership remain mandatory
before exposing normal New Window commands. No native/physical pass is implied by
portable tests or Windows-target compilation.
