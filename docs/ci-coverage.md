# CI baseline and explicit qualification

The automatic **Native build and runtime evidence** workflow runs on pushes to
`main` and pull requests, plus manual dispatch. Experimental branch pushes no
longer start it a second time. It has no `continue-on-error` steps: build,
bootstrap, baseline test and packaging failures remain failures. Cancellation of
an obsolete run only supersedes it with the newer revision's run.

## Blocking automatic coverage

- **Windows 2025 / MSVC:** actual native application and all test executables;
  terminal, process/session transport, portable workspace/configuration/command
  tests, cursor motion and context-menu tests, Windows settings services,
  renderer PNG/smoke, startup notice, console-backend and all five app smoke
  modes. New feel-native tests remain blocking, not qualification exclusions.
- **macOS 15 / LLVM 20:** actual AppKit application, portable core/POSIX PTY,
  toolchain, input, PNG and offscreen Metal renderer tests. Other Ink fixtures
  remain automatic; compact-alt resize and the onscreen presentation timestamp
  gate are explicit qualifications.
- **Ubuntu 24.04:** portable core and real POSIX process/session tests, including
  all registered Ink cases. There is no Linux desktop application target.
- Bootstrap boundary and negative-control Ink tests remain required. Windows
  additionally checks bundled-console cached repair, portable installation and
  installed settings launch; packaged-console app smoke; shell staging safety,
  managed HOME/drive paths; explicit and app-owned Zsh app smoke; and repeated
  owned settings teardown. These are not substitutes for resize qualification.

CTest uses `-LE qualification --no-tests=error`. Only the five Windows Ink cases,
macOS `ink_compact_alt`, and `native_macos_window_smoke` receive that label. Plain local `ctest` still runs
all registered tests; nothing is disabled or made an expected success.

## Explicit, strict qualification

Run **Explicit compatibility and stress qualification** (`qualification.yml`)
with `workflow_dispatch` on the revision to qualify. Its Windows and macOS jobs
build real native binaries and retain the prior strict assertions and nonzero
failure exits:

- Windows Ink primary, fullscreen, compact-alt, host-close and static-margin
  cases; raw transport capture and independent replay.
- Packaged-console comparison, including missing-host rejection, static margin,
  all Ink cases, raw transport and provider stress.
- Bundled Zsh native sessions against inbox and packaged providers, each with
  six repeated resizes, plus staging and app/teardown verification.
- macOS compact-alt resize, plus the onscreen smoke gate and genuine presentation timestamp
  requirement. Zero timestamps and offscreen rendering are not a pass.
- Isolated additional-blur WARP lifecycle experiment, not physical blur proof.

A green automatic baseline **does not qualify** Windows Ink/resize stress,
bundled-Zsh repeated resize, macOS compact-alt resize, hosted macOS onscreen
presentation, physical input,
IME, accessibility, display latency or visual quality. Known compatibility and
hosted-display gaps remain open until the corresponding strict run produces
passing evidence. Moving a gate does not fix its underlying behavior.

Both workflows retain test XML/logs, configure logs and available screenshots
and traces as artifacts even on failure. Artifact names distinguish baseline
`evidence-*` from `qualification-evidence-*`.

The six historical MSYS source/cursor/resize, ConPTY width and macOS presentation
diagnostic workflows are manual-only. Existing source-branch guards remain for
experiments that require those branches: select the matching branch explicitly.
They do not change production defaults. No account notification settings are
changed; genuine automatic baseline failures still notify normally.
