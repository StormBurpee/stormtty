# Find in the active terminal

On Windows, **Ctrl+Shift+F** or **Find in terminal** opens a native strip above
only the active pane. It reserves layout space instead of covering terminal
rows. In narrow panes the count and controls move to a second row. Very small
splits must be enlarged before opening Find. Shrinking an open pane below that
minimum dismisses the strip and restores terminal input, retaining the query.

- Type a literal query. ASCII matching is case-insensitive; other Unicode
  scalars are exact. There is no regex, normalization or case toggle.
- The first completed result selects the newest match and scrolls to it.
  Initial selection recovers across output/resize until you navigate manually;
  each fresh result token is attempted at most once. **Enter** moves toward newer matches; **Shift+Enter** moves toward older
  matches, wrapping at either end. The count is ordered oldest to newest.
- **Tab / Shift+Tab** traverses the query, previous, next and close controls.
  **Escape** cancels the search and returns focus to the same terminal. The
  selected text remains available for copying.
- The query supports UTF-8 scalar-aware arrows, Home/End, Backspace/Delete,
  select-all, copy/cut and paste. Control characters/multiline text and queries
  over 4096 bytes are rejected without replacing the current text. The caret
  scrolls using native font measurement. Search text is not sent to the PTY.
- Selecting another pane/tab/workspace or clicking outside dismisses the strip.
  Results never migrate to another session. Search history is not saved or
  exported. Opening it again reuses the in-memory query, not result positions.

## Backend boundary

Find uses the existing bounded `Session::search` service, not a separate index.
The UI accepts only the current request ID and query; every select/navigation
passes the core's complete request/revision/generation token. Output, eviction,
reset and resize invalidate positions. The actor re-searches live output and
rejects stale navigation; the UI never computes retained-text coordinates.
Running, exhausted-budget, error and no-match states are distinct. Limited
results are not presented as an exhaustive count. Exited retained documents
use the same API and do not restart their process.

No search animation or timer is added. Existing reduced-motion policy remains
unchanged. This implementation does not claim a native UIA TextPattern or
physical IME qualification; those remain explicit accessibility gates.

## Evidence

Portable tests cover query editing/UTF-8 bounds, measured caret viewport,
request/query staleness, limit reporting and narrow strip geometry. Existing
terminal/session tests exercise scrollback, token rejection, bounded work,
selection scrolling and retained-exit behavior.

The native project smoke prints an authored marker through its owned shell,
then exercises asynchronous Find, navigation, narrow resize, no-match and rapid
query replacement, Escape and workspace isolation. It checks that query and
navigation never enter the frontend PTY text/key/paste submission paths (total
transport bytes also include legitimate automatic replies during resize) and emits `find-results`, `find-next`,
`find-narrow` and `find-empty` actual framebuffer captures. These are synthetic
native checks, not a substitute for physical keyboard/IME/accessibility QA.

Design influence: [JetBrains search-field guidelines](https://plugins.jetbrains.com/docs/intellij/search-field.html)
(on-demand scope, stable controls and visible keyboard focus) and
[tool-window navigation](https://www.jetbrains.com/help/idea/tool-windows.html)
(return focus to the working surface). No IDE branding, file index or AI panel
is introduced.
