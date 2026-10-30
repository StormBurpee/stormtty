# Application session attention

`SessionAttention` owns memory-only Command Watch and parked-attention policy for
all live application sessions. It is UI-thread confined and independent of
`ActorLifetimeRegistry`, native windows, configuration, placement and presentation
caches. It owns neither Session handles nor process lifetime, and cannot deliver
notifications, start shells, navigate, render, or persist anything.

## Host contract

- The host validates project membership, current actor identity and placement.
  Arming requires an eligible foreground observation and matching session ID.
- Observation runs for hidden projects as well as visible ones. A stale Watch
  incarnation is erased and reports **changed**, even though the underlying
  `CommandWatch::observe` reports no result transition for that reset.
- Successful park commits placement first, then establishes its current snapshot
  baseline **before** any reset/update. Failed park makes no attention change.
  A missing baseline during observation starts quietly at the supplied snapshot.
- Successful ordinary restore clears parked attention, not an unread Watch.
  Failed restore/navigation, preview, Escape and visibility never acknowledge.
- Watch Open captures the result generation before navigation and acknowledges
  only after successful navigation to the same actor. Dismiss requires the exact
  Watch identity and generation, and clears only the matching parked completion,
  never a newer parked generation. A delayed background-menu Dismiss captures its
  generation rather than clearing a subsequently armed result.
- Cancel removes only a waiting Watch; it does not touch parked attention or
  close an actor. Stored Watch entries can never be idle: arm inserts only after
  success, and observation immediately erases any identity-reset idle state.
  Thus the old background-menu `!unread` cancel and command `watching` cancel
  remain equivalent for every reachable owner state.
- `ApplicationActors::dispose` coordinates registry disposal, presentation-cache
  cleanup and `attention.forget(session)`. Repeated forgetting is harmless and
  cannot remove another session's attention.
- Notice delivery and batch priority remain in the host. The existing pure
  `ParkedNoticeBatch` and `ParkedAttention` policies retain failure over process
  exit over completion, duplicate suppression, and Watch/parked badge dedup.

## Queries and qualification

`stored_watch` deliberately differs from `current_watch`: command availability
and background-menu construction historically inspect raw stored state before
observation removes stale identities. Indicators, labels, Activity and actions
still validate current identity. This distinction is characterized, not silently
changed by centralization. Queries expose const values or a borrowed const range;
references must not survive erasure or reentrant host operations.

Activity's failure-only rollback uses an opaque parked-only qualification
checkpoint scoped to fixture sessions. It preserves baseline, notification history
and unread generation without a mutable-map backdoor. The host validates original
actor incarnations before restoring it. Successful fixture paths still use real
park/restore operations; no checkpoint can replace Watch state or unrelated
sessions' attention.

`SessionObservationCache` separately encapsulates the application-shared process
state and background-label observations. Their lifetime and comparison policy
remain unchanged; see [session observations](session-observations.md). A future
per-view migration requires separate multi-window, transfer and invalidation
characterization; it is not part of either ownership extraction.

Portable tests exercise the actual owner and project placement integration,
including exact/stale acknowledgment, identity-reset change reporting, baseline
ordering, failed park/restore/navigation, repeated forget, isolation and notice
priority. Native layout Watch/Activity, project park/restore/stale navigation,
independent close, provider retirement and process-exit qualification remain
separate required evidence; portable tests and Windows GNU compilation do not
establish native runtime or physical UI behavior.
