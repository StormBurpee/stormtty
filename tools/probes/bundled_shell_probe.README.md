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
