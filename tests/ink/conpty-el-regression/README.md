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
are serialized during this check; another unrelated concurrent direct-child
launch causes a fail-closed ambiguity error. This is deliberately an isolated
qualification mechanism, not a general production concurrent-process discovery
contract. A missing host is rejected before DLL execution or child creation.

A provider-directory argument with a nonexperimental provider is rejected.
POSIX rejects Windows provider selection. `bundled_provider` is set only after
host verification; advanced protocol qualification remains false. No signatures
or Windows runtime behavior are claimed verified by the Linux cross-compilation
checks. The signature gate and actual native executions must pass in Windows CI.
