# Observed foreground command lifecycle

`TerminalCore::command_snapshot()` and `SessionInfo::command` expose an owned,
constant-size snapshot of OSC 133 metadata. It is an untrusted display hint, not
process status, service readiness, an execution authority, or command history.
No command text is collected. Unsupported shells remain `unavailable`.

- C starts a generation; repeated C while running is ignored.
- D ends only a running generation and records its optional explicit exit status.
  Startup/stale D and duplicate D are ignored. A/B never imply completion.
- RIS, EOF and actor failure interrupt running commands without inventing an exit
  code or completion. Explicitly completed commands remain completed at EOF.
- Generations belong to one terminal/session, not a global identity. A consumer
  must retain that identity alongside its acknowledged `completed_generation`.
  A C-D-C burst may publish `running` with a newer completion generation; compare
  completion generations rather than relying only on phase changes. Intermediate
  completions may coalesce: this is not a lossless notification/event stream.
- `last_exit_code` belongs to the last explicit completion, not necessarily the
  currently running or interrupted command. A D without a status clears it.

The opt-in managed Zsh preset emits scalar C/D from preexec/precmd and A/B around
its prompt. Prompt return ends the foreground command; it does **not** mean that
shell background jobs have finished. A foreground server normally remains running
until it returns to the prompt, regardless of any “ready” text it prints. Other
programs can spoof markers; applications must never act on them as trusted input.

## Verification scope

Linux terminal tests exercise startup markers, both OSC terminators across every
split point, duplicate markers, optional/nonzero/zero status, C-D-C coalescing,
per-terminal identity, immutable values, RIS/stale D and EOF/late-marker rejection.
Real POSIX child/session tests distinguish command completion from process exit,
then exercise EOF and an injected actor callback failure with transport reaping.
These are core/POSIX evidence, not Windows ConPTY, bundled MSYS2 Zsh, macOS runtime,
UI/footer rendering, or notification delivery verification.
