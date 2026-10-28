# StormTTY Zsh runtime staging (Windows x64 qualification)

No installer, package scripts, global PATH changes or user dotfile edits. This stages the 17 exact official MSYS2 archives in `shell-bootstrap.lock.json`, plus StormTTY's original session-only preset. It does **not** fork Zsh, enable a default profile, or establish a Windows runtime pass.

## Windows CI

Use official Python **3.14+** with its `compression.zstd` standard-library module (no pip dependencies):

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

CMake target/workflow wiring is root-owned. Layout is exactly `root/usr/bin/zsh.exe` and `root/stormtty/zsh/.zshrc`. The destination must not exist; the script will never update an installed/live runtime in place. For local Python 3.10–3.13, pass an existing trusted executable explicitly: `--zstd /absolute/path/to/zstd`. Python 3.14 standard-library Zstandard support is documented in [tarfile](https://docs.python.org/3.14/library/tarfile.html).

`--offline` requires every selected archive already in the cache. `--sources` also downloads and verifies all 17 complete source archives into the cache (~306 MB); it does not execute/extract them or mean a public source offer exists. Binary package downloads total 15,190,478 bytes. Never use a fresh rolling-repository solve in a release build.

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

## Publication gates — not optional

`stormtty-shell-manifest.json` deliberately reports:

- `qualification_only: true`
- `default_activation_allowed: false`
- `release_notices_complete: false`

The staged `notices/` packet retains the exact package/source inventory, original package notices and explicit pending obligations. It is **not** yet a completed redistribution notice audit. Before default/public packaging: pass native Windows and UI/compatibility tests; supply missing common/component license texts; retain per-file exceptions (Zsh `_darcs` is GPL-2.0-or-later; runtime DLL and utilities differ); and publish complete corresponding source/recipes/patches for that exact binary release with clear accessible links. GPLv3 section 6(d) is the planned distribution route, not merely a link to upstream. No Ghostty GPL shell integration was copied. Do not flip these booleans as a substitute for the evidence.
