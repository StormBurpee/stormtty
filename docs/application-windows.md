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
