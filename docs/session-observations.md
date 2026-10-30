# Application session observation cache

`SessionObservationCache` owns the Windows application's last-observed process
states and background display labels. These are chrome-invalidation history, not
authoritative `Session` state, attention policy, actor lifetime or per-view data.
The owner is UI-thread confined and shared by all native windows through
`ApplicationActors`. It does not find sessions, format labels, change placement,
load configuration, deliver notices, render, or persist anything.

## Exact observation policy

- `observe_state` records the supplied state and reports whether it changed.
  Every missing entry reports changed, including a first `starting` observation.
- `observe_background_label` compares exact string bytes with the prior label.
  A missing entry is first initialized to empty, so an initial empty label is
  recorded but reports unchanged. An initial nonempty label reports changed.
- State and label observations are independent. Each changed result separately
  increments the host's chrome generation, preserving double invalidation when
  both change. Repeated equal observations are quiet.
- Queries do not insert. An absent label differs from a stored empty label.
  State queries return values; label queries borrow const strings and must not
  survive observation, forgetting, qualification restoration or host reentrancy.

`NativeWindow::update_sessions` still iterates its scoped project catalog,
including hidden projects. Foreground observation follows Watch observation and
pane presentation; parked observation follows Watch and parked-attention
observation, then compares the label before the process state. Label formatting
stays in `background_title`: managed/configured profile lookup, user/actor/fallback
title selection, Watch or unread completion suffix, and terminal status remain
host responsibilities. Configuration-dependent strings are not normalized by the
cache.

The cache is not cleared by view reset, ordinary park/restore, project visibility
changes, or reassignment to another window. A new observing window inherits the
application's previous values; it does not receive an artificial first-observation
change. Different view-dependent formatting can still change the shared label.
This preserves existing behavior rather than establishing that shared
invalidation is the ideal multi-window design. A per-view migration requires
separate transfer and invalidation-delivery qualification.

## Cleanup and qualification

`ApplicationActors::dispose` first disposes through the lifetime registry, then
forgets the session's observations, then forgets attention. Forgetting is
idempotent, applies even without a registered actor, and leaves other sessions
untouched. The registry remains declared last so actors close before presentation
metadata is destroyed.

Activity's **failure-only** qualification rollback historically copies and swaps
**all application background labels**, but not process states. Its opaque,
move-only label checkpoint preserves that exact broad behavior, including labels
observed by other fixture windows and labels forgotten after capture. Restoration
is an allocation-free, noexcept map swap, not an identity-checked or scoped
restoration. A checkpoint is single-use; moved-from or already-restored capsules
are inert. Capture allocates the copied map; moving the capsule only transfers
a unique pointer, avoiding platform-dependent allocation in map move
construction. It must not be confused with the separately scoped and
identity-validated parked-attention checkpoint. Improving that inherited fixture
behavior is outside this encapsulation; successful Activity paths continue using
normal park/restore operations.

Portable `session_observation_cache_tests` exercise the actual owner: all process
states, insertion asymmetry, exact/owned UTF-8 labels, independent observations,
shared history, repeated cleanup and whole-label-only rollback. Existing native
partial-registration and independent-window disposal assertions now query this
owner. Portable tests and a Windows GNU syntax check do not establish native
runtime, MSVC, physical UI, or cross-window invalidation correctness.
