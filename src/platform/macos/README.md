# macOS host: implemented source, native verification pending

This is an AppKit/Objective-C++ platform host, not a WebView or a separate terminal
model. `Workspace`, commands, declarative configuration, `Session`, the POSIX
`Process`, `TerminalCore` and owned `RenderFrame` remain shared with Windows.
Windows remains the first release priority.

## Native build contract

- Official Xcode/Command Line Tools and the Apple macOS SDK. Native API baseline:
  macOS 11; **distribution minimum is not qualified** until the non-system
  libc++/unwind dependencies are packaged and their deployment targets audited.
- C++20 and Objective-C++20; compile `.mm` with `-fobjc-arc`.
- Xcode 16.4's Apple libc++ lacks the shared actor's required cancellation APIs.
  Use Homebrew **llvm@20 / 20.1.8**, including its matching libc++ headers and
  runtime, through `cmake/MacOSLLVM.cmake`. LLVM 20 made `std::stop_token` and
  `std::jthread` stable; no experimental libc++ macro is enabled. Both CXX and
  OBJCXX use the same compiler and standard library. New headers plus Apple
  system libc++ is not a supported combination.
- Renderer: `renderer.mm`, `png_decoder.mm`, linking AppKit, CoreText, CoreGraphics,
  Metal, QuartzCore, ImageIO (Foundation/CoreFoundation transitively).
- GUI: `src/app/macos_main.mm`, `window.mm`, linking the shared workspace/session
  and renderer libraries. Root CMake/CI owns the final target definitions.
- The exact-pinned libghostty-vt macOS library must match the machine architecture.
  There is no downloaded unofficial SDK or alleged Linux-to-macOS native proof.

Native setup after the normal pinned Ghostty bootstrap:

```sh
brew install llvm@20
cmake -S . -B build-macos -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/MacOSLLVM.cmake" \
  -DSTORMTTY_LLVM_ROOT="$(brew --prefix llvm@20)" \
  -DSTORMTTY_GHOSTTY_ROOT="$PWD/.deps/ghostty-native" \
  -DSTORMTTY_BUILD_APP=ON
cmake --build build-macos --target macos_toolchain_probe --parallel 2
ctest --test-dir build-macos -R '^macos_toolchain_probe$' --output-on-failure
otool -L build-macos/macos_toolchain_probe
cmake --build build-macos --parallel 2
ctest --test-dir build-macos --output-on-failure
build-macos/stormtty.app/Contents/MacOS/stormtty --smoke \
  --capture "$PWD/build-macos/artifacts/native-macos.png"
```

`macos_toolchain_probe` verifies the loaded LLVM libc++ path with dyld, compiler
and header major, stop callback ordering, real `jthread` cancellation,
`condition_variable_any` wake cancellation and destructor stop/join. It is a real
CTest runtime test with a timeout, not just a successful header compile. A Linux
semantic pass explicitly identifies itself as non-Apple evidence.

The development app currently references Homebrew runtime paths. A distributable
bundle must relocate and sign the matching libc++/libc++abi/libunwind and Ghostty
dylibs, retain notices, and audit their Mach-O minimum OS/load commands and rpaths.
A Homebrew bottle may require macOS newer than the app's API baseline. Setting
`CMAKE_OSX_DEPLOYMENT_TARGET=11.0` does **not** prove the resulting dependency tree
runs on macOS 11. Packaging, signing/notarization and oldest-OS validation remain
separate release gates.

Normal launch starts the configured absolute shell executable (default `/bin/zsh`)
through the shared asynchronous PTY/session actor. `--config /absolute/path` is
supported; otherwise the optional path is
`~/Library/Application Support/StormTTY/config.stormtty`. No config is created or
modified automatically. Relative executable names are rejected rather than
interpolated into a shell command.

## Host behavior

Native AppKit window controls, fullscreen and window dragging are retained. A
unified compact titlebar hosts the visual area; custom native tab buttons are in
an AppKit titlebar accessory view, **not** subviews of the Metal layer-hosting
view. Traffic-light clearance and terminal safe insets come from native button
geometry and `contentLayoutRect`, not guessed Windows caption dimensions. Split
geometry is shared Workspace geometry in AppKit points, with backing pixels only
at the renderer/PTY boundary. Tabs, splits, focus, zoom, divider drag/double-click,
keyboard divider resize, tab rename/reorder and pane-to-new-tab preserve sessions.

Cmd shortcuts use the shared macOS binding resolver. Ctrl+C is terminal input.
`NSTextInputClient` keeps marked text local, exposes candidate coordinates and
sends committed text once. Option/dead-key text uses AppKit interpretation rather
than a fabricated US keyboard string. Native tab/button accessibility and a basic
bounded terminal text accessibility value are implemented. Clipboard writes happen
only on explicit Copy; paste snapshots bytes before confirming multiline/control
text. OSC clipboard requests remain denied in the shared adapter. Links are not
opened. No terminal output is treated as a command or a URL action.

