# First-frame and workspace continuity

The first frame is the working terminal, not a splash screen. The Windows host
establishes its DPI-scaled window dimensions before creating renderer/actors,
then prepares one real retained scene before `ShowWindow`. This render uses the
existing nonblocking presentation path. It never waits for a shell prompt,
sleeps, locks keyboard input, or fabricates terminal content. A busy/occluded
present is retried through the ordinary exposed-window render loop.

Newly encountered workspace, tab and pane selections start in their actual
state. Only changes between observed selections interpolate. Restored pane
labels no longer fade from invisible on each workspace switch; terminal
contents and keyboard ownership switch immediately. Existing 120 ms selection
handoffs, continuous rapid reversal, and OS/config reduced-motion handling are
retained. The footer says `starting` only while the actual actor reports that
state; process launch is not labeled shell/prompt readiness.

Portable geometry tests cover first-observation seeding, existing-state
transitions, rapid reversal, local identity reset and reduced motion. Native
smoke captures `startup-prepared` before HWND visibility and `startup-exposed`
on the first successful visible present. Workspace transition captures now
include the immediate `workspace-focus-000ms` scene, with assertions that the
active tab is already legible and the real session already owns input. The
existing 40 ms/reversal/settled evidence remains unchanged.

Pane identity now stays immediate on every focus change: the full accent marker
and primary-label ink track the selected pane, with unchanged font weight and
geometry. Only the existing alpha-89 header wash interpolates from the semantic
surface to selected color over 120 ms. High contrast omits that wash and retains
exact OS ink; normal launches still honor OS/user reduced motion.

The owned `--smoke-layout` fixture samples actual native paint at controlled
0/40/120 ms, reversal at 40 ms, settled reversal at 160 ms, and midflight
reduced-motion cancellation. A scoped synthetic policy applies **only** to pane
decoration while the smoke guard and clock are active; global/caret policy and
OS preferences are never overridden. Actual OS high contrast makes this normal-
motion qualification unavailable rather than silently bypassing accessibility.
The workflow requires all six `craft-layout-pane-focus-*.png` captures:
`000ms`, `040ms`, `reversed-040ms`, `settled-160ms`, `settled-120ms`, and
`reduced-040ms`, plus the invariant marker. Actors, pane/grid
bounds, split ratios and frontend key/mouse counters stay unchanged; normal
focus/protocol writes and live terminal output remain allowed. No global frame
generation stability is claimed. Guard/clock/policy are restored on failures;
successful completion restores original focus and ends pane decoration work.
The shared chrome timer must stop when no other chrome transition remains;
unrelated legitimate chrome motion may retain it.
These samples establish synthetic interpolation and native paint, not physical
smoothness, timer delivery cadence, DWM latency or keyboard-to-photon latency.

These are native canvas and synthetic input checks, not physical DWM flash,
high-DPI, monitor transition, or startup-latency qualification. PNG readback is
explicit smoke-only and deliberately excluded from normal startup.
