# Fixed passive runtime review — run 40924737063

Exact remote commit `7e56aaca0a6e916e08d06ad94244fd20c2259626`, job
`123627103130`, terminal **failure**. Its published tree is the local candidate
`48ed444e2d5db01174d481c9f567ec2a6baa1d5a` tree (root verified publication).
No rerun or runtime/default promotion follows from this review.

Artifact `12921257464`, ZIP 1,648,782 bytes, SHA256
`aa969c8306006214c10e0909820dd4347595b5844a91555d1f63ecb397d328a2`.
Local archive hash and all 103 build plus 37 runtime manifest entries verified.
Packet has 24,835,474 inventoried bytes, no missing required or rejected files.
Evidence retained at `/workspace/shared/passive-40924737063-evidence/artifact`.

## New native evidence

Both source builds and actual linked errno/handler/emitter gates passed. Both
strict native fixture compiles, reader imports and isolated staging passed.
This closes the earlier missing linked-code execution precondition, not general
async-signal safety or production qualification. DLL hashes are recorded build
identities; the packet deliberately excludes binaries:

- stock `bfb24849cc8d492380b01813418a65c1f9f69463dfc2b9dd7944cffeb228983e`
- refined `66f6b318fa4e9a00696a220942fd8fd8534f03d7516d18dc6d65d63ab81ef024`

All eight fixed trials ran once. Both cases, both variants, both orders failed
byte/fixture qualification. All eight reader alarms exited124; outer controllers
were reaped without watchdog timeout, truncation or cleanup error. Native queue
residue was zero. Trace schemas are valid, finalized, with no overflow or context
incompleteness. Full planned path verdicts remain **INCONCLUSIVE**: owned outcome
incomplete. No failure is reclassified by the observations below.

Each trace contains one unique complete same-thread read chain that copied one
byte into the caller buffer, then returned -1 with EINTR(4), despite copied=1.
The next read returns one byte in partial-zero or two in positive-prefix; the
subsequent read does not finish before the reader alarm. Per-return fixture logs
contain counts, not byte values; alarm termination prevents a final synthetic
hex line. Do not invent exact recovered tail-byte content from these logs.

All eight traces independently show changed WINDOW_BUFFER_SIZE_EVENT(4) at
index0, geometry110x30 to109x30, sole SIGWINCH(28) request, paired handler events,
input_winch return, and the copied=1/EINTR read end. The intended changed mouse
path was not reached. No master queue read/write event16 appears in any trace;
master observations/lock events alone do not prove the planned queue-rewrite
path. The predeclared mouse/master full predicates must not be loosened to turn
these resize failures green.

## Interpretation and bounded next action

Interrupted partial-read reporting is now an actual numeric observation in
both instrumented variants, not only the earlier external EINTR hypothesis.
The reviewed source's `fhandler_console::read` sig_exit unconditionally assigns
buflen=-1 after setting EINTR, even when copied_chars>0. This is consistent with
and localized by these traces. It does not uniquely explain Settings leading-ESC
loss and does not establish stale-prefix retirement coverage.

A separate narrow source proposal could preserve an already copied positive
count at sig_exit while retaining zero-copy EINTR. It needs its own source/model
review, signal/restart and input-mode reasoning, and unchanged native byte gates
before any production decision. Do not combine it implicitly with refinement,
change timing, retry/split input, suppress resize, bypass bracketed paste, or
rerun this unchanged matrix to chase green. Runtime replacement remains disabled.
