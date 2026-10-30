# Bounded application-layout envelope (not enabled in the native host yet)

`application_layout_codec` is a standalone portable codec. The currently qualified
Windows host still reads/writes the v1 workspace library and creates one window.
This codec does not change any user file or enable New Window.

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

The host must first honor `library.restore_on_launch`; the presence of history
does not override a disabled restore preference. For normal launch with restore
enabled, choose the latest individual-close or Quit record. If `clean`
is false, the host offers checkpoint recovery; it must not treat that checkpoint
as an individual-close event. Explicit New Window and a launch forwarded to a live
owner bypass cold restoration. Internal Quit teardown must not record individual
window closes after the pre-Quit group snapshot.

A v1 file imports read-only into the original library plus a one-window close
record (when nonempty). `requires_legacy_backup` is true. No original preset is
renamed or dropped. Import and serialization are not disk migration: a large v1
file may import successfully but exceed the envelope cap when its history is also
encoded. In that case serialization fails and the original must remain untouched.

## Required host integration transaction

Before switching the native writer from v1 to this envelope:

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
6. Commit a running checkpoint before treating a newly launched application as
   recoverable. Record approved individual closure before removing its layout;
   Quit records all windows once before teardown. Final clean-marker persistence
   failure is reported; do not claim crash recovery or restore succeeded.

The Windows `application_layout_services` implementation now provides this
transaction, but **the native application does not call it yet**. Its backup is
held open without write/delete sharing through original replacement; identical
existing backups also require an exact DACL/protection match. Encrypted/read-only
originals are refused before backup creation. Windows-only tests cover migration,
backup reuse/conflict, stale original, unknown version, hard-linked backup, bounds
preflight and missing-path behavior, plus protected/custom DACL preservation,
same-byte/different-permission backup refusal, and a backup already open for writes.
These sharing guards cover ordinary data-write/delete access, not arbitrary
privileged or hostile namespace/security mutation; the original service remains
optimistic concurrency, not filesystem compare-and-swap. Cross-compiling those tests is not a native
runtime pass. Until that gate and host integration exist, native writes remain v1.
Do not call the codec directly from a second window to create competing writers.

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

Preferred integration: arm a one-shot Windows thread-pool wait only for each dirty,
GPU-blocked renderer. Its callback posts an owned weak window/application wake;
the UI thread re-arms only if rendering remains blocked. The main loop can then
wait on the message queue rather than a 64-handle array. Cancel/drain each wait
before its latency handle or renderer is destroyed; callbacks must never access
renderer/view memory. Test close while signaled, repeated arm/cancel, minimized
windows and concurrent wakes. Keep an explicit native window cap if resource
limits demand a smaller value, and do not silently drop restored windows.

## Verification and remaining gate

Portable tests cover roundtrip, exact v1 library/preset preservation, backup flag,
sequence ordering/import/saturation, invalid versions/numbers/lengths/ownership,
live bindings, mutation robustness and the whole-envelope size cap. The backup/file-conflict service and its Windows-only tests are implemented but
still require native runtime evidence; crash-marker timing, second-process routing
and GPU wait lifetime behavior still need host implementation and Windows evidence.
