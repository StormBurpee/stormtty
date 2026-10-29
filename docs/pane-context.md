# Pane context presentation

`stormtty/pane_context.hpp` is a portable, header-only, display-only helper. It
reads a single already-owned session/workspace snapshot; it performs no Git,
filesystem, subprocess, host/SSH inference, or terminal input operations. It adds
no persisted schema, shell defaults, subscriptions, animation, or polling.

## Minimal native host integration

1. Copy `SessionInfo` once for the pane being painted, not just the active pane.
2. Build `PaneContextInput` with the existing resolved title (preserve user and
   application/OSC text), profile label, `info.working_directory`, workspace
   configured working directory, `info.shell_context ? &*info.shell_context :
   nullptr`, whether this is an integrated shell, and whether exited output is
   retained. The pointer is borrowed only during `make_pane_context(input)`.
   **Do not use path/process string heuristics to classify title provenance.**
3. `make_pane_context` returns owned UTF-8 display fields: `primary`, `directory`,
   `head`, `repository_root`, `configured_directory`, `directory_source`,
   `detached`, `retained_exited`, and `detail`. No directory key is exposed.
4. Within the existing 28-DIP pane header, use one aligned text row: primary
   title at normal emphasis, secondary context at subdued emphasis. Reserve
   existing focus/close/status affordances first. Use native text measurement
   and ellipsis; do not change the terminal grid or grow header height.
5. `pane_context_secondary(value, PaneContextDensity::directory_and_head)` adds
   branch or `HEAD <id>`. At constrained width use `directory` density first,
   then `title_only`; preserve primary title priority. Full paths are retained
   rather than collapsing different roots to identical basenames. Truncate only
   visually with native metrics, never slice UTF-8 bytes. Optional path-tail
   elision must retain distinguishing ancestors, not claim basenames are unique.
6. Use `detail` for the existing full read-only tooltip or a future context
   command. Recompute on session metadata/workspace changes; compare complete
   presentation values when deciding chrome invalidation. Nonfocused metadata
   changes must refresh their pane header and tooltip. Text changes snap to the
   latest snapshot: no crossfade, path interpolation, or animation timer.

An integrated-shell context clear deliberately shows no secondary cwd even if
`info.working_directory` still holds an old value. A nonintegrated observed cwd
can be shown, and configured-only fallback is explicitly prefixed `Start:`.
Detail separately labels observed directory and configured start folder. Exited
retained snapshots retain context, but detail calls them retained output; no
freshness or live process claim is made. Host process-state affordances remain
host-owned, so do not append a second “exited” label to an already resolved title.

## Safety and tests

All fields use the existing untrusted-display token policy (visible control,
bidi, invisible format and invalid-UTF-8 escaping), with LF also escaped for
single-line chrome. The helper preserves complete accepted field text in the
read-only detail; rendering must clip/scroll its own surface. These strings are
never paths to open, command arguments, clickable authority, or Git truth.

Portable verification:

```
g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Iinclude \
  tests/pane_context_tests.cpp -o pane_context_tests
./pane_context_tests
```

Tests cover absent/cleared context, observed versus start folder, detached HEAD,
Unicode and long paths, same basenames under different roots, retained exited
output, explicit/application titles, metadata replacement, stable/no-time
presentation, narrow priorities, and untrusted text. These tests do **not**
verify native header rendering, DPI, hover, typing latency, or physical UI.

## Windows host

The existing 28-DIP split header preserves its resolved title, then adds subdued
context only when native text metrics leave room. Branch/HEAD is dropped before
directory; title has priority. No new bar, grid resize, title heuristic, process
probe, or path animation is introduced. Each actor wake copies one `SessionInfo`
per pane into owned host metadata; complete presentation changes invalidate
nonfocused chrome and tooltips. Workspace/profile edits resolve against that
owned snapshot without waiting for another shell output event.

**Commands → Inspect pane context** opens a frozen read-only snapshot in the
existing stationary Commands surface. It owns keyboard/mouse input like Commands,
wraps fields with native font metrics into bounded, keyboard-paged rows, and
filters those rows without interpreting them. Enter and Alt+Enter never execute
metadata. Escape returns focus. The existing 100-ms/reduced-motion transition
applies to the surface only; context text itself never animates. Hovering a
split header exposes the full labeled record. Configured start folders and
unavailable observations remain explicitly distinguished.

The native-craft layout scenario runs an explicit native context test child in
three existing owned actors, then captures `context-headers`, `context-inspect`,
`context-narrow`, and `context-high-contrast-fixture`. These are fixture OSC
records, not live Git discovery. Assertions cover per-actor snapshots, exact
observed title preservation, detached HEAD, long/Unicode context, keyboard
inspection without transport/frontend input, unchanged actor/grid identity,
and cleanup. The high-contrast image is a host color-mode fixture, not an OS
accessibility certification. Native runtime/capture results must be reported
separately from portable tests and cross-compilation.

Inspector wrapping uses the renderer's existing single DirectWrite layout and
line metrics, not per-scalar prefix measurement. The owned line cache keys the
full text, width and DPI: opening/new geometry invokes one native wrapper;
filtering, paging and repeated paints invoke zero. Portable counting coverage
includes 64 KiB of hostile label bytes (589,824 escaped display bytes), 1,000
cache hits, width/DPI changes and intact Unicode/combining text. Existing native
renderer coverage validates lossless Unicode wrapping and measured widths.
This bounds layout creation count; it is not a measured latency claim.

Transport-isolation evidence distinguishes actual sent frontend key/text/paste/
mouse bytes from focus reports and automatic VT replies. Every queued write
retains its origin through partial sends; counters are updated alongside
`bytes_sent` under the existing mutex only after transport acceptance. Their
sum must equal total bytes in the same `SessionInfo` snapshot. Context inspection
requires unchanged frontend bytes and UI submissions, and exactly accounts any
raw delta as focus/protocol traffic; unexplained bytes remain a failure. No raw
input bytes are recorded. Portable accounting tests cover every origin/mixed
partial counts; the real session search fixture forces 3-byte transport chunks
while mixing text, paste and Enter, with unchanged echo/ordering assertions.
