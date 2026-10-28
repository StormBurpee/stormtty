# Bundled-shell resize evidence

This probe is diagnostic qualification, not a terminal behavior fix. Only the
private fixture's synthetic commands are journaled; never source this fixture
into a user's shell or enable its ZLE wrappers in the production preset.

`--resize-repeats N` (last arguments, 1–50; default 1) alternates 111×35 and
100×30. Each Zsh/native size acknowledgement includes the current prompt sequence,
so output from an earlier resize cannot pass a later phase. A nonzero/missing
command completion fails rather than resubmitting the command. Size observation
polling remains bounded; it does not force SIGWINCH or repair shell state.

The journal retains the newest 256 bounded records in memory. It writes only on
explicit post-phase/post-failure flush. If prompt synchronization is lost, flushing
may be impossible: any existing disk journal is the **last successful flush**,
not necessarily the failed phase. The original failure frame and Windows
requested/applied grid generations are printed before diagnostic commands.
Applied means the native resize API succeeded, **not** that Zsh observed it.
Post-failure Zsh → stty → Zsh → native size → Zsh observations cannot convert the
original failure to a pass, even if running a child incidentally refreshes state.

## Local harness verification (POSIX only)

Build with `-DSTORMTTY_TEST_SHELL_PROBE=ON`, then:

```sh
python3 tools/probes/bundled_shell_probe_tests.py \
  /absolute/build/bundled_shell_probe /usr/bin/zsh /absolute/new/evidence-directory
```

This checks six alternating phases, failed-command preservation, rejection of
stale sequence acknowledgements, repeat-count bounds, and a 600-record isolated
ring test (256 retained, no file before explicit flush). It is not Windows runtime
qualification. Evidence directories must not already exist.

## Suggested native CI gate

For each existing inbox/packaged provider invocation, append
`['--resize-repeats', '6']` **after** provider arguments. Run each through
`tools/run-owned-process.py`'s owned-process API with `timeout=180` seconds,
retain stdout/stderr and timeout/cleanup metadata, and continue the other provider
when one fails. A timeout is failure, never an allowed skip. This bounds the two
qualification cases to 360 seconds plus bounded runner cleanup; Linux timings
are not evidence for Windows performance. Native results must be reported
separately for the two providers.

### Qualification-only Windows input evidence

With `BUILD_TESTING=ON` and `STORMTTY_TEST_SHELL_PROBE=ON`, the Windows probe
links **only** `stormtty_qualification_session` and
`stormtty_qualification_process`, both compiled with
`STORMTTY_QUALIFICATION_INPUT_EVIDENCE`. The production application continues to
link the normal Session/Process libraries; its `ProcessOptions` layout and writer
code contain no recorder, branch, callback or filesystem operation. Do not define
this macro globally, mix the two Session/Process variants, or use this path for a
user session. The shared terminal library does not use `ProcessOptions`.

The existing explicit private synthetic fixture invocation opts in via a shared
recorder. Native helper modes and normal process launches do not opt in. This does
not inspect launch environment or history. The process options retain shared
ownership until detached workers finish; there are no observer callbacks or
reentrancy into Session/Process. The single writer records immediately after
`WriteFile` returns, preserving its error before instrumentation. Snapshots use a
separate mutex and never hold a Process/Session lock; no new sleeps, barriers or
queue-drain synchronization are introduced. Instrumentation still has finite
copy/mutex cost and must not be mistaken for an uninstrumented timing run.

`PTY_WRITE` records include ordered call sequence, dequeued message identity and
byte offset, requested byte count, BOOL success, reported count, error and accepted
count. `accepted_hex` is **only** the successful WriteFile prefix; a failed call's
reported count is diagnostic, not proof of acceptance. `requested_hex` is separate.
Protocol replies and fixture input take the same writer path and are included in
actual order (without guessing their source from text). Windows pipe acceptance
is not proof of ConPTY translation or MSYS/ZLE consumption.

Memory is fixed at 256 records with at most 1024 requested bytes per record.
Snapshots/hex formatting happen only on test-driven phase success/failure or final
result, outside the writer. Dumps are incremental; `missing_since_after` identifies
records lost before the next dump, `overwritten` is the cumulative ring overwrite
count, and byte truncation is explicit per record and cumulatively. An incomplete
trace must never be called byte-complete evidence. The original failure is dumped
before shell diagnostic commands; final dumps retain any subsequent calls without
changing the original test result.

`qualification_input_evidence_tests` deterministically covers partial writes,
protocol reply ordering, failed/zero writes, ring eviction, byte truncation and
concurrent snapshots. Linux execution verifies recorder mechanics only; Windows
cross-builds do not qualify native runtime behavior. After a Windows cross-build,
inspect the link graph: the probe must contain qualification libraries but no
`libstormtty_session.a`/`libstormtty_process.a`; the app must contain normal
libraries and no qualification libraries. `strings stormtty.exe` must have no
`PTY_INPUT_EVIDENCE` or `PTY_WRITE` format tokens; the probe must have the
trace format. This is a build-isolation check, not a Windows runtime result.

For a Ninja Windows build (MSVC native or GNU cross-build) containing both targets, run:

```sh
python3 tools/probes/check_input_evidence_isolation.py /absolute/build-directory
```

This checks both link dependency lists and the actual two executable artifacts.
It does not execute either binary or claim native runtime coverage; optimized C++
class names are deliberately not required as evidence markers.
