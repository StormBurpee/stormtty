# Settings document owner characterization plan

Baseline: `e7c501e69a4fd07ab356a8466e0bbcdd7b6f8df8`.

## Current evidence and limits

* Root cleared the production gate after PR 19's Windows MSVC build, 56 baseline
  tests, 89 launcher checks, 271 shell-state checks, Settings smoke and independent
  window ownership smoke passed. That evidence is for the preceding launch slice,
  **not native runtime evidence for this extraction**.
* Fresh Linux Release build of this extraction: `settings_document_session_tests`
  **176 checks**, `settings_panel_tests` **50,809 checks**, `core_config_tests`
  **2,831 checks**, all passed. No stale archive was used.
* Focused Windows GNU compilation of the native host passed with strict warnings
  and no diagnostics. This is cross-compilation, not native execution.
* The matrix below remains a requirements/characterization plan. In particular,
  thread-construction/allocation failure for all three operations is source-reviewed
  but **not deterministically fault-injected**. The service seam deliberately does
  not introduce a generic executor solely to manufacture that failure.
* Completion outcomes explicitly report load success/existence and save commit,
  error/conflict without exposing the consumed concurrency token. Native worker
  results stay private; token/path and candidate ownership transfer by move.
  Compile-time checks require nonthrowing token assignment and Config movement,
  preserving the original no-token-copy persistence acknowledgment boundary.
* Host font/GPU failure ordering and survivor error presentation are preserved in
  source; portable owner tests establish commit/token outcomes, not native GPU,
  focus/IME, actor/input, process-exit or physical desktop correctness. Native
  Settings/reload and independent-window smoke must still run on this exact slice.


## Ownership boundary

`SettingsPanel` already owns original running configuration, initial editor draft,
current draft, dirty state and pending UI actions. Keep it intact. A new
`SettingsDocumentSession` owns the serialized file worker, completion mailbox and
mutex, busy/load state, pending editor-open intent and latest binding request,
`SettingsReload` generation gate and exact `ConfigFileSnapshot` save token.
It retains copied per-request paths/file-operation inputs and a weak wake capability, never HWND,
NativeWindow, application, renderer, actor, or a live Config reference.
The native host retains focus/IME, overlay exclusivity, preview/apply/rollback,
error presentation, rendering and SettingsPanel calls.

Three values must not become one baseline:

1. Running Config at editor opening: Cancel restores this presentation.
2. Fresh valid disk Config (or running Config when disk is absent): initial draft.
3. Snapshot from that exact read: optimistic Save token, never silently refreshed.

## Existing characterization to preserve

* `settings_panel_tests`: draft actions, cancel and persistence-result semantics.
* `settings_services_tests`: same-read conflict token, missing/invalid file,
  external replacement, discovery and running-versus-disk editor baselines.
* `core_config_tests`: reload editor/busy rejection, generation mismatch,
  duplicate completion, canceled reload, missing/invalid/read-failed document.
* Native settings smoke stage 0: hold delivery, Escape, reopen with a binding,
  reopen with no binding; one worker survives and latest intent wins.
* Native settings smoke: save pending/nonblocking state, exact saved bytes,
  repeated save/reload and unchanged actor identity.
* Native independent-windows fixture: block an authored save before I/O;
  confirm first-window close; survivor must produce output before releasing save;
  only then retire closed view. A worker join hidden in WndProc deadlocks this
  fixture and is a failure, not a completion marker.

## New focused tests at the owner seam

Use deterministic promise barriers, not timing sleeps, to hold file operations.
Use value-returning test services only if needed; do not expose mailbox, mutex,
worker, or mutable flags merely for tests.

| Scenario | Required assertion |
| --- | --- |
| Open while idle | One load plus discovery; worker receives copied profiles/path. |
| Escape while load is held | Intent and binding clear; worker and busy state remain. |
| Reopen repeatedly while load is held | Same job, latest binding including empty wins. |
| Completion after canceled open | No editor-open result and no UI resurrection. |
| Load read/discovery failure | Busy clears; requested editor gets error, no draft or token adoption. |
| Modal blocks completed open | No editor opening or token adoption; next open rereads. |
| Open/save/reload while another incompatible operation runs | No replacement thread or mailbox overwrite; rejection is explicit. |
| Same-read save | Service receives exact original token, not latest disk state. |
| Save conflict/failure | Existing token retained; candidate is not authoritative. |
| Save success | New token and saved candidate returned before any native presentation call. |
| Reload generation | Duplicate/stale completion cannot replay; missing file is not defaults. |
| Successful reload application | Prior editor token invalidated only after host accepts apply. |
| Failed reload presentation | Running Config rolls back; no file mutation; old token behavior preserved. |
| Closed host, pending I/O | Retirement reports pending immediately and does not join. |
| Closed host, completed save failure | Consume exactly once; error reaches surviving window through host orchestration. |
| Closed host, completed load | No open intent escapes retirement path. |
| Wake endpoint expired | Completion remains owned/reapable without a raw window callback. |
| Final shutdown | Explicit join after HWND destruction and outside WndProc; no lost worker. |

