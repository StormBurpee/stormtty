# Retained terminal documents

## Scope and opt-in

`Session::start(process, terminal, wake, SessionRetentionPolicy::retain_document)`
keeps a bounded, read-only terminal document after the process finishes. The
normal `final_frame_only` default is unchanged: the actor/VT engine is disposed
on exit and the last immutable viewport remains available. No native host opts
into retention yet. This is not session persistence across application exit,
unbounded logging, a shell job-control command, or a promise to recover output
already evicted by the configured scrollback limits.

The host owns the Session independently of a terminal view. Hiding a view must
not destroy the last Session owner or call `request_close()`. A retained document
is disposed by explicit close or destruction of its last Session owner. Destruction
and close only signal the actor; neither joins or waits on the caller/UI thread.

## Two distinct completion conditions

- `process_finished()` / `wait_for_process_exit(timeout)`: the process has reached
  a terminal result available in `info()`. On natural exit, all final delivered
  output is parsed and its frame published before this condition. A failed
  launch or terminal/session failure also settles it; a fatal parser failure
  cannot promise complete output and instead exposes its error. Process callbacks
  arrive after the output reader stops and OS resources are reaped. The actor
  releases its Process owner and retains only its diagnostics.
- `closed()` / `wait_closed(timeout)`: the actor and its TerminalCore have been
  disposed. A retained, exited document deliberately does **not** satisfy this
  condition until explicitly closed. Existing final-frame-only callers retain
  their original completion meaning.

Both waits are test/background-worker conveniences, never UI operations. Use
`SessionInfo.state` and the process-completion condition to determine whether a
process can run—not `!closed()` when retention is enabled. A running shell says
nothing about whether its foreground command has completed or a service is ready.

## Operations after exit

Input (`key`, `text`, `paste`, mouse protocol and focus protocol) returns `closed`
after the transport exit is observed. No operation restarts a process. Inputs
accepted before the exit race are not a guarantee that an exiting child consumed
them.

Retained scroll, scroll-to-bottom, selection, clear-selection, copy, search,
search navigation, local resize/reflow, and default-color updates remain available
through the same bounded actor queues. Resize has no PTY/OS side effect after
exit. Theme updates preserve OSC overrides and explicit colors as before and
remain a single coalesced slot outside input queue capacity. Search keeps its
existing bounded incremental work, memory and result limits; it can continue
across natural exit and is cancelled by disposal. Published frames and search
results remain owned immutable snapshots.

Explicit close of a running process retains the existing shutdown/discard policy
and disposes the document even when retention was selected. It does not silently
turn a user-requested close into indefinite retention. Terminal/actor failures
also dispose a possibly unsafe terminal instead of retaining it indefinitely.

## End-of-output semantics and idle behavior

`TerminalCore::finish_output()` is an irreversible, idempotent actor operation.
It releases an outstanding synchronized-output hold, exposing bytes actually
received before EOF even when the child never sent the closing hold sequence.
A trailing incomplete control sequence remains incomplete: no terminator or
printable text is fabricated. This also corrects a pre-existing final-viewport
edge case for the default policy, which could retain the obsolete pre-hold frame.

Finishing freezes image animations and removes their deadlines. Display changes
can still produce new snapshots, but time passing alone cannot animate a dead
process's document. Further `feed` calls are rejected. A retained idle actor
sleeps indefinitely on its condition variable; it wakes only for a submitted
operation, a bounded active-search quantum, one pending display publication, or
explicit disposal. It does not poll a dead Process or continuously wake the UI.

## Verification

The focused test additions cover a real child producing 3,000 history rows,
search continuing across exit, scroll/select/copy and new search after exit,
query/work limits and latest-query coalescing, a separate bounded-history eviction
case, post-exit theme/resize with unchanged transport counters, rejected input,
final held output plus an incomplete CSI, stable old snapshots, idle wake/frame
silence, launch failure, last-owner destruction, explicit disposal (including queued callbacks holding
that last owner) and immediate close/start races. Direct terminal tests verify image-animation freezing and
post-finalization display operations.

Linux/core execution, cross-compilation, Windows runtime and physical interaction
are separate evidence levels. These tests do not establish footer behavior,
Windows interaction, process survival across application exit, or native latency.
