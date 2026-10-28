# Real Ink workload verification

## Boundary and current evidence

`tests/ink/fixture.mjs` is a small React application rendered by the official
**Ink 8.0.0** npm package with **React 19.3.1**. Both direct versions are exact;
the npm-generated `package-lock.json` pins transitive versions and integrity
hashes. No Babel, browser, webview, mock shell, `ink-testing-library`, or JavaScript
PTY substitute is involved. The app uses React elements directly, so it does not
need a transpiler.

`ink_session_tests` starts the real Node executable through the same C++
`Session → Process → TerminalCore` path as the native host. The platform transport
is Windows ConPTY or POSIX `forkpty`; child bytes enter the pinned libghostty-vt
engine unchanged. Assertions inspect owned `RenderFrame` cells and process state,
not stripped ANSI text, expected frames, terminal logs, or a fixture side channel.

Evidence as of 2026-10-28:

| Check | Evidence |
| --- | --- |
| Linux integration, actual POSIX PTY | Passed with GCC 14.2, Node 24.22.1, pinned libghostty-vt `46e9dc9a927b2e7e7cba79e6902c176127dbdf2c`, `CI=true` inherited |
| C++ test warnings | Built with `-Wall -Wextra -Wpedantic -Werror` |
| Integrated Linux regression suite | All 9 current CTest suites passed with `STORMTTY_TEST_INK=ON` after the coherent-checkpoint revision |
| Coherent-checkpoint revision | Passed 5 consecutive Linux runs; a scratch copy of the real Ink app with deliberately wrong borders failed after 12s with 147 incomplete and 0 coherent observed frames |
| Separate fullscreen scenario | Passed 10/10 local runs with Node 24, including real input/animation/shrink/grow/exit and full viewport-height assertions; an independent reference replay also avoided the residual top border with an old-width update in flight. Native rerun pending |
| Reproducible fixture install | `npm ci --ignore-scripts --no-audit --no-fund` passed using npm 11.9.0 |
| Non-TTY negative check | `npm test --prefix tests/ink` passed: piped CI invocation fails rather than renders a fallback |
| Native Windows ConPTY and MSVC | CI commit `c7f4a7a`, run `40622096276`: Ink failed an immediate live-box alignment assertion. Revised coherent-checkpoint waits/diagnostics need native rerun; Windows Ink is not yet a pass |
| Native macOS POSIX PTY | CI commit `c7f4a7a` passed this suite in 3.70s on Node 22; later `2eef03e`, run `40624900777`, exposed a persistent compact alternate-screen resize failure (151 observed frames, 1 coherent, 150 incomplete). macOS Ink resize is therefore not qualified |
| CI Node version | CI uses Node 22; local Node 24 results are not a Node 22 pass |
| Physical native window, fonts, animation smoothness | Not established by these headless Session tests |

This is evidence for **this one pinned workload**, not broad Ink, all React CLIs,
accessibility, IME, graphics-protocol, or performance certification. Native
renderer tests remain separate. No performance or perceptual-quality threshold
is asserted here.

## What the C++ test checks

It has separately invokable ordinary primary-screen/default, full-height
alternate-screen/incremental, and compact alternate-screen/incremental paths,
each requiring:

- Real `stdin.isTTY`, `stdout.isTTY`, and raw-input capability inside Node.
- Visible Ink box borders, bold/color styling, accented text, a combining
  grapheme, and correct double-width CJK/emoji lead/continuation cells.
- At least four distinct **visible** spinner states, progress bars, ticks, and
  published frame generations at each of three terminal sizes. This cannot pass
  from a final static screenshot or a non-interactive transcript.
- Immutable retained frames while later React renders occur.
- Up/Down selection with a moving visible marker; committed Unicode input;
  Backspace editing; and Unicode bracketed paste delivered separately from keys.
- Real host-requested shrink/grow, `80×24 → 56×20 → 96×28`, seen both in VT
  geometry and the app's `useWindowSize` display. No requested dimensions are
  passed to the fixture as arguments or environment variables. The active box
  is bounded by its title and top/bottom borders; every side border must align
  with the expected terminal cell. Alternate screen additionally rejects any
  nonblank row outside that box.
- Natural Enter-triggered exit with code zero, drained final output, restored
  cursor and primary screen, and rejection of input after closure.
- Host close while an alternate-screen animation is running, followed by actual
  process/session reap.

The full-height variant uses a normal Ink `Box` with `height` from
`useWindowSize().rows`, allowing upstream's own fullscreen resize/redraw behavior
to apply. It additionally asserts the live box occupies every viewport row.
It is a separate workload, **not a replacement** for the strict compact variant
or a workaround in the host. The compact variant retains the known regression
below; running `all` still runs it and fails if the defect occurs.

