# Terminal Find request ownership

## Boundary

`TerminalFindController` owns one UI-thread search lifecycle: stable session ID,
existing bounded query editor, request ID, owned search snapshot, initial-selection
intent and error. The query editor is exposed directly rather than wrapped in a
second set of single-method editing facades. Editing does not implicitly submit.
The Windows host admits/binds the actor and retains it for each synchronous call;
the controller never stores a `Session`, registry, window, callback or native
resource. A retained exited document remains searchable; process exit is not a
reason to reject the actor.

Host ownership remains explicit: control focus/pressed state, actor lookup,
keyboard/text/paste precedence, clipboard, pointer capture, geometry, IME,
invalidation, timers and all rendering. Cancellation of the old actor stays at
the existing close/focus-loss boundary. Activation requires the host to dismiss
a different bound session first, as before.

## Ordering contracts

- Submission clears transient error/result, queues `clear_selection`, then queues
  search or empty-query cancellation. It stores the returned request ID even on
  rejection, sets initial intent only for a nonempty query, then reports failure.
- The host's registry retain occurs after its existing admission check; this
  UI-thread registry cannot mutate between check/retain and the synchronous call.
- Snapshot adoption distinguishes rejected, unchanged and changed. Rejection
  returns before initial selection. Changed snapshots invalidate chrome and emit
  the existing diagnostic before the host consumes/submits an initial token.
  Unchanged snapshots may still consume initial intent. Token consumption precedes
  actor submission, including rejection: one attempt per exact token.
- `begin_navigation` cancels initial intent before the host tests actor existence.
  `navigate` reads a fresh owned actor snapshot and passes its exact token, with
  the original direction and wrap behavior. No stale-cell retry is introduced.
- Explicit close clears transient error. Focus-loss reset deliberately preserves
  it and does not acquire explicit-close scheduling side effects. Both preserve
  query text, drop request/result/initial intent and unbind the session.
- Input routing, repeat/key-up, AltGr/IME, clipboard and geometry code changes are
  only field-to-owner access substitutions. No routing policy is moved here.

## Characterization before production movement

Commit `1a327e7` captured the old host state machine in
`tests/terminal_find_legacy.hpp` and exercised it in `terminal_search_tests` before
production edits. The oracle is independent of the production implementation.
The final test keeps its policy independent; only API spelling was updated for
explicit reset reasons and the host's pre-lookup navigation boundary.

Both legacy and extracted controllers run the same finite real-Session lifecycle:
retained exited document, matching/obsolete/unchanged snapshots, exact token and
one-attempt selection, revision/generation changes, missing-actor navigation
intent, fresh navigation rather than cached synthetic token, replacement/no-match
selection clearing, empty cancellation, rejected request storage, focus-loss error
preservation, explicit-close error clearing, and query persistence/reopen.
Synthetic snapshots are copied app-owned data, never edits to published frames.
All waits retain the existing search fixture's 8-second await and 5-second cleanup
budgets. There are no fake actor facades or production failure callbacks.

Final no-match selection state establishes the observable outcome of clearing the
old selection. Exact clear-before-search enqueue ordering is additionally a source
review invariant, not falsely claimed as an instrumented actor-call assertion.

## Evidence and remaining gates

Local GNU/Linux current-source test build against the cached pinned terminal
engine: pre-extraction 13/13, extracted 14/14 search cases passed. This includes
existing stale-generation, retained-document, bounded search and I/O fairness
checks. Strict Windows direct-cc1 checks cover owner, search tests and host; syntax
is not MSVC or native execution. Private test binaries are removed after receipts.

Required native qualification: Windows MSVC build, `terminal_search_tests`, and
`native_projects_smoke` Find progression (initial match, next match, resize,
no-match clearing, rapid replacement, project switch, retained actor retirement,
zero frontend terminal-input submissions). Keep the full existing host/settings,
cache/renderer/PNG and twelve final-teardown regressions. Native IME/physical UI
claims require their separate evidence; no deadlines, skips or paste safety rules
are changed. No macOS host integration is included in this slice.
