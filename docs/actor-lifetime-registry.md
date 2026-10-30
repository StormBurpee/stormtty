# Windows actor lifetime registry

`ActorLifetimeRegistry` is application-owned and UI-thread confined. It owns
registered `Session` handles, session IDs, watch host/incarnations, and wake-route
discoverability. It does not start shells, inspect configuration, change workspace
placement, or join actor threads. `SessionIdentities` is its portable identity
policy; it is not a second actor registry.

## Boundaries and ordering

- The host still prepares launch inputs, allocates an ID, appends the exact
  candidate ledger, creates the wake callback, starts the actor, records smoke
  ownership, and registers the returned handle. Registration inserts the handle
  before issuing/inserting the incarnation, preserving failure ordering.
- Session IDs issue the maximum value once, then throw on exhaustion. Watch
  host/incarnation counters never issue their maximum value: saturation produces
  invalid zero identities without reuse. A watch host belongs to the application,
  not an individual window.
- `find` and `read` borrow registration without allocation or shared-pointer
  copies. They are UI-thread-only; no borrowed actor survives its erasure or a
  reentrant host operation. `retain` explicitly copies ownership for queued work.
  `snapshot` owns handles and identities for qualification comparisons, not frame
  construction or polling.
- Unused-actor retirement checks the whole application catalog, including hidden
  projects and parked actors. It advances its read iterator before disposal;
  closing a placement does not introduce a snapshot allocation failure.
- `ApplicationActors::dispose` is the single coordinator for registry disposal
  and state, label, parked-attention, and watch cleanup. Those metadata maps remain
  outside the registry intentionally; this slice does not encapsulate their
  feature policy.
- Closed-window retirement supplies application completion before close/removal
  and retains its explicit fallback wake afterward. Candidate rollback still
  destroys the candidate first and disposes only its exact creation ledger.
- Forgetting a route removes discoverability, not callback lifetime. A callback
  retains its route and weak endpoint after registry removal/destruction. Disposal
  also forgets routes for IDs whose start failed before handle registration.
- Registry destruction requests close without allocating or joining. Registry
  member ordering preserves close requests before presentation metadata teardown.

## Qualification and limits

Portable `session_identities_tests` exercises the production counter helpers and
identity policy: exhaustion, absent registration, idempotent forgetting, distinct
application hosts, and stale incarnation rejection. Existing
`session_wake_router_tests` covers late callbacks, endpoint expiry, rebinding,
registry destruction, and replacement-route isolation. Neither suite is evidence
of real Session disposal or native UI behavior.

Native two-window qualification now also exercises the actual unregistered-route
and metadata coordinator with real `WakeState` endpoints, preserves the candidate
survivor's incarnation, and checks metadata removal after closed-window disposal.
Existing native ownership/project/park/watch scenarios remain necessary for real
actor lifetime and complete process exit.

Stale-action fixtures no longer mutate registration maps. Activity refreshes a
stale owned presentation; previous-terminal tests use stale history and a dormant
placement. Navigator launches one additional disposable actor, waits for its
first output nonblockingly so final provider verification still applies, captures
its target, and disposes it through the real coordinator. Every original actor
must survive unchanged. The Find fixture now registers an incarnation as well as
its actor handle; incidental fixture token counts are not a compatibility claim.

Local verification for this slice is portable tests and Windows GNU compilation,
not native Windows execution. Native MSVC, runtime smoke, full process-exit, and
physical UI evidence must be recorded separately. The pre-existing PR20
`session_tests` final-frame-only disposal deadline failure is not changed or
attributed to this refactor.
