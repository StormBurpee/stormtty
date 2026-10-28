# StormTTY Zsh runtime staging (Windows x64 qualification)

No installer, package scripts, global PATH changes or user dotfile edits. This stages the 17 exact official MSYS2 archives in `shell-bootstrap.lock.json`, plus StormTTY's original session-only preset. It does **not** fork Zsh, enable a default profile, or establish a Windows runtime pass.

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
- Preset reads no user dotfiles and uses only the bundled function directories. Original directory/exit/duration prompt, ZLE editing and completion; no Git/network subprocess in the prompt. No history/cache writes yet (`HISTFILE` unset, `SAVEHIST=0`, `compinit -i -D`). It unexports its bootstrap variables.
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
