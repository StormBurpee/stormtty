# Typing caret investigation

## Recording observations (2026-10-29)

Two user-supplied recordings were inspected locally before code changes. No
recording, extracted frame, private path or terminal transcript belongs in this
repository. Both videos are 30 fps; their video durations are 6.100 s and 3.967 s.
The first includes a history autosuggestion; the second shows ordinary typing
without an autosuggestion in a quoted commit-message argument.

In the second recording, frames 4–5 (0.133–0.167 s) show the block cursor in the
blank cell after the last typed character. Frames 6–8 (0.200–0.267 s) show it one
whole cell to the left, over that same character, including its inverse glyph.
This is not merely growth of the cursor's right edge. The recording does not
contain keyboard-event or PTY timestamps, so it cannot establish input latency
or identify which component produced that cursor position.

## Source-level separation

`CursorMotion` returns a destination-cell reveal fraction, not a position.
The Windows renderer multiplies a block/underline cursor's width by that
fraction (a bar's height instead). It never moves the left edge to a previous
column. The reveal starts at 0.58 and settles over 85 ms. A backwards frame
cursor immediately cancels the reveal. Therefore removing this animation alone
is not an evidence-backed fix for the observed whole-cell retreat.

The regression in `cursor_motion_tests` models an echoed forward step, an
authoritative backward cursor update, and cursor-only restoration, both with
and without text in the destination cell. It checks immediate cancellation,
no fabricated forward position, no mutation of text/cursor state and no new
animation on cursor-only restoration. Existing cases cover reduced motion/IME,
alternate screen, resize, selection, focus, blink and input expiry.

## Verification boundary

The portable C++20 test passes on Linux. This is not a Windows runtime or
physical typing verification. Resolving the reported retreat still requires
correlating native input, parsed PTY cursor state and presentation while the
real line editor repaints. Do not clamp backwards cursor movement, delay PTY
input, predict characters or claim that this regression test fixes the report.

## Source-backed candidate: avoid per-character MSYS history forks

The pinned autosuggestion module's modify widget has two different paths:
typing an existing suggestion returns without fetching, whereas new unmatched
text calls its async fetch. That fetch starts a process substitution, waits to
read the child's PID, then installs an asynchronous ZLE response/redraw handler.
The managed preset uses only the `history` strategy, an in-process lookup of
zsh's bounded history table. There is no external completer to keep off-thread.

The candidate leaves upstream async policy on POSIX, but unsets its async flag
on MSYS/Cygwin after loading the bundled history-only module. An explicitly
configured async flag or a custom non-history strategy retains upstream async;
later user overrides remain untouched. This removes that
per-character fork/PID handshake and second response redraw, without touching
PTY input, frame publication, renderer cursor positions or user text. It does
not disable suggestions, syntax highlighting, IME, backspace or alternate
screen applications. This is a source-supported candidate for the differing
typing paths, not proof of which native component caused the recorded retreat.

Real pinned-module tests exercise both matching and unmatched quoted text with
the async entry point replaced by a failing sentinel, and preserve POSIX async
policy. A POSIX PTY test also selects the native history policy and exercises
actual suggestion acceptance after a failed command. Local timing of 100
unmatched lookups against 20,000 authored history entries was 205.751 ms total
(about 2.06 ms/lookup); this is Linux evidence, not Windows timing or an input
latency claim. Native Windows and physical typing verification remain required.

The native managed-state probe now adds a bounded warm-session typing phase
after its existing history-suggestion acceptance. It uses ordinary per-character
`Session::text` for a unique authored quoted no-op command, then exercises an
extra character and Backspace. It requires the expected final row/cursor,
successful command lifecycle, history-only policy and zero calls to an async
fetch sentinel. For 150 ms after each observed echo it samples new frame
generations, counting cursor retreats without failing them: unsynchronized
line-editor repaint may legally publish intermediate positions. The complete
typing/observation window is bounded to 15 seconds. Numeric results include
character/frame/retreat counts and aggregate/maximum enqueue-to-observed-frame
echo times; polling resolution and actor publication affect these numbers.
They are neither physical display latency nor proof that every frame was seen.
No frame contents are logged, and no authoritative frame is changed or hidden.
