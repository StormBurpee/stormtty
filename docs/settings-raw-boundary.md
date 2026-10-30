# Authored Settings-close input boundary

This isolated Windows diagnostic is not a production switch or runtime qualification.
Configure `STORMTTY_TEST_SETTINGS_RAW_BOUNDARY=ON` to build the separate, non-installed
`stormtty_settings_raw_probe` target. Normal `stormtty` keeps its normal transport ABI.
The diagnostic refuses ordinary launches; use `--smoke-settings-raw` with the existing
`--shell-runtime` option pointing at the unchanged bundled Zsh runtime. Its provider
selection uses the existing configuration/explicit console-runtime option; no candidate
runtime, provider, compiler pin or production default is changed. Completion-seed builds
cannot be combined with this target.

The host creates a private GUID directory and one raw Zsh `-df` reader with private HOME
and ZDOTDIR. It opens the real asynchronous Settings panel, presents it, cancels it,
performs exactly one +80-physical-pixel window-width transition through the normal
WM_SIZE/layout/session path, and presents the closed scene. Only after a new requested
resize generation is applied with a changed grid does it post a separate input event.
The posted event rechecks that exact generation and grid; there is no restore or retry. That event submits only
`PASTE_QUAL_0123456789_[x]_end` and Enter through the existing Session paste/key path.
The terminal must have negotiated mode 2004. Human keyboard, mouse, clipboard, IME and
drop ingress is disabled for this diagnostic. Read bytes are never shell commands.

`SETTINGS_RAW_POST` records mode 2004, provider, baseline/requested/applied resize
generations, before/after client widths and column counts. The reader writes
at most 2048 hexadecimal characters into its private result file and exits. After child
exit the host reports exact raw-file bytes and completed ConPTY WriteFile evidence,
including missing/truncated/failed-write indicators. The raw result is not reconstructed
from rendered terminal cells. Any extra protocol bytes remain visible and cause a strict
mismatch; they are not silently stripped. Missing/incomplete evidence is INCONCLUSIVE.
The existing smoke deadlines, owned process teardown and private-directory cleanup are
unchanged. There is one trial per provider and no retries. This is not part of normal
CI; publishing the designated diagnostic branch intentionally triggers isolated execution.

A PASS establishes this one authored event only. Writer-exact/raw-different evidence
narrows the discrepancy to after accepted writes versus raw child consumption, but does
not distinguish ConPTY from MSYS internals. It does not test ZLE, user configuration,
physical input feel or general reliability. A clean event does not erase prior failures.

Portable helper tests and cross-compilation do not constitute a native execution result.

The event requires an applied resize since RAW_READY and the explicit transition.
No resize, stale generations, unchanged grid or drift before posted paste is
INCONCLUSIVE and cannot launch a substitute trial. The reused reader requests raw/no-echo
termios; direct Win32 input-mode bits remain explicitly unobserved. Mode 2004 and raw
termios are different contracts; neither is promoted into direct Win32 mode evidence.

## Isolated native workflow

The branch-only `diagnostic/settings-raw-boundary` workflow is an intentional execution
trigger: publishing that branch runs it. It restores the existing pinned engine,
compiles the production app and separate diagnostic, checks graph and binary-marker ABI
isolation, and runs portable/source/runner contracts before the two native events.
Only stock shell and existing inbox/packaged console providers are used. The owned
runner retains its 60-second bound and 10-second reap bound; there are no retries.

Each provider keeps an authored log and runner metadata with exact log hashes, source
commit, diagnostic executable and stock shell/runtime hashes. No binary, private HOME,
raw fixture directory or screenshots are uploaded. Missing cleanup, truncated capture,
malformed/duplicate markers, unsuccessful child or incomplete writer evidence cannot
pass. A byte mismatch remains failure; missing or inconsistent resize evidence remains
INCONCLUSIVE (workflow exit 2), even if a partial log contains a byte PASS. No result
permits runtime promotion or general native qualification.