Background-only translucency uses an AppKit material behind a premultiplied Metal
surface. Shared `auto`/`acrylic` preferences map to AppKit
`UnderWindowBackground`; `mica` maps to AppKit `WindowBackground`. These are
native macOS material choices, not Windows Mica/Acrylic implementations. Text
and explicit ANSI/TUI backgrounds stay opaque. Reduce Transparency,
Increase Contrast and `backdrop = none` force an opaque canvas. Reduce Motion
suppresses cursor/text blinking; no decorative animations run. Pending callbacks
are weak-owned and coalesced; unchanged scenes do not present. Process close/reap
and PTY waits never run in AppKit callbacks.

## Renderer and deliberate limits

This is an initial **correctness backend**, not Ghostty performance parity:

- CoreText shapes attributed rows with fallback/contextual runs. UTF-16 clusters
  are mapped back to the VT-owned grapheme cell spans. Dirty rows are rasterized
  into bounded BGRA textures. Metal really composes backgrounds, image layers,
  row textures and cursor/chrome quads into one persistent window surface.
- Default row cache: 16 MiB / at most 16,384 entries. Image cache: 64 MiB / at most
  1,024 entries. Per-frame new texture uploads: 64 MiB. At most two command buffers
  are in flight; referenced evicted textures can live until those buffers finish.
  Configurable cache ceilings are 64/128 MiB. Surface limit: 128 MiB and 16,384
  pixels per dimension. Driver/drawable storage and ImageIO internals are additional
  allocations; these are **not** a hard whole-process RSS/VRAM cap.
- Shared texture storage on Apple-family GPUs, managed textures/readback
  synchronization on Intel/AMD. Hardware paths require native CI/physical tests.
- Changed scenes recompose the entire target; only text rasterization is row-dirty.
  This is not a glyph atlas, optimized GPU text backend or damage-only compositor.
  Text in chrome/preedit is rasterized separately; terminal row caches are bounded.
- Block/bar/underline/hollow cursors, wide spans, underline variants, rounded
  rectangles and stroked lines are implemented. Sophisticated bidi grid mapping,
  fallback fragments, all font-feature combinations and perfect IME preedit
  styling still need native qualification/iteration. Color-emoji glyphs are not
  recolored inside a block cursor.
- Shaping bounds: 4,096 cells, 64 KiB text and 131,072 output glyphs per row;
  individual graphemes above 4 KiB render as a replacement glyph. The original
  VT text is not discarded. Shared grapheme anchors preserve positions across
  CoreText fallback fragments. These are safety limits, not performance targets.
- CoreText fallback/color-emoji rendering is implemented, but multilingual shaping,
  ligatures, glyph placement and Retina/monitor transitions are not visually
  certified from Linux. Native controls and terminal frames have not been called
  beautiful, physically verified or performance-qualified merely because they exist.
- `CAMetalLayer.nextDrawable` may briefly wait per Apple's contract. Two in-flight
  buffers and three drawables avoid intentionally saturating the pool, but this is
  not a proven strictly nonblocking presentation path. Renderer statistics count
  submitted presentations; smoke readback separately requires a nonzero drawable
  `presentedTime` for the target frame.

The PNG decoder preflights the complete PNG container, 8,192-pixel dimensions,
16-megapixel limit, compressed bytes and caller RGBA budget before pixel decode.
It rejects APNG, 16-bit images and compressed text/color-profile chunks, checks
ImageIO's metadata agrees with IHDR, and disables decoded image caching. Core
Graphics premultiplied data is converted to the adapter's straight RGBA contract.
ImageIO's internal scratch allocations are not allocator-controlled. Failure is
contained at the callback boundary. Capture uses ImageIO only on explicit smoke
output paths and captures this renderer, not the desktop or unrelated windows.

## Evidence and test commands

`tests/macos_input_tests.cpp` passes as a **portable** C++ test on Linux. That
checks Apple physical key identities and the shared Cmd/Ctrl/IME-routing policy;
it does **not** verify AppKit text services.

Native targets to run with the official SDK and a supported Metal device:

- `macos_png_tests`: straight RGBA, byte/dimension bounds, malformed PNG, actor
  thread operation.
- `macos_renderer_tests`: offscreen Metal composition/readback, CoreText ink,
  default-alpha versus explicit background, cropped images, dirty row reuse,
  cursor-only reuse, Retina metrics, resource reconstruction, zero-size suspend.
- `stormtty --smoke --capture /absolute/path/native-macos.png`: real AppKit window,
  controlled `/bin/sh -c` without user startup/profile hooks, actual PTY ready
  marker, host-sent input and Enter, parsed success marker, Metal submission plus
  **onscreen `presentedTime`**, framebuffer PNG, successful child exit and reaping.
  The PNG is the actual Metal terminal/chrome surface; native titlebar tab buttons
  and system traffic lights are not in that surface. It is not a whole-window
  screenshot or a substitute for physical AppKit UI inspection.
  Capture/readback keeps the renderer Host-owned and gives one worker exclusive
  access while UI renderer operations are deferred; it does not block an AppKit
  callback. No Metal device or desktop
  session is a failed gate, not a silently passing mocked renderer.