Ink is explicitly rendered with `interactive: true` only **after checking real
TTY streams**. This is necessary because hosted CI environment detection would
otherwise disable interactive terminal behavior despite the real PTY. `debug`
stays false. `CI` is deliberately not cleared by the C++ harness. Color output is
made deterministic with `FORCE_COLOR=3`; inherited `NO_COLOR`, `INK_SCREEN_READER`,
and `NODE_OPTIONS` are removed for the child. This verifies rendering of styled
output, not automatic color detection. Ink generates the control sequences;
the fixture does not draw raw ANSI frames or special-case StormTTY.

### Coherent checkpoints, not assumed pipe-frame atomicity

Each startup, input, selection, animation, and resize checkpoint waits for a
complete live box **and** its requested state within 12 seconds. The same strict
border, Unicode, style, mode, and alternate-screen cleanliness checks apply to
every candidate. Only coherent frames count toward animation progression.
Distinct observed incomplete generations are counted and reported by scenario
and phase; no unchanged frame is repeatedly counted. Stable invalid output or
an incomplete redraw that never finishes fails the checkpoint rather than
being skipped or treated as success.

Failure diagnostics include provider/advanced-protocol status, frame generation,
geometry, the failing border row/column and cell UTF-8/span, and bounded owned
cell snapshots (at most 32 rows, 120 columns, and 8 KiB per snapshot). No general
PTY transcript or environment variables are captured. Final post-exit state is
still checked after process/actor closure, when no repaint remains in flight.

This fixes a test sequencing assumption: a text substring could appear before
the remainder of a translated ConPTY repaint, and the old test then immediately
asserted full-box geometry. Microsoft's pseudoconsole documentation describes
UTF-8 pipe communication and an internal console buffer; it does not promise
that each read is an atomic application frame. Microsoft's later synchronized-
output implementation is not evidence that the selected **inbox** ConPTY has
equivalent passthrough semantics. `advanced_protocols_verified` remains false.
Whether the original Windows failure was transient or persistent still requires
the native rerun with these phase diagnostics; a Linux pass does not decide it.
The later macOS run did establish persistent damage, not just an incomplete
snapshot: a clipped old top border remained above the otherwise complete box
for the entire 12-second checkpoint. The strict alternate check rejects it.

### Observed resize caveat

The primary-screen fixture can leave historical partial menu rows **above**
the current Ink box after shrinking and growing. This was observed on Linux;
the final local run reported five outside rows after shrink and three after
grow. The current box, size, selection, input, and animation are correct. The
selection uniqueness assertion is scoped to the current bordered Ink region on
the primary screen, while alternate-screen cleanliness covers the whole viewport.
The test does **not** establish a pristine viewport after primary-screen reflow.
Upstream also notes resize ghosting in the `useWindowSize` documentation. This
behavior needs further native visual qualification rather than a host-side
fixture-specific erase, output filter, or fabricated frame.

During implementation, an earlier fixture used a fixed width computed from
`useWindowSize().columns`. It exposed an additional **alternate-screen** defect:
Ink's resize handler recalculated Yoga and rendered before the hook's React
state committed, briefly painting the old 72-column border into the new
56-column terminal. Five partial rows then remained above the live box. This
was not attributed to the host without checking: an independent Python
`forkpty` capture bypassed StormTTY's Session/Process implementation, and the
unchanged bytes plus ordered resize events reproduced those residual rows in
both the pinned Ghostty engine and official `@xterm/headless` 6.0.0. The capture's
first post-shrink render still contained `Size: 80x24`; `Size: 56x20` appeared in
the next render. The fixture now uses ordinary responsive Yoga layout
(`width: '100%', maxWidth: 72`) rather than a stale fixed-width React prop. The
strict alternate-screen check passed the initial Linux runs, but this did not
eliminate the scheduling-dependent race below. No production terminal code or
byte stream was changed. The xterm package was used only in separate diagnostic
scratch, not added to StormTTY or its fixture dependencies.

### Persistent compact alternate-screen resize race

The responsive fixture still has a distinct resize limitation under valid
in-flight output ordering. Genuine output from its current Ink 8.0.0 app was
captured through an independent POSIX PTY with `TERM_PROGRAM=stormtty`. Replaying
the **identical byte stream**, with only the resize event moved earlier by one
already-written animation update, reproduces the native macOS failure in both
pinned Ghostty and official `@xterm/headless` 6.0.0:

1. Baseline: consume all 4,326 old-size bytes, resize `80×24 → 56×20`, then consume
   4,862 post-resize bytes. The alternate viewport is clean.
2. In-flight ordering: consume the first 4,143 bytes, resize, then consume the
   remaining 183 old-size bytes followed by the same 4,862 post-resize bytes.
   No terminal bytes are deleted, changed, duplicated, or reordered.
3. The 183-byte update redraws an animation suffix padded for 72 columns. At the
   new 56-column width it wraps, advancing the terminal cursor one extra row.
4. Ink's resize handler uses a cursor-relative erase with its remembered line
   count. It consequently preserves the old clipped top border at row 0 and
   places the correct new box at rows 1–14. Later incremental updates never erase
   that residual row. This matches the native failure's persistent shape.

