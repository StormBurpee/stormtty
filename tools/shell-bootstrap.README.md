# StormTTY Zsh runtime staging (Windows x64 qualification)

No installer, package scripts, global PATH changes or user dotfile edits. This stages the 21 exact official MSYS2 archives in `shell-bootstrap.lock.json`, plus the owned managed loader and pinned OMZ/plugin sources. It does **not** fork Zsh or establish a new Windows runtime pass. The app's existing default activation remains separate from addon qualification.

## Windows build and CI

The normal Windows app build stages this pinned shell automatically through the build helper; there is no app-launch download or global installation. The manual command below remains useful for isolated qualification. Python **3.12+** suffices: on Windows x64, the stager automatically downloads/caches the hash-pinned official Zstandard build tool if `compression.zstd` is unavailable. Python 3.14 uses its standard-library decoder. No pip dependency or manual decompressor setup is required.

```powershell
python tools/shell-bootstrap-tests.py
python tools/shell-bootstrap.py --destination .deps/stormtty-shell --cache .cache/shell-bootstrap
$root = (Resolve-Path .deps/stormtty-shell).Path
$fixture = (Resolve-Path tools/probes/bundled_shell_fixture.zsh).Path
$work = Join-Path $env:RUNNER_TEMP ('zsh-probe-' + [Guid]::NewGuid())
& .\build\bundled_shell_probe.exe "$root/usr/bin/zsh.exe" $fixture $work inbox
# Fresh work directory; explicit signature-verified ConPTY package directory:
& .\build\bundled_shell_probe.exe "$root/usr/bin/zsh.exe" $fixture ($work + '-packaged') --packaged-provider $verifiedConptyDirectory
```

