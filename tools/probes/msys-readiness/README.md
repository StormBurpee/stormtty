# Stock-only MSYS readiness-path diagnostic

This is a new, isolated diagnostic, **not** a replacement qualification, runtime
patch, production fix, or reason to promote candidate 0007. The completed
partial-app run `40948783314` remains a failed qualification: its unchanged raw
Zsh resize-only reader loses the leading ESC on all 18 launches, including the
untouched official runtime. Earlier refined 0006 results are separate; neither
this fixture nor its workflow builds or combines runtime patches.

## Prespecified question and matrix

Does removing the readiness wait from Zsh's one-byte `sysread` change the
observed resize-only failure under otherwise matched conditions? The timed
reader uses `sysread -i 0 -s 1 -t 30 -c read_count ch`; the direct reader omits
only `-t 30`. Both have identical byte/status/timestamp bookkeeping. This
comparison narrows a readiness/timing hypothesis; it cannot identify the
internal instruction that loses a byte or establish causality.

Only the untouched official `msys-2.0.dll` SHA-256
`d3b5324eef7fe377c3412bd8d9f5c526ba1136940e34105436b15e5f1ae90717`
and the inbox ConPTY provider are allowed. No packaged provider, runtime source
archive, runtime compiler, runtime rebuild, trace hook, interposition, sleep
workaround, focus traffic, or signal trap is introduced. The existing EXIT trap
restores terminal mode; there is **no WINCH trap**. Internally retried EINTR and
SIGWINCH count are explicitly **unobserved**, not zero. The app/session boundary
still records completed `WriteFile` requests in the existing qualification-only
build; the MSYS DLL itself is uninstrumented.

Two opposing fixed orders, eight fresh launches total:

1. timed/plain, direct/plain, timed/resize, direct/resize;
2. direct/resize, timed/resize, direct/plain, timed/plain.

Every launch runs at most 20 rounds and stops at its first byte/status/write
failure. Every
later planned launch still runs after a behavioral failure, timeout, or failed
launch. Runtime identity drift stops dependent launches rather than running
unverified bytes. No cell is retried, extended, dropped, or reordered based on
results. `results.json` records the entire ordered plan before preflight and
persists each observed result, including incomplete evidence.

## Unchanged authored workload

Derived from the unchanged, SHA-pinned `tests/paste_input_probe.cpp` kind 2,
modes 0 and 2: isolated Zsh `-df`, fresh HOME/ZDOTDIR/working directory, original
environment, `stty raw -echo`, 180x50 initial grid, identical four-request resize
sequence, and payload `PASTE_QUAL_0123456789_[x]_end`. Each round authors one
intact 41-byte bracketed paste followed by a separate CR (42 bytes total).
No per-byte writes, split delimiters, added delays, or focus events. The direct
reader may block; the same 10-second host observation and 180-second outer
watchdog remain in force, with 10 seconds for outer reaping. The writer callback
has the existing 250 ms publication observation, never an input resend.

For each observed round the log preserves child hex, every returned sysread
status/count pair, maximum and post-ESC inter-read gaps, resize generations,
and full requested/accepted write hex. Standalone cursor replies are separately
identified only in *writer attribution*; child input is never filtered. Passing
requires exact child bytes, each status/count `0,1`, and exactly two complete
authored writes. The producer itself checks that shape before advancing; split
writes with the same concatenated bytes still stop the launch, without resend. A timeout/error preserves available partial screen and tail
write evidence, but cannot report bytes the blocked reader has not returned.
The bookkeeping itself changes timing relative to the original fixture; even
reproducing or eliminating the failure is not causal proof. Eight launches are
not a rate estimate or general safety qualification.

Cleanup records the owned Session close, child exit status/error, and scratch
removal. The outer runner records timeout, truncation and reaping. These are
specific lifecycle observations, not proof about arbitrary descendant trees.
`complete` means the outer/result/cleanup lifecycle was observed consistently;
`exact_round_evidence_valid` records the strict reader/write/gap evidence check;
it is separate from resize coverage and the overall `passed` decision.

Resize coverage is assessed from the existing post-round snapshot, without
waiting or altering the authored schedule. Plain rounds require requested,
applied, call and failure counters all zero. Resize round r requires exactly
4*r requested generations, zero failures, a successful applied generation in
that round's range (4*(r-1), 4*r], and an increased native-call count. Coalescing
may skip generations; applied need not equal requested. The single controller
may have one call in flight, so calls cannot exceed requested or applied+1
(applied alone when it has caught up); call-count growth cannot exceed applied
generation growth plus that one in-flight allowance. Stale, regressing, absent
or inconsistent telemetry makes `resize_coverage_complete=false` and prevents
`passed`, even if all byte checks pass. This is **incomplete exposure evidence**,
not evidence of a byte-loss failure or proof that Zsh observed SIGWINCH.

The portable `contract-tests.cpp` test exercises the exact producer write-shape
predicate, including every two-write split, combined writes, standalone CPR,
partial/failed/extra writes and lost evidence. The Windows workflow builds and
runs it alongside the existing input-evidence unit test.

## Run and review

Portable contracts (including a Linux local-Zsh PTY script check when available):

```sh
python tools/probes/msys-readiness/tests.py
```

The dedicated workflow builds only the Windows fixture and evidence unit test,
then runs the exact matrix. Its single experiment-branch publication trigger is
for root review/publication; do not push workflow-only edits or rerun a failed
run to obtain a green result. Artifacts allowlist only diagnostic text/JSON and
build/test logs, never binaries or HOME. The comparator verifies all official
runtime files and both fixture binaries before each launch and after the last,
and preserves source/binary hashes and the actual checkout commit.

A local portable pass is **not Windows runtime execution**. No native result is
claimed until that exact reviewed commit's Windows logs have been inspected.