## Integration constraints

`NativeApplication::retire_destroyed_views` must ask an owner retirement operation
for pending/completed outcome; it cannot reach into the mailbox through friendship.
Normal completion consumption joins only a published result's worker; blocking
final join is a separately named operation. Publication happens after file I/O;
worker still performs its bounded wake notification before returning.

The native fixture now acquires its token through a real asynchronous load, then
uses a bounded service decorator to gate the real save before I/O. No mutable
thread accessor, token setter, or fabricated already-completed result is exposed.
It still requires actual authored persistence and live survivor output.

Save-success presentation exceptions do not undo persistence: close Saving,
retain the committed candidate and report restart-needed presentation failure.
Reload presentation exceptions instead restore the previous running Config.
Neither path creates/restarts actors or changes launch-time AI capability.

## Verification gates

Before production extraction: obtain the launch slice's native CI result from root (done).
After extraction: focused owner/model tests, supported Windows compilation, then
native settings/reload and independent-windows fixtures including full process
exit. Linux or cross-build success alone does not establish native correctness.
Keep physical IME/focus/GPU claims separate from synthetic/native CI evidence.
No shell-security, AI/Leyline, glass/material or renderer teardown changes belong
in this slice.

## Implemented interface and review decisions

The concrete owner has a small value-based command/result interface:

* `request_open(binding, configured_profiles)`: accepts/reuses a pending load,
  rejects incompatible work; copied inputs only. Loading visibility derives
  from `busy && open_requested`, not from whether an OS thread is joinable.
* `cancel_open()`: clears intent/binding only; never stops or joins file I/O.
* `request_reload(editor_open)`: owns the SettingsReload begin/generation gate;
  returns existing user-facing rejection/error status.
* `request_save(candidate)`: consumes a copied candidate using the owner's
  exact token and copied request path; reports a missing-token/busy failure before starting.
* `poll_completion(editor_may_open)`: consumes at most one mailbox value and
  reaps only its completed worker. Returns a value event (load/open result and
  latest binding, save result/candidate, reload result, blocked/canceled load).
  It adopts a load token only if open intent still exists and host permits open.
  Host supplies `!modal` as a value, not a callback or retained context.
* `accept_reload_application(operation)`: invalidates the old editor token
  after successful host application; failure leaves previous state unchanged.
  Operation-ID matching prevents accidental stale acknowledgment.
* `poll_retirement()`: pending/completed/idle value outcome with save error;
  closes open intent and does not emit load/reload UI effects. Application can
  report errors to survivors but cannot inspect or consume the mailbox itself.
* `join_after_window_destroyed()`: explicit final blocking reap, called after
  HWND teardown outside WndProc; destructor remains a final ownership backstop.
* `status()`: by-value busy/load/open intent/generation/job identity diagnostics;
  no writable field, worker, mailbox or mutex references.

A snapshot diagnostic may return a COPY for existing smoke byte assertions;
production callers must not receive a mutable reference or supply replacements.
A monotonic job identifier can replace native smoke access to thread IDs while
preserving the stronger invariant that canceled/reopened load uses one operation.

Keep the file-service seam specific: read snapshot, discover configured profiles,
save candidate with token. Default implementations call existing guarded services.
A qualification-only gate blocks a real save before I/O and can release on promise
destruction. Do not introduce a generic task executor or mutable worker handle.
The chosen seam is one grouped portable `SettingsDocumentServices` interface with
a concrete Windows adapter that calls the unchanged guarded native services.

### Review additions

The owner must not freeze options.config_path in NativeWindow's constructor:
create() rewrites it for isolated settings smoke, and the independent-window
fixture authors a different pending-settings file. Copy the path per request.

Add thread-start failure cases for all operations: load clears intent/load/busy
before rethrow; reload clears busy and cancels generation before reporting;
save clears busy before panel failure. Discovery warnings remain nonfatal;
discovery exceptions fail the requested load. Preserve the existing token on
modal/rejected load and on reload presentation failure. Test stale/duplicate
host apply acknowledgments against operation tokens.

`settings_panel_tests` now characterizes the exact host two-call open sequence:
open(running,disk), then open_for_binding(running,latest_binding). The second
call changes focus only because the panel is already open; it must not replace
the disk draft, running rollback value, or queued initial preview.
