# Windows settings services

`settings_services.hpp` is portable; its I/O implementation is Windows-only.
All operations are **synchronous background-thread work**, never work for a
WndProc/render/input callback. The host owns workers, cancellation/generation
checks, window lifetime and publishing owned results onto its UI thread.

## Discovery and launch policy

`discover_windows_shells()` returns at most four candidate `Profile` values. It
does not alter configuration, launch executables, enumerate the disk or PATH,
download/install software, change global environment, or inspect WSL distros.
Candidates are verified ordinary files at:

- Command Prompt and Windows PowerShell: `GetSystemDirectoryW()` and its
  `WindowsPowerShell/v1.0` child.
- Stable PowerShell 7: fixed x64/x86 MSI `InstalledVersions` registration keys
  in both registry views, then known Program Files `PowerShell/7/pwsh.exe`.
- Git Bash: HKCU/HKLM `Software/GitForWindows/InstallPath`, both registry views,
  then known Program Files `Git` and Local AppData `Programs/Git` locations.

Missing candidates are simply not offered. Portable/custom paths and unrecognized
Store/MSIX installations remain user-managed profiles; this bounded probe is not
a claim to enumerate every possible installation. Existing/custom profiles win
over discovery IDs. Selecting a candidate adds it explicitly; detection alone
never changes the default or saves a file.

Git Bash uses **`bin/bash.exe`**, never `git-bash.exe` or mintty, with separate
`--login` and `-i` arguments. `CHERE_INVOKING=1` preserves the host-selected cwd
through Git's login initialization; user shell scripts can still change cwd.
The profile's blank `working_directory` lets the host inherit the chosen project
directory. No shell command string or injected `cd` command is constructed.
Profile/default changes apply only to **new** terminal sessions; existing PTYs
are not restarted. Normal login profiles execute only when the user opens a
terminal, never during discovery. StormTTY does not modify shell startup files.

Primary sources checked October 28, 2026:
[Git installer registration](https://github.com/git-for-windows/build-extra/blob/main/installer/install.iss),
[Git's cwd-preserving environment](https://github.com/git-for-windows/build-extra/blob/main/please.sh),
[GitHub CLI's bash-versus-mintty guidance](https://cli.github.com/manual/gh_help_mintty),
[Bash invocation flags](https://www.gnu.org/software/bash/manual/html_node/Invoking-Bash.html),
[PowerShell MSI registration](https://github.com/MicrosoftDocs/PowerShell-Docs/issues/6589),
[PowerShell installer tests](https://github.com/PowerShell/PowerShell/blob/master/test/packaging/windows/msi.tests.ps1).

## Explicit Save contract

1. Load the editing baseline using `load_settings_snapshot(path)`. Retain its
   exact owned bytes, identity, timestamp, and matching parsed Config together.
   Missing files return a default Config and an absent-file token without writes.
   Invalid files fail visibly rather than falling back to a writable default.
2. Preview/Cancel are memory-only. Preserve the original host Config for Cancel.
3. On explicit Save, call `save_settings_config(path, draft, token)` on a worker.
   Do **not** silently refresh the token before saving stale edits. If the live
   Config came from an earlier startup read, ensure that baseline still matches
   the loaded file before opening the editor.
4. Apply the whole draft and retain the new token **only on successful Save**.
   Failure retains the active Config and shows the error; a conflict requires an
   explicit reload/reopen to review disk changes. Never partially apply shortcuts
   or profile selection on a failed save.

Persistence requires an absolute ordinary local drive path supplied by the host.
Resolve relative CLI paths once before loading. UNC/network drives, device paths,
alternate data streams, traversal/ambiguous DOS names, hard-linked config files
and reparse points in the file or its ancestor directories are refused. Saving
an EFS-encrypted file is refused rather than silently removing encryption. This is
deliberately conservative; redirected/junction-based config locations need an
explicit future design rather than silently following links.

The existing strict parser/validator bounds encoded UTF-8 data to 1 MiB. Parent
directories are created only for explicit Save and held open without delete
sharing to prevent ordinary ancestor substitution. The original file is opened
without write sharing, compared byte-for-byte plus identity/timestamp, and checked
again immediately before commit. A unique `CREATE_NEW` same-directory file is
written, its existing target DACL preserved, flushed, then atomically renamed by
owned handle. Errors clean up the staged file by owned handle, not by an untrusted
pathname. No fallible work occurs after the successful rename. Missing-file saves
never replace a file which appeared since the snapshot.

This is **optimistic concurrency**, not an OS compare-and-swap transaction:
Windows's rename API cannot condition replacement on the destination file ID.
A noncooperating process can rename a new destination in the tiny interval after
the final check. Hosts serialize their own saves; ordinary external edits before
Save are rejected, but arbitrary hostile simultaneous namespace mutation is not
claimed safe. There is no unsafe truncate-in-place or delete-and-retry fallback.
An interrupted process/machine can leave a uniquely named staged file; normal
error paths clean it up, and discovery does not scan/delete unrelated files.
The host must not claim native runtime/physical UI verification from cross builds.

The commit uses [rename-by-handle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle)
rather than `ReplaceFileW`, whose [documented failure states](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)
include partial rename states. Filesystems that refuse the atomic rename fail
without applying a draft.

## Tests

`settings_services_tests` is a **Windows-native** test. It covers missing/default
load, Unicode filenames, successful repeated saves and token stability, invalid
draft retention, wrong-path tokens, external edits/deletion/replacement, read-only
and sharing failures, hard links, optional privilege-dependent symlink fixtures,
malformed/oversized input, unsafe path forms, temp cleanup, and bounded discovery.
Git Bash launch/ConPTY/cwd behavior still requires a Windows runtime with Git for
Windows installed; this test intentionally never executes discovered shells.
