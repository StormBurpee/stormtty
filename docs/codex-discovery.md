# Codex installation discovery

Discovery only reads a bounded set of installation paths. It never executes a
version probe, npm, Node, a shell shim, an installer, or Codex. It never reads
`auth.json`, credentials, tokens, or a workspace's contents. The host may use the
returned executable for an app-server status connection; that is separate from
an explicitly approved model task.

Supported defaults:

- Windows OS-known Profile/LocalAppData/RoamingAppData/ProgramFiles folders:
  standalone `Programs/OpenAI/Codex/bin`, `.codex/packages/standalone/current`,
  and npm global `npm/node_modules` / `nodejs/node_modules`.
- macOS/Linux OS account home: `.local/bin`, standalone current selection, and
  `.npm-global/lib/node_modules`; system `/opt/homebrew` and `/usr/local` installs.
- npm platform packages and fallback bundled vendor packages, for x64/arm64.
  The native image is selected directly; `.cmd`/`.ps1`/JavaScript shims never run.

Both lexical and canonical paths must be local absolute paths inside known
installation roots, with no repository (`.git`) ancestor. Canonical symlink
escapes are rejected. Native-image magic and executable permission (POSIX) are
checked. This is **not a cryptographic publisher/signature verification**; an
attacker able to replace the user's installed native binary remains outside this
check's protection. The returned status working directory is the verified binary
parent, not the active workspace.

No PATH or current-directory search is performed. Custom npm prefixes, nvm,
pnpm, Bun, relocated `CODEX_HOME`, and unusual Homebrew locations are not inferred.
Use Advanced to explicitly choose a native executable for unsupported layouts.
Discovery never installs or upgrades anything. Installation help should link to
[official instructions](https://developers.openai.com/codex/cli).

Layout evidence (read, not executed):

- [Codex v0.165 npm loader](https://github.com/openai/codex/blob/rust-v0.165.0/codex-cli/bin/codex.js)
  uses platform package `vendor/<target>/bin/codex[.exe]`, with bundled fallback.
- [Codex v0.100 npm loader](https://github.com/openai/codex/blob/rust-v0.100.0/codex-cli/bin/codex.js)
  uses the previous `vendor/<target>/codex/codex[.exe]` layout.
- [Official Windows installer](https://github.com/openai/codex/blob/main/scripts/install/install.ps1)
  and [POSIX installer](https://github.com/openai/codex/blob/main/scripts/install/install.sh)
  define the standalone/default visible-bin locations.

Portable fake-filesystem tests cover missing CLI, npm native resolution, old npm
layout, standalone symlinks, repository rejection, relative/UNC paths, escaping
symlinks, text/shim rejection and architecture selection. No real Codex process,
authentication flow, browser or model is used by these tests. Windows/macOS
runtime installation discovery still needs native qualification.

Global npm optional dependencies may be nested under
`@openai/codex/node_modules/@openai/codex-<platform>-<arch>` rather than hoisted
next to `@openai/codex`. Discovery checks that exact bounded nested location first,
matching the official wrapper's package-relative `createRequire(...).resolve()`
lookup; both locations retain the same native-image, canonical-root and repository
exclusion checks. No JavaScript or npm command is executed to resolve them.
