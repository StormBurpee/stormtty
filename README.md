<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/readme-assets/wordmark-dark.svg">
    <img src="docs/readme-assets/wordmark-light.svg" alt="StormTTY" width="430">
  </picture>
</h1>

<p align="center">A native terminal for Windows and macOS. Windows first.</p>

<p align="center">
  <a href="#why-stormtty">About</a> &nbsp; / &nbsp;
  <a href="#build-from-source">Build</a> &nbsp; / &nbsp;
  <a href="#using-the-windows-host">Controls</a> &nbsp; / &nbsp;
  <a href="#configuration">Configuration</a> &nbsp; / &nbsp;
  <a href="#roadmap">Roadmap</a>
</p>

> [!IMPORTANT]
> **StormTTY is in active development, not ready for daily use.** The Windows
> application builds with MSVC and its native UI checks run successfully;
> the full suite still has known compatibility failures. Physical interaction
> and visual qualification are also underway. There is no production release or supported binary download.
> See [platform status](#platform-status) before building.

## Why StormTTY?

StormTTY is being built for people who spend their day in a terminal: editing,
compiling, reviewing changes and running full-screen command-line applications.
The aim is a considered desktop application around that work, not another layer
between you and your shell.

- **A native application, not a browser surface.** Win32 windowing, ConPTY
  sessions, DirectWrite text and a Direct3D-backed renderer on Windows. The macOS
  target uses AppKit, CoreText, Metal and POSIX PTYs. No Electron or webview.
- **A workspace that keeps sessions intact.** Tabs and split panes share a
  portable workspace model. Reorder, resize, zoom and move panes without
  restarting their processes. The Windows host exposes these through commands
  and direct manipulation; cross-window tear-off is still planned.
- **An interface that leaves room for the terminal.** Integrated tabs, quiet
  dividers and a collapsible workspace rail. The Graphite direction adds slight
  background-only translucency: glyphs, the cursor and explicit terminal cell
  backgrounds stay opaque. Native visual qualification remains a separate gate.
- **Keyboard commands without stealing the shell's keys.** A searchable command
  palette and configurable bindings; `Ctrl+C` stays interrupt. Split and movement
  commands have no default shortcuts that would displace a TUI's bindings.
- **Compatibility has to survive the whole stack.** Terminal-state parsing is
  only part of the job. Input, PTY transport, resize, Unicode, synchronized output
  and graphics must work together, including demanding Ink applications.

The last point is a release requirement, not a claim that qualification is done.
The same applies to performance: there are [measurement plans and acceptance
criteria](docs/acceptance-gates.md), not published benchmark wins.

## Platform status

| Platform | Implementation | Verification boundary |
| --- | --- | --- |
| **Windows** | Native application, custom chrome, tabs, splits, command palette, ConPTY and DirectWrite/Direct3D renderer | Native MSVC build passes. Session, terminal and renderer tests execute on Windows; full-suite qualification, physical interaction and visual QA remain gates. |
| **macOS** | Shared core, POSIX PTY transport and AppKit/CoreText/Metal host | Native compilation, core/PTY and offscreen renderer checks pass. Onscreen presentation verification remains unresolved; no supported macOS application yet. |
| **Linux** | Portable core, terminal adapter, process and session test host | Core/process/session tests pass locally. A Linux desktop application is not a current product target. |

The [native workflow](.github/workflows/native.yml) separates Linux, Windows and
macOS evidence. A cross-compiled executable is not a Windows runtime pass; a CI
frame capture is not a test of physical input, desktop composition or
accessibility. Windows ARM64 and universal macOS packaging are not implied by an
x64 or single-architecture build.

## Build from source

Run these commands from the repository root. You need **Python 3.10+**, **Git**,
**CMake 3.24+** and a **C++20 compiler**. The Windows commands below also use Ninja.

The bootstrap builds the pinned terminal library, not Ghostty's application.
`--install-zig` explicitly opts into downloading the checksum-pinned Zig toolchain
inside this checkout. It does not install a system compiler or SDK. Sources,
artifacts and caches stay under `.deps/` and `.cache/` by default.

### Windows

Use an **x64 Native Tools Command Prompt for Visual Studio** with MSVC, the
Windows SDK and Ninja available:

```powershell
python tools/bootstrap.py --install-zig --prefix .deps/ghostty-native
cmake -S . -B build-msvc -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTORMTTY_GHOSTTY_ROOT=.deps/ghostty-native -DSTORMTTY_BUILD_APP=ON
cmake --build build-msvc --parallel 2
ctest --test-dir build-msvc --output-on-failure
.\build-msvc\stormtty.exe
```

Use a fresh build directory when changing compilers. CMake caches its first compiler
selection; a previously configured `build` directory may still select an older LLVM
installation even inside a Visual Studio prompt. The explicit `cl` selection above
uses the compiler matching the installed MSVC headers. Do not disable STL version
checks or edit Visual Studio headers to work around a compiler mismatch.

The build copies the shared terminal library next to the executable. Keep them
together when running the development build. Do not substitute a Windows GNU
engine archive into an MSVC build; the import check rejects the ABI mismatch.

### macOS: native development build

Install Xcode Command Line Tools and Homebrew's `llvm@20`. Apple's libc++ in
Xcode 16.4 does not expose the standard cancellation APIs used by the session
layer. The supplied toolchain uses LLVM 20's compiler, headers and matching
runtime together; substituting headers alone is not sufficient.

```sh
brew install llvm@20
python3 tools/bootstrap.py --install-zig --prefix .deps/ghostty-native
cmake -S . -B build-macos -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/MacOSLLVM.cmake -DSTORMTTY_LLVM_ROOT="$(brew --prefix llvm@20)" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTORMTTY_GHOSTTY_ROOT=.deps/ghostty-native -DSTORMTTY_BUILD_APP=ON
cmake --build build-macos --parallel 2
ctest --test-dir build-macos --output-on-failure
open build-macos/stormtty.app
```

The native host and this toolchain path are awaiting native CI qualification.
The application is not a self-contained distribution: its Ghostty and LLVM
runtime dependencies still need audited packaging. macOS 11 is the source API
baseline, not a verified minimum OS for the Homebrew-built runtime. See the
[macOS implementation notes](src/platform/macos/README.md).

### Linux: core and PTY tests

Use a native C++20 toolchain. Linux does not have an application host.

```sh
python3 tools/bootstrap.py --install-zig --prefix .deps/ghostty-native
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTORMTTY_GHOSTTY_ROOT=.deps/ghostty-native -DSTORMTTY_BUILD_APP=OFF
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

For an existing Zig installation, custom caches, offline builds, target selection
and cross-compilation, see the [bootstrap guide](tools/bootstrap.README.md).
Dependency revisions and archive hashes live in
[`tools/bootstrap.lock.json`](tools/bootstrap.lock.json).

## Using the Windows host

The development host starts Command Prompt by default. Profiles can select a
different shell; StormTTY does not replace or emulate the shell itself.

| Action | Default shortcut |
| --- | --- |
| Show commands | `Ctrl+Shift+P` |
| New tab | `Ctrl+Shift+T` |
| Close active pane | `Ctrl+Shift+W` |
| Next / previous tab | `Ctrl+Tab` / `Ctrl+Shift+Tab` |
| Copy selection | `Ctrl+Shift+C` |
| Paste | `Ctrl+Shift+V` |
| Window menu | `Alt+Space` |

Use **Show commands** for splits, pane focus and movement, pane zoom, tab
renaming, split equalization and fullscreen. The command palette also shows
unavailable actions with a reason. Select an action and press **Alt+Enter** to
assign and save its shortcut. Conflicting or reserved shortcuts are rejected.

**Settings** includes appearance previews, font controls, shell selection and
shortcut editing. Close confirmations, context menus and settings share the
app's renderer-owned interface. Find-in-terminal has a tested core search service,
but its user-facing command is not enabled yet.

The [command registry](src/core/commands.cpp) defines the full set of actions and
platform-specific bindings. Close and paste behavior still need native
interaction testing; do not use the development build for irreplaceable work.

### Bundled Zsh on Windows

The Windows app build bundles upstream Zsh 5.9.2 with StormTTY's own startup
preset: directory, exit-status and duration prompt; ZLE editing; and completion.
It is not a Zsh fork. It leaves your existing shell dotfiles and HOME alone and
does not write history yet. The full extension/module system is not implemented.

**Build the `stormtty` target, then Run.** The first Windows app build fetches
checksum-pinned dependencies into its private cache and stages `runtimes/zsh`
beside the executable. It does not require a manual shell-download step or
`--shell-runtime` launch arguments. Subsequent builds reuse the cache. Downloads
happen at build time, never when opening a terminal, and no global PATH, shell
installation or user dotfile is changed.

- **Fresh configuration:** managed Zsh is selected automatically.
- **Existing saved configuration:** your shell choice wins. Open Settings → Shell,
  choose **Zsh · managed**, and Save once. New tabs/panes use it; existing processes
  keep running. Only a stable profile reference is saved, not an installation
  directory or a copy of your PATH.
- **Moving the app:** keep `runtimes/zsh` beside the executable. Rebuilding repairs
  a missing staged runtime. If managed Zsh is selected but unavailable, the app
  reports an error instead of silently running another shell. An unrelated saved
  shell remains usable.

Core-only and non-Windows builds do not fetch this Windows runtime. Downstream
Windows builds can explicitly set `-DSTORMTTY_BUNDLE_ZSH=OFF`. An already staged
runtime can be supplied with `-DSTORMTTY_DEVELOPER_SHELL_RUNTIME=<absolute-root>`;
that is an advanced offline override, not a required setup step. No IDE-specific
project files are needed.

Native Windows CI has passed both inbox and packaged-console Zsh probes:
interactive/native handles, Unicode and working directories, shell/native resize,
Ctrl-C, suspend/background/foreground jobs, and exit status. All five explicit
app-with-Zsh smoke scenarios also pass. This is runtime evidence, not physical
keyboard/UI qualification. Compact alternate-screen Ink layouts still have a
resize-redraw race; the full Windows suite is not green.

The optional pinned Microsoft console backend remains a separate experiment,
selected with `--console-runtime <absolute-directory>`; bundling Zsh does not
replace Windows system files or silently enable that backend.

The build includes upstream notices. Public redistribution also requires an
accessible matching source download; the exact inventory and source generator
are in the [distribution packet guide](tools/shell-notices/REVIEW.md). See the
[runtime staging guide](tools/shell-bootstrap.README.md) for low-level tools and
[console compatibility evidence](tests/ink/conpty-el-regression/README.md).

## Configuration

The Windows host reads `%LOCALAPPDATA%\StormTTY\config.stormtty` if it exists.
To use a different file, launch `stormtty.exe --config path\to\config.stormtty`.
Use **Settings** to edit and save supported options, or edit the file directly
and restart. Appearance previews can be cancelled; changing the default shell
applies to new sessions, not processes already running. Saving detects external
file changes rather than silently overwriting them.

A small, complete configuration:

```ini
version = 1

[appearance]
background_opacity = 0.94
backdrop = "auto"
reduced_motion = "system"

[font]
family = "Cascadia Mono"
size_dip = 14

[terminal]
scrollback_lines = 10000

[bindings.windows]
pane.split_right = "Ctrl+Shift+D"
pane.split_down = "Ctrl+Shift+E"
pane.zoom = "Ctrl+Shift+Z"
```

This is **StormTTY's declarative format, not general TOML**. Unknown keys,
duplicate assignments and conflicting shortcuts are errors. There are no
includes, scripts, environment expansion or shell expressions. An invalid
startup file is rejected rather than partially applied.

`background_opacity` accepts `0.0` through `1.0` and affects only the default
terminal background. On supported Windows builds, `backdrop = "auto"` requests
Desktop Acrylic; `"mica"` explicitly selects wallpaper-based Mica and `"none"`
selects a solid background. Appearance shows the requested material or the
current fallback reason. Saved opacity is preserved; 100% covers the material.
Windows can substitute a solid surface for inactive windows, accessibility,
power or graphics conditions ([Microsoft's material guidance](https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic)).
Cascadia Mono is bundled; other font names refer to installed fonts.

For a custom shell, add a profile with a separate executable and argument list.
Use an installed executable's full path, and escape Windows backslashes as `\\`.
See [`config_format_documentation()`](src/core/config.cpp) for the complete
format, themes, profiles, environment entries and binding rules. Unspecified
settings retain their defaults.

## Architecture

StormTTY owns the application and platform integration. It reuses
[Ghostty's `libghostty-vt`](https://github.com/ghostty-org/ghostty) for terminal
parsing and state through its public C API. It is not a port of Ghostty's GUI,
and upstream terminal features still need end-to-end qualification in this host.

```text
Native window and input
          │
Commands, configuration and workspace
          │
       Session ────────── ConPTY / POSIX PTY ───── Shell or TUI
          │
 Terminal adapter ─────── libghostty-vt
          │
    Owned frame
          │
DirectWrite + Direct3D / CoreText + Metal
```

The C++20 core does not own window-system handles. Platform adapters own native
windowing, process transport, input, clipboard integration and presentation.
The terminal adapter publishes owned frames rather than retaining borrowed
Ghostty data across mutations or threads. The Windows renderer uses one
compositor surface per window, not a separate swapchain for every pane.

| Area | Start here |
| --- | --- |
| Workspace, commands and configuration | [`src/core/`](src/core/) |
| PTY/session coordination | [`src/session/`](src/session/) |
| Terminal adapter and upstream boundary | [`src/terminal/`](src/terminal/) |
| Native hosts and renderers | [`src/platform/`](src/platform/) |
| Foundation and platform decisions | [`docs/decisions/`](docs/decisions/) |
| Release criteria and adversarial cases | [`docs/acceptance-gates.md`](docs/acceptance-gates.md) |

The pinned Ghostty revision is
[`46e9dc9a927b`](https://github.com/ghostty-org/ghostty/commit/46e9dc9a927b2e7e7cba79e6902c176127dbdf2c).
Its work and authorship remain upstream; the
[dependency audit](docs/decisions/ghostty-audit.md) records the integration choice.

## Roadmap

The next milestone is a reproducibly tested native application, not a longer
feature list.

| Work | State | What still needs to be established |
| --- | --- | --- |
| Shared workspace, commands, config, terminal adapter and sessions | Implemented; portable tests pass | Broader regression and adversarial coverage |
| Windows native application | Native MSVC build passes; session and renderer tests pass | Complete native regression suite, desktop visual QA, DPI, Snap and interaction checks |
| macOS application | Native build, core/PTY and offscreen renderer checks pass | Verified onscreen presentation, physical input and platform UI parity |
| Modern terminal and Ink workloads | Packaged Windows console fixes the verified right-margin defect | Compact alternate-screen resize race; broader graphics/protocol coverage |
| Text input and accessibility | Incomplete | Full TSF/IME and UI Automation behavior, screen-reader and physical-input testing |
| Settings, shortcuts and owned menus | Implemented; native Windows app checks pass | Physical interaction, accessibility and platform parity |
| Managed Zsh | Windows app build bundles the runtime; native shell/job-control and explicit app checks pass | History, modules, public release packaging and physical interaction |
| Find and cross-window tear-off | Core search implemented; UI/tear-off pending | User-facing implementation and native tests |
| Performance and distribution | Pending | Recorded-hardware measurements, ConPTY provider qualification, packaging and complete notices |

First-class Codex and Claude workflows are a longer-term product direction:
agent sessions and their harness should feel native to the workspace, rather
than bolted onto a shell. That layer is not implemented yet. It does not relax
the terminal compatibility, performance, input or accessibility requirements.

See the [acceptance gates](docs/acceptance-gates.md) for the detailed test matrix.
Proposed latency and memory budgets there are targets, not results.

## Ink workload checks

An optional qualification target runs a pinned, real Ink/React application through
StormTTY's PTY and session pipeline. It covers changing animation frames, input,
paste, Unicode and resize, and refuses a noninteractive fallback. The initial
Linux run passes. Native Windows tests distinguish the inbox and explicit
packaged console providers; the latter resolves the captured right-margin
incompatibility, but repeated compact alternate-screen resizing still fails in
some runs. macOS also has a compact-layout resize regression. One fixture is not
a claim that every Ink application works.

```sh
npm ci --prefix tests/ink --ignore-scripts --no-audit --no-fund
npm test --prefix tests/ink
```

Use Node 22 or newer and add `-DSTORMTTY_TEST_INK=ON` to your CMake configuration.
The normal desktop build does not require Node. See the
[verification record](tests/ink_verification.md) for commands, evidence and resize
limitations.

## Testing and contributing

Small changes with a reproducible test are especially useful at this stage.
Include the OS, architecture, compiler, dependency revision and exact commands
in a bug report. For terminal behavior, provide the smallest byte sequence or
program that reproduces it, plus expected and actual output. Remove credentials,
private paths and shell history from logs and captures before sharing them.

Run the native CMake tests above and the bootstrap boundary checks:

```sh
python3 tools/bootstrap_tests.py -v
```

On Windows, the opt-in development smoke mode runs a controlled ConPTY session
and can save its renderer framebuffer:

```powershell
.\build-msvc\stormtty.exe --smoke --capture frame.png
```

Smoke mode does not read user configuration or profile scripts. A framebuffer
capture cannot establish desktop-composited translucency, IME behavior,
screen-reader support or human-perceived latency. Use the
[window](tests/window_verification.md) and
[renderer](tests/renderer_verification.md) checklists for those boundaries.

Keep platform code out of the portable core, preserve upstream notices and avoid
unrelated changes. A new capability needs a test at the layer that actually
implements it; a parser test alone cannot qualify a native UI feature.

### Security

Terminal output is untrusted input. Titles, hyperlinks, clipboard requests and
images must not become implicit permission to execute commands or access local
resources. Application-requested clipboard reads and writes are denied by the
current adapter; explicit user copy/paste is a separate path. Resource limits
and malformed-input behavior remain part of the release review, not a claim of
a completed security audit.

Do not include secrets or weaponized payloads in public reports. A dedicated
private security-reporting channel has not yet been established.

## Licensing and acknowledgments

**A license for the original StormTTY application code has not yet been
selected.** Do not assume that the upstream engine's license applies to the
whole repository or grants redistribution rights for the application.

`libghostty-vt` is part of the MIT-licensed Ghostty project. Its
[copyright and license notice](docs/decisions/GHOSTTY-LICENSE.txt) is retained
here and by the bootstrap. Binary distribution still requires an audit of all
included dependencies and artifact-specific notices.

Thanks to the Ghostty contributors for the terminal engine this project builds
on. StormTTY's native application, integration choices and verification claims
are its own responsibility.
