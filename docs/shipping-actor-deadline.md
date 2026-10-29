# Production-ABI Windows deadline integration (default OFF pending gates)

The native qualification A/B run 40856744340 established a real scheduling cause:
median deadline overshoot fell from ~7.8 ms to ~0.43 ms with byte-identical phase
output, while the 8 ms publication policy stayed unchanged. This integration
moves that waiter into the production Session ABI, not the qualification build.
`STORMTTY_HIGH_RESOLUTION_ACTOR_DEADLINES` remains **OFF by default** until root
reviews the shipping native gates. It is an internal build rollout switch, not a
user setting or automatic console-provider selection.

## Correctness and fallback

- Portable Session owns an abstract `ActorDeadlineWait`; only Windows code knows
  about unnamed, non-inheritable event/timer handles. Handles stay alive until all
  Session actor/notifier owners release the implementation.
- Factory capability/resource failure returns null without throwing. The Session
  uses its existing condition-variable path. A prepare/timer/wait failure disables
  this waiter once for that Session and resumes that same CV predicate/deadline.
  Handles are **not** destroyed while concurrent notifiers might use them.
- Event reset occurs under the queue mutex, only when the queue/close predicate is
  false. Producers signal under that same mutex and only for still-pending work.
  This removes stale signals without a gap that can lose a producer wake. Normal
  condition-variable notifications remain for queue backpressure, close/waiters,
  retained-document waits and fallback.
- High-resolution timers are armed only when a live publication is dirty, with
  the existing earliest deadline and unchanged 8 ms frame policy. Untimed idle,
  write retries without dirty output, and retained-document waits keep the old
  path. No idle periodic timer, global timer-period change, spin, input delay,
  redraw suppression, PTY byte change, or provider default change is introduced.
- SessionInfo carries fixed-size counters (requested/available/waits/wake reasons,
  fallback failures and deadline lateness), never terminal bytes or private data.
  Smoke-only app logging attests which actual Session path was exercised.

## Gates

Push-only branch `diagnostic/shipping-deadline` builds production baseline OFF and
production candidate ON from the same source. Candidate app typing/projects/settings
smokes retain real screenshots and require available timer counters, zero fallback
failures, zero stale notification wakes, and actual wait activity. No live AI use.

Both builds run existing real-child Session tests. Dedicated candidate tests run
512 queue-locked producer/reset races plus timeout/rearm/close/lifetime checks.
Two separately linked test factories run the complete real Session suite with
unsupported capability and a timer failure, verifying clean CV fallback exactly
once. Those factories are never linked into the application. Each provider runs
one identical authored 25-character managed-shell fixture in each production mode;
all four outcomes remain visible, with no retries or best-of selection. Failures
in one stage do not prevent retaining other evidence.

This is not a physical input-to-pixel benchmark. Inbox/System ConPTY still emits
split redraws about 15 ms apart; faster truthful publication may expose more
intermediate retreats. Packaged results do not authorize changing the default
provider. Root owns the later default-enable decision after native gates.

## Microsoft references

- [CreateWaitableTimerExW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
- [SetWaitableTimerEx](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimerex)
- [CreateEventW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw)
- [WaitForMultipleObjects](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)
