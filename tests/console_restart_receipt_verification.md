# Console restart failure receipts

This test-only change records the restart variant and original native wait/exit
result when the console backend restart assertion fails. The variants are bundled
reload, relocated missing host and relocated corrupt DLL. Output is failure-only,
bounded scalar metadata: no command, path, environment or child output is emitted.

The child invocation, handle inheritance, environment and working directory are
unchanged. The original 30-second wait, five-second cleanup, query order and
assertions remain intact. A failed wait reports that exit query was not attempted;
cleanup's forced exit 99 is never reported as the original child result.

Source review and strict Windows GNU compilation passed. Native PR #49
[run 41147702687](https://github.com/StormBurpee/stormtty/actions/runs/41147702687)
checked out 1502c4ea003ba0785512d40417cd8e84d73087eb, tree
d68608e45fe8beb9f16e9be043ad2a53a898740a, matching source
794e56ee6cc3ec6b2ce77d5e9ffa1cf109ddb15b. Windows baseline passed 72/72,
including 127 console-backend checks and Session tests. macOS and Linux jobs passed.
No restart-failure receipt was emitted: the earlier main failure did not recur.
This is not a runtime fix or a diagnosis of the child's failing predicate.

The full Windows job failed: developer layout exited without its expected marker
(actor 5, 33x13, stale suffix); history-file errors were also observed but are not
an established cause. Explicit Settings repeats 1, 2, 5 and 7 failed with malformed
bracketed paste. Application cases passed 14/15, repeats 6/10 explicit plus 3/3
default. All 29 compact receipts recorded reaped processes without outer timeout,
cleanup error or truncation: 24 successes and five failures. Cleanup success is
not scenario success. No shell fix or fully green Windows result is claimed.
