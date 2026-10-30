# Terminal frame materialization ownership verification

## Boundary and resource inventory

Private `TerminalFrameMaterializer` owns the Ghostty render-state handle, row,
cell and image iterators, last immutable frame, and frame/row generation counters.
Its single production caller is `TerminalCore`. Capture takes only a synchronous
terminal borrow, current geometry and image-budget value. No terminal model,
actor, lock, scheduler, renderer context or deferred pixel-copy phase is added.

Core remains the mutation/thread authority. It checks owner-thread and sticky
callback failures before public snapshot calls, advances animation and hold
expiry, and decides whether to return the last held frame. Its render-hold
callback invokes materialization at the exact existing pre-hold boundary, before
setting hold time/state and before subsequent bytes in the same feed mutate VT.
The callback's existing noexcept wrapper retains any exception as a fatal Core
failure; materialization does not turn this into a recoverable search-style error.

The materializer's default construction allocates nothing. Core explicitly calls
`initialize` once immediately after terminal allocation and before key/mouse
encoder/event allocation. The four materialization handles retain their creation
and reverse destruction order. The owner is declared after the terminal, so all
its engine resources are freed before that terminal. The native handles retain
relative destruction order, but the cached owned FramePtr now releases after
encoder/event handles rather than before them; its immutable data contains no
engine borrow. This is not a claim of identical all-member destruction order.
Initial render colors and
palette are sampled at the original later constructor point and returned by
value; applying terminal defaults remains with Core. No raw render-state handle
escapes the owner.

Rows, graphemes, selections, placements and pixel bytes are fully owned before
capture returns. Unchanged frames retain pointer identity; unchanged rows retain
identity/content generations, including cursor-dirtied rows whose cells still
compare equal. Default-color changes force row re-resolution. Image placement
reuse and generation-keyed payload sharing retain the existing behavior. Only
newly copied payload bytes are charged to the capture's image allowance; shared
payloads are not copied or recharged. Placements remain capped at 65,536 and
stable-sorted by layer then z, retaining engine order for ties.

Render-state cleaning follows successful row and image construction; cached-frame
publication follows successful cleaning. A failed construction retains the old
published frame but may consume frame/row generation values. Counters are not
made transactional by this extraction. Temporary maps are bounded by the visible
rows and admitted placements; retaining arbitrary historical frames is still not
free or permitted by this boundary's sharing mechanism.

## Executed characterization

Against unchanged production at `89b3091` (tree `da6e89b8`):

- Rebuilt and executed the existing real-Ghostty Linux terminal suite: **33/33**.
- Added three public-Core characterization cases, then executed **36/36** before
  extraction. Commit `673a3bb` contains only these tests.
- Captured a private deterministic public-Core trace: **1,109 lines / 70,686
  bytes**, SHA-256
  `49f00a3e6ce168933b3532570274bd230ecf97dcc195e440e3b4f0ac36d378b2`.

The new tests cover cursor/mode-only frame changes with unchanged row identity,
cell-size-only resize, wrong-thread snapshot rejection, shared image payloads
across placements, three image layers, equal-z ordering through placement removal,
source crop/offset/dimensions, text-only image reuse, and exact same-feed held-image
capture with old pixel bytes retained across replacement/deletion/Core destruction.
An initially incorrect test deletion command was corrected to Kitty `d=i` before
committing or moving production; no production change was used to make it pass.
Existing tests additionally exercise theme/palette changes, selection, reflow,
alternate screens, virtual/relative images, animation, PNG preflight, zero image
budget, callback poison, hold timeout and finished-output behavior.

After extraction, the unchanged extended suite passed **36/36**, search passed
**17/17**, and Session integration passed (real child, ordering, immutable frames,
bounded queue, retained-document lifecycle, flood close and immediate-close races).
The same trace source produced byte-identical output. It records all frame,
cursor, row/cell, placement and pixel fields, with first-seen identities replacing
opaque RowIds and pointer addresses. All frames are retained during the trace to
prevent address reuse. It covers unchanged snapshots, metadata/styles/default
colors, cell-size changes, shared/cropped/layered placements, deletion, image
replacement during held output, alternate screen, reflow, history and output
completion. It excludes wall-clock values and is not an exhaustive state-space
proof. Animation remains covered by deadline-relative suite assertions.

## Reproduction and limits

Direct builds use `g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O0`, repository
`include`, and the existing pinned Ghostty include/library directory. Terminal
and trace builds link `terminal.cpp`, `terminal_search.cpp`,
`terminal_frame_materializer.cpp` (after extraction only), and `shell_context.cpp`
with `-lghostty-vt -pthread`. Search/Session also link `session.cpp`,
`terminal_find_controller.cpp`, POSIX `pty_process.cpp` and `-lutil`.
Workspace-backed TMPDIR and compact reused binaries avoid dependency/full builds.
Private trace source, build helper and logs are under
`/workspace/shared/frame-materializer-evidence`; characterization tests are committed.

No production-global failure injection was added. Actual OS allocation failure,
render getter/clean failures, row-count mismatch, unsupported decoded format,
inconsistent image generation/length, and the placement-cap failure path were
not deterministically injected. Callback poison is exercised through the existing
effect-budget failure, not a forced allocation failure inside `on_hold`. RGB/gray
format branches and every cursor/selection/grapheme combination are not newly
exhaustively covered. Exceptional ordering for these cases is source-reviewed,
not claimed as runtime proof.

Native Windows/macOS qualification is pending for this source. No new physical
UI, performance, native decoder or onscreen presentation qualification is claimed.
Known Windows shell qualification failures remain separate; this slice neither
fixes them nor activates AI, Liquid Glass/HDR or Leyline.
