# Workspace navigator interaction ownership

## Domain boundary

The existing `WorkspaceNavigator` now also owns its visible interaction lifetime:
background filter, frozen activity-project scope, bounded query editor, control
focus, and switch-button gesture identity. No forwarding controller, actor,
registry, project service, window, renderer, callback or native resource is added.
Catalog values and labels remain owned data. Catalog access stays const.

The existing `open`, `refresh`, `search`, selection and layout interfaces retain
model-only semantics for existing callers. In particular, catalog initialization
(`opened_`) is not overlay visibility (`visible_`). `begin` starts a fresh visible
interaction; `replace_catalog` selects the historical ordinary `open` versus
sticky-activity `refresh` policy and reapplies the editor query. A mutable editor
borrow exposes only the existing bounded UTF-8 editor, not mutable catalog data.

The host still samples, filters and sorts catalog rows using the same actor/watch
lookups and profile rules. Filter/scope values are read from the owner; the owner
does not gain authority to enumerate actors or projects. Host keyboard handling
retains its exact key-up/repeat/AltGr/IME and clipboard routing policy.

## Preserved ordering and lifetimes

- Modal/overlay admission and minimum size checks precede the already-open toggle.
  Find dismissal, drag cleanup and competing overlay cleanup stay in the host.
- `begin` groups the old local model/editor/focus reset with scope/visibility setup.
  The intervening closing-palette vector clears/transition assignment are local,
  nonthrowing operations with no message pump or callback. Catalog sampling still
  happens after those clears, before IME reset and invalidation/timers.
- Selected IDs are now read in `replace_catalog`, after catalog sampling rather
  than before it. Sampling reads owned actor snapshots and does not mutate this
  UI-thread navigator or pump messages; it therefore cannot change that selection.
- Closing preserves catalog/query/focus/background filter, but clears visibility,
  pressed eligibility and activity scope. Reopening resets model/query/focus.
- Switch release compares the captured **optional target**, not merely a session
  number. It consumes a boolean before the host releases capture, lays out the
  button and dispatches the existing live commit validation. Two null targets
  deliberately compare equal: a stale displayed activity row can trigger the
  existing stale-selection notice. Returning an optional target would be wrong.
- Capture loss/cancel/focus loss clears gesture state at the same host positions.
  Fresh begin also discards dormant pressed-target storage; it was unobservable
  while pressed=false and every fresh arm overwrote it previously.
- The activity smoke rollback preserves the prior closed focus/background values
  while clearing model/editor/scope using owner operations. It does not bypass
  actor validation or add qualification-only production APIs.
- `commit_navigator` retains its full live revalidation, notice, capacity, dismissal,
  project activation and restore/focus sequence. Preview never launches a dormant
  shell or acknowledges a watch. Host IME/capture/timers remain host-owned.

## Independent characterization

Test-only commit `719accd` captured the original model and host lifecycle before
production movement in `navigator_interaction_legacy.hpp`. It intentionally does
not call the new interaction implementation. `navigator_interaction_tests` runs
both implementations through the same cases:

- all background/activity scope combinations, frozen scope and stable selection;
- close preserving state versus reopening clearing it;
- query/focus lifetime, filtering to an empty target;
- ordinary refresh versus sticky activity refresh, unchanged versus changed query;
- actor incarnation and parked-placement changes, reordering and repopulation;
- switch capture cancellation, one-shot release, changed target rejection, and
  stale/null-target activation eligibility.

The tests do not pretend to sample live actor catalogs or prove native input APIs.
Existing `command_watch_tests` additionally characterizes removed selections,
empty catalogs and activity commit availability. Existing `window_geometry_tests`
retains model-only navigator filtering/layout coverage.

## Evidence and native gates

Local GNU checks: original interaction 79 checks; original plus extended owner
158 checks; existing command-watch suite passed. Strict direct Windows cc1 covers
owner, new tests, command-watch tests, geometry tests and native host. This is not
a native MSVC or Windows runtime claim. Small private executables are removed after
receipts; no full local CMake build/download or heavyweight cache rebuild is used.

Required native qualification: MSVC build and all three navigator/watch/geometry
tests; actual native navigator, activity and background smoke progression. Verify
preview without project activation, stale live actor rejection, frozen activity
scope, unchanged watch unread state, compact two-stage lists, zero frontend input,
Escape focus and unchanged retained actor ownership. Keep full Find/settings,
cache/renderer/PNG and twelve teardown regressions. No deadlines, skip gates,
paste safety or paused features are changed. macOS model consumers remain valid;
this slice does not claim a new macOS navigator UI or physical IME verification.