Physical follow-up gates: Japanese/Chinese/Korean IMEs, dead keys and layouts,
VoiceOver reading/selection, OS Secure Event Input (not implemented), tab overflow/drag, split
stress, fullscreen traffic lights, Intel/AMD and Apple silicon, 1x/2x transitions,
color emoji/Arabic/combining clusters, material contrast, selection/paste controls,
repeated close/resize while a child is producing output, and measured CPU/GPU/memory.
In-app settings editing/reload, scrollback-wide find, pane swaps/moves to an
existing tab, tab drag/overflow menus and rich AX text ranges are not implemented.

## Toolchain references

- [LLVM 20 stable stop-token/jthread implementation](https://releases.llvm.org/20.1.0/projects/libcxx/docs/ReleaseNotes.html)
- [Homebrew llvm@20 compiler/runtime layout](https://formulae.brew.sh/formula/llvm@20)
- [LLVM alternate standard-library configuration](https://libcxx.llvm.org/VendorDocumentation.html)
- [Apple C++ support table](https://developer.apple.com/xcode/cpp/) (does not currently establish a newer-Xcode-only fix for P0660)

## Official API references used

- [AppKit layer-hosting constraints](https://developer.apple.com/documentation/appkit/nsview/wantslayer)
- [CAMetalLayer and drawable lifecycle](https://developer.apple.com/documentation/quartzcore/cametallayer)
- [Drawable presentation evidence](https://developer.apple.com/documentation/metal/mtldrawable/presentedtime)
- [Metal resource storage modes](https://developer.apple.com/documentation/metal/setting-resource-storage-modes)
- [Managed-resource synchronization](https://developer.apple.com/documentation/metal/synchronizing-a-managed-resource-in-macos)
- [NSTextInputClient](https://developer.apple.com/documentation/appkit/nstextinputclient)
- [IME candidate coordinates](https://developer.apple.com/documentation/appkit/nstextinputclient/firstrect(forcharacterrange:actualrange:))
- [Reduce Transparency](https://developer.apple.com/documentation/appkit/nsworkspace/accessibilitydisplayshouldreducetransparency)
- [ImageIO cache control](https://developer.apple.com/documentation/imageio/kcgimagesourceshouldcache)

## Configured native menu shortcuts

The AppKit Terminal menu and command pop-up now show keyboard equivalents from
`Config::bindings`, rather than a second hard-coded map. Native menu dispatch
therefore remains available when a tab button, rather than the terminal view,
has keyboard focus. Missing bindings stay unassigned; custom bindings replace
rather than supplement defaults. Shared OS-reserved chords remain AppKit-owned.
Menu validation suppresses terminal commands while marked text, a modal text
editor/alert, or host shutdown is active. Ctrl+C remains terminal input unless
an explicitly permitted custom binding overrides it.

The mapping uses Apple's documented lower-case base character plus explicit
modifier mask; Backspace/Forward Delete use AppKit's menu-specific characters,
not the event bytes. See [key equivalent documentation](https://developer.apple.com/documentation/appkit/nsmenuitem/keyequivalent)
and [modifier masks](https://developer.apple.com/documentation/appkit/nsmenuitem/keyequivalentmodifiermask).
`macos_menu_tests` constructs owned AppKit menus and verifies real dispatch,
disabled items, custom rebinding and SDK key constants. The exact-branch
`macos-menu-native.yml` builds the actual app with the official SDK and executes
those native tests. Portable mapping tests alone do not establish AppKit behavior,
physical keyboards, layout variants, or IME composition correctness.

**Settings parity limit:** `settings.open` still explains manual configuration;
the macOS host does not instantiate the shared SettingsPanel or native persistence
service. The portable theme exchange parser is available but macOS import/export
pickers are not implemented. Adding direct permanent imports without reversible
Settings drafts would misrepresent Windows parity, so this change does not do so.
No Liquid Glass or other material default is enabled by this keyboard change.

## Read-only pane context inspector

`pane.inspect` is available in the native Terminal menu and command pop-up,
using the configured shortcut machinery (no new hard-coded binding). It takes
one owned active-session snapshot and presents selectable, wrapping text in a
scrollable attached AppKit sheet. Escape or Close dismisses it and returns focus to the
terminal. It does not send terminal input, resolve paths, probe repositories,
open links, or change session ownership. Terminal labels use the shared display
sanitizer. Observed directory and configured start folder remain distinct;
missing observed context stays explicitly unavailable. Retained exited sessions
are labeled. Close/reopen refreshes the snapshot; this is not a live inspector.

`macos_pane_inspector_tests` covers native text controls, untrusted labels,
provenance, owned snapshot lifetime, text selection, and repeated actual sheet
presentation/dismissal. `macos-pane-context-native.yml` builds the real app and
runs it alongside menu and portable context contracts on macOS. These are native
fixture checks, with light/dark owned-view PNGs, not physical keyboard, VoiceOver, IME, or full host-interaction
verification. Pane-header/footer context parity and scrollback-wide Find remain
unimplemented on macOS.
