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

These are native canvas and synthetic input checks, not physical DWM flash,
high-DPI, monitor transition, or startup-latency qualification. PNG readback is
explicit smoke-only and deliberately excluded from normal startup.
