# Windows settings services

`settings_services.hpp` is portable; its I/O implementation is Windows-only.
All operations are **synchronous background-thread work**, never work for a
WndProc/render/input callback. The host owns workers, cancellation/generation
checks, window lifetime and publishing owned results onto its UI thread.

## Module ownership

`windows_shell_discovery.cpp` owns bounded native shell probes and candidate
construction. Its private `shell_discovery_policy.hpp` characterizes first-valid
selection, encoded-path bounds, launch arguments and configured Cmd equivalence
without querying or changing the test host. The resolver seam is only the native
candidate-to-UTF-8 probe, not a general discovery framework.

`windows_local_path.cpp` is the single shared lexical/local-drive path policy.
Discovery checks ordinary executable attributes; it does not acquire the stronger
ancestor/target guards used for persistence. `settings_services.cpp` still owns
all guarded snapshots, the single commit mutex, DACL handling, legacy backup
verification and atomic replacement. Document-specific codec adapters remain there
for a later boundary; existing Settings and workspace workers remain unchanged.

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
2. Open the editor from that freshly parsed Config even when it differs from
   the running Config. Keep the running value separately as the Cancel baseline;
   the fresh disk value is the initial draft/dirty baseline. Missing files retain
   the running defaults. Repeated open requests never replace an open draft.
3. Preview/Cancel are memory-only and affect only theme, font and appearance.
   Opening changed disk content previews those presentation fields and displays
   “Loaded changes from disk.” Shell/profile and shortcut changes remain inactive
   until Save. Cancel restores the prior running presentation without writing.
4. On explicit Save, call `save_settings_config(path, draft, token)` on a worker.
   Do **not** silently refresh the token before saving stale edits. A disk change
   during editing still fails the exact bytes/identity/timestamp check.
5. Apply the whole draft and retain the new token **only on successful Save**.
   Failure preserves the open draft and shows the error; Cancel then reopen reads
   the new disk value/token without a restart. Never partially apply shortcuts or
   profile selection on a failed save. Invalid disk files fail explicitly, and
   repairing the file permits the next open attempt without cached failure state.

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

`shell_discovery_policy_tests` is portable characterization of selection order and
stopping, empty/oversized/UTF-8 executable values, arguments/environment/cwd, and
exact Cmd alias equivalence without mutating configured profiles. It does not
qualify Windows registry reads, UTF-16 conversion, filesystem attributes or launch.

`settings_services_tests` is a **Windows-native** test. It covers missing/default
load, Unicode filenames, successful repeated saves and token stability, invalid
draft retention, wrong-path tokens, external edits/deletion/replacement, read-only
and sharing failures, hard links, optional privilege-dependent symlink fixtures,
malformed/oversized input, unsafe path forms, temp cleanup, and bounded discovery.
It also exercises actual external file edits through the shared editor: open and
Cancel, explicit adoption on Save, changed-during-edit conflict with draft
retention, Cancel/reopen/retry, and invalid-file repair. Portable panel tests cover
the two independent baselines and fresh file reads; they are not native I/O proof.
Git Bash launch/ConPTY/cwd behavior still requires a Windows runtime with Git for
Windows installed; this test intentionally never executes discovered shells.

## Reload settings (Windows)

`Reload settings` is available in the command palette and workspace menu. It
reads the same global `config.toml` selected at startup (including `--config`),
on the serialized settings worker, then validates the complete candidate before
applying it. This is an explicit action, not a file watcher. It does not reload
workspace layouts, discover/install shells, save files or execute commands.

Theme/ANSI palette, font, appearance/material, Ribbon and shortcuts update live.
Profiles, default shell, scrollback capacity and console backend affect future
sessions; existing actor identities and retained terminal output stay intact.
Command-line runtime overrides continue to win. All workspaces share these global
settings even when Reload is chosen from a particular workspace's menu.

Missing, malformed, inaccessible and oversized files leave the working settings
unchanged and report an app-rendered notice. A missing file is deliberately not
interpreted as a request to reset to defaults. Repeated activations while a read
or save is pending are refused, and generation checks discard stale/duplicate
completion results. Reload while Settings is open is refused: explicitly Save
or Cancel the editor first, so an unsaved draft is never discarded. Opening the
editor is also blocked during reload, and its next open takes a fresh exact disk
snapshot for optimistic-concurrency Save.

Portable tests cover admission, missing/invalid data and stale completions. The
Windows Settings CI smoke additionally edits its isolated saved file, dispatches
Reload, repeats the action while pending, checks live actor/output preservation,
and checks malformed/missing-file rejection. Cross-compilation is not native
runtime or physical UI evidence.


## Shell-discovery extraction qualification

[PR #47 run 41143384322](https://github.com/StormBurpee/stormtty/actions/runs/41143384322)
checked out synthetic merge `d6516cb6f0384829294ee41ff6d0472d360e5477`.
Windows and macOS logs confirm the checkout; its tree
`8ccad1880443bcce0ffc5dcfd0d701f9a2d5020f` matches reviewed source
`7fb1567f94f213c392716423b9a984aafe65db31`.

Windows baseline passed **72/72**, including 42 discovery-policy checks,
290 native Settings-service checks, 232 native application-layout-service
checks, and the Session suite. macOS passed **62/62** and Linux CI passed.
These results qualify the exercised native services, not launching every
discovered shell or physical UI/performance.

The full Windows job failed. Application cases passed **14/15**: only bundled
`smoke-projects` failed, with malformed `[200~` paste, trailing `~` and
`zsh: = not found` at a stage-0 deadline. Repeated Settings passed **9/10
explicit + 3/3 default**; explicit case **4** failed with the same observed
paste signature at a stage-8 deadline. Installed-console Settings passed.
All 29 compact receipts reported reaped processes, no outer timeout, no cleanup
error and no truncation: 27 successes and two failures. Failure classification
comes from the full log, not the compact receipts' null signature fields.
No shell fix, common root cause or fully green Windows result is claimed.
