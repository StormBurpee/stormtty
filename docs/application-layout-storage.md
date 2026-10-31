# Bounded application-layout storage

The Windows application coordinator uses the `STORMTTY-APPLICATION-LAYOUT 1`
envelope through `load_application_layout_snapshot` and `save_application_layout`.
`application_layout_codec` defines the portable format; the guarded Windows file
service is implemented in `src/platform/windows/guarded_document_store.cpp`.
Typed codec/preflight adapters remain in `settings_services.cpp`; the file service
retains the entire verified-backup handle lifetime and shared atomic writer.

This is a **single-persistent-window integration**, not enabled grouped startup or
normal New Window. Reads accept legacy v1 workspace libraries without modifying
them; saving an imported library requires a verified sibling `.v1-backup` before
atomic replacement. The format's ability to describe multiple windows does not
mean the native host can restore or save them. Second-process ownership is also
not provided by the file format or its optimistic conflict checks.

## Data and limits

The `STORMTTY-APPLICATION-LAYOUT 1` envelope contains:

- `clean` (0/1): whether the previous application shutdown was orderly;
- the original v1 WorkspaceLibrary, retaining existing restore preference/presets;
- last individually closed window, last explicit-Quit group, and last checkpoint;
- a sequence number on each present record, and active-window index per group.

Each window is a length-delimited v1 library payload containing only its
WorkspaceSnapshot (no nested presets). Existing snapshot validation strips/refuses
live bindings. A group cannot contain duplicate persistent workspace identities.
Individual-close records contain exactly one window. Quit/checkpoint may represent
zero windows, such as a still-running macOS app after its last window closes.

The whole envelope, not each record alone, is capped at 1 MiB. A group allows at
most 64 windows and 256 workspaces in total. Counts, byte lengths and sequences
are checked decimal unsigned values with overflow rejection. Zero sequence means
absent and requires zero count/selection. Present record sequences are nonzero and
unique; import resumes allocation after their maximum, with saturation rather than
wraparound. Unknown versions, truncated/extra data, invalid layouts and malformed
records fail without returning replacement defaults.

The current host honors `library.restore_on_launch`; history does not override a
disabled restore preference. With restoration enabled, clean startup selects the
latest individual-close or Quit record; unclean startup selects the checkpoint.
Only a selected single-window layout is restored. An unclean file without a
checkpoint, or a selected group with multiple windows, produces a notice and
invalidates the writable baseline so the original is not autosaved over.

For future grouped integration, explicit New Window and a launch forwarded to a
live owner must bypass cold restoration. Explicit Quit must capture the group
once; its internal teardown must not add later individual-close records. Those
native routes are not enabled by the current single-window adapter.

A v1 file imports read-only into the original library plus a one-window close
record (when nonempty). `requires_legacy_backup` is true. No original preset is
renamed or dropped. Import and serialization are not disk migration: a large v1
file may import successfully but exceed the envelope cap when its history is also
encoded. In that case serialization fails and the original must remain untouched.

## Guarded save and migration contract

The current coordinator uses this transaction through the application-layout
service. Earlier plans to switch the native writer are superseded by this
integration; the remaining grouped-host gates are distinct.

1. Read through the existing guarded local-file snapshot service. Keep its exact
   original bytes/path/file identity token; never re-read and silently refresh a
   conflicting token. Detect v1 using the codec's explicit backup requirement.
2. Prepare and validate the entire bounded envelope before any replacement.
3. For legacy migration, write a create-new, guarded sibling backup of the exact
   loaded v1 bytes. Reuse only a verified identical existing backup; a different
   existing backup or unsupported path is a visible failure, never overwritten.
   Flush/close and verify the backup before permitting original-file replacement.
4. Recheck the original file against the loaded token after backup creation, and
   use the same safe atomic replacement path as settings_services. If another
   process/editor changed it, refuse replacement; do not merge unrelated history.
5. Only successful replacement advances the writer's baseline. Failures keep the
   original and verified backup. Unknown/corrupt envelope versions must never be
   converted to defaults or overwritten by an autosave.
6. The host schedules a running checkpoint and records a clean individual close
   for final flushing. A scheduled or in-memory record is not a durable save;
   failures must be reported rather than claiming recovery succeeded. Future
   grouped Quit must record all windows once before teardown; this is not yet a
   current host operation.

