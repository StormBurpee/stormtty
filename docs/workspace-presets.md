# Saved workspaces

The Windows host saves a bounded `workspaces.stormtty` library beside its configuration file. It restores the last open workspace layout on launch by default. This is layout persistence, not process resurrection.

Open **Workspace settings…** from the command palette or app menu, or right-click the workspace rail. Each workspace has a name, a built-in vector icon, an optional `#RRGGBB` accent, a launch directory and a preferred configured shell. The preferred-shell value `@default` means the app default; it is distinct from a configured shell whose ID is `default`.

The same menu saves all current workspaces as a named set, adds saved sets as additional workspaces, deletes saved sets, and toggles restore on launch. Opening a preset never closes existing terminals or background jobs. New copies get new durable workspace identities. Changing a shell affects new panes and currently unbound restored panes, not running sessions.

## What survives

- Workspace order, active workspace and durable opaque identity.
- Names, icons, accents, configured project launch directories and preferred shell IDs.
- Tab titles, tab/pane ordering, split ratios, focus and zoom.
- Up to 32 explicitly named presets.

The application does **not** save shell commands, output, environment values, credentials, process handles or session IDs. Process-derived pane titles are cleared. Parked live jobs are not restarted. Shell-observed OSC directory strings are never promoted into executable launch intent. A project launch directory is not a claim to preserve each shell's most recent `cd`.

Only the selected workspace starts fresh shell processes on restore. Hidden workspaces start when selected. Missing shell profiles leave panes unbound; missing directories produce a failed fresh session. Both preserve the layout and report an error rather than silently substituting another folder or shell. Directory existence is checked by background process startup, not a blocking UI filesystem probe. Set the launch folder/shell, then choose **Start restored panes**. Automatic restore is limited to 64 panes per selected project. Larger layouts remain available without auto-launching their processes.

## Storage and compatibility

The library's `STORMTTY-WORKSPACES 1` envelope embeds the established `STORMTTY-WORKSPACE 1` layout records unchanged. The library is limited to 1 MiB; project, pane, text, UTF-8 and nesting limits are validated before application. Unknown versions and malformed records fail closed. Durable workspace IDs are 32 lowercase hex characters; they are identifiers, not authorization tokens. Runtime project IDs remain process-local.

Windows reads and writes use the same guarded local-file service as configuration: no reparse-point traversal, same-directory staged writes, flush before atomic rename, exact optimistic-concurrency tokens and existing ACL preservation. Corrupt or unreadable libraries disable automatic writes for that run. Concurrent edits fail instead of clobbering another instance. Filesystem work runs outside window callbacks; changes are coalesced and saved in a worker, with a final flush after the window loop exits.

The codec and model are portable. Disk persistence and restore/editor host wiring in this change are Windows-only; macOS host integration is not claimed. Cross-compilation does not constitute native Windows interaction or visual verification.

### Isolated native restart qualification

On Windows, `ctest -R native_workspace_persistence_smoke --output-on-failure`
(or `python tools/workspace-native-smoke.py --app <absolute stormtty.exe>`) creates
one unique temporary fixture and launches the actual app three times. The first
launch saves two customized workspaces, a split layout, active selection and a
named preset through the production shutdown writer. The second launches from
that file, compares the complete restored snapshot, checks hidden panes stay
unlaunched, and opens the preset additively while checking existing actor identity.
The third loads deliberately corrupt bytes and verifies shutdown leaves them
unchanged. Fixture files are removed after the run.

This dedicated `--smoke-workspaces` mode refuses absent/unmarked fixture paths;
ordinary smoke modes still bypass workspace persistence. The test uses real
HWND/WARP/ConPTY sessions and isolated Cmd, not the user's shell/profile scripts.
Passing cross-compilation or portable codec tests does not qualify this native
restart flow, physical UI, or macOS behavior.

Pass `--artifacts <directory>` to the Python runner to retain four actual WARP
framebuffer PNGs: `workspace-seed.png`, `workspace-seed-menu.png`,
`workspace-restore.png`, and `workspace-restore-menu.png`. CTest uses
`build/artifacts`. Captures wait for a later settled presented frame; they show
synthetic fixture data and the owned customization menu, not desktop blur,
physical input, or macOS visual qualification.
