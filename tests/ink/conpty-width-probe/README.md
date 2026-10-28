# Native ConPTY width-accounting diagnostic

Qualification only, not an Ink compatibility pass or an EL semantics change.
Build Windows target `conpty_width_probe` with
`-DSTORMTTY_TEST_CONPTY_WIDTH_PROBE=ON`. No npm install or Ink CMake option is
required. Run with a **fresh absolute directory**, once per provider:

```powershell
./build/conpty_width_probe.exe "$PWD/inbox-width"
./build/conpty_width_probe.exe "$PWD/packaged-width" --packaged-provider "$PWD/conpty-probe"
```

The packaged argument uses the existing exact-pinned, hash-checked provider path
and actual OpenConsole identity check. Prepare it using the existing signed
package qualification workflow. Never silently substitute inbox.

## What is measured

Each provider gets six independent native processes: 56, 72 and 73 columns,
with `WriteConsoleW` and UTF-8 `WriteFile` routes separately. Every process:

1. Writes an ASCII exact-fill row.
2. Writes the exact Unicode atoms from the fixture, querying
   `GetConsoleScreenBufferInfo` immediately after every atom (including separate
   base e and combining acute), padding and the final border.
3. Writes the Unicode row in one call, avoiding atom-write timing as a confound.
4. At 73 columns, additionally writes the fixture's actual 72-column capped box
   row. The original 73-column observation was not an exact-fill box.

All cursor samples, original/effective console modes, code pages, and intended
UTF-8 output bytes are written to separate `CREATE_NEW` files. No diagnostic
text goes through the measured console. The native child supplies **no EL**.
Any EL in its transport is provider-generated. Short fixed pauses permit old
frame-based emitters to observe completed rows; they are observation windows,
not a synchronization guarantee or acceptance oracle. Source output captures
are written immediately before each native write and mean intended output if a
write fails; child exit status must be checked.

`WriteConsoleW` plus OR-ing `ENABLE_VIRTUAL_TERMINAL_PROCESSING` into the
inherited output mode matches the inspected libuv output mechanism; it is not
a claim of reproducing every Node version's initialization. UTF-8 input/output
code pages are explicitly set and logged (WriteConsoleW is codepage-independent).
`WriteFile` is a separate comparison, not a substitute for Node. Record the
actual CI `node -p JSON.stringify(process.versions)` and compare its libuv
revision before claiming a complete fixture-mode match.

Artifacts per process:

- `*-cursor.tsv`: raw native cursor/buffer/window coordinates and modes.
- `*-cursor.stdout.bin`: intended UTF-8 writes, in call order.
- `*-transport.bin`: exact received ConPTY bytes, including final drain.
- `*-ghostty-viewport.txt`: pinned consumer's final visible rows; no ANSI strip.
- `*-metadata.txt`: provider diagnostics, verified bundled identity bit, child
  exit code/error, drain/reap result and byte count.

Transport is bounded at 1 MiB; overflow fails instead of dropping bytes. The
parent uses the real TerminalCore only to answer terminal queries and report
its viewport. A 15-second child deadline plus bounded close/reap protects CI.
Success means collection completed, **not** that borders were correct. Replay
both source and transport against pinned Ghostty and independent xterm.js;
retain exact border checks. No resize is requested anywhere in this probe.

Record the OS build/revision and executable file versions/hashes in the job.
System32 conhost metadata alone is not proof of the spawned inbox image.
Do not identify inbox binary code with an arbitrary public Terminal commit.

## Source-backed motivation

- Microsoft old emitter, release `v1.21.3231.0`, commit
  `0c98216d2ff8718511f2e06baa1fa82291523940`,
  [`src/renderer/vt/paint.cpp`](https://github.com/microsoft/terminal/blob/0c98216d2ff8718511f2e06baa1fa82291523940/src/renderer/vt/paint.cpp):
  `_PaintUtf8BufferLine` tracks cluster columns, optimizes spaces with ECH/EL,
  and explicitly suppresses EL when its tracked cursor is past the margin.
  Its comments warn EL resets delayed wrap. This is evidence about that public
  implementation, **not** a source mapping for the tested inbox binary.
- Packaged release `1.25.260930003`, matching Terminal source
  `dc8ae365847d41f62d1ebca93815bb00959bc12f`,
  [`src/host/_stream.cpp`](https://github.com/microsoft/terminal/blob/dc8ae365847d41f62d1ebca93815bb00959bc12f/src/host/_stream.cpp):
  `WriteCharsVT` updates the local parser then forwards original chunks, with
  newline translation and required mode reinjections. The old renderer/vt
  directory is absent. This explains a structural provider difference, not
  proof of every supported protocol.
- [libuv v1.51.0 tty.c](https://github.com/libuv/libuv/blob/v1.51.0/src/win/tty.c):
  `uv__tty_emit_text` uses WriteConsoleW; `uv__determine_vterm_state` ORs VT into
  existing output mode. Compare with the CI Node/libuv version.

The captured 56-column Unicode row has 54 codepoints, 55 UTF-16 code units and
56 terminal cells under Unicode11-compatible widths. Its trailing EL erases a
border in Ghostty but not xterm.js. The old emitter's explicit suppression means
**internal width/state disagreement is a live hypothesis**, not evidence of an
intentional xterm.js-only dialect. This probe tests that hypothesis. GSBI gives
cursor coordinates, not the internal pending-wrap flag or renderer cluster
counts; if cursor samples agree but output still disagrees, source-level emitter
instrumentation of a separately built OpenConsole may be required.

## Separate resize issue

Pinned Ghostty has no public EL dialect option. It does have documented
`GHOSTTY_TERMINAL_OPT_RESIZE_PULL_SCROLLBACK`; StormTTY already sets it false for
ConPTY. Primary resizes reflow with autowrap, alternate-screen resizes never
reflow. xterm.js windowsPty changes scrollback/reflow/wrap heuristics, not EL0.
Locally, the existing exact-hash compact-alt replay retains its residual row for
all four settings: default, windowsMode=true, conpty build19041 and conpty
build26200. All corresponding baseline-offset controls are clean.

Microsoft's exported resize writes a signal packet; successful return is not an
output-stream barrier. Neither disabling primary reflow nor changing EL can
repair an already demonstrated old-width tail arriving after alternate resize.
No undocumented flags, blank clears, stream replay, or VT filtering are proposed.

## Verification scope

Linux-host Windows GNU strict compilation and linking are compile gates only.
Native MSVC execution, inbox/package comparisons and artifact interpretation
remain required. No production source is modified by this diagnostic target.
