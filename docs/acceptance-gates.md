# StormTTY acceptance gates — Windows-native, not a terminal-shaped demo

Research baseline: 2026-10-28. Ghostty inspected at `46e9dc9a927b2e7e7cba79e6902c176127dbdf2c`; StormTTY repository was empty. **These are proposed release criteria, not passing results or performance measurements.** No build, dependency installation, Windows execution, publication, or Codex task launch was performed for this note.

## The decision

Ship a runnable Windows application with real child processes, tabs and independent split panes. A polished screenshot, C parser library, cross-compiled EXE, or fixture-only project does not meet that bar. First-class Ink means unmodified Ink applications work through native Windows Node + ConPTY, not that StormTTY draws an imitation React UI. Graphics must be enabled in the shipped default configuration, with bounded resources; animation must not require an always-busy rendering loop.

**Four non-interchangeable evidence levels:**

| Level | Required evidence | Does NOT prove |
|---|---|---|
| L — Linux library | Pinned `test-lib-vt` runtime results, C ABI fixture adapter, parser/model snapshots, allocator/fuzz regression results | ConPTY, Windows fonts/GPU/input, native app behavior |
| X — cross-compile | Windows binaries link; imports/architecture/assets recorded | The binary starts or works on Windows |
| W — Windows runtime CI | Exact EXE + core/ConPTY hashes; native child round trips, exit/resize/backpressure tests, process/handle results | Physical input, IME candidate placement, display latency, Narrator usability |
| P — physical Windows | Signed-off interaction/video and timed traces on named hardware, monitor/DPI, keyboard layouts and accessibility tools | Other hardware/OS combinations not tested |

A hosted Windows CI runner without an interactive desktop may satisfy W's headless ConPTY cases, not GUI or P cases. Wine, WSL-only execution and static inspection cannot be relabeled W/P.

## What the inspected Ghostty tree actually gives us

* `build.zig:359–385` runs Zig and C-interface tests under `test-lib-vt`; `test-lib-vt-build` is explicitly compile-only. `-Demit-test-exe` installs the C-interface test executable for another machine. Do not run target binaries implicitly during cross-compilation.
* `include/ghostty/vt/` exposes terminal/grid/render snapshots, key/mouse/focus/paste, selection, and Kitty graphics APIs. `src/terminal/c/terminal.zig` includes clipboard limits, explicit-opt-in title/checksum reports, resize, and synchronized-output tests. `Screen.zig` covers wide-character reflow and bounded scrollback. Reuse these; do not replace them with five smoke tests.
* `test/esctest/README.md` says its CI job reports failures without failing the build, with known DECSTR/isolation and resize-harness limitations. Preserve the tally and explicit expected-failure list; an upstream green badge is **not** a conformance pass. `test/windows/test_dll_init.c` is a manual DLL/CRT regression test, not a Windows terminal integration suite.
* `terminal.h:2659–2684` specifically recommends `GHOSTTY_TERMINAL_OPT_RESIZE_PULL_SCROLLBACK=false` for ConPTY. Make this a regression assertion. `terminal.h:1987–2085` requires the host to capture the render-hold boundary and supply its own timeout; polling mode 2026 before drawing is insufficient.
* `kitty_graphics.h` requires an enabled storage budget, a PNG decoder for PNG input, host-side image drawing and animation ticks. A capability/API existing does not show that StormTTY actually displays it. File/shared-memory graphics media are a separate permission decision.

