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

## Parked Windows footer attention

The host keeps one `ParkedAttention` value per parked SessionId, outside project
placement persistence. Successful parking baselines existing completions; a newer
completion sets a subtle persistent footer dot and emits one “Command finished”
notice (with an explicit status when supplied). Repeated wakes cannot replay it,
and C-D-C bursts remain observable even when the latest phase is running. The
footer tooltip/menu includes “command finished” while ordinary process state
remains visible. Restoring successfully acknowledges it; failed restore and
canceled close preserve it. Retirement removes the entry, and reparking starts a
fresh baseline without replaying old work.

Overflow chips also carry a dot when a hidden parked entry has unread completion.
Within one host update, the single notice slot prioritizes failure over process
exit over command completion, independent of session iteration order.

The same bounded policy chooses process-exit notices. Simultaneous completion and
exit produce one notice. A later exit matching the already-notified completed
command/result is suppressed; a failure, different process result, or exit of a
new running/interrupted command is not suppressed. This is a conservative display
heuristic, not proof that an OSC marker identifies the OS process that exited.

Portable policy and placement tests cover these decisions. The Windows project
smoke additionally emits C/D inside a still-live parked shell, checks attention
survives failed restore/canceled close, and checks successful restore clears it.
The Cmd fixture uses Windows PowerShell to construct control bytes inside the
child; managed Zsh uses builtin printf. Cross-compiling that smoke is not evidence
that it ran, nor visual verification of the footer dot.

With a capture path, project smoke also saves a sibling `-command-finished.png`
after presenting the actual pending-completion footer in its own workspace,
before restore or shell exit. This is separate from the final restored capture.
