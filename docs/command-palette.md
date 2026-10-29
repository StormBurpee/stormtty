# Owned command palette and background navigation

Open **Ctrl+Shift+P**. Search accepts ordered, non-contiguous letters (`nwtb`
→ New tab, `splr` → Split right). Word starts and consecutive letters rank
higher; equal scores retain registry/session order. Empty search shows actions
without injecting suggestions or history. Action rows are single-line; secondary
text is reserved for actionable disabled reasons and actual session context.
The search field identifies the palette without a redundant heading.

- Up/Down moves through results; Home/End and PageUp/PageDown navigate longer lists.
- Enter runs the selected action; Escape dismisses without terminal input.
- Alt+Enter still opens shortcut editing for application commands.
- **Restore background terminal…** opens a session-only picker. Direct restore
  results also appear in ordinary search, with the workspace and terminal title.
  Raw known-shell executable titles are shown as Zsh, PowerShell or Command
  Prompt. Explicit user tab names and custom program titles are retained; real
  observed CWD is shown when available, never inferred or invented. User tab
  naming is preserved separately from the process title across backgrounding.
- Restoration validates live workspace/session IDs and attaches the same actor
  in a new tab. It does not start a new shell. An empty foreground workspace can
  still restore its background terminals.
- Choices are scoped to the active workspace. Use Next/Previous workspace first
  to navigate another project's sessions. No default terminal shortcuts were
  taken over; the restore command is available for user-defined bindings.

Search decodes UTF-8 codepoints (not byte fragments), folds ASCII case, and
matches other scripts exactly. This is not locale-aware case folding. Queries
are bounded to 64 codepoints/256 bytes, candidate fields to 512 codepoints;
malformed UTF-8 fails matching. Punctuation is literal, never a regex.

## Verification and native evidence

Portable `core_commands_tests` covers ranking, abbreviations, stable empty
ordering, literal punctuation, malformed UTF-8, Unicode and resource bounds;
`background.restore` availability is tested with no foreground panes.

The existing Windows `--smoke-projects --capture <path>.png` flow now captures
real renderer frames named `<path>-palette-commands.png`,
`<path>-palette-fuzzy.png`, `<path>-palette-no-results.png` and
`<path>-palette-background.png`. Its palette stage sends Enter through native
key handling, checks that restore retains the exact session actor, rejects a
stale entry, and reparks that actor before continuing existing lifecycle checks.
This requires a real Windows runtime; a cross-build is not runtime evidence.

Physical review remains separate: narrow/wide windows and DPI, typography and
clipping, IME candidate placement, repeated open/Escape, Alt+Enter shortcut
editing, and restore of a running/exited background terminal. No browser or
mock screenshot substitutes for this review.

## Craft pass: action, then context

The native palette uses the same line-drawn folder, pane and terminal language
as workspace navigation. A restrained accent wash marks the selected action;
its description lives in one shared detail shelf, rather than repeating a
second line beneath every result. A position indicator makes long keyboard
navigation visible. Shortcut and query-caret widths use native DirectWrite
measurement, not byte-count estimates. IME placement follows that measured
caret. No terminal output is animated and the existing reduced-motion policy
continues to disable the palette reveal.

`layout_command_palette` is the shared painting/hit-testing/page geometry.
Portable tests cover empty results, mixed-height rows, first/last/stale indexes,
tiny through wide windows, and compact rename mode. Native `--smoke-projects`
also exercises End/Home, Escape/reopen without terminal input, and captures
`-palette-end.png` and a real resized `-palette-narrow.png` alongside the
existing states. `.github/workflows/native-craft.yml` publishes those actual
frames early on `feature/native-craft` or manual dispatch. This is not a claim
of physical keyboard, high-DPI or IME qualification.

Design reference: [Anthropic frontend-design skill](https://github.com/anthropics/skills/blob/main/skills/frontend-design/SKILL.md),
reviewed 29 October 2026: make the hierarchy specific to the tool, use motion
in response to action, and remove decoration that does not help navigation.
StormTTY keeps its own Graphite tokens (`#11151B`, `#181D24`, `#EEEDEC`,
`#8C9EFF`), native chrome typography and quiet workspace structure. This pass
deliberately avoids a splash screen, staged startup delays, arbitrary palette
changes, and any expansion of the paused AI interface.
