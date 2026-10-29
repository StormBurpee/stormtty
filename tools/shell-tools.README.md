# Pinned fzf / zoxide qualification slice

This is a **local development/qualification overlay**, not a complete shell
runtime and not approval for public binary redistribution. It changes no base
MSYS runtime versions or native/default activation gates. No binary or source is
patched, installed globally, or executed during staging.

## Exact candidates and evidence

- MSYS2 UCRT `mingw-w64-ucrt-x86_64-fzf 0.74.4-1`, 2,167,539 bytes,
  SHA-256 `f966079723c9398719e1395daaed65f26cffbc1db77e1e1aab11ec7da7ec71cc`.
- MSYS2 UCRT `mingw-w64-ucrt-x86_64-zoxide 0.10.0-1`, 463,834 bytes,
  SHA-256 `b9140f8ee12eeb8dcdf4f755f4cc948caa360b6f2cd51b6d67ba336f84fa946e`.
- Exact official URLs, matching source-only tarball SHA-256/size, PE import lists,
  and signature fingerprint are recorded in `shell-tools.lock.json`. Both detached
  signatures verified against the official MSYS2 keyring; GPG global ownertrust
  was not asserted. The live fzf package page now lists 0.74.5: this slice does
  **not** silently upgrade to it.
- Both `.PKGINFO` files declare no runtime package dependencies. Linux `objdump -p`
  finds only Windows system/UCRT imports, no MSYS/GCC private DLLs. This is static
  import evidence, **not** proof of native loading, OS compatibility or behavior.
- Both package `.BUILDINFO`, `.PKGINFO`, `.MTREE`, all shipped scripts/docs/licenses
  and complete `ucrt64` payload are retained. Never flatten into `usr/bin`.
- Nested upstream source archives match their PKGBUILD hashes: fzf
  `1046857c337f5bd05f6fa482446b5a42a011615105743efbe4efee0970b24bb7`, zoxide
  `4fcd4272b013a10b637dbcc299c58a9924b94470a9042677ca1a204cc2e9150e`.
  The zoxide recipe contains no source patch; fzf has the packaged architecture
  detection patch and external-link build flag. Neither recipe is executed here.

## Build-only use

```
python tools/shell-tools.py --cache /absolute/cache --destination /fresh/overlay \
  --zstd /absolute/trusted/zstd --offline --sources
python tools/shell-tools-tests.py
```

Without `--offline`, only staging downloads exact hash/size-pinned official MSYS2
archives. Shell startup has no downloader, installer, self-update, `zoxide init`
process or generated-code eval. Upstream fzf still uses its own mature option
restoration/widget implementations, including internal eval with upstream
quoting; we have not replaced those widgets with an imitation.

### Integrated development build

The build bootstrap now stages the intact UCRT tools beside MSYS, then inventories
all files with the base runtime. The developer stager verifies exact executables,
completion scripts, adapters, source inventory, and tools lock. CMake tracks these
inputs, and no-op preparation repairs missing required executables. The preset
loads tools after user configuration/completion and before syntax highlighting.
Absent `SHELL` defaults to the bundled Zsh executable so native fzf custom commands
run in the expected shell; explicit values, including empty, are preserved.

The notice packet retains package notices and both top-level sources, plus a
separate conservative static source inventory. `--source-packet` also includes the
138 verified static archives/texts from `--tools-source-cache` (default:
`CACHE/static-sources`). This cache must already exist; no unpinned dependency
resolution occurs. `release_notices_complete` remains false because compiler and
static linked-closure review is unfinished. Base-runtime notice completion is
recorded separately. Nothing declares public redistribution qualified.

Only zoxide uses an owned adapter (`shell-tools-zoxide.zsh`), explicitly derived
from its MIT template with notice retained. It is **not claimed to be captured
output from the Windows executable**. It uses `cygpath -w -- "$PWD"` (physical
path when requested), converts returned Windows query paths with `cygpath -u`,
keeps `z` and `zi`, and never replaces `cd`. It registers only a `chpwd` hook,
not one executable on every prompt; the initial directory is learned on movement,
not at shell startup. Existing `z`/`zi` functions, aliases and commands survive.
A literal-path concern in the pinned Windows init template must be checked against
actual native `zoxide init zsh --hook pwd` output before claiming an upstream bug.
The adapter can be replaced with a reviewed native-generated version if preferred.

`STORMTTY_SHELL_TOOLS=0` disables all integration. Individual switches:
`STORMTTY_FZF`, `STORMTTY_FZF_COMPLETION`, `STORMTTY_FZF_KEYS`, `STORMTTY_ZOXIDE`.
All default on; zero opts out. FZF variables and `_ZO_*` options are not rewritten.
`ucrt64/bin` is appended so explicit user command selection wins. In a trusted
persistent managed session, an absent `_ZO_DATA_DIR` is scoped per invocation to
the native-validated `_stormtty_state`; explicit overrides, even empty, survive.
`STORMTTY_EPHEMERAL=1` installs no zoxide jump functions or persistence hook.
No global MSYS argument/environment conversion flags are changed.

