# Footer ribbon

The native Windows footer uses a portable, closed data-binding registry in
`ribbon.hpp`. Settings → Ribbon previews visibility and left-to-right order;
Save changes persists through the existing atomic settings service, and Cancel
restores the running configuration. Toggle and move buttons are keyboard
reachable. Appearance → Clock and Ribbon → Clock share `show_clock` for old
config compatibility. A hidden clock binding also suppresses the clock.

`[ribbon] order` is exactly the six registered IDs, with no duplicates. `hidden`
is an optional subset. Unknown keys, arbitrary bindings and malformed updates
fail full config validation; the existing configuration remains active. This is
a declarative foundation, not an extension runtime or package installer. It
accepts no shell text, callbacks, scripts, remote assets, paths or custom icons.
Host data stays host-owned and workspace-qualified. Agent availability/attention
enters geometry through `RibbonHostData`; it never starts or restores a task.

Width admission uses host-defined priorities: agent, background, session status,
directory, Git, clock. The background binding is presented as **Activity &
background terminals** on Windows. After admission, user order determines
placement. Hidden and omitted items have empty rectangles shared by drawing, tooltips and hit tests.
Agent attention and active session errors override hiding. Background restore remains reachable through
Commands (`Ctrl+Shift+P` → Workspace activity… or Restore background terminal…),
even when its ribbon item is hidden or omitted. Agent approval sheets remain owned by the agent host,
not by customization. macOS can reuse registry/config/geometry; native macOS
footer wiring is not claimed by this change.

## Verification and native capture plan

Portable tests cover strict parsing, persistence roundtrip, settings keyboard
controls and cancellation, essential attention, hidden hitboxes and overlap/bounds
for widths 0–1499. Linux tests are not physical native verification.

On Windows, capture these states at 100% and 150% DPI:

1. Settings → Ribbon, toggle Git off and move Directory below Activity &
   background terminals; observe immediate footer preview. Cancel and confirm exact original order/visibility.
2. Repeat and Save, close/reopen the app, and confirm persistence.
3. Park two quiet terminals, hide Activity & background terminals, then restore
   one using the command palette. Click the old target location and confirm no
   hidden restore target remains. Unread attention deliberately overrides hiding.
4. Resize through 1200, 640, 400 and minimum width; inspect clipping and hit targets.
5. Hide Clock with the old appearance control; enable it through Ribbon and check
   both controls agree and the displayed minute updates, including narrow widths.
6. With a clearly labelled fake agent fixture, show attention while Agent is hidden;
   verify visible attention, keyboard access, and no provider execution on restore.
7. Exercise Tab/Shift+Tab, Enter/Space, arrow navigation, reduced motion and high
   contrast; inspect screenshots for readable labels and focus outlines.

No live Codex process, authentication or task is needed or authorized by this test.

## Current Windows activity summary

The Windows host supplies `WorkspaceActivitySummary` from the active workspace's
watched-or-parked union, deduplicated by retained actor. It uses the existing
`workspace.background` preference and compact target, not a new binding or band.
Clicking opens the Navigator scoped to the opening workspace's activity. Status
labels are whole measured tokens; unknown or unavailable Watch results are not
successful completions. Full details remain in the tooltip and list.

Unread results and failed terminals receive attention priority; quiet running
Watches obey hiding. Switch/Restore validates workspace, incarnation and placement
without acknowledging a Watch. A successful restore clears only the separate
parked-completion attention. Explicit Open result and Dismiss watched result retain
Watch acknowledgment semantics. See [workspace activity](workspace-ribbon.md) and
[Command Watch](command-watch.md#workspace-activity-ribbon).

## Portable parked-only summary contract

The following `WorkspaceBackgroundSummary` remains available for parked-only
consumers; it is not the current Windows host's complete activity model.

Hosts can opt into `RibbonHostData::compact_background` without changing any
saved binding ID, order, visibility or legacy chip layout. In this mode
`background.overflow` is the single workspace Navigator target, and `chips` is
empty. Do not route it into a second picker or use its index as a restore target.
The host supplies a fresh `WorkspaceBackgroundSummary` by iterating only the
active workspace's parked placements and calling `add(unread_completion)`.
`label()` is `Background N`; `completion_label()` is `N finished`, counting
terminals with unread explicit command completions, not completed commands or
exited shells. Its accessible label spells out this distinction. A later command
can be running while that terminal still has an unread prior completion.

Set `background_attention` from `summary.unread() != 0`. Like errors, unread
completion remains reachable if customization hides the quiet binding. Merely
opening or previewing the Navigator must not erase attention; remove its entry
only after a successful restore (existing `ParkedAttention` lifetime contract).
Quiet hidden Background remains available through commands. Compact narrow
layout preserves visible Background, session errors and agent attention before
ordinary bindings, dividing their space if necessary. Clock is the first
ordinary binding to lose admission. All placements still follow saved order.
The host should truncate labels in small targets and provide the complete
accessible/tooltip label; geometry never copies a terminal title or path.

`terminal_status_label(info)` gives terminal lifecycle precedence over OSC
metadata. Only an explicit running command phase says `Command running`; an
integration-unknown live shell says `Terminal open`. A matching explicit
completion says `Command finished`, while process exit says `Terminal exited`.
This display helper grants no process control, inferred progress, CPU health,
elapsed-time or Stop-command semantics. Use 12 DIP metadata in the native host;
this portable change does not itself change native typography or animations.

Focused portable tests additionally cover empty/one/many summaries, very long
untrusted titles, unknown integration, completion followed by a new command,
repark baseline, lifecycle precedence, hidden/unread overrides and both saved
orders across widths 1–1499 with zero/one/10,000 parked terminals. These portable
checks alone do not qualify native rendering, DPI, keyboard or mouse behavior. Current Windows Activity fixtures are described in the
[Watch contract](command-watch.md#workspace-activity-ribbon); physical verification
remains separate.
