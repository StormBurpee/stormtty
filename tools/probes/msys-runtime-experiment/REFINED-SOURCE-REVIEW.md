# Both-event processed-prefix retirement review (source/model only)

No native source, binary, experiment, matrix, runtime pin, or default changed.
Pinned source is console.cc.before SHA256
535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33.

## Reject naive both-event input_winch return

The MOUSE_EVENT caller at console.cc:1648-1649 ignores send_winch_maybe's
return in stock. Promoting it to input_winch adds read EINTR at 1379-1385
and select EINTR (prepared select.cc:1179-1186) where stock proceeds.

Concrete additional loss relative to stock: raw VMIN=2, buflen=2. First
peek contains only a; read copies it at 1416-1417, then loops at 1425-1427.
Second peek is [mouse,b], dimensions changed, mouse reporting disabled,
SIGWINCH handler returns without SA_RESTART (no subsequent pending signal).
Stock processes b and reports ab, count 2. Naive candidate returns
input_winch, jumps to sig_exit:1438-1440, and reports -1/EINTR despite a
already consumed. A retry cannot recover a. Resize already exposes this
stock weakness, but exposing it through mouse is an actual new path.

## Refined return: mouse returns existing stat; resize returns input_winch

Both branches retire i records (exclude current event) only when dimensions
changed, before the existing unlock/signal/relock, and immediately return.
Mandatory guard: do NOT invoke discard_key_events(0); its meaning at
1883-1886 is default num_input_events_processed, not an empty prefix.
No change on unchanged dimensions.

Under full successful retirement, cooperative input-mutex users, ordinary
returning handler, and eventually stable geometry, no new byte-loss or
readiness failure was found:

- input_processing with canonical partial RA retains the incomplete line;
  read retries. Native mouse remains queued, so no new owned-event/readiness
  bridge is needed. Resize can be removed by master and has stock winch status.
- input_ok means prior raw line_edit supplied ready bytes. Returning it keeps
  the mutex held until existing read copy/release. If VMIN is unmet, copied_chars
  survives the existing read_more loop. If VMIN is met, short-read boundaries
  may differ: [a,mouse,b], VMIN=1, len=10 returns a now then b later, versus stock
  ab. This is a new batching difference, not missing input.
- select len=0 exits upon its first ready key (1864-1865), before a later mouse.
  A changed mouse with stat=input_processing causes the existing select loop
  to recheck native readiness; it does not invent read_ready or explicit EINTR.
- mouse_aware and button/position updates occur only on fresh re-entry, not
  before the changed-size early return. Normal master processing does not remove
  mouse records. Double-click normalization changes only the local old peek,
  which has not happened yet, so a fresh event is normalized once.
- surrogate handling at 1582-1606 sets i=second_pos and skips intervening events
  in stock. This is preserved, not redesigned. At a visited mouse/resize, i is
  still the complete native prefix already traversed, including skipped events.
- discard_key_events updates con.num_processed by actual discarded count at
  1902. Excluding current event therefore preserves master's remaining-prefix
  accounting. num_input_events_processed is not reset by explicit discard in
  stock or candidate; the next line_edit resets it at 1845. Do not broaden this
  patch into an unreviewed global reset.
- longjmp during signal no longer leaves the successfully retired processed
  prefix native; RA remains as in stock. Current mouse remains native just as
  stock before post-signal processing. No promise is made about general handler
  mutation/flush, translated-byte signal paths, or asynchronous signal escape.
- partial/failed ReadConsoleInput retirement is still unacknowledged (void)
  at 1877-1902 and may duplicate processed prefixes. This is inherited, not
  repaired. The patch must not claim guaranteed ownership under API failures.

## New liveness caveat

A sequence of dimension changes exactly before every reprocessing of the
same head mouse can defer it forever: every iteration sends winch and returns
without consuming it. Stock processes the mouse after its first signal and
moves on. Thus the refined variant's progress argument requires eventually
stable geometry (or a separately reviewed bounded-retry design). Do not label
it unconditionally equivalent or production safe. It is a bounded source-review
candidate with better stale-peek containment, not a demonstrated runtime fix.

## Executable evidence

model_tests.py: 13 simplified control-flow tests pass. Tests
include the naive new-loss trace, changed/unchanged-size paths, target resize
removal, zero-prefix guard, ready-prefix cutoff, and refined statuses. They
are not native Windows tests and do not prove scheduling/API success assumptions.

## Required bounded native cases before any promotion discussion

- Same target ESC-loss schedules with unchanged raw probe, plus mouse-induced
  dimension discovery before a later resize. Preserve every failure.
- Raw VMIN=2 partial-copy then changed mouse; non-SA_RESTART handler returns.
  Check exact bytes/counts across retries, not only aggregate buffers.
- Mouse reporting off/on, wheel/button/double-click/motion; report appears once
  with expected state and coordinate adjustment after fresh peek.
- select with handler returning and ignored/default SIGWINCH; canonical partial
  line and delimiter; raw short read with VMIN=1 and larger requested length.
- UTF-16 surrogate high/low pair with intervening mouse/resize/focus records:
  preserve stock skip behavior, no introduced replacement/duplicate bytes.
- SIGWINCH handler longjmp after partial canonical/raw prefix; prefix must not
  be replayed after successful retirement. Document actual handler assumptions.
- Repeated geometry changes while a mouse is head-of-queue: establish bounded
  completion once geometry stabilizes; measure deferral rather than hiding it.
- Failed/partial retirement fault injection if practical, and competing readers
  or native queue mutation outside input_mutex. Shared/native queue consumers
  remain outside the ownership guarantee; stock's locking is not a universal
  Windows queue lock. Do not represent these inherited risks as fixed.

The running stock-versus-conditional experiment is unchanged. This separate
patch is for source review only; it is not proven safe or promotable.

Patch dry-run applied cleanly to exact preserved console/header. No C++ compile
or Windows execution performed. Patch SHA256:
`ceb70e9c04be478e051f2be891409e4d7e4ad30c8654141c86f520f165b70693`.

## Native compile-only correction (2026-10-29)

The original review patch above was attempted in native run40875619536. Stock
built; refined failed GCC15 misleading-indentation at the unbraced mouse return.
Current `0006-refined-prefix.patch` adds braces around only that existing mouse
return branch, retaining `return stat` and every reviewed control-flow choice.
Current patch SHA256 `1afc3483da5dcc8848f336619f858c0ffcfbef608d1446464e6566135b2aff66`.
No warning suppression, semantics broadening, or runtime pass is implied. See
REFINED-QUALIFICATION.md for preserved failure/artifact and source identities.
