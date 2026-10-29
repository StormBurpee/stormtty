# Workspace Navigator

Windows **Ctrl+Shift+O**, **Switch workspace or terminal…** in Commands, or
clicking the footer directory opens a native navigator over the current terminal.
It searches workspace names/folders and existing terminal titles/directories.
No files or terminal output are indexed, no network is used and no shell starts.

The wide view gives workspaces their own column, with terminal counts and a
separate list of their live or retained terminals. Process state and background
placement are distinct. A compact view uses the same two lists as separate
pages; click the workspace heading or use Tab/Left/Right to change lists.
Below 320 × 260 DIP the navigator dismisses instead of clipping controls.

- Typing filters the metadata snapshot. Unicode matching uses the same bounded
  literal subsequence matcher as Commands (ASCII case-insensitive). Workspace
  order stays stable; a still-matching selected identity is preserved.
- Up/Down browses the focused list without switching the terminal underneath.
- Tab cycles query, workspaces and terminals. Left/Right changes focused lists;
  in the query field it edits the caret instead.
- Enter in the workspace list opens its terminal list. Enter in the query or
  terminal list commits the selected terminal. Double-click or the Switch /
  Restore button also commits. Escape and an outside click cancel.
- Switching validates both workspace and session identities against the live
  catalog. A parked actor is reattached; an existing pane is focused. Dormant
  restored shells are deliberately not launched by this command.
- Query, mouse and navigation input are trapped while open. Terminal grid size
  does not change. The query and selection are ephemeral and not saved.

Text comes from owned metadata, with control/bidi escaping before display and
filtering. A cleared managed-shell directory is not replaced with an invented
current directory. The detail shelf can show the explicitly labelled workspace
folder instead. No job progress, elapsed time or readiness is fabricated.

## Evidence boundary

Portable tests cover stable identities, workspace/terminal filtering, parked
placement, empty results, capability-gated binding and normal/compact geometry.
The isolated native craft smoke uses its actual two owned shell actors to check
preview without switching, normal/compact captures, empty results, cancellation,
zero frontend input submissions, actor-preserving commit and stale identity
rejection. Native runtime/pixel inspection is required before promotion.
The surface uses the same 100ms stationary opacity transition as other overlays,
reusing their owned-frame reveal helper and shared chrome scheduler. Input and
hit targets change immediately; reduced motion/high contrast bypass it, and the
shared timer stops at settle. No terminal pixels or grid geometry animate.
Physical IME and UIA qualification are not claimed by these tests.
