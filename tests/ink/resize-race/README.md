# Ink 8 compact alternate-screen resize reproduction

**Diagnostic, not a compatibility pass. Do not publish an upstream issue without
the user's approval.** The live C++ `compact-alt` scenario remains strict.

Observed on native macOS in commit `2eef03e`, CI run `40624900777`: after shrinking
from 80×24 to 56×20, a clipped old top border remains at row 0 above a complete
current Ink box at rows 1–14. The condition persisted for the 12-second wait
(151 observed frames, 1 coherent, 150 incomplete).

## Independent reproduction

`trace.json` contains actual UTF-8/VT bytes captured from our official npm
Ink 8.0.0 / React 19.3.1 fixture through Python's standard-library `forkpty`, not
StormTTY's process transport. Node 24.22.1 ran the compact alternate/incremental
scenario, with a real TTY and `TERM_PROGRAM=stormtty`. The fixture source hash
identifies its revision before the later fullscreen option was added; compact
behavior is unchanged. Only fixed test content is present, with no user data.

The two replays consume the identical byte stream without dropping, rewriting,
duplicating, or reordering terminal bytes:

- **Baseline:** resize after byte 4,326. The current alternate box is clean.
- **In-flight:** resize after byte 4,143, allowing the remaining 183-byte
  old-width animation update to arrive at the new width. It wraps and moves the
  cursor one extra row. Ink's next relative erase leaves the clipped old border.

Subsequent genuine updates remain in the trace; this is not an intermediate
snapshot. Both pinned Ghostty and official xterm/headless 6.0.0 reproduce the
same persistent row displacement. This is evidence of an app/library redraw
contract gap under legitimate resize ordering, not a host-specific parser bug.
This independent capture is not the failing macOS CI stream; the exact timing
in that native run is inferred from the matching shape, not directly observed.

## Replay with the independent parser

Install the reference only in a **separate scratch directory**, not in the
application or fixture dependencies:

```sh
npm install --prefix /absolute/scratch/ink-reference --save-exact \
  @xterm/headless@6.0.0 --ignore-scripts --no-audit --no-fund
node tests/ink/resize-race/replay-xterm.cjs \
  /absolute/scratch/ink-reference/node_modules/@xterm/headless
```

The script checks the reference version and capture SHA-256 values, prints both
visible viewports, and asserts the diagnostic reproduction. A successful replay
means **the gap was reproduced**, never that Ink compatibility passed.

## Likely upstream repair area

Ink's `resized()` calls cursor-relative `log.clear()` using the previous line
count. A compact alternate-screen app does not take its fullscreen absolute
home/erase redraw branch. Its resize redraw therefore cannot recover this
cursor displacement. `shouldSynchronize()` is TTY/interactive-based, not gated
on `TERM_PROGRAM`; DEC2026 does not serialize a host window resize with child
SIGWINCH handling. A correct repair should restore the app-owned alternate
viewport and line accounting on resize, not erase primary-screen user history.

No production host erase, byte filtering, artificial frame, upstream package
patch, or expected-pass reclassification is included in this reproduction.
