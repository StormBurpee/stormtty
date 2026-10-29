# Pre-instrumentation external matrix — 2026-10-29

Read-only audit of run **40892442025**, job **123512562493**, exact source commit
`54fd5d0beef2c56a00771b2741908d90e3d65adf`. This run failed; no retries were made.
Artifact **12908241223** ZIP SHA256:
`f87cdca53f6f1c63dddbf861a82bd7d5cfb21b2bb1d13f7d98fb49679d7360db`.
All 24 actual trials and their individual reader/injector logs were retained.

| Scenario | Stock, two orders | Refined, two orders | Actual result |
|---|---|---|---|
| raw-vmin2 | 2 failures | 2 failures | read -1/EINTR, then select ready, reader alarm exit124 |
| select | 2 failures | 2 failures | read -1/EINTR, then select ready, reader alarm exit124 |
| canonical | 2 passes | 2 passes | exact `61620a` |
| mouse-on | 2 failures | 2 failures | read -1/EINTR, then select ready, reader alarm exit124 |
| surrogate | 2 failures | 2 failures | `f09f988062`, expected `61f09f988062` |
| longjmp | 2 passes | 2 passes | exact `61620a` |

Totals: eight passes, sixteen failures; no verdict difference by variant/order.
Every trial's exact internal-path verdict is **INCONCLUSIVE**. Native queue
residue was zero in all 24. All outer processes were reaped without outer
watchdog timeout, output truncation or cleanup error. A reader alarm exit124 is
still a real scenario failure, not a successful cleanup-based result. Surrogate
reader/injector exit0 does not override its failed exact-byte comparison.

Locally rehashed provenance confirms the unchanged official source archive
`4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204`, stock
console `535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33`,
refined console `bc68a642dca059ed76b6ae79b21db85e34ff3376f98a5efe4bb2b0136b79f0ae`,
and braced behavior patch
`1afc3483da5dcc8848f336619f858c0ffcfbef608d1446464e6566135b2aff66`.
Both native compiler identities are GCC/G++15.3.0, GNU++17. Recorded build and
runner DLL hashes agree (the artifact excludes binaries, so these are recorded
identities, not fresh local rehashes of DLL bytes):

- stock: `f1e51f2f5ed00bd382d720e401f6cc2122f9110c83b2fefa6c3f1461f95de5af`
- refined: `a29e5e8529638019b4f4e1137b0921866a36cef0b00c51055ba9204dfe6cfa2e`
- original: `d3b5324eef7fe377c3412bd8d9f5c526ba1136940e34105436b15e5f1ae90717`

Source review hypothesis, **not a proved execution cause**: the common
`fhandler_console::read` sig_exit sets buflen=-1 even if copied_chars is positive.
EINTR followed by missing `a` is consistent with partial-read interruption,
but external records do not prove the copy count or which mouse/resize branch
caused the signal. No behavior change is added here. The passive recorder is
intended to discriminate those paths while preserving these failures.