The Windows application-layout service implements the file transaction above and
is called by `NativeApplication::save_workspaces`. Its backup is
held open without write/delete sharing through original replacement; identical
existing backups also require an exact DACL/protection match. Encrypted/read-only
originals are refused before backup creation. Windows-only tests cover migration,
backup reuse/conflict, stale original, unknown version, hard-linked backup, bounds
preflight and missing-path behavior, plus protected/custom DACL preservation,
same-byte/different-permission backup refusal, and a backup already open for writes.
These sharing guards cover ordinary data-write/delete access, not arbitrary
privileged or hostile namespace/security mutation; the original service remains
optimistic concurrency, not filesystem compare-and-swap. Cross-compiling those
tests is not a native runtime pass. Native saves produce the envelope; embedded
workspace payloads retain the v1 library encoding. Do not call the codec directly
from a second window to create competing writers.

## Second executable launches: mandatory first-shippable boundary

Application ownership inside one process does not coordinate two executables.
Before shipping New Window, choose and test one of:

- A per-user/config-root owner broker: a second process validates the existing
  owner and forwards explicit launch intent through restricted IPC, then exits;
  only the owner owns this file. Bound/validate every request and preserve explicit
  launch directory/profile semantics. Separate elevation/integrity domains; never
  forward a lower-integrity arbitrary command into an elevated terminal.
- Explicitly isolated storage for an independently running instance, with a
  visible isolated-instance indication and no writes to the main restore file.

A last-writer-wins file policy is not acceptable. Existing conflict refusal
prevents overwrite but is not by itself a usable multi-instance coordination plan.
A first UI slice can guarantee one normal per-config-root process with routed
launches while refusing unsupported elevated/cross-domain reuse visibly. Generic
cross-process live-terminal transfer is not part of this scope.

## Native wait strategy for up to 64 model windows

Do not put 64 renderer latency handles into `MsgWaitForMultipleObjectsEx`: its
message-queue slot reduces the available object count. Do not rotate subsets on
a timeout, which can starve an excluded window or introduce idle polling.

The current `RendererReadyWake` integration arms a one-shot Windows thread-pool
wait for a dirty, GPU-blocked renderer. Its callback posts through the owned wake
endpoint; the UI thread re-arms only if rendering remains blocked. The main loop
waits on the message queue rather than a 64-handle array. Cancel/drain each wait
before its latency handle or renderer is destroyed; callbacks must never access
renderer/view memory. Test close while signaled, repeated arm/cancel, minimized
windows and concurrent wakes. Keep an explicit native window cap if resource
limits demand a smaller value, and do not silently drop restored windows.

## Verification and remaining gates

Portable tests cover roundtrip, exact v1 library/preset preservation, backup flag,
sequence ordering/import/saturation, invalid versions/numbers/lengths/ownership,
live bindings, mutation robustness and the whole-envelope size cap. Windows
`application_layout_services_tests` exercise the actual migration/file-conflict
service. `native_workspace_persistence_smoke` uses
`tools/workspace-native-smoke.py` to exercise real application launches.

The earlier “local qualification pending” adaptation notes describe implemented
source, not an outstanding switch from v1 to the envelope. Their test descriptions
remain evidence scope, not a substitute for the recorded result of a particular
native run. Cross-compilation does not prove native I/O, crash-marker timing or
GPU wait lifetime behavior. Grouped startup, explicit grouped Quit, normal New
Window and second-process coordination remain unimplemented host gates.

## Current single-window envelope host behavior

`NativeApplication::load_workspaces` and `save_workspaces` read/write the envelope
through the guarded backup/migration service. Saving refuses any model with other
than one window. Its existing library reference remains anchored to the
same document member. Successful startup schedules a running checkpoint even when
restoring panes caused no layout edit. Unchanged declarative checkpoint content is
compared before advancing sequence; preset changes still save without fabricating
a new arrangement. Normal last-window shutdown records a clean individual close
before draining pending writes, so an older worker completion cannot overwrite
that in-memory close state.

This is still a single-persistent-window adapter. If the selected recovery record
contains multiple windows, or an unclean envelope lacks a checkpoint, restoration
is refused with a visible notice and the writable baseline is cleared; original
bytes remain untouched.
It must not flatten an unsupported group. Full grouped startup and explicit Quit
are separate next steps. The native workspace fixture extracts its own authored
v1 library from a freshly saved envelope, then checks exact backup bytes and clean
new-envelope output on the next real launch. This fixture does not establish
multi-window recovery, a second-process broker or physical UI qualification.

### Migration source-handle lifetime

After verifying backup bytes and DACL, migration releases its extra original-file
read handle before invoking the existing atomic writer. Classic FileRenameInfo
replacement rejects an open target; delete sharing is not POSIX replacement
semantics. The backup remains held without write/delete sharing, ancestor guards
remain held, and the writer independently rereads/revalidates the exact original
baseline and preserves its DACL before the unchanged atomic rename path. No ACL
check, conflict guard, link rejection or read-only/encrypted refusal is relaxed.
See [Microsoft FileRenameInformation error conditions](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/1d2673a8-8fb9-4868-920a-775ccaa30cf8).
