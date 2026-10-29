# Synthetic typing transport/publication trace

## Evidence boundary

The uninstrumented native typing candidate passed final-state and no-async checks
on both providers, but sampled 13 retreats on inbox versus 0 on packaged ConPTY.
That does **not** establish a visual fix. The providers can emit different byte
streams or chunk boundaries; Session currently consumes one ordered output
event per actor iteration and may publish at its 8 ms deadline while additional
output is queued. Neither observation justifies hiding cursor moves or delaying
input. Intermediate unsynchronized line-editor cursor movement can be legal.

## Isolated instrumentation

Configure `STORMTTY_TEST_SHELL_PROBE=ON` and explicitly build
`managed_shell_typing_trace`. It uses the existing separately compiled
qualification Process/Session libraries. The shipping ABI has no evidence field,
and the shipping objects contain none of these hooks. The fixture source and
normal per-character input are the same as `managed_shell_state_probe`.

Only the authored warm-session fixture is traced. Version 2 records a compact
prelude, then detailed evidence only during the 25-character typing phase. Fixed storage admits at most
2048 records and 1 MiB of copied input/output bytes. Exhaustion marks the trace
inconclusive (exit 2); it never truncates, reorders or discards terminal transport.
There are no evidence callbacks or filesystem writes on the actor/transport
threads. Dumping occurs after the fixture closes. The bounded evidence mutex and
copying still perturb timing: compare the existing uninstrumented control too.
Never enable this in a user session or capture user recordings/history.

The prelude stores all output bytes exactly once in one growing `read` record.
Its `prelude_feed` records coalesce contiguous actor feeds only until the next
actor command; command opcode and sequence remain explicit and ordered. Prelude
read boundaries, enqueues, writes and publications are intentionally not retained.
This is initialization for exact replay, not startup timing evidence. The same
2048-record/1-MiB limit includes the prelude and detailed phase together.

Arm/freeze are qualification-only commands ordered by the Session actor alongside
input and output. The fixture awaits each acknowledgement; it does not start
individual-character input before arm, or run erase/Enter/cleanup before freeze.
These barriers add no shipping command or scheduling policy. Freeze makes storage
immutable even while cleanup transport continues. A marker contains the actor
sequence, consumed output offset, current read extent and phase (size 1/2).
Read/enqueue operations may straddle arm; replay allows an unmatched initial feed
only within the retained prelude read extent. Reads queued after freeze's actor
sequence may remain unfed: replay verifies the consumed prefix rather than inventing
an extra publication. The final header must say frozen, with exact record/byte
counts; incomplete phases and overflow are inconclusive, never a performance pass.

Detailed records identify:

- `write`: actual completed input `WriteFile`, requested bytes, reported count,
  success/error, message and partial-write offset. Successful reported bytes
  reconstruct exact completed phase input, including terminal protocol replies.
  They deliberately exclude setup and cleanup input.
- `read`: bytes returned by each output `ReadFile`, with cumulative stream
  offset. These are **read chunks, not child write boundaries**.
- `enqueue`: existing Session global sequence and byte range, including its
  existing 32 KiB splitting. Commands and output retain existing arrival order.
- `command`/`feed`: actor-applied sequence, opcode or exact parsed byte range.
- `publication`: the already-created immutable frame's cursor and cursor-row
  fingerprint, consumed byte offset/sequence, last applied command, queued next
  output and command sequences. No extra `TerminalCore::snapshot` is introduced.
  Queue counts are sampled just before mailbox publication, not a claim about
  future chunks or compositor/display timing.
- `marker` size 1/2: actor-ordered start/end of the 25-character no-suggestion typing phase. Explicit
  Backspace and command entry occur outside these markers.

All timestamps are relative steady-clock recording times, not exact hardware
arrival or display times. Only synthetic fixture bytes may be saved as artifacts.
No user recording, extracted frame or private user input belongs in these logs.

## Native invocation and replay

Use distinct log files, the same runtime/fixture and exact provider pins:

```
managed_shell_typing_trace.exe <absolute-runtime> > inbox-typing.log
managed_shell_typing_trace.exe <absolute-runtime> <absolute-provider-directory> > packaged-typing.log
qualification_typing_evidence_tests --replay inbox-typing.log
qualification_typing_evidence_tests --replay packaged-typing.log
```

The replay tool is portable and links the pinned TerminalCore. It accepts exactly
one complete, non-overflowed trace, preserves detailed-phase feed boundaries and
all actor command order (only prelude feed runs are coalesced), and snapshots only at recorded publication offsets. Every cursor
and row fingerprint must agree; previous owned frames must remain unchanged.
The authored fixture only uses input commands; unsupported document operations
are rejected rather than silently skipped. This is not a generic session replay
format (e.g. resize payloads or arbitrary synchronized-output timing).

Compare `input_bytes` and `input_hash`, then inspect actual accepted byte records
before claiming identical inputs. Protocol response differences must be explained,
not normalized away. Replay reports typing-phase retreats, retreats with queued
output, and those whose next output precedes the next queued command. These are
ordering opportunities, **not automatic defects or permission to combine events**.
If a retreat is already encoded in the output prefix, the renderer did not invent
it. If exact replay disagrees with publication, investigate adapter/event-state
handling. If queued earlier output completes a repaint, preserve all arrival
order and obtain a terminal synchronization contract before changing policy.

Portable synthetic tests cover a legal forward/back/forward stream, exact
sequence/range replay, immutable frame ownership, fixed-capacity overflow and
inconclusive-trace rejection. Cross-compilation is not native execution; neither
is a physical typing or input-to-pixel performance qualification.

The responsiveness driver accepts only version 2, frozen phase, bounded exact
record/byte counts and both markers, then **runs the native C++ replay** for each
provider. Replay failure fails the workflow even if capture itself passed. Its
sanitized log preserves `prelude_feed` records. The owned-process runner retains
its existing 1 MiB textual-output cap (hex expansion can reach this before the
binary trace byte cap); any truncation is explicitly inconclusive/failing, never
silently accepted. The compact ordinary fixture is well below either cap; no
larger unbounded artifact capture is introduced.
