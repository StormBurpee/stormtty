# Native Codex panel qualification

The Windows panel is native `WindowScene` chrome, not a webview or an ANSI/TUI
capture. Ctrl+Shift+J opens/reopens it; Escape returns terminal focus. Drafts are
in-memory and keyed by the durable workspace identity. Closing the view does not
stop its provider. Neither workspace restore nor reopening replays a prompt.

Normal launch has no provider configured and shows **Unavailable**. Choose
**Setup** in the panel to enter an already installed absolute local `.exe` path
and `chatgpt:<email>` account reference. Save is inert: it never inspects the
executable, probes a version, installs software, signs in or launches a provider.
No setup task is created. Input validation is syntactic only.

These non-secret values are saved through the guarded atomic settings service to
the OS-known LocalAppData folder (`SHGetKnownFolderPath`, not the ambient
`LOCALAPPDATA` environment variable), under `StormTTY\agent.local.stormtty`,
separately from config and portable
workspace files. It never stores credentials, tokens, prompts or approvals.
Cancel discards edits; reopening uses the saved values. **Reload saved** explicitly
refreshes the disk baseline after external changes or a repaired corrupt file;
unsaved edits require **Discard & reload** confirmation, and Cancel keeps them.
Reload is allowed only while providers are disconnected and never launches one. Clear both fields and
Save to remove configuration. Connected providers must be explicitly disconnected
before changing Setup. Changed executable/account discards old conversation
associations and advances a settings-generation guard; unchanged Save preserves
associations. Metadata restores for another account are ignored, never connected.

Optional `--codex-executable <absolute path> --codex-account-reference chatgpt:<email>`
and `--codex-runtime-version <display metadata>` remain inert overrides. When used,
Setup explains why editing is locked; remove them to use app-owned settings. For
CLion these are local Run/Debug program arguments, not committed IDE settings.
Default CLion launches now need no flags to configure the provider in the app.
The account reference must match the already signed-in ChatGPT account; it is not
a token. Runtime metadata defaults truthfully to `not-probed`; it satisfies the service
identity field without executing a version command. No executable detection,
login, sandbox installation, network listener or
automatic child launch occurs.

The native **Review connect → Connect** action separately authorizes launching
that executable's app-server in the displayed workspace. It does not start a task.
**Review start → Start task** shows workspace/provider/execution scope and is the
only path that creates a turn. Connection failures never retry a task. The core
service revalidates generation, complete approval key and execution policy.

## Authored native fixture capture (no Codex use)

Run a native Windows build in an isolated smoke directory:

```
stormtty.exe --smoke-agent --capture C:\evidence\agent.png
```

Do not supply Codex options: the parser rejects them in smoke mode. This launches
only the existing isolated terminal smoke host, never a Codex executable. Captures
`agent-agent-1.png` through `agent-agent-5.png` show unavailable, running fixture,
approval fixture, error fixture and authored Setup. The Setup fixture never saves
a real machine setting. Simulated states visibly say **UI fixture**.
The smoke also checks terminal focus isolation, Escape, retained draft and no
replayed action on reopen. Fixture screenshots are **not** live-provider proof.

Manual native follow-through: type Unicode (including surrogate pairs), paste a
multiline draft, Shift+Enter, Tab/Shift+Tab through controls, cancel confirmation,
close/reopen, resize and change DPI. Confirm no input reaches the PTY while the
panel owns focus. Inspect long transcript/scope scrolling with PageUp/PageDown.
Check an approval arriving in an inactive workspace exposes the ribbon attention
state and opens that workspace. Repeat under high contrast/reduced motion and
with a screen reader; these accessibility checks have not been claimed by unit
or cross-compilation evidence. IME composition is currently blocked by the host's
modal input guard, not qualified as supported composer input.

## Evidence levels

- `agent_panel_tests`: authored snapshot, consent, unavailable, stale approval,
  scope/generation change, workspace drafts, pointer cancellation and scene tests.
- `agent_provider_settings_tests`: strict machine-local format, syntactic paths/account
  validation, round trips and rejection of token/unknown fields.
- Windows `settings_services_tests`: isolated atomic save/load/stale-save fixtures,
  no provider execution. Must run on Windows for runtime evidence.
- `agent_panel_controller_tests`: inert/no-provider configuration and rejection of
  task actions without a configured connection; **never launches a process**.
- Windows GNU cross-build: compiler/linker gate, not Windows runtime evidence.
- Native `--smoke-agent`: actual native drawing/input routing with authored data;
  must run and inspect captures before claiming visual runtime verification.
- Live Codex authentication/task test: separate authorization required. Not run.

macOS can consume the same panel/controller but does not yet have native host
wiring. Runtime credentials and machine-specific executable paths are not saved
into portable workspace files. Conversation metadata persistence remains owned by
the provider/store seam, not the panel.

Approval presentation escapes Unicode direction/invisible formatting controls and
C0/C1 controls as visible `[U+202E]`-style markers. Invalid UTF-8 bytes are shown as
`[byte:FF]`. Escaping happens before wrapping; Unicode scalars and escape markers
are never cut between lines or at display truncation. These are presentation-only
transformations: permission decisions retain the exact original scope and keyed
request identity. Ordinary Unicode text is preserved.

Windows transcript/approval wrapping now uses the actual DirectWrite chrome font
and line metrics through `Renderer::wrap_chrome_text`. It receives only escaped
presentation text, never provider objects, and its errors fall back to portable
word-boundary/glyph-width layout. The portable fallback is the default on other
hosts; there is no new macOS renderer symbol/link dependency. Scalar and marker
integrity checks reject unsafe native line breaks. One cache is bounded to 2 MiB
source / 8 MiB displayed bytes / 262,144 lines; oversized authored snapshots are
rendered without retention rather than silently truncating approval evidence.
Native renderer-smoke tests cover full-width fixture scope, actual measured line
width and Unicode reconstruction; these require Windows runtime to execute.
Buttons now use centered labels and Settings-derived semantic fills, borders,
focused/hovered/pressed states and disabled ink. Native visual inspection is still
required before calling the panel polished.
