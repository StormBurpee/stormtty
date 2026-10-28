# Resize-record retirement model: scope and unresolved cases

`retirement-model-tests.py` is an original deterministic Python queue model,
not a native MSYS reproduction, implementation proof, or test of Windows I/O.
It does not establish the cause of current command/prompt code3 failures.

The model isolates one proposed ordering change in
`fhandler_console::process_input_message`: retire the peeked prefix through
`WINDOW_BUFFER_SIZE_EVENT`, then call `send_winch_maybe`, returning without
another discard. The intended protected interval is **before this resize
case's signal-related unlock**, not a claim that the entire peek snapshot has
always been protected.

Reviewed exact official prepared source: msys2-runtime3.6.11-4, console SHA256
`535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33`.
Relevant unmodified locations:

- console.cc1225–1245: `send_winch_maybe` compares cached/current size and may
  release the input mutex around SIGWINCH; the comment explicitly permits a
  signal-handler longjmp.
- console.cc1648–1649: `MOUSE_EVENT` also calls `send_winch_maybe` **before** a
  later resize case can be reached in the same peeked batch.
- console.cc1834–1876: existing resize case may signal before the common
  `discard_key_events(min(total_read,i+1))` path.
- console.cc1880 onward: `discard_key_events` returns void and may stop on a
  failed or zero-length `ReadConsoleInputW`; it does not report full retirement.
- termios.cc457 onward: line-edit signal handling already discards the current
  prefix before releasing the input mutex. This supports the ordering
  discipline, but does not prove the proposed console path is correct.

## Positive model properties under explicit assumptions

Assume the peeked prefix is still the live queue prefix when the resize case is
entered, and the discard fully succeeds. The tests show that moving retirement
before that case's unlock avoids a second discard of a subsequently arriving
key, and leaves the resize retired even if signal handling escapes. An already
line-edit-buffered prefix key is retained once in the model's read-ahead state.

The unchanged-size path now returns `input_processing` immediately. A remaining
key must be handled by a later processing invocation, instead of continuing
through the old snapshot. Read and select entry points are covered as explicit
model parameters. `key_makes_ready` abstracts the native line-edit/input-ready
predicate; it is not a full canonical/noncanonical line editor or select loop.
Native read/select readiness, latency, and signal behavior remain unverified.

## Negative properties deliberately retained

1. **Partial or zero discard:** the resize can remain queued. The model asserts
   this limitation rather than pretending a void discard is acknowledged.
2. **Earlier mouse unlock:** another reader can consume the original mouse and
   resize while mouse-triggered signaling has released the mutex. The later
   resize case still has a stale index. Even pre-signal retirement at that point
   can discard a new key. This path is explicitly modeled as unfixed.
3. These tests do not cover every path that can invalidate a peek snapshot,
   runtime delivery, shared-memory races, native API failures, or mode flags.

Run with `python tools/probes/msys-runtime-experiment/retirement-model-tests.py`.
A passing result means the stated abstract examples and limitations are stable;
it is never native qualification or permission to activate a runtime.
