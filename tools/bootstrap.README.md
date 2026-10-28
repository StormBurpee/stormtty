# Pinned native engine bootstrap

`bootstrap.py` builds the **terminal-state library**, not Ghostty's GUI or a
replacement for StormTTY's native platform adapters. Python 3.10+, Git, and
Zig **0.17.0** are required. CMake and Ninja are not installed by this script.
The lock records the versions used for the initial build (CMake 4.4.4,
Ninja 1.13.2); the native C++ compiler and SDK are supplied by the platform.

## Windows first

Run from an **x64 Native Tools Command Prompt for Visual Studio**, with MSVC,
the Windows SDK, Python and Git installed:

```powershell
python tools/bootstrap.py --install-zig
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSTORMTTY_GHOSTTY_ROOT="$PWD/.deps/ghostty-vt/x86_64-windows-msvc"
cmake --build build/windows
ctest --test-dir build/windows --output-on-failure
```

`--install-zig` is opt-in and downloads only the exact official Zig host
archive listed in `bootstrap.lock.json`, verifying SHA256 before extraction.
Alternatively pass `--zig C:/path/to/zig.exe`. Windows ARM64 is a separate
target and verification gate; an x64 test does not establish ARM64 support.

## macOS and Linux/core

On macOS install Xcode Command Line Tools; `xcrun --show-sdk-path` must work.
The native default is `aarch64-macos` or `x86_64-macos`. On Linux the native
default is `x86_64-linux-gnu` or `aarch64-linux-gnu`.

```sh
python3 tools/bootstrap.py --zig /absolute/path/to/zig
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DSTORMTTY_GHOSTTY_ROOT="$PWD/.deps/ghostty-vt/aarch64-macos"
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

Substitute the actual target prefix printed by bootstrap. Linux validates the
portable core only, not a supported Linux desktop application. macOS is a
product target, but successful Linux builds cannot prove AppKit/Metal/PTY UI.

## Reproducibility, cache and transport

- Ghostty is checked out at exact commit
  `46e9dc9a927b2e7e7cba79e6902c176127dbdf2c`; modified or wrong-revision
  checkouts are rejected. `--source /path/to/existing/checkout` never edits it.
- All source archives have SHA256 pins **and** their original upstream Zig
  content-hash pins. Archive SHA256 values were recorded from the corresponding
  official source downloads; Zig content hashes remain the upstream trust
  anchor. The arocc git dependency uses the same commit's official Codeberg
  archive, whose Zig content hash must exactly match the git dependency pin.
- Zig 0.17.0 standalone `fetch` has a cache-recompression issue for a tarball
  with a top-level directory: it verifies normalized package contents but
  writes a double-prefix global cached archive. The script uses the supported
  `zig fetch --save` workflow against a **throwaway verification manifest**,
  which enables local package storage and correct recompression. It checks
  the resulting content hash against the unchanged upstream pin; Ghostty's
  source and manifest are never edited. `--pkg-dir` without `--save` does not
  enable local storage for standalone fetch in 0.17.0. No check is disabled.
- `.cache/bootstrap/` contains archives, Zig package/object caches and temporary
  files. `.deps/` contains the exact source, optional Zig, and installed engine.
  `--cache` and `--prefix` can move them to another workspace-backed volume.
  A cache lock rejects concurrent bootstrap builds. Do not use two different
  caches to concurrently write the same output prefix. After a killed build,
  remove a stale `bootstrap.active` only after verifying its process stopped.
- `--offline` requires downloaded inputs. `--fetch-only` resolves only the
  needed build dependency graph and exits. It does **not** use `--fetch=all` or
  `--system`, which would change dependency/system-integration behavior.
- SIMD and all upstream VT features stay enabled; `--no-simd` is a diagnostic
  baseline, not the default shipping configuration. ReleaseSafe and baseline
  CPU are defaults. `--jobs 2` bounds compiler concurrency.
- Each installed prefix includes `stormtty-build.json` (source/toolchain,
  host/target, flags, artifact SHA256), the complete bootstrap lock, and
  Ghostty's license. A complete artifact-specific third-party notice/SBOM audit
  is still required before distribution; a root MIT notice alone is not enough.

## Cross-compilation is not runtime verification

```sh
python3 tools/bootstrap.py --zig /path/to/zig --target x86_64-windows-gnu
cmake -S . -B build/windows-gnu -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/ZigWindowsGNU.cmake \
  -DSTORMTTY_ZIG_EXECUTABLE=/path/to/zig \
  -DSTORMTTY_GHOSTTY_ROOT="$PWD/.deps/ghostty-vt/x86_64-windows-gnu"
cmake --build build/windows-gnu
```

This produces a GNU-ABI Windows engine for compile-only checks. It does not
prove MSVC compatibility, Windows execution, ConPTY, DirectWrite, IME, graphics,
or native behavior. Never link its static archive into an MSVC application.
The CMake import helper rejects GNU artifacts for MSVC. Native Windows CI
and actual Windows UI verification remain distinct required gates.
`ZigWindowsGNU.cmake` is an x86-64 Unix-host toolchain with generated compiler
and archiver wrappers. It does not run the produced Windows executables or
replace native SDK/resource-compiler validation.

macOS builds require an actual macOS SDK/host; this helper does not fetch or
redistribute Apple or Microsoft SDKs. No claim of a universal macOS binary is
made by a single-architecture build.

Native build paths containing spaces remain an explicit integration check:
the Python wrapper preserves argument boundaries, but upstream archive-combiner
behavior must also pass on Windows/macOS. Prefer a short, space-free cache path
for the initial native gate; do not infer that this validates path-space support.

Bootstrap's offline boundary checks run without a compiler:

```sh
python3 tools/bootstrap_tests.py -v
```

`tools/bootstrap_smoke.c` is a tiny public-C-ABI consumer for shared/static
link checks. It verifies create, ANSI input, resize, formatted text and cleanup;
it does not represent full terminal compatibility testing.
