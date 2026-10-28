# Inbox ConPTY EL-at-right-margin incompatibility

This is a diagnostic evidence package, **not** a compatibility pass, expected-fail
conversion, or proposed change to Ghostty's global EL semantics.

Native Windows job `122654507942`, commit `edf353d`, reproduced missing Unicode-row
right borders at static 56×20 and 72×20, but not 73×20. No resize was involved.
Producer stdout replay was coherent. The same exact ConPTY stream was damaged in
Ghostty and coherent in xterm.js 6.0.0 with Unicode11. That difference alone does
**not** prove a Ghostty bug: the two engines intentionally disagree about EL
while autowrap is pending.

## Minimal reproduction

`minimal.bin` is seven synthesized ASCII/control bytes: `ABCD ESC [ K`.
SHA-256: `de5c570be816d7018e0bd2448f72e91e07747084dc5cce8666650b6dfff7f921`.
At four columns, printing D sets pending wrap. EL then produces:

| Input / width | Pinned Ghostty | xterm.js 6.0.0 |
|---|---|---|
| `ABCD ESC[K` / 4 | `ABC ` | `ABCD` |
| `ABCD ESC[0K` / 4 | `ABC ` | `ABCD` |
| `ABCD` / 4 | `ABCD` | `ABCD` |
| `ABCD ESC[K` / 5 | `ABCD ` | `ABCD ` |
| `ABCD ESC[KZ` / 4 | `ABCZ`, blank row 2 | `ABCD`, `Z` on row 2 |

Ghostty's selection text also loses D, ruling out a StormTTY frame-cache-only
problem. `repro.cpp` checks these pinned semantics through real TerminalCore;
`replay-xterm.cjs` checks the independent implementation without needing Unicode.
Both were run successfully on Linux; the C++ diagnostic also strictly
cross-compiled for Windows GNU with `-Wall -Wextra -Wpedantic -Werror`.

`unicode-row.bin` is the unmodified first Unicode row extracted from the native
56×20 transport capture, including its final `ESC[K CR LF`. Start it at column 0
in a 56-column terminal. The final border occupies the last cell before EL.
The parent capture SHA-256 is
`bd3f4e33071e83b43d78350e5ce7a4192de4dfed35a0319cc4b3abccbd469061`.
Unlike most rows, this ConPTY-rendered row contains a trailing EL; that explains
why Unicode appeared implicated. The seven-byte reproduction needs no Unicode,
Ink, Node, resize, or Windows execution.

## Semantics cross-check (2026-10-28)