## Sources and notices: deliberately incomplete release gate

Top-level package licenses are MIT, **the static closure is not all MIT**.
The supplemental inventory is `shell-tools-sources.lock.json`:

- 11 exact Go modules from fzf's embedded buildinfo verified against Go `h1` file
  tree hashes (not just downloaded ZIP SHA). Includes Apache-2.0 and BSD licenses.
- 127 exact registry crates from zoxide's Cargo.lock, SHA-256 verified. This is a
  conservative superset including dev/build/other-platform dependencies, **not** an
  assertion that every listed crate is linked. It contains multiple licensing
  choices plus Zlib, MPL-2.0 and Unicode terms; no blanket MIT classification.
- Source archives and available license/notice paths/hashes are pinned. Eight
  crates in the superset have no conventionally named license file in their crate
  archive; legal metadata alone is not a completed notice review.
- Go 1.27.1, Rust 1.96.1 and exact MinGW static support/compiler-runtime notice
  review remains necessary, as does distinguishing the actual Rust linked closure.
  Original `.BUILDINFO` identifies those build versions; do not upgrade runtimes
  just to simplify this review.

`python tools/shell-tools-notices.py --cache /verified/static-sources
--destination /fresh/packet` verifies all supplemental source archive and notice
hashes offline, retains their source bytes, and emits an explicitly review-pending
packet. It does not replace existing base-runtime notices or claim source is public.

## Tests and honest performance scope

15 Linux tests cover opt-outs, no zoxide invocation during startup, ephemeral
behavior, retained overrides, z/zi and previous-directory behavior, conversion
arguments, spaces/Unicode and literal command-substitution/backtick filename data,
nonduplicating hooks, corrupt cache, traversal, Windows aliases, foreign-prefix
payload and symlink rejection. Zsh syntax and Python compilation checks also pass.
The exact official packages stage offline under a spaces/Unicode directory.

A 50-pair warm Linux PTY sample (10 warmups, `compinit -i -D` in both sides) is in
`shell-tools-benchmark.json`: medians ~196.56 ms baseline / 199.79 ms tools; median
paired increment ~1.48 ms. This is **not native Windows startup performance** and
includes neither first-slice prompt overhead nor database operations. An attempted
ptrace process trace was unavailable on this executor; it is not claimed verified.

## Required native Windows qualification before activation

1. Integrate atop the separately-qualified OMZ/native-private-state slice; stage
   all tools/notices offline. Verify exact manifest digests and executable versions.
2. Capture unmodified `zoxide.exe init zsh --hook pwd` output; compare Windows pwd
   handling to exact source. Exercise it before attributing a template issue.
3. In StormTTY ConPTY, exercise official fzf Ctrl-T, Ctrl-R, Alt-C and `**<Tab>`;
   accept and cancel each, empty result and multiline history, changed keymaps,
   custom FZF options/commands, return to prompt and subsequent ordinary input.
4. `z`, `zi`, `z -`, direct paths, explicit `--`, `_ZO_ECHO`, `_ZO_RESOLVE_SYMLINKS`,
   exclusion/data overrides, empty HOME/ephemeral state, failed hook and repeat load.
   Observe the DB only inside the authorized private state/default or explicit
   override, never the runtime/CWD. Retain other hooks and previous exit status.
5. Install/runtime/HOME/CWD/selected paths with spaces, Unicode, percent, quotes,
   backticks and literal `$()`; verify selected paths remain data. Test drive-root
   and UNC conversion; no global MSYS conversion-disable workaround.
6. Test clean PATH with no separately installed fzf/zoxide; inspect native imports
   and loaded modules under the supported Windows baseline. Confirm user-selected
   external binaries still win without silently asserting they are this pin.
7. Measure cold/warm startup and process creation with tools on/off on the same
   native machine and PTY. No zoxide executable at startup/prompt; DB work occurs
   on navigation. Confirm no network access while disconnected.
8. Complete static notices/source review separately before any public binary release.

## Native probe and first Windows evidence

`tests/shell_navigation_probe.cpp` builds as `shell_navigation_probe.exe`. The
native workflow invokes it only after the real managed OMZ private-state gate
passes, using both inbox and packaged ConPTY under an owned process timeout.
The probe creates only isolated synthetic known-folder fixtures and exercises:
- Exact fzf/zoxide versions, module presence, and no pre-navigation database.
- Upstream Ctrl-T/Ctrl-R/Alt-C and `**<Tab>` selection; cancel/no-match return.
- Real zoxide `z`, `z -`, `zi` accept/cancel, spaces/Unicode and literal shell
  syntax in directory names; database stays in the native-validated private state.
