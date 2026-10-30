# Workspace activity and background terminals

The Windows host's single 26-DIP footer aligns with the workspace column, below
its terminal area; the sidebar continues to the window bottom. It adds no extra
activity band. Directory, Git, status and clock keep their existing order and
visibility rules. Focus mode hides this strip; its task-row button remains
available to restore it. The directory tooltip includes full pane context;
task-tab tooltips and Commands → Inspect pane context remain available when
directory is hidden, the sidebar is collapsed, or there is only one pane.

One target brings together the active workspace's explicitly watched terminals
and retained parked terminals. A terminal that is both watched and parked counts
once. Its label fits whole status tokens into the available width: failures and
unknown results precede successful Watch results, running Watches and quiet
background access. Quiet parked-only activity can still say `Background N`;
narrow layouts can fall back to `Activity`. The tooltip and list retain the full
breakdown. Counts describe terminals and observed results, not jobs or readiness.
An open shell alone is labelled `Terminal open`, not a running command.

Click the target or choose **Commands → Workspace activity…** to open the existing
Workspace Navigator in Activity mode. It includes only watched-or-parked actors
from the workspace in which it was opened. Preview and search are metadata-only;
**Switch** or **Restore** revalidates workspace membership, actor identity and
placement before returning to the same retained terminal. An exited terminal
keeps its retained output; opening it never starts a new shell. Missing or changed
targets fail visibly rather than selecting a replacement. Escape leaves the
foreground context unchanged. The ordinary Ctrl+Shift+O Navigator and background
commands retain their existing scope.

Previewing, canceling, switching and restoring do not acknowledge **Command Watch
results**. Only explicit **Open result** or **Dismiss watched result** does so.
**Parked-completion attention** is separate: a successful restore clears that
terminal's parked attention, but not its Watch result.

Settings → Ribbon names this binding **Activity & background terminals**. The
saved `workspace.background` ID, order and visibility remain compatible. Quiet
running Watches obey hiding; unread results and failed terminals retain attention
priority, including narrow layouts. The clock has lower priority. Hidden activity
remains accessible through Commands. There are no timers or progress estimates.

Native fixtures exercise the footer target, scoped watched-or-parked list,
preview/cancel, same-actor switching and restoration, stale targets, unread
preservation and zero frontend terminal input. Controlled captures do not qualify
physical assistive technology, display latency or shell authenticity. See the
[Watch contract](command-watch.md#workspace-activity-ribbon) for state semantics
and the [CI boundary](ci-coverage.md) for baseline versus qualification coverage.
