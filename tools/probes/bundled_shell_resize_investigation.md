# Native resize input loss: 2026-10-28 investigation

## Observed, not inferred

Native CI run **40696384306**, Windows job **122844156357**, candidate
**f3f4a6cf99f4db777aa4a4d9e83cd6caf777e8ec**:

- Packaged ConPTY completed all six alternating resize phases.
- System ConPTY passed phases 1–3, failed phase 4 (111×35 → 100×30).
- Submitted `stormtty_probe_size`; shell executed `tormtty_probe_size`, status
  **127**. The failure is input corruption, not failure to update dimensions.
- Memory ring record 1: prompt sequence 18, old 111×35, line-init status 0.
  Record 2: sequence 18, new 100×30, first self-insert key `t`, empty buffer.
  Record 38 accepts the truncated command. The missing `s` was never seen by
  the instrumented self-insert widget; the ring had only 98 records (no wrap).
- Before diagnostic commands: requested and applied generations both **4**,
  requested and applied grids both **100×30**, four resize calls, zero failures,
  zero queued input bytes. This is native API success, not an ordering fence.
- After preserving the original frame/controller snapshot, journal flush ran;
  then Zsh 100×30 → stty `30 100` (rc 0) → Zsh 100×30 → native 100×30 (rc 0)
  → Zsh 100×30. No forced signal. Original qualification remained failed.

Job: https://github.com/StormBurpee/stormtty/actions/runs/40696384306/job/122844156357

## Concrete upstream race candidate, not yet a traced root cause

The pinned MSYS2 package is **3.6.11-4**. Its packaging metadata identifies
runtime commit **00eb8435b1c498e9211c7a7cee01584d76bd5f21**. Inspected console.cc
blob **a7cdbed500c6235e21f41fe9d0be5075a8c471f1** is identical at that commit and
branch msys2-3.6.11.

The reader peeks input records under input_mutex. On a resize record,
`send_winch_maybe()` temporarily releases that mutex around signal delivery.
The master thread can acquire it, remove the resize event, and put normal keys
back. The reader then reacquires the mutex but discards records using its old
peek index. With a leading resize followed by `stormtty_probe_size`, this can
remove `s` instead of the resize record already removed by the master.

Relevant locations: reader peek at 1460; resize dispatch at 1834; final discard
at 1871; mutex release/reacquire at 1240–1242; master removes resize at 553–576
and writes normal records back at 580 onward.

- Exact runtime source: https://github.com/msys2/msys2-runtime/blob/00eb8435b1c498e9211c7a7cee01584d76bd5f21/winsup/cygwin/fhandler/console.cc
- Package recipe: https://github.com/msys2/MSYS2-packages/blob/master/msys2-runtime/PKGBUILD
- Package runtime identity: https://github.com/msys2/MSYS2-packages/blob/master/msys2-runtime/msys2-runtime.commit

`python3 tools/probes/msys_resize_race_model.py` deterministically demonstrates
that interleaving and that retiring the consumed record before releasing its
lock preserves the first character. **This model is not a native reproduction
or evidence that this exact interleaving occurred in the CI process.**

## Defensible next experiment and fix boundary

A narrowly instrumented MSYS runtime should record only event type/count and
fixed-fixture key identifiers around reader peek, mutex release, master removal,
and reader discard. Run the existing unweakened six/50-phase probe against
both providers. Alternatively, compare an isolated runtime candidate that
retires the resize record under the input lock before notifying SIGWINCH and
avoids discarding the same record again. Audit mouse-event callers and already
buffered characters as part of that upstream change; blindly moving the
signal or holding the lock across it risks reentrant-handler deadlock.

Before attributing the native incident conclusively, independently establish
that the complete synthetic command reached the ConPTY pipe (a qualification-
only bounded transport tap, not production command logging). Current ZLE
journaling alone cannot distinguish terminal encoding, ConPTY, and MSYS loss.

Do not add sleeps, resend the truncated command, force WINCH, or substitute the
correct postfailure dimensions for the failed assertion. Serializing the native
resize call with WriteFile is not a demonstrated fix: ResizePseudoConsole writes
a separate signal pipe and does not acknowledge shell/event consumption. A
single packaged-provider pass does not establish that provider is immune.

No production patch or dependency change is included here. Building/pinning a
patched runtime also requires reproducible package provenance and license/source
notices; do not silently modify or replace the existing pinned binary.