CMake target/workflow wiring is root-owned. Layout is exactly `root/usr/bin/zsh.exe` and `root/stormtty/zsh/.zshrc`. The destination must not exist; the script will never update an installed/live runtime in place. Non-Windows hosts without `compression.zstd` can pass an existing trusted executable explicitly: `--zstd /absolute/path/to/zstd`. Python 3.14 standard-library Zstandard support is documented in [tarfile](https://docs.python.org/3.14/library/tarfile.html).

`--offline` requires every selected archive already in the cache, including `zstd-v1.5.7-win64.zip` when the automatic Windows decoder is needed. `--sources` also downloads and verifies all 17 complete source archives into the cache (~306 MB); it does not execute/extract them or mean a public source offer exists. Binary package downloads total 15,190,478 bytes. Never use a fresh rolling-repository solve in a release build.

## Managed app launch contract

- Executable: absolute `root/usr/bin/zsh.exe`; argv: `-d`, `-i` (non-login).
- Scoped environment: `ZDOTDIR=<root>/stormtty/zsh` in forward-slash Windows form; `STORMTTY_SESSION_ID=managed`; `TERM=xterm-256color`; `COLORTERM=truecolor`; `LANG=C.UTF-8`; `MSYSTEM=MSYS`; unset `MSYS`, `ENV`, `BASH_ENV`.
- Preserve actual `HOME`, CWD and other inherited environment. Prefix the runtime `usr/bin` only in this child's PATH; never mutate application/global PATH.
- The managed loader uses bundled functions and modules, private bounded history/cache, and an optional dedicated user-created `~/.stormtty/zsh/config.zsh` (never global shell dotfiles). Semantic-colour directory/Git/exit/duration prompt; no Git/network subprocess in the prompt. It unexports its bootstrap variables. See the managed OMZ section below for state, security and qualification details.
- A successfully loaded preset sets the non-exported readonly shell parameter `STORMTTY_MANAGED_ZSH=1`; host smoke can assert this alongside nonempty `ZSH_VERSION`. It proves preset loading, not full qualification.
- Existing user-configured profiles and defaults remain separate. Do not quietly replay commands through another shell after launch failure.

## Integrity and extraction

Each archive's exact byte count and SHA-256 is checked, including cached inputs. The lock derives from locally inspected official packages whose detached signatures were verified against the official MSYS2 keyring. CI rechecks the fixed hashes; it does not claim to rediscover an independent signing-key trust chain.

Extraction rejects absolute/traversing paths, Windows reserved names/ADS, case collisions, symlinks, special/sparse files, unexpected roots and duplicate files. Upstream tar hardlinks (including `zsh.exe`) are materialized as independent regular files only when they target an earlier verified same-package file. No Windows symlink privilege is needed. Entry/member/total-byte limits apply. Package `.INSTALL` files are retained under `.stormtty-packages/<name>/` as inert audit data, never run. A fresh temporary staging tree is renamed only after all verification/extraction succeeds.

### Existing Windows checkout: exact notice-byte recovery

Older Windows checkouts may retain CRLF-converted vendor notices even after pulling `.gitattributes`. The build now recovers their exact pinned bytes in memory: it tries the raw bytes first, then CRLF-to-LF and UTF-8 BOM-removal candidates, accepting a candidate only if **both the original SHA-256 and byte count match**. Runtime notices, build-tool license copies and both notice copies inside the full source ZIP receive those verified bytes. No manual checkout repair or cache deletion is needed for this case, and the checkout itself is never modified.

Original upstream CRLF assets remain byte-exact because their raw pins are checked first. Unrelated edits still fail with expected/actual hashes and sizes; executable, package/archive and build-evidence validation never uses text recovery.

## Build-tool provenance and cache policy

`tools/shell-zstd-bootstrap.lock.json` pins the [official facebook/zstd v1.5.7 Windows x64 release](https://github.com/facebook/zstd/releases/tag/v1.5.7), asset ID 233456224. ZIP SHA-256: `acb4e8111511749dc7a3ebedca9b04190e37a17afeb73f55d4425dbf0b90fad9` (1,747,181 bytes). The selected `zstd.exe` is independently pinned to `8076aae03feac7c66b319579e82172eed168deed2a3f25e5e2d3c60f55e84111` (1,601,409 bytes). PE inspection finds only Windows `KERNEL32.dll` and `msvcrt.dll` imports; no extra DLL is copied.

Trust is the inspected official HTTPS release asset plus reviewed fixed hash, **not** a fabricated signature claim: upstream publishes signatures for source archives, but no detached signature/checksum sidecar for this Windows ZIP, and its GitHub API digest field is null. HTTPS redirects are restricted to GitHub and its official release-asset host. Every reuse rechecks ZIP, executable, README and license text hashes. Unsafe/duplicate ZIP paths and linked/reparse cache files are refused. Only `zstd.exe`, upstream README, LICENSE and COPYING are placed in `<cache>/tools/zstd-v1.5.7-win64/`; the original archive remains cached. Corrupt caches fail rather than being silently repaired. No download is taken from a moving `latest` URL.

This decoder is a **build tool only**, not part of the app runtime or default shell package. Its BSD/GPL alternative notices are preserved in the private build cache; no global PATH lookup or installer is used. `python tools/shell-zstd-bootstrap.py --cache CACHE [--offline]` may prewarm it without executing the executable. Windows runtime execution is verified by native CI, not by Linux extraction tests.

## Qualification and public distribution are separate

The current lock records the successful native Windows Zsh **subset** (both ConPTY providers and five app smoke modes) at [CI run 40650974545](https://github.com/StormBurpee/stormtty/actions/runs/40650974545), commit `cab6eb52cf2f3d227abb4967ea812ab56883d9bf`. It does not claim the whole workflow passed: separate Ink/macOS-presentation failures, physical UI and broader compatibility remain outside that subset.

`stormtty-shell-manifest.json` now reports `qualification_only: false` and `default_activation_allowed: true` for the pinned build-default runtime. Raw staging still reports `release_notices_complete: false`; the app build wrapper sets that true only after including the reviewed `shell-notices` packet. `public_redistribution_ready` and `public_source_availability_verified` remain **false** independently. Build-default activation is not a claim of public release readiness.

Before a public binary download, preserve the completed notice overlay and all upstream notices, and actually provide the matching complete corresponding-source ZIP with a clear direct link beside that binary. Generate it with `shell-notices.py --source-packet`; all 19 original archives, static dependencies, recipes, patches and actual build-assembly scripts are included. GPLv3 section 6(d) is the intended route, not a vague written offer or upstream links alone. See `tools/shell-notices/REVIEW.md` for the exact component mapping and the retained upstream dumper license-file inconsistency. No first-party relicensing or Ghostty GPL shell-integration copying is performed.

## Managed Windows home and drives

The staged **private runtime** now owns two manifest-hashed files, `etc/fstab`
and `etc/nsswitch.conf`; they are not edits to the user's MSYS2 installation or
Windows environment. Drive roots appear as `/c`, `/d`, etc. (not `/cygdrive/c`),
with case-insensitive/no-POSIX-ACL drive mounts. UNC paths remain UNC paths.
The runtime root `/` still points to the bundled runtime, not the Windows drive.

An existing `HOME` wins. When absent, the host reads a **wide Windows environment
snapshot** and supplies `HOMEDRIVE` + `HOMEPATH`, then `USERPROFILE`, to this child
only. This avoids the runtime's early ANSI passwd lookup losing Unicode. If those
are absent, `db_home: env windows` retains Windows account-database fallback.
Windows-form paths receive the runtime's normal POSIX conversion. An explicit
POSIX `HOME`, including an old `/cygdrive/...` value, is **not** silently repaired:
update that caller setting if it no longer names the desired location. Explicit
empty `HOME` is restored by the preset using a child-only host marker because the
pinned runtime imports even empty path variables through its converter. The
marker is consumed, not exported into descendant shells. No `cd` is inserted:
project CWD and saved shell choices still win; Global HOME shell dotfiles are not loaded or written; only the dedicated
`~/.stormtty/zsh` state/config contract below applies. Global PATH is unchanged.

`/tmp` uses the runtime's `usertemp` mount (`GetTempPathW`), so ordinary temporary
files go to the launching user's Windows temp location rather than the possibly
read-only install directory. `TMPDIR`, `TMP`, and `TEMP` are not replaced. Windows
chooses its temp location from that process's environment; it must already exist
and be writable. A path below this mount can legitimately display as `/tmp/...`
instead of `/c/...`. This is not a private sandbox or a temp-cleanup service.

Both automatic preparation and explicit developer-runtime staging reject stale
or missing managed config; restage with the current `shell-bootstrap.py`. The
install validator checks the same files. Corresponding-source packets include
the two original configuration assets and regression gate; upstream binaries,
licenses, source pins and public redistribution gates are unchanged.

Semantics checked against the pinned package's Cygwin source baseline, commit
`867eb7748e6e5a0e8430ce9f46af27b4b1e8f204` (`mount.cc`, `uinfo.cc`, `environ.cc`),
its MSYS2 mount/empty-environment patches (0005, 0013, 0030, 0031), and official references:
- [MSYS2 filesystem paths](https://www.msys2.org/docs/filesystem-paths/)
- [Cygwin mount table and usertemp](https://cygwin.com/cygwin-ug-net/using.html)
- [Cygwin home lookup](https://cygwin.com/cygwin-ug-net/ntsec.html)

Run `python tools/shell-path-tests.py --runtime <absolute-staged-root>
--resolver-probe <build>/bundled_shell_probe.exe` on Windows
from a local-drive workspace. It invokes the actual host wide-API resolver and
checks the managed preset, HOME fallback,
explicit/empty HOME, Unicode/spaced project CWD, drive navigation, `/tmp` writes,
and user-dotfile isolation. It fails explicitly on non-Windows hosts; Linux
staging tests/cross-compilation do **not** qualify native path behavior. This new
configuration needs that native gate; prior shell/ConPTY qualification is not a
substitute for it.

## Managed Oh My Zsh source bundle (new; native qualification pending)

The managed profile now loads **real upstream `oh-my-zsh.sh`**, with its
completion, key-binding, history, Git, compfix and async-prompt libraries.
`tools/shell-addons.lock.json` pins exact official source commits, archive sizes,
SHA-256 hashes and licenses for Oh My Zsh (MIT), zsh-autosuggestions (MIT), and
zsh-syntax-highlighting (BSD-3-Clause). `shell-addons.py` downloads only at build
preparation, verifies every cache reuse, and stages an allowlisted source subset
rather than thousands of unused plugins/themes. Full original source archives
are retained in the build cache and included in the corresponding-source ZIP;
upstream copyright/license files remain next to each runtime component.

The managed MSYS/Cygwin profile registers a narrow npm completion adapter after
`compinit` and before the optional user config. The bundled Zsh `_npm` otherwise
asks npm to generate shell code; native npm rejects the real `MSYSTEM=MSYS`
environment with `ENOTSUP`. Our adapter queries candidates directly using npm's
own `COMP_*` Zsh protocol, with `TERM=cygwin` **only for that child query** to
select npm's supported Cygwin-compatible completion path. It does not evaluate
returned code, launch npm at startup, wrap normal npm commands, alter `MSYSTEM`,
or change the shell's terminal type. Newline-separated candidates preserve spaces
and punctuation and Zsh performs prefix filtering/quoting. Query failures return
no matches, with stderr suppressed as in npm's own Zsh integration. Ordinary npm
errors remain visible. No cache location, permissions or logging configuration is
changed; an npm-cache log-write error still needs separate diagnosis if it occurs
on a normal command. POSIX and non-managed profiles keep their original behavior;
private user config can replace the managed completion with `compdef`.

`tools/shell-managed-tests.py` covers the adapter with executable spies and the
available real npm, including script names, non-execution, child-only environment,
failure/empty results and opt-out/profile boundaries. Linux protocol tests are
not native Windows qualification. Before claiming native support verified, test
on the bundled Windows Zsh with the user's native Node/npm: tab `npm run bu`
against `build`, `dev`, `test`, `build:watch` and a spaced script name; verify the
buffer gets a properly quoted completion without executing scripts or printing
errors, and confirm ordinary `npm --version` and `npm run` still see the original
`TERM=xterm-256color` and `MSYSTEM=MSYS`.

An isolated native check is available through the root-owned
`diagnostic/npm-completion` push workflow. It records the exact commit and
preinstalled Node/npm versions, stages the pinned runtime, and runs
`shell_navigation_probe --npm <runtime> <node-directory>` with inbox ConPTY.
It checks real ZLE Tab buffers, literal script candidates, no script execution,
startup laziness, unchanged ordinary-command environment, user overrides and
fixture cleanup. It uploads only fixed result fields, process metadata and tool
versions; no HOME, package cache, history or terminal transcript is retained.

Three explicit distribution changes to OMZ are applied by the build recipe:
1. The loader's revision lookup is replaced with the pinned commit, eliminating
   a pointless Git process against an archive without `.git` metadata.
2. `tools/check_for_upgrade.sh` is replaced by a no-op. Merely setting OMZ's
   update mode to disabled is insufficient: upstream first migrates the user's
   `~/.zsh-update`. We must neither migrate that file nor update this immutable
   bundle. CLI/update-management libraries and unrelated themes/plugins are not
   staged. Completion security (`compinit -i`, not `-u` or `-C`) remains enabled.
3. When persistent state is unavailable, the loader skips cache creation and
   uses `compinit -i -D` without dumps, while still loading the same real OMZ
   libraries and packaged editing modules. No fallback HOME or temporary
   persistent directory is invented.

On POSIX hosts, only `~/.stormtty/zsh/` is writable state. On Windows the
host prepares `%LOCALAPPDATA%/StormTTY/managed-zsh-v1` through the native helper
(described in `docs/windows-managed-shell-state.md`), then supplies a
launch-scoped reserved `STORMTTY_NATIVE_ZSH_STATE` path. It overwrites/removes
ambient values after environment merging; the loader consumes and unsets it
immediately, including on empty HOME. The loader converts drive paths through
the verified `/c` mount contract and preserves UNC spelling. No native proof
is inherited by descendant shells. Both locations hold: bounded history (10,000 persisted,
20,000 in memory), versioned completion dump/compiled cache, and an optional
**user-created** `config.zsh`. The loader never writes a config template or reads
`~/.zshrc`, `.zprofile`, `.dircolors`, or existing OMZ configuration. It rejects
linked or group/other-writable private state directories rather than changing
permissions on existing data. Existing config/history/completion dump and compiled
dump must be regular, owned files without group/other write permissions before
any are read or sourced. The HOME ancestor itself must also be owned and not
group/other writable. Empty/unusable HOME or rejected state keeps the same real
OMZ/editing modules in an explicitly nonpersistent mode: no history, cache or
user-config access. `STORMTTY_EPHEMERAL=1` exposes this mode for qualification.
On MSYS/Cygwin, persistent state is accepted only through that native-validated
host path: synthetic POSIX uid/mode values on `noacl` mounts do not prove Windows
file security. Without the helper's path the trusted modules remain ephemeral.
Native helper and loader bridge still require Windows runtime qualification;
POSIX contract simulations and cross-compilation do not establish ACL safety. All existing HOME/CWD/temp/drive
resolution and alternative-shell profiles remain unchanged. History may contain
secrets: a leading space excludes a command; never attach state directories to
bug reports or timing artifacts. Delete private history only by explicit choice.

The owned two-line prompt uses semantic terminal colours, not RGB palette
assumptions or mandatory Nerd Font glyphs. A plain associative
`STORMTTY_CONTEXT` snapshot supplies cwd, branch/detached HEAD, exit and elapsed
time. Branch discovery reads at most 64 ancestors and bounded 4 KiB Git metadata
files, handles worktrees/submodules, and does **not** invoke Git, parse repository
config, scan dirty files or execute hooks. Paths/branches have terminal controls
removed and percent escapes doubled before prompt rendering; `PROMPT_SUBST`
remains off. OSC 133 command lifecycle is unchanged. No native footer transport
is claimed yet; that future protocol must treat metadata as untrusted data and
must never execute host commands. Dirty/ahead/behind, fzf, zoxide and a native
footer context adapter are not shipped by this first source slice.

Verification:

```sh
python tools/shell-managed-tests.py
# With verified source archives already in the normal shell cache:
python tools/shell-managed-tests.py --assets-cache /absolute/cache --benchmark
# Native Windows: use the exact freshly staged runtime executable, not system Zsh:
python tools/shell-managed-tests.py --assets-cache C:/cache --zsh C:/runtime/usr/bin/zsh.exe --benchmark
```

The benchmark labels the actual platform/execution and reports numeric process-to-exit cold/warm elapsed timings only—not first-prompt latency.
“Cold” means a fresh private state directory, **not** an OS filesystem-cache
flush. No user paths/history/config contents are emitted. Linux source tests
cannot qualify MSYS2, Windows ACL/noacl semantics, ZLE rendering/key interactions,
or physical UX. The existing native qualification in the base shell lock does
not qualify these new modules; the runtime manifest explicitly records that
addon qualification remains pending. Run native shell/path/lifecycle gates and
interactively check Tab, Up, Right-arrow suggestion acceptance, Ctrl-R, Ctrl-C,
Unicode paths, multi-tab history, invalid commands and prompt redraws before
claiming the new managed experience is qualified.

### Hook compatibility and user opt-outs

The managed `precmd` captures the original exit status into `STORMTTY_CONTEXT`
and OSC 133 D, then returns **zero**. Zsh supplies the original command status to
each hook, but a hook failure aborts later hooks. Returning the command's failure
would suppress later highlighting/third-party hooks. See the upstream
[hook contract](https://zsh.sourceforge.io/Doc/Release/Functions.html#Hook-Functions).
The focused POSIX PTY test checks `false` → OSC D=1 → later hook still sees status
1 → next prompt → Right-arrow accepts a real autosuggestion and executes it.
It is not Windows ConPTY or physical UI evidence.

In the dedicated user-created `config.zsh`, `STORMTTY_PROMPT=custom` explicitly
opts out of the owned prompt renderer. A user theme (including a future optional
P10k integration) may then set `PROMPT`/`RPROMPT`, enable `PROMPT_SUBST`, and install
its own hooks; the managed code will not replace its prompt or unset that option.
Structured context and OSC C/D lifecycle still run. Custom themes must integrate
OSC A/B prompt boundaries themselves if desired—the loader does not scrape or
rewrite their prompt strings. `STORMTTY_AUTOSUGGESTIONS=0` and
`STORMTTY_HIGHLIGHTING=0` opt out of those respective packaged editing modules.
No optional theme is downloaded, enabled or configured automatically.

## Bundled nano

The private managed Windows runtime includes pinned GNU nano 9.2-1 by default,
plus `file`, `libbz2`, and `liblzma`; no system install or global PATH edit is
needed. Existing package versions remain unchanged. Restage after updating the
lock: the new bundle identity invalidates old developer runtime receipts.

`managed_nano_probe <runtime> [packaged-provider-directory]` launches the managed
Zsh through the real Session/ConPTY path, opens a synthetic Unicode filename,
replaces its contents, saves with Ctrl-O, exits with Ctrl-X, and compares exact
UTF-8/LF bytes. CI runs both Windows providers. This is not physical UI evidence;
`nano_qualification` remains pending until native evidence is recorded. Historical
base-Zsh qualification and the closed public redistribution/source gates are not
expanded by the package addition. See `shell-notices/REVIEW.md` for provenance.

### Native nano fixture evidence

The pinned 9.2 source audit maps the title to `src/winio.c`'s `PACKAGE_STRING`
branding, write-out input to `src/files.c`'s `Write to File`, and the optional
`Wrote %zu line(s)` status to `src/files.c` (minibar can use `report_size` instead).
`src/nano.c` supplies `Save modified buffer?`, used only as a diagnostic flag.
`QNANO_READY` / `QNANO_EXIT_0` are synthetic shell handshake markers, while
`original` / `edited caf` are synthetic fixture contents, not upstream UI strings.

Saving is authoritative only when the synthetic file matches the exact expected
UTF-8/LF bytes. The probe then requires Ctrl-X to return nano status zero to Zsh
and checks those bytes again. It does not depend on an English save-status line.
Phase 41 means waiting for exact disk bytes, 42 means waiting for editor exit,
and 43 means rechecking saved bytes after exit. Success still ends at phase 5.
A previous phase-4/status-text timeout did **not** establish a successful save.

`run-nano-probe.py` retains only known numeric diagnostic/result fields, bounded
in length and count. It writes separate provider logs even on timeout/failure;
CI uploads these immediately. No arbitrary screen output, error strings, paths,
file contents, environment, or configuration enters those artifacts.