Reference: [pinned Ghostty source](https://github.com/ghostty-org/ghostty/tree/46e9dc9a927b2e7e7cba79e6902c176127dbdf2c).

## Minimal convincing corpus

`fixtures/terminal-vectors.json` contains **inert JSON**, not executable tests: escaped streams plus checkpoint assertions and host actions. All fixtures start in a fresh isolated terminal unless explicitly testing persistence. Later implement one small replay adapter against the actual app/core, not a second terminal implementation. Check cells, attributes, cursor, modes, response bytes, image metadata and selection; screenshots supplement, never replace, semantic assertions. Re-run every stream whole, byte-at-a-time, at each escape/UTF-8 boundary and with reproducibly randomized chunking. Report expected policy refusals separately from parser errors.

| Group | Adversarial coverage / acceptance condition | Levels |
|---|---|---|
| VT core | CUP/relative cursor, CR/LF/BS/TAB, EL/ED, margins/origin, insert/delete, delayed wrap, save/restore; normal/alternate isolation and cursor restore; correct bounded scrollback/reflow after shrink/grow | L/W/P |
| Color/text | 16/256/truecolor, bold/dim/inverse/underline/reset; combining marks, CJK wide cells, VS15/VS16, ZWJ family, skin tones, flags, invalid and split UTF-8; right-edge placement and erase half of a wide glyph | L/W/P |
| Width contract | Pin Unicode/width policy and font set; distinguish grapheme content, cell width and glyph shape. Compare Ink's width assumptions with actual cells, including ambiguous-width mode. No promise that every emoji is one glyph/two cells on every font | L/W/P |
| Input | Legacy/application cursor and keypad, Backspace versus Delete, Ctrl+C/D/Z, Alt/AltGr/dead keys; Kitty query/push/pop and modifiers/press/repeat/release; no duplicate WM_KEY/committed-text delivery | L/W/P |
| Paste/mouse/focus | Mode 2004 paste is atomic text, not hotkeys; terminator injection blocked; 1000/1002/1003 + SGR 1006 drag/wheel/release and coordinate boundaries; 1004 focus follows actual active pane/window only; default selection gesture overrides are predictable | L/W/P |
| Synchronized output | 2026 holds last completed frame, releases atomically, handles release+next-hold in one read; crashed client/resize/reset releases it; repeated starts cannot indefinitely extend timeout | L/W/P |
| Graphics | Kitty direct RGBA + PNG, chunking, replacement at same ID/size, delete, layering, scrolling/clipping, alternate-screen isolation, resize, two-frame animation and stop; no stale texture after ID reuse | L/W/P |
| Native lifecycle | Native `pwsh`, `cmd`, a Win32 console-API probe and Windows Node; correct quoting/Unicode executable paths/environment/CWD; missing executable, immediate exit, nonzero exit, child tree, close during write, final-output drain, resize-after-exit, startup query reply | W |
| Backpressure | Child saturates output while user types, resizes, selects and closes another pane; opposite direction uses a large paste into slow reader; bounded queues, prompt control/UI responsiveness, no dropped/reordered bytes or blocked teardown | W/P |
| Tabs/panes | Independent process, modes, cursor, scrollback, selections and graphics; switch/reorder/split/resize/close repeatedly while another pane animates; no input misrouting, focus theft or stale async callback after close | W/P |
| Selection/clipboard | Click/drag/word/line/keyboard selection across wraps, combining/wide cells and scrollback; exact copied text with declared newline policy; selection survives permitted reflow or explicitly clears; OSC clipboard separate from user Copy/Paste | L/W/P |
| Windows usability | Japanese/Chinese/Korean IME composition/commit/cancel at caret; move between DPI monitors; AltGr/dead keys; keyboard-only tabs/panes; UIA text/selection/focus and Narrator; high contrast, reduced motion, 100/150/200% scaling | P |

### Ink is a real child-process fixture, not a snapshot trick

Pin **Ink 8.0.0 + compatible React 19.3 + Node >=22** in the fixture lockfile, plus the previous supported Ink major in a second test lane. The official release page identifies v8.0.0; source/package versions must be recorded again when implementation starts. Ink's rendering is text/TUI output, not a general-purpose pixel-graphics protocol. Test graphics separately. [Release](https://github.com/vadimdemedes/ink/releases/tag/v8.0.0) · [package](https://github.com/vadimdemedes/ink/blob/v8.0.0/package.json)

Build a tiny `ink-lab` child: live counter/spinner, colored progress bar, bordered layout, persistent `<Static>` log, text field, focusable buttons, viewport dimensions and a numbered key/paste event log. Required scenarios: narrow/wide resize including exact viewport fill; 30-FPS default then requested 60 FPS; incremental rendering off/on; alternate screen off/on; Kitty auto/off; nested focus; Ctrl+C and thrown error; mount/unmount; suspend to child and resume. No stale lines, flicker from half-cleared frames, duplicate keys, leaked mouse/control replies into text, lost static log or terminal modes left enabled afterward.

Use real TTY stdin/stdout. Record `isTTY`, raw-mode support, rows/columns and chosen interactive mode. A CI-only non-interactive snapshot is a **negative control**: Ink disables terminal paths there. Its `usePaste` and `useInput` must receive the correct separate events; Kitty auto-detection must complete before its documented 200-ms deadline when advertised. Do not claim the terminal renders at 60 FPS because Ink's timer fired 60 times. [Pinned README](https://github.com/vadimdemedes/ink/blob/v8.0.0/readme.md) · [render implementation](https://github.com/vadimdemedes/ink/blob/v8.0.0/src/ink.tsx)

### Runnable-fixture plan after implementation approval

1. Add the normal Windows app executable first, with a reproducible local build and default shell path. `stormtty.exe -- <absolute-fixture.exe> ...` (proposed interface) launches a genuine child via the same production session path. No parallel mock backend for “demo mode.”
2. `vt-probe` writes the JSON streams, reads query/input replies and exposes a separate bounded test-control channel for readiness/checkpoints. `win32-probe` additionally toggles console modes, queries dimensions/cursor and emits console-API output. They must never infer success from their own stdout alone.
3. `ink-lab` uses pinned dependencies and real Windows Node. Readiness/action completion goes to the test-control channel, while its TUI remains on ConPTY. Include a session with no test adapter attached.
4. An app-side test observer records semantic render snapshots/selection/modes and presentation timestamps without changing protocol behavior. Drive public app actions or real input; compare raw probe replies and screen state. Keep test-only hooks off in release builds.
5. Run L → X → W → P and save exact binary hashes, case results, OS/ConPTY versions, traces/screenshots, failures and unsupported cases. Prove launch+shell+input+resize+exit before expanding the corpus. Never deliver only the harness.

## Windows-specific failure gates

Provider choice and exact-version qualification: [ConPTY baseline note](conpty-baseline.md). Recommended candidate is official stable `Microsoft.Windows.Console.ConPTY` `1.25.260930003`, on Windows 11 25H2+; this is a testable proposal, not a verified runtime result.

ConPTY streams are UTF-8/VT, but behavior varies with its implementation and child console modes. Record OS build and whether using inbox or bundled ConPTY, including package/version/hash. Kitty graphics passthrough/cursor reconciliation and Kitty key events must pass **through that exact provider**; a parser-only feature must not be advertised end-to-end. Microsoft's APC cursor-desynchronization regression is evidence for this test, not a claim that all current builds remain broken. [API](https://learn.microsoft.com/en-us/windows/console/createpseudoconsole) · [Microsoft regression](https://github.com/microsoft/terminal/issues/19926)

Input and output must be serviced without circular blocking; continue output draining through shutdown, and respond to required cursor queries. Test blocked output + close and pending input + exit. A 5-second watchdog fails the test and captures stacks; killing the process is cleanup, not success. [Microsoft lifecycle/deadlock guidance](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session)

Raw key events are not a substitute for composition-aware Windows text input. Verify committed IME text arrives once, preedit is not sent prematurely and the candidate window tracks the caret after pane/DPI changes. UIA must expose navigable text ranges/selection and meaningful pane/tab focus, not only a giant pixel surface. Test Narrator's actual reading experience, including a deliberate screen-reader rendering mode. [Custom text input](https://learn.microsoft.com/en-us/windows/apps/develop/input/custom-text-input) · [TSF](https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework) · [UIA text patterns](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-implementingtextandtextrange)

## Security gates — terminal output is hostile data

**Default policy proposal:**

* Output cannot execute commands, install configuration, invoke a shell, fetch network resources, silently open a URL/file, or synthesize user actions. Titles/CWD/notifications are untrusted display metadata, never command strings. Unknown OSC/APC sequences are bounded and ignored, not forwarded to an interpreter. Throttle bells, notification/title churn and repeated permission requests.
* OSC 52 reads are denied unless the user explicitly permits that request/session. Writes require a deliberate permission action with the requested byte count and a safe preview; default deny unattended/background requests. User Copy is different from application-requested writes. Denials emit protocol-appropriate replies and no clipboard data. No persistent grant based on output-controlled title/CWD/process name. Apply the same policy to OSC 1337/5522 and rich MIME variants, not only OSC 52.
* Ghostty callbacks are synchronous: do not block the parser or retain borrowed callback/reply pointers while awaiting a dialog. Deny the current request, show an independent permission affordance, and require a new request after a grant. Freeze the approved paste bytes; do not re-read a clipboard that may have changed during confirmation. Test bracketed-paste terminator injection and unbracketed multiline confirmation.
* OSC 8 links require an explicit click and visible destination preview. Allow only parsed `https`/`http` by default; reject control characters and shell/script/custom-protocol payloads. `file:`/local paths require a separate deliberate action, normalized local-only paths, no UNC/device/ADS/remote-authority ambiguity and no execution of `.exe`/`.cmd`/`.ps1`/shortcuts. Do not use `cmd /c`, shell expansion, or concatenated command lines to open destinations.
* Enable direct Kitty graphics by default; disable arbitrary file, temporary-file deletion and shared-memory media initially. PNG/zlib decoders must check dimensions, overflow and decoded bytes before allocation; impose aggregate frame/image/GPU budgets, not just compressed-byte limits. Later enabling file media needs a separate audited path/ownership/reparse-point policy. [Kitty protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/)

**Provisional resource contract, to implement and test rather than claim:** 64 MiB decoded images per pane, 256 MiB application-wide image/GPU cache, 16 megapixels/image, 1,024 placements/pane, 256 animation frames/image; 10,000 scrollback lines **and** 64 MiB/pane; 4 MiB pending host input/output queue per direction; 1 MiB retained unfinished/unknown-sequence data; 1 MiB decoded clipboard payload; 1-second maximum synchronized-output hold. All aggregate allocations must include in-flight decode and duplicated GPU/staging buffers. Parser-specific protocol limits may be smaller and must be published; cap exhaustion must reject/evict predictably without corrupting later valid text. These are starting budgets, not observed memory consumption or proof of security.

Adversarial run: huge CSI numeric parameters, unterminated OSC/DCS/APC, malformed UTF-8/base64/PNG, declared-dimension overflow, high-compression image expansion, frame/placement floods, infinite sync hold, clipboard read/write flood, OSC 8 dangerous schemes, repeated close/resize/reset during decode. Generate bounded malicious inputs only inside the isolated harness. No crash, out-of-bounds/UAF, unauthorized side effect, unbounded growth or persistent UI starvation is acceptable. Replay existing Ghostty fuzz crashes and extend at the C boundary/Windows host; report coverage and runtime, not “fuzzing proves safe.”

## Performance — ambitious targets, honest evidence

Provisional first-release budgets below are **targets** requiring agreement against a named mainstream Windows reference laptop, not fabricated benchmark results. Use optimized release binaries, clean config, same font/size/grid (120×40), fixed power mode, local shell/fixture and DPI. Record CPU/GPU/driver/RAM/OS/build, refresh rate, background load, warm-up policy and exact commits. Run on AC and battery, plus 8 panes/4 active; separately report cold font/shader cache and warm cache.

| Metric | Measurement and proposed gate |
|---|---|
| Startup | Instrument process start → first presented terminal frame → child-ready marker separately. At least 30 fresh-process runs; true cold runs after reboot/cache policy, not mislabeled relaunches. Target p95 first terminal frame ≤300 ms; shell-ready separately, target ≤800 ms with clean `pwsh -NoProfile`, excluding first-install initialization |
| Input-to-paint | Timestamp Windows input receipt → child echo → state update → actual presented frame, ≥1,000 events at randomized refresh phase. Target p95 ≤2 refresh intervals and p99 ≤3 for local echo. Present-time software traces are not physical key-to-photon; verify with high-speed camera/photodiode on P hardware before making that claim |
| Animation pacing | Deterministic 60-update/s text + separate image animation, 60-second traces; count presented **changed** frames, p95/p99 intervals and gaps, not render calls. Target ≥99% updates presented within two display periods, no unexplained gap >100 ms; match animation cadence, support 120-Hz testing, and honor reduced motion |
| Idle CPU/GPU | Five-minute quiescent visible and occluded/minimized sessions, animations and cursor blink both off; then defaults separately. CPU = process CPU-time / elapsed / one logical core. Target <0.5% of one core with zero periodic GPU presents when nothing changes; report child/ConPTY cost separately and whole-tree total |
| Throughput | Fixed-size ASCII logs, colored logs and adversarial cursor-heavy workloads; measure producer start → last marker parsed **and** last frame presented. Bytes/s without correctness is invalid. Report parse-only separately from end-to-end and rendering; target no >10% regression from a frozen same-machine baseline while input latency remains within its gate |
| Memory/resources | Private committed bytes, working set, GPU allocated/resident bytes and handles at empty/1/8 panes, saturated scrollback, graphics budget and after 100 open/close cycles. Target no continuing growth after bounded caches saturate; trend/leak confidence and budgets matter more than one screenshot. Provisional empty-app private-commit target ≤120 MiB excluding child processes; report whole-tree total too |

Use Windows QPC/ETW and present tracing with recorded tool versions; identify which timestamps observe CPU submit versus compositor/display presentation. Save raw data and distribution, not just a mean. Compare release builds with identical output, fonts, grid, features and display policy. A highly optimized parser benchmark cannot prove fast interactive painting; inflated batching can win throughput while making typing feel worse.

## Stop “better than Ghostty” claims at the evidence boundary

Ghostty is a reference implementation and potential dependency, not a universal test oracle. Differences need a documented protocol/policy decision. Require all applicable W/P product gates, default graphics/Ink demonstrations and reproducible performance data before saying “fast,” “first-class” or “ready.” A stronger comparison must name the exact metric/workload/platform/configuration and trade-offs. Cross-OS Ghostty-versus-Windows timings do not establish an apples-to-apples winner; compare native Windows peers on the same machine, and call Ghostty cross-platform measurements contextual only. Never turn “inspired by” or “uses its core” into “better than Ghostty.”

**Current status:** source/doc review complete; inert vectors validated as JSON only. Runtime tests, actual Windows integration and physical performance/input/accessibility measurements are **not run**. Implementation/build execution remains outside this council note; creating or starting a Codex task requires the user's approval.
