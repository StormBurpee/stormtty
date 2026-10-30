# Candidate view creation transaction

## Boundary and phases

Windows `ViewCreation` borrows only `ApplicationWorkspaces` and
`ApplicationActors`. It owns a staged window ID, exact actor-ID journal, deferred
model-change bit and idle/staging/publishing phase. It owns no NativeWindow,
HWND, GPU resource, settings worker, persistence service, callback or view context.
It does not launch actors. There is no destructor-driven rollback or blanket
noexcept cleanup, and no mutable journal/catalog getter.

The host reserves view-container capacity and applies the original admission
checks before beginning. It constructs/restores/creates its local unique_ptr
candidate and retains the original pre-HWND/post-renderer qualification boundaries.
Candidate `new_session` records its allocated ID before `Session::start` and route
creation. Committed survivors do not record IDs, even if reentry launches their
actors while another candidate is staged. Never replace this journal with a global
registry difference or restore a whole-application snapshot.

Commit validates the catalog and clears the journal but stays in publishing phase.
The host marks the view committed and transfers ownership to its reserved vector;
only then does `finish_publication` clear suppression and return deferred dirty
state. Restore/dirty notifications remain outside the creation catch. Invalid
phase/candidate calls report programming errors rather than silently authorizing
rollback of published ownership.

Failure retains qualification references, destroys the native candidate, then
explicitly calls `rollback_after_candidate_destruction`. The journal no longer
lives inside the destructed view. Catalog abort precedes exact actor disposal;
route-only/unregistered IDs receive the same cleanup as registered IDs. Only after
disposal does the owner clear the creation guard and return deferred dirty state.
The host then notifies persistence against the rolled-back model and rethrows.
Survivor edits are not rolled back. Existing notification/cleanup exceptions are
not swallowed or converted to success.

## What deliberately remains host-owned

Native construction and its reentrancy, raw view registration, candidate/native
resource destruction, reserved publication, smoke evidence, persistence scheduling
and postcommit notification stay in NativeApplication/NativeWindow. No new friend
or callback bridge is introduced.

Retirement is unchanged. It still runs outside WndProc, after persistence completion
draining, and retains the persistence/last-window gates. Pending Settings work
prevents retirement; completion reporting precedes catalog detachment. Existing
actor completion-route rebinding/wake, view destruction and final persistence
flush ordering are unchanged. This slice does not claim to complete application
or view lifetime separation.

## Characterization and evidence

Test-only commit `b25d496` captured the old creation state transitions independently
in `view_creation_legacy.hpp` before production movement. This was source/syntax
characterization, **not an executed native pre-extraction baseline**. The same
native test cases now run against the captured legacy logic and the new owner.
Subsequent shared cases strengthen coverage for a reentrant survivor launch and a
successfully published actor; the legacy oracle's policy is unchanged.

`windows_host_lifetime_tests` uses real app-authored event-gated actors and a real
message-only candidate window. It checks:

- nested creation rejection and dirty suppression;
- wrong-window journal rejection without poisoning valid rollback;
- native candidate destruction before explicit rollback;
- registered candidate disposal and allocated-but-unregistered route/metadata cleanup;
- survivor metadata edits and exact actor pointers/incarnations, including a real
  survivor actor allocated after staging began (a global ID diff would fail);
- monotonic candidate identity, catalog commit retaining the publication guard;
- successful publication retaining its actor, including through a later clean abort;
- deferred dirty-state delivery and no dirty leakage into the next transaction.

Readiness requires authored output, not elapsed time or raw bytes alone. Existing
5-second readiness/cleanup waits and 15-second bounded child event waits remain
unchanged. No actor facade, production fault callback or external shell/tool is used;
the real subprocess is the authored test executable itself.
The synthetic postcommit exception is outside the test transaction; actual host
notification placement is a separate source-review invariant, not a claimed native
failure injection.

Local portable `application_workspaces_tests`: 1811 checks passed, including
unpublished catalog rollback/restore and survivor preservation. This is model
coverage, not Windows coordinator execution. Strict Windows direct cc1 checks
cover owner, native lifetime tests and host. No full local build/download was used;
private portable executable removed after receipts.

Required native gate: MSVC, windows_host_lifetime_tests (both named creation cases),
existing native_windows_ownership_smoke pre-HWND/post-renderer rollback and pending
Settings-save retirement, workspace persistence, and the full renderer/cache/PNG
and twelve final-teardown suite. No deadlines, assertions, paste security or paused
features are changed. Physical UI and macOS adaptation remain separate work.
