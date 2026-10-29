# Native Codex panel qualification

The Windows panel is native `WindowScene` chrome, not a webview or an ANSI/TUI
capture. Ctrl+Shift+J opens/reopens it; Escape returns terminal focus. Drafts are
in-memory and keyed by the durable workspace identity. Closing the view does not
stop its provider. Neither workspace restore nor reopening replays a prompt.

Normal launch has no provider configured and shows **Unavailable**. Optional
`--codex-executable <absolute path> --codex-account-reference chatgpt:<email>`
and `--codex-runtime-version <display metadata>` configure an inert adapter. The reference must match the already signed-in ChatGPT account returned by the
provider; it is not a token. Do
not pass credentials in these arguments. No executable detection, version probe,
login, sandbox installation, network listener or automatic child launch occurs.
For a normal CLion launch, put these options in the local StormTTY Run/Debug
configuration's program arguments (not CMake options or committed IDE files).
The executable path must point to an already installed native Codex executable;
StormTTY does not install it. Without arguments the panel explains the two required
flags and remains disabled. No setup task is created by opening the panel.

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
`agent-agent-1.png` through `agent-agent-4.png` show unavailable, running fixture,
approval fixture and error fixture. Simulated states visibly say **UI fixture**.
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
