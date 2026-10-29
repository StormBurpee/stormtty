# Workspace background summary

The footer groups only the active workspace's parked terminals into one
`Background N` target. Observed unread command completions add `N finished`;
opening the list or canceling it does not acknowledge them. A successful
existing-actor restore does. A live shell alone is labelled `Terminal open`,
not a running command.

Click opens the existing Workspace Navigator with parked terminals only.
Workspace preview and search are still metadata-only; Enter validates the
stable workspace/session pair before restoring the retained actor. Escape
returns to the unchanged foreground context. The ordinary Ctrl+Shift+O
Navigator and keyboard command-palette restore remain available.

Saved footer order/visibility is retained. Unread background attention remains
reachable in narrow layouts; the clock is lower priority. The summary uses
shared hover motion and theme colors, without timers or progress estimates.

Native fixtures cover the real footer target, parked-only preview/cancel,
zero frontend input, real observed completion attention, and keyboard restore
of the same exited actor. They do not qualify physical assistive technology.
