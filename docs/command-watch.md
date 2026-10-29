# Watch this command

## Delivered scope

The Windows host integrates the tested, header-only `command_watch.hpp` policy
in native Commands, tabs, workspace rows and the Navigator. It lets you explicitly
arm one observed foreground command per live session,
leave it in its tab, switch workspace, and retain one quiet result. No command
text, output, history, OS notification, polling, persistence, input injection,
process termination, automatic navigation, or focus change is involved.

The source is the existing untrusted OSC 133 `CommandSnapshot`, not OS process
inspection. “Foreground” means the host's currently selected terminal and an
observed shell C marker. It cannot prove process identity, authenticity, service
readiness, or background-job completion. Unsupported shells cannot be armed.

## Windows controls

Open **Commands** with **Ctrl+Shift+P** and choose **Watch this command** while
shell integration reports a running command in the active terminal. The action
is unavailable otherwise; it never launches a command or enables integration.
Watch actions have no default shortcut.

- **Cancel command watch** removes a pending watch, not the running process.
- **Open result · workspace · terminal** entries in Commands return to the
  existing actor, including a parked terminal, and acknowledge only on success.
- **Dismiss watched result** acknowledges the active terminal's result without
  navigating. Parked-result actions also offer dismissal.

Ordinary tab/workspace selection and restore do not clear a watched result.
Nothing automatically focuses a terminal, sends input, or posts an OS notification.
Watches are not saved across application restarts. macOS has no command-watch UI.

## Policy

- `CommandWatch::eligible(identity, info, foreground)` requires a nonzero,
  host-qualified identity, a live process, a running command, and a nonzero
  generation newer than the last completion. `arm` rejects replacing a watch or
  unread result. Arm only from an explicit action after revalidating the target.
- Identity has three scalar fields: `host`, `session`, and `actor`. The host must
  mint non-reused incarnation tokens; `SessionInfo` contains **no** actor identity.
  A pointer/PID alone is unsafe because it can be reused. Host lifetime tokens and
  a monotonically assigned session/actor token suffice; never wrap/reuse tokens.
  A mismatched actor cancels even an unread old result. Remove entries on retirement.
- A matching `completed_generation` yields `completed` and its optional exit code,
  including a coalesced C-D-C snapshot or a matching D followed by EOF. This is an
  observed command result, **not** a claim that the session exited successfully.
  A D without a code has unknown status; do not silently display success green.
- A later completed generation yields `completion_unconfirmed`, never the later
  exit code. Latest-state snapshots cannot distinguish C-D-C-D from
  C-RIS-C-D: the watched command might have been interrupted. Suggested copy:
  **“Later command finished · watched result unavailable.”** This is deliberately
  not “Your command succeeded” or even an assertion that the watched command ended
  with D. Recovering the exact lost result requires additional actor-side
  provenance; do not invent it in the host.
- A newer C without a matching D, reset/interrupted phase, closing, or EOF without
  matching D yields `interrupted`. Suggested copy: **“Watch ended · no command
  result.”** Actor failure or regressed/inconsistent metadata yields `unavailable`.
  Failure does not erase a matching explicit D: present any session failure through
  the existing stronger process-status channel, without calling it command success.
- Repeated snapshots and later commands cannot change/replay the stored result.
  `cancel()` only removes the watch; **Cancel watch is not Stop process**.
  `acknowledge()` clears a result only after explicit Dismiss or successful Open
  result. Tab/workspace visibility, ordinary restore, hover, and failed navigation
  are not acknowledgment. Nothing automatically arms the next command.

## Windows integration invariants

1. Allocate one host token plus stable actor incarnation at session insertion;
   retain a bounded `map<SessionId, CommandWatch>` only for armed/unread entries.
   Token identity remains unchanged across tab/workspace navigation, park/restore,
   and pane movement. Clear it on actor retirement/replacement. Do not persist it
   in workspace serialization or derive it from untrusted terminal metadata.
2. Add “Watch this command” to the active-terminal action surface. Resolve the
   current host-qualified session, fetch fresh `info()`, and call `eligible` then
   `arm`. A stale action must fail harmlessly. Offer “Cancel watch” while waiting,
   and “Open result” / “Dismiss” for an unread result. Explanatory disabled copy:
   “Available while shell integration reports a running command.” Do not silently
   launch a command or enable shell integration to make eligibility true.
3. In the existing actor wake-driven `update_sessions`, observe each watched actor
   exactly once, including hidden workspaces and parked sessions. No new timer.
   Invalidate chrome only when the policy transitions. Keep terminal rendering,
   process state, and existing failure-priority notices independent. Do not send
   toast/OS notifications; use a quiet persistent result indicator.
4. Use `surface(same_actor_parked)` to select exactly one representation. A watched
   actor parked before or after completion uses its existing parked row, with
   `parked.unread_completion() || watch.surface(true) == WatchSurface::parked`:
   count it once, not twice. Do not emit a new watch toast alongside existing
   parked completion notices. Restoring transfers the watch indicator to its tab;
   it does not clear the watch. Existing generic parked attention's restore-ack
   semantics remain separate. An explicit Open result may restore and acknowledge
   the watch only after success; failed restore/canceled close must retain it.
