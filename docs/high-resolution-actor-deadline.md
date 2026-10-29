# Isolated Windows actor-deadline A/B

The scoped native evidence run 40852791511 established that read→feed is tens of
microseconds, but last-feed→publication remains roughly 14 ms on the packaged
provider. Existing policy is an 8 ms dirty-frame deadline followed by
`condition_variable_any::wait_until`. Timer granularity was a hypothesis, not a
causal result. This candidate measures the deadline instead of hiding cursor
movement or changing the 8 ms policy.

## Candidate and boundaries

Only the isolated qualification ABI exposes `qualification_high_resolution_deadlines`.
`managed_shell_typing_trace --high-resolution-deadlines` opts in; normal binaries
cannot enable it. Session calls a portable deadline-wait interface. Only its
Windows implementation owns an unnamed, non-inheritable auto-reset producer event
and high-resolution one-shot timer. All handles are RAII-owned for the lifetime
of the Session implementation; no handle is closed while a wait is in progress.

Timed actor waits arm a relative negative 100-nanosecond due time rounded up,
zero period and zero tolerable delay, then wait for the producer event or timer.
The event has priority when both are signaled. A producer signal remains latched
across releasing the queue mutex and entering the native wait. The actor always
rechecks the existing queue/close predicate. Actor backpressure notifications are
not mirrored to its own event, avoiding a self-signal busy loop. Untimed waits and
retained-document waits remain existing condition-variable waits; all existing
condition-variable consumers still receive notifications. Timer/event creation
or wait failure fails the candidate explicitly, without a mislabeled fallback.

There is no global `timeBeginPeriod`, polling spin, coalescing window, new input
queue, delayed input, byte discard/reordering, cursor suppression, or production
setting. Read/write/input/publication policy is unchanged. The candidate may use
more power while dirty; idle untimed waits do not arm a timer. This must be
considered before any promotion.

## Bounded wake evidence

Both baseline and candidate record `kind=wake` inside the same 2048-record/1 MiB
phase-only budget. Existing exact replay ignores no unknown kinds: it validates
wake ordering/timestamps before accepting them as non-display observations.

- `sequence`: wait serial (independent of actor input/output sequence)
- `offset`: scheduled deadline, microseconds since trace origin
- `size`: wait entry time
- `auxiliary`: return time; for standard condition-variable wait this necessarily
  includes mutex reacquisition, whereas native wait return is recorded before it
- `next`: queue mutex reacquired time
- `reported`: 0 notification/predicate, 1 deadline (native failures fail the run)
- `success`: queue/close predicate true after reacquisition
- `alternate`: 0 standard wait, 1 opted-in high-resolution timer
- `us`: evidence append time after releasing the queue mutex

A latched stale notification can produce a bounded extra actor iteration; it
cannot consume a command or fabricate PTY output. The exact feed/publication replay
still gates acceptance. No timing result is physical input-to-pixel evidence.

## Native comparison

Only push branch `diagnostic/typing-highres` triggers the isolated workflow.
Per provider it records one normal control, one standard-wait trace/replay and one
high-resolution trace/replay, with identical authored 25-character input. No retry
or best-of selection. It requires both modes to be attested by their wake records,
all exact replays to pass, and all four completed input fingerprints to match.
Existing startup attribution remains separate and cannot mask typing failure.
Native tests cover timer deadline, notification already pending before wait,
concurrent notifications, rearming, stale-signal exhaustion, shutdown wake and
handle destruction after notifier join. Their deadlines are safety bounds, not
speed pass thresholds. The A/B data determines benefit; there is no invented
performance threshold that would turn a noisy result into a correctness failure.

## Primary Microsoft API references

- [CreateWaitableTimerExW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw): high-resolution flag is supported on Windows 10 1803 onward.
- [SetWaitableTimerEx](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimerex): relative due times, one-shot period, tolerable delay and hardware accuracy limitations.
- [WaitForMultipleObjects](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects): wait-any ordering, return reasons and prohibition on closing actively waited handles.
- [CreateEventW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw): auto-reset events and non-inheritable unnamed handles.
