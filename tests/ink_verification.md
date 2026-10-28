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
| Integrated Linux regression suite | All 8 CTest suites passed with `STORMTTY_TEST_INK=ON`; the Ink suite then passed 10 consecutive repeat-until-fail runs |
| Reproducible fixture install | `npm ci --ignore-scripts --no-audit --no-fund` passed using npm 11.9.0 |
| Non-TTY negative check | `npm test --prefix tests/ink` passed: piped CI invocation fails rather than renders a fallback |
| Native Windows ConPTY and MSVC | Not run in the Linux implementation environment; native CI required |
| Native macOS POSIX PTY | Not run in the Linux implementation environment; native CI required |
| CI Node version | CI uses Node 22; local Node 24 results are not a Node 22 pass |
| Physical native window, fonts, animation smoothness | Not established by these headless Session tests |

This is evidence for **this one pinned workload**, not broad Ink, all React CLIs,
accessibility, IME, graphics-protocol, or performance certification. Native
renderer tests remain separate. No performance or perceptual-quality threshold
is asserted here.

## What the C++ test checks

It runs both the ordinary primary-screen/default redraw path and Ink's
alternate-screen/incremental redraw path, requiring:

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

Ink is explicitly rendered with `interactive: true` only **after checking real
TTY streams**. This is necessary because hosted CI environment detection would
otherwise disable interactive terminal behavior despite the real PTY. `debug`
stays false. `CI` is deliberately not cleared by the C++ harness. Color output is
made deterministic with `FORCE_COLOR=3`; inherited `NO_COLOR`, `INK_SCREEN_READER`,
and `NODE_OPTIONS` are removed for the child. This verifies rendering of styled
output, not automatic color detection. Ink generates the control sequences;
the fixture does not draw raw ANSI frames or special-case StormTTY.

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
strict alternate-screen check passes; no production terminal code or byte stream
was changed. The xterm package was used only in separate diagnostic scratch,
not added to StormTTY or its fixture dependencies.

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

The executable also supports direct invocation with two absolute paths:

```sh
build/ink_session_tests /absolute/path/to/node /absolute/repo/tests/ink/fixture.mjs
```

For manual native-window review, open StormTTY, change into `tests/ink`, and run
`node fixture.mjs`, then `node fixture.mjs --alternate-screen --incremental`.
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

The official npm registry was also queried before installation. The package
lockfile was produced by npm, not reconstructed manually from documentation.