- [Original XTerm util.c](https://github.com/ThomasDickey/xterm-snapshots/blob/master/util.c):
  `ClearRight` erases inclusively from `cur_col`, asserts it is no greater than
  the last column, and resets wrap. `do_erase_line` calls this for EL0.
- [Microsoft Terminal AdaptDispatch](https://github.com/microsoft/terminal/blob/main/src/terminal/adapter/adaptDispatch.cpp):
  `EraseInLine` resets delayed wrap and fills from the current cursor column.
- [Pinned Ghostty Terminal.zig](https://github.com/ghostty-org/ghostty/blob/46e9dc9a927b2e7e7cba79e6902c176127dbdf2c/src/terminal/Terminal.zig):
  `eraseLine` uses the last-column cursor, resets pending wrap; its existing
  `eraseLine resets pending wrap` test requires subsequent printing to overwrite
  the last column. This agrees with the original XTerm behavior.
- [xterm.js 6 InputHandler](https://github.com/xtermjs/xterm.js/blob/6.0.0/src/common/InputHandler.ts):
  `eraseInLine` allows the one-past-end cursor; EL0 has an empty erase interval
  there and subsequent printing still wraps. **xterm.js is not original XTerm.**

Do not globally change EL or filter PTY bytes merely to turn this test green.
The evidence establishes an inbox ConPTY repaint/consumer-semantics incompatibility.

## Next supported experiment (not implemented here)

Compare the same native static-margin and transport probes with the official
[Microsoft.Windows.Console.ConPTY package](https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY)
(published stable version observed: `1.25.260930003`) versus inbox. Pin and verify
the package before use; keep its `conpty.dll` and matching `OpenConsole.exe`
together. Resolve create/resize/close from the same provider and explicitly
verify the spawned provider rather than silently accepting inbox fallback.
[Microsoft's loader source](https://github.com/microsoft/terminal/blob/main/src/winconpty/winconpty.cpp)
shows that a missing adjacent/architecture-specific OpenConsole can fall back
to inbox. No package was installed for this research.

[Windows Terminal 1.22's announcement](https://github.com/microsoft/terminal/discussions/18516)
records the rewritten console hosting implementation. This makes the packaged
provider a narrowly motivated experiment, not an established fix. Compare
actual emitted bytes, exact border assertions, resize, input, and shutdown;
do not assume every protocol is verified if this one defect disappears.

The [documented inbox CreatePseudoConsole API](https://learn.microsoft.com/en-us/windows/console/createpseudoconsole)
only lists flags 0 and INHERIT_CURSOR. The historical Windows Terminal
[experimental passthrough profile setting](https://learn.microsoft.com/en-us/windows/terminal/customize-settings/profile-advanced#vt-passthrough-mode)
is not a documented kernel32 passthrough API for this host. Do not invent or
hard-code a numeric passthrough flag. The current packaged source exposes glyph
width choices; those are not proof of a passthrough switch or of this defect's fix.

## Running the small probes

Compile `repro.cpp` against the same StormTTY include directory,
`libstormtty_terminal`, and pinned `libghostty-vt` used by `terminal_tests`.
Then run the resulting executable. For the independent replay:

```sh
node tests/ink/conpty-el-regression/replay-xterm.cjs \
  /absolute/scratch/node_modules/@xterm/headless
```

This diagnostic records known differences; the existing Ink acceptance tests
remain strict and unchanged.

## Explicit experimental provider qualification

An opt-in implementation now exists; it does **not** change the product default.
On an isolated native Windows x64 CI runner, prepare a fresh job-private path:

```powershell
./tools/prepare-conpty-probe.ps1 -Destination "$PWD/conpty-probe"
# Stop if preparation/signature validation failed. Never continue from a partial path.
$provider = "$PWD/conpty-probe"
$node = (Get-Command node).Source
$fixture = "$PWD/tests/ink/fixture.mjs"
./build-msvc/ink_session_tests.exe $node $fixture provider-missing-host --packaged-provider "$provider/missing-host"
./build-msvc/ink_session_tests.exe $node $fixture static-margin-probe --packaged-provider $provider
./build-msvc/ink_session_tests.exe $node $fixture transport-probe "$PWD/packaged-captures" --packaged-provider $provider
./build-msvc/ink_session_tests.exe $node $fixture primary --packaged-provider $provider
./build-msvc/ink_session_tests.exe $node $fixture fullscreen --packaged-provider $provider
./build-msvc/ink_session_tests.exe $node $fixture compact-alt --packaged-provider $provider
./build-msvc/ink_session_tests.exe $node $fixture host-close --packaged-provider $provider
```

CI must record each exit status separately and retain strict failures; a successful
negative packaging test is not Ink qualification. Ordinary invocations remain
inbox. Use fresh capture directories. Never put either binary in system folders
or global PATH. The package is acquired only from its exact official NuGet URL;
its published catalog SHA512 was independently matched during preparation of this
experiment. The script pins the resulting archive and binary SHA256 values,
requires NuGet signature validation, then validates Microsoft Authenticode on
both extracted binaries before use.

The runtime separately verifies the pinned file hashes and holds handles denying
modification/deletion throughout use. It loads the absolute DLL with restricted
DLL search flags, pairs all HPCON operations with that module, and verifies a new
direct child's executable image is the expected OpenConsole before launching the
fixture. No private HPCON structure layout is read. Our Process host creations
are serialized during this check. New children are matched by the exact expected
OpenConsole image path, so unrelated Node clients launched by other panes are
permitted. Zero or multiple matching new hosts fail closed, as do identity-query
errors other than a child that already exited. This remains experimental; it
does not establish a general identity contract for external components that
concurrently launch the same OpenConsole image outside our creation lock. A missing host is rejected before DLL execution or child creation.

A provider-directory argument with a nonexperimental provider is rejected.
POSIX rejects Windows provider selection. `bundled_provider` is set only after
host verification; advanced protocol qualification remains false. No signatures
or Windows runtime behavior are claimed verified by the Linux cross-compilation
checks. The signature gate and actual native executions must pass in Windows CI.


### Repeated concurrent qualification

Run `ink_session_tests NODE FIXTURE provider-stress --packaged-provider DIR`
with a 600-second CI timeout. The same mode without provider selection is a
portable POSIX control. It creates four concurrently live panes for each of
12 rounds: primary, fullscreen, and two compact alternate. Each round includes
an intentionally missing child executable followed by successful session launches,
unique per-pane input, four coherent animation frames, overlapping shrink/grow
requests, then mixed natural exit and host close with drain/reap checks.

This totals 48 pane resize cycles, including **24 compact alternate cycles**;
any checkpoint failure remains a failure, and all reachable pane outcomes are
reported. This does not waive or disprove the separately captured upstream
compact-resize race. Local Linux control completed 48/48 cycles with zero
failures; full Windows GNU executable linking and strict provider/test compilation
passed. Native packaged-provider concurrency results and their limits are recorded below.

Before any eventual default switch, centralize package/version/archive and payload
hashes in the dependency lock rather than independently updating C++ and PowerShell
constants. Build packaging must use the same verified pair, retain official license
metadata, check shipped hashes, and exercise missing/corrupt/wrong-architecture
failures. Keep inbox an explicitly labeled fallback, not silent substitution.
Default selection, broader OS/architecture qualification, and production package
layout remain separate reviewed work; this diagnostic does not enable them.


### Native repeated qualification: 2026-10-28

[Run 40642828448, Windows job 122664693371](https://github.com/StormBurpee/stormtty/actions/runs/40642828448/job/122664693371)
checked commit `b0ab3104e9b6129531fd4ed5ae6386e210de5689`, official packaged
ConPTY `1.25.260930003`, native MSVC, Node 22, and Windows Server 2025.
NuGet verification and Microsoft Authenticode succeeded; the exact pair hashes
matched and actual OpenConsole identity was verified. Inbox controls still
failed the previously isolated right-margin EL condition.

The packaged provider passed the missing-host rejection, all three static-margin
cases, and individual primary/fullscreen/compact-alt/host-close cases. Repeated
concurrent qualification was **43/48, not a pass**:

| Stress scenario | Completed cycles | Failures |
|---|---:|---:|
| Primary | 12/12 | 0 |
| Fullscreen alternate | 12/12 | 0 |
| Compact alternate | 19/24 | 5 |

All five failures were pane 2 in rounds 1, 3, 5, 7, and 9, at the 80×24→56×20
shrink checkpoint. Every failure observed 151 distinct incomplete frames and
zero coherent frames over its checkpoint window. The last viewport had a clipped
old top border at row 0, an otherwise complete current box at rows 1–14, visible
`Size: 56x20`, and an intact Unicode right border. Session state remained running
with an empty error field; the provider identity was verified and advanced
protocol qualification remained false.

This is the same persistent residual-row shape as the independent
[compact alternate resize reproduction](../resize-race/README.md), which uses
identical captured bytes with two resize offsets and reproduces the failure in
both Ghostty and xterm.js. It is **separate from the inbox EL defect**. The native
stress run did not capture each failure's raw byte/resize ordering; attributing
its exact timing to that independently reproduced mechanism is an inference,
not a claim that the native streams were themselves replayed.

No identity, failed-launch recovery, pane-input, or asserted close/reap failure
was logged. All 12 intentional failed-launch recovery checks completed without
a reported failure, and all 43 completed panes passed their applicable mixed
natural-exit/host-close assertions. The five failed panes did **not** proceed to
grow or natural-exit assertions: their cleanup requests run, but the fixture
destructor does not assert its close-wait result. Thus this run is not evidence
of 48/48 successful lifecycle qualification, nor of exhaustive leak freedom.

#### Recommendation for opt-in application integration

Proceed only with explicit `--console-runtime` selection of the verified pinned
pair; retain inbox as the unchanged default and retain a visible experimental
provider identity. Missing/corrupt/wrong-runtime packaging must fail before
launch, never silently substitute inbox. The evidence supports removal of the
inbox EL artifact for this workload and successful primary/fullscreen concurrent
cycles; it does **not** justify claiming general Ink resize compatibility,
compact-alt qualification, all advanced protocols, physical input/IME, or a
production-default provider switch.

Keep the strict compact regression and aggregate stress result red. Do not
filter bytes, clear the host viewport, skip failed rounds, or relabel them as
expected passes. The opt-in application smoke must separately verify actual
provider identity, multiple pane creation, resize/close, and error reporting;
it cannot inherit native GUI verification from this headless Session workload.

#### Follow-on cleanup assertion (not present in the run above)

`provider-stress` now independently checks every pane that failed rendering:
request close, require `wait_closed(12s)`, then require exited state and no
lifecycle/transport error. It prints `failed-pane cleanup PASS (rendering still
failed)` or an additional cleanup failure. These checks do not increment the
completed-render-cycle count and cannot turn 43/48 into a pass.

The added branch was exercised on Linux using a separate scratch fixture that
immediately exits without rendering: all 48 rendering cases remained failures,
all 48 cleanup checks passed, and the diagnostic correctly returned exit 1.
This checks failure accounting and cleanup of already-exited actors; native
cleanup of the persistent live resize failures still requires the next CI run.
The existing seven Linux Ink/process/session cases passed, the full Windows GNU
executable linked, and strict Windows test compilation passed after this change.