Ink 8's `shouldSynchronize()` depends on real TTY/interactive status, **not** a
`TERM_PROGRAM` allowlist; the fixture therefore emits DEC2026 markers. Those
markers do not synchronize an OS window resize with the child's SIGWINCH handler.
The resize handler's initial `log.clear()` is also outside its render sync block.
For a compact alternate-screen app, upstream takes the relative-clear branch;
its absolute alternate-viewport clear is reserved for its fullscreen heuristic.

This establishes a reproducible app/library repaint-contract gap under valid
resize ordering, not a StormTTY-specific VT parser failure. A host-side forced
erase, discarded old bytes, arbitrary sleeps, or fabricated clean frame would
hide it and is not used. Merely reordering the host's resize request cannot
guarantee that the child has stopped emitting old-width output. The strict
compact alternate-screen assertion remains an unresolved qualification gate;
an initial passing run is not a guarantee against this race.
The independent replay matches the native failure's shape, but is not a raw
capture of that failing macOS CI run; its exact byte-arrival timing remains an
inference, rather than evidence obtained from the native transport.

The portable [captured-byte reproduction](ink/resize-race/README.md) includes
integrity-checked trace data and an independent-reference replay script. A
successful diagnostic replay means the gap was reproduced, not compatibility
verified. It is prepared for review; no upstream report has been posted.

## Run the qualification

Node **22 or newer** and npm are test-only prerequisites, not requirements for
building or running the terminal application itself. Do not download Node or
install packages as a side effect of the native build.

From the repository root, explicitly install the pinned fixture:

```sh
npm ci --prefix tests/ink --ignore-scripts --no-audit --no-fund
npm test --prefix tests/ink
```

Use an existing pinned Ghostty bootstrap and platform toolchain when configuring:

```sh
cmake -S . -B build -DSTORMTTY_GHOSTTY_ROOT=/absolute/pinned/ghostty-prefix \
  -DSTORMTTY_TEST_INK=ON
cmake --build build --config Release --target ink_session_tests
ctest --test-dir build -C Release -R '^ink_session_tests$' --output-on-failure
```

`STORMTTY_TEST_INK` defaults to `OFF`; missing Node or fixture dependencies must
not become a silent compatibility pass. The enabled CMake integration requires
Node >=22 and installed dependencies. Use `-DSTORMTTY_NODE_EXECUTABLE=...` when
needed to select the exact runtime. Windows and macOS native CI enable this
option, install with `npm ci`, and run the same binary. CTest has a 90-second
outer timeout; internal waits are bounded and failed scenarios request cleanup.

The executable supports two absolute paths and a scenario selector. Omission
of the selector is equivalent to `all`; no scenario silently changes to an
expected pass:

```sh
build/ink_session_tests /absolute/path/to/node /absolute/repo/tests/ink/fixture.mjs primary
build/ink_session_tests /absolute/path/to/node /absolute/repo/tests/ink/fixture.mjs fullscreen
build/ink_session_tests /absolute/path/to/node /absolute/repo/tests/ink/fixture.mjs compact-alt
build/ink_session_tests /absolute/path/to/node /absolute/repo/tests/ink/fixture.mjs host-close
build/ink_session_tests /absolute/path/to/node /absolute/repo/tests/ink/fixture.mjs all
```

CI should name these cases separately so positive workload results and the
known compact-alternate regression remain visible independently. Classification
of a known-failing gate must be explicit in CI, not hidden in this executable.

For manual native-window review, open StormTTY, change into `tests/ink`, and run
`node fixture.mjs`, `node fixture.mjs --alternate-screen --incremental --fullscreen`,
and `node fixture.mjs --alternate-screen --incremental` separately.
Use Up/Down, type accented text, Backspace, paste CJK text, resize narrower and
wider, and press Enter. Check glyph alignment, clipping, animation smoothness,
input responsiveness, scrollback after resize, and cursor restoration. Record
OS, Node version, actual PTY provider, build commit, and screenshots/video before
claiming native visual acceptance.

## Official sources checked when pinning

- [Ink 8.0.0 release](https://github.com/vadimdemedes/ink/releases/tag/v8.0.0)
- [Pinned Ink package metadata](https://github.com/vadimdemedes/ink/blob/v8.0.0/package.json)
- [Pinned Ink API documentation](https://github.com/vadimdemedes/ink/blob/v8.0.0/readme.md)
- [Pinned Ink resize implementation](https://github.com/vadimdemedes/ink/blob/v8.0.0/src/ink.tsx)
- [React 19.3.1 release](https://github.com/react/react/releases/tag/v19.3.1)
- [Official xterm.js reference implementation](https://github.com/xtermjs/xterm.js)
- [Microsoft pseudoconsole session communication](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session)
- [Microsoft synchronized-output implementation](https://github.com/microsoft/terminal/pull/18826)

The official npm registry was also queried before installation. The package
lockfile was produced by npm, not reconstructed manually from documentation.