- Unmodified native `zoxide init zsh --hook pwd` captured to the synthetic
  `build/artifacts/shell-tools-zoxide-init.zsh` for exact template comparison.

The first Windows run below reached the final walker check but failed. This test
is not a claim of complete widget/keymap compatibility, physical feel, Windows
performance or completed native qualification.
The separate checklist above remains the full activation/release review scope.

## History-widget support closure

The exact upstream Ctrl-R widget needs Perl or awk. The clean bundled PATH has no
Perl, so it now includes official MSYS2 gawk 5.4.1-1, libreadline 8.3.006-1 and
mpfr 4.2.2-1 (about 2.3 MB additional compressed binaries). Existing bash, libintl,
ncurses, gmp, GCC support and MSYS runtime pins are unchanged. The three detached
binary signatures verified with the official MSYS2 development key fingerprint
5F944B027F7FE2091985AA2EFA11531AA0AA7F57. Static PE imports match this closure;
native awk execution passed in the first Windows run below; full release
qualification remains pending.

Matching source archives and exact gawk/MPFR/readline notice assets are pinned.
Readline source is already in the base supplemental inventory and is deduplicated
only when size/hash match. gawk is GPL-3.0-or-later; MPFR is LGPL-3.0-or-later.
The private runtime preserves the package layout and materializes the gawk hardlink
as identical regular bytes, using the existing bounded MSYS extractor. No package
installation scripts run. Sources/notices are assembled with the other components,
not substituted for the unfinished Go/Rust compiler/static notice review.

The native navigation probe now explicitly requires absent Perl and working awk
on its clean PATH, exercises Ctrl-R, and also tests default fzf Ctrl-T/Alt-C walkers
with exact nested-space/Unicode path assertions. The final default Alt-C case
failed in the first native run; it remains a blocking qualification check.


### First native run: partial evidence, not qualification

[Run 40784574566](https://github.com/StormBurpee/stormtty/actions/runs/40784574566)
tested commit `52622928af2208eb9a49bc60c4e692521ef12127` on Windows 2025 with both
inbox and packaged ConPTY. Both probes returned `code=1 phase=13 checks=152`.
Earlier required phases passed: exact versions and awk with Perl absent, upstream
widget selection/cancellation and history replay, z/zi navigation, literal-path
safety, private database location, fuzzy Tab, and the default Ctrl-T nested Unicode
file. The final default Alt-C directory selection/teardown sequence is unresolved;
numeric-only subphase diagnostics are being added without relaxing its assertion.

The same run captured **unmodified** `zoxide 0.10.0 init zsh --hook pwd` output in
`evidence-windows-2025` artifact `12859197845`, member
`build/artifacts/shell-tools-zoxide-init.zsh` (4732 bytes):

- Artifact ZIP SHA-256: `98d6e2b3bea32575559c552351b0042e44a0df5676825be9dd026283183cc3ae`
- Init file SHA-256: `850a0e63991fcf5b073427a7dccd0c9e4de64f92161680772a4fd4c79389e08a`

Its Windows pwd function is verbatim:

```zsh
function __zoxide_pwd() {
    \command cygpath -w "\builtin pwd -L"
}
```

This is captured generated text, not evidence that evaluating that unmodified
function works. StormTTY does not evaluate it: its maintained adapter converts
`$PWD` explicitly, and the adapter's native navigation/private-state phases passed.
The full upstream template, other options and release checklist still require
review. Probe startup samples are not a controlled tools-on/tools-off benchmark.

### Owned-console UTF-8 setup

The native comparison in run `40794132587` observed CP437 and bytes `3f3f` for
`路径` through both direct ConPTY and MSYS-spawned native ReadFile clients. CP65001
preserved `e8b7afe5be84` on both providers. The inbox comparison also proved that
one-shot setup survives helper exit, fixes exact fzf Unicode directory selection,
leaves another live PTY unchanged, and can restore the original codepages. The
packaged run stopped earlier at a cancellation synchronization check; its complete
one-shot comparison was not qualified by that run.

The app now builds `stormtty-console-setup.exe` as an owned sibling executable.
Managed launches resolve only that fixed sibling (never PATH or a profile-supplied
helper), overwrite the reserved bridge, and consume/unset it before user config.
The helper uses its inherited console association, sets and verifies UTF-8 input
and output codepages, and rolls back partial failure. It never attaches to another
console or changes system settings. Missing/invalid app sidecars block managed
launch; runtime setup failure is explicitly reported and does not set the success
flag. Non-managed profiles are unchanged. This adds one startup process; its cost
has not yet been measured in a controlled native benchmark.

Production sidecar integration has local structural/managed tests and Windows
cross-build evidence only until its exact native CI run passes. Both providers
must still pass the actual built sidecar bridge, private-state tests, Unicode
widgets and independent-session isolation. Upstream runtime/tool pins and their
source/notice inventories are unchanged.