5. Dismiss must also suppress the **same generation's** parked indication, if
   present: call `ParkedAttention::acknowledge_completion(watch.generation())`
   before clearing the watch. It clears only an unread latest observed completion
   whose generation exactly matches; zero, old, future and repeated acknowledgments
   return false. It preserves completion/notification baselines and process state,
   so newer completions and distinct process exits/failures still notify normally.
   Never rebaseline the whole parked entry or clear unrelated later completions.
6. Opening navigates to the existing actor/pane after identity revalidation, never
   creates/restarts a shell. If the actor has retired, show unavailable or remove
   the obsolete result rather than targeting a reused local ID. An unloaded
   workspace must not be started to recreate an old watch.

## Native fixture coverage and remaining gates

Portable tests are not Windows/macOS UI evidence. The acceptance matrix is:

- Start a fixture shell with observed C, arm through the actual action, switch
  workspace/tab without parking, emit D (0, nonzero, and absent code variants),
  and assert exactly one persistent indicator, unchanged focus/placement/input
  bytes, and a still-live shell. Repeated wakes do not replay anything.
- Capture the armed state and unread result in real native chrome. Keyboard and
  pointer Open result must target the same actor; plain workspace/tab activation
  keeps unread state. Explicit Dismiss/Open result clears it only once.
- Feed C-D-C in one publication and multiple later completions in another. Verify
  exact-match status vs “watched result unavailable” and no borrowed newer code.
  Feed C-RIS-C-D in one publication: no watched-success claim.
- Exercise reset, EOF without D, exact D then EOF, and injected actor failure.
  Session failure indication outranks quiet command completion. Verify no stale
  result reappears after close/replacement with the same numeric local ID.
- Watch → park → complete, complete → park, failed restore, successful ordinary
  restore, explicit Open result restore, canceled close, and park → restore →
  park must never multiply indicators or turn mere visibility into watch ack.
  Explicit dismiss of one result must preserve any newer parked result.
- Repeat across two native hosts using equal local SessionIds. No cross-host
  result, action, or acknowledgment may leak. Saturated identity counters must
  disable new watches rather than reuse an identity.

## Portable verification

`tests/command_watch_tests.cpp` checks eligibility, exact/coalesced results,
missing status, duplicate snapshots, cancellation, identity replacement, reset,
EOF/failure, later commands, and parked presentation selection. It is runnable
without Ghostty/process libraries:

```sh
c++ -std=c++20 -Wall -Wextra -Werror -pedantic -Iinclude \
  tests/command_watch_tests.cpp -o command_watch_tests
./command_watch_tests
```

## Windows host implementation

The host integrates the policy in existing Commands, tab captions, workspace
rows, Navigator state and parked rows—no new permanent bar. “Watch this command”
revalidates current owned actor metadata; “Cancel command watch” does not stop
a process, and “Dismiss watched result” acknowledges only that result. Qualified
“Open result” rows navigate to the existing actor and acknowledge only after
success. Ordinary workspace/tab navigation or restore never acknowledges a
watch. Parked summaries OR watch/unread attention, and precise-generation
acknowledgment cannot erase newer generic parked results.

Host and actor incarnation counters saturate rather than reuse tokens. Watches
are in-memory only and removed on retirement. Observation piggybacks on the
existing actor wake snapshots, including hidden workspaces and parked actors;
there are no timers, shell commands, output/history capture, filesystem probes
or OS notifications. Result text is neutral: absent status remains unknown;
coalesced later generations never supply the watched command’s exit code.

The native layout fixture uses an explicit controlled child, not claimed real
command provenance. It arms through Commands and exercises hidden tabs and an
empty second workspace, exit 0/7/unknown, one-write C/D bursts, reset, cancellation,
keyboard/pointer Open, visibility without acknowledgment, park/ordinary restore/
repark, canceled close, and stale-identity rejection. A burst may arrive in more
than one actor publication: exact earlier completion or conservative unconfirmed
result is allowed, but borrowing the newer code is never allowed. It requires
actual PNGs for armed/result/Commands states and unchanged frontend UI input.
EOF/failure/two-host identity/saturation remain portable policy coverage; this
fixture does not claim corresponding native physical/process qualification.
The Windows integration and indicator fixtures are implemented and integrated;
report their native workflow results separately from the full compatibility suite.
A controlled native capture is not physical UI, accessibility, or desktop-blur
qualification, and cross-compilation alone is not native runtime evidence.

## Persistent tab indicator

The existing tab has a reserved 6-DIP status mark before its label: hollow for
an armed watch, filled for an unread result. Shape—not success color—distinguishes
the states, including high contrast. Result takes priority if split panes in one
tab contain both states. Text/tooltip and Commands retain explicit wording.
No tab bounds, close/dropdown hit targets, terminal grids, timers or input paths
change. At unsupported widths below 46 DIP the mark is omitted rather than
covering controls. Portable geometry tests cover that boundary and fixed text
separation. The native fixture retains the seven-cycle Watch gates and
captures a deliberately long Unicode tab title plus narrow/high-contrast unread
states; these are controlled native fixtures, not physical display evidence.
