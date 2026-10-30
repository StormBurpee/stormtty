# Incremental search ownership verification

## Boundary and invariants

Private `TerminalSearch` owns the capped allocator, engine search handle, query
validation, request/generation/update counters, fed revision, cumulative work,
materialization buffer, owned immutable snapshots and navigation policy.
`TerminalCore` is its sole production caller. Core owns the terminal and content
revision and checks owner thread/fatal callback failures before every public
search operation. Selection candidates contain only an index and owned cell
coordinates. Core resolves those to ephemeral GridRefs, sets actual selection,
reveals the row, releases synchronized output if held, then records the action.
Session scheduling, superseded-request guards and Find-controller selection
clearing have not moved.

The owner is noncopyable/nonmovable and introduces no construction allocation.
The allocator has a stable address and outlives its engine handle; search members
retain their previous reverse destruction order. Core declares the owner after
its terminal handle so search destruction precedes terminal destruction. Match
getter references never survive an actor call. Temporary arrays are bounded by
`max_matches` (at most 4096); each conversion unit handles at most 64 coordinates.
The immutable snapshot copy on selection/action remains bounded by that same cap.

Content changes invalidate conversion/selection but do not replenish cumulative
request work. Cancellation retains query/work metadata, advances generation and
zeros match count without clearing the separately owned terminal selection.
Empty feed and unchanged resize do not advance revision. Literal matching,
ASCII-only folding, inclusive endpoints and newest-first ordering are unchanged.

## Executed characterization

Before moving production code from `bc4b3f5`:

- The original real-Ghostty Linux `terminal_search_tests` passed **14/14**.
- Three public-API characterization cases were added and executed against the
  original production source: **17/17** passed. Commit `b8dbdd4` records those
  tests before ownership extraction.
- A private public-Core trace was captured: **347 lines, 34,743 bytes**. The same
  source was compiled and executed after extraction; the traces were byte-equal,
  SHA-256 `18a89b59396ec72956b3dec044186630693123bcdc3d7309dd29bd558312a3b0`.
  It records all search snapshot fields and coordinates, pointer identity as
  changed/stable (not addresses), visible cell text/selection and render-held
  state. Scenarios include history navigation, stale/reused tokens, cancellation,
  materialization interrupted by output, resize, alternate screen, RIS, malformed
  queries, result limits and work limits. It is a selected deterministic trace,
  not an exhaustive state-space proof.
- The extended suite passed **17/17** after extraction, including real Linux PTY
  coalescing/fairness, retained-document search and existing Find lifecycle cases.
- Adjacent Linux checks passed: **33/33** `terminal_tests` and the `session_tests`
  executable (real child, retained-document lifecycle, ordering, bounded queues,
  flood close, launch failure and immediate-close races).

The additional normal tests cover cancellation metadata and selection ownership,
validation priority, immutable prior snapshots, cumulative work across revision
changes, no-op feed/resize, exhausted-request recovery, option boundaries, and
cancellation before feeding, after feeding and during materialization.

## Reproduction and evidence limits

Final production/test binaries were rebuilt from the final source with
`g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O0`, repository
`include`, and the existing pinned Ghostty include/library directory. Search tests
link `terminal.cpp`, `terminal_search.cpp`, `shell_context.cpp`, `session.cpp`,
`terminal_find_controller.cpp` and POSIX `pty_process.cpp`, with `-lghostty-vt
-pthread -lutil` and an rpath to the existing shared library. The before build
omits the not-yet-created `terminal_search.cpp`. No dependency rebuild/download or
full application build was performed. The unchanged private trace driver has two
misleading-indentation warnings and was built without `-Werror`; production and
normal-test builds use `-Werror`. Private trace source/logs remain outside
the repository; normal characterization source is in `terminal_search_tests.cpp`.

Memory-budget denial and successful restart are exercised through the real
engine, but actual OS allocator failure, pathological allocator alignment,
getter/API errors, changed match-buffer length, coordinate conversion failure,
and failure while allocating an error publication are not deterministically
injected. There are no new production-global injection hooks. Existing behavior
is retained: search-step exceptions become error snapshots when publication can
succeed; begin/cancel/action publication can throw, and error publication can
itself fail. Fatal callback poison remains a Core exception, not a recoverable
search error. Source review is not runtime proof of those exceptional corners.

## Native qualification

[PR #45 run 41134651142](https://github.com/StormBurpee/stormtty/actions/runs/41134651142)
checked out `403a9ad873b7b4369a2b472b9b2c493751b9748e`, the synthetic merge of
source `c957a5f8580663dbba193d1174749e9086474b06` into `8d53b991`.
Both Windows and macOS checkout logs confirm that commit; its tree
`9936edb80f4346f7f245d8fa437ac2f7a3684629` matches the reviewed source tree.

- Windows native baseline: **71/71** passed; macOS native baseline: **61/61**
  passed. Both executed **17/17** search cases, **33/33** terminal-core cases
  and the Session suite. Linux CI also passed.
- All **15** Windows application cases passed across packaged, bundled and
  developer configurations. Installed-console Settings passed too.
- Settings repeats passed **9/10 explicit + 3/3 default**. Only explicit case
  **10** failed (return code 2): Settings stage 8 reached its phase-2 deadline,
  with malformed `[200~` input, a trailing `~`, and `zsh: = not found` in the
  terminal rows. This is an observed signature, not a proven root cause.
- All **29** compact receipts reported reaped processes, no outer timeout,
  no output truncation and no cleanup error: **28 successes, one failure**.
  Successful cleanup does not make the failed scenario pass. Classification of
  malformed paste comes from the full log; compact signature fields were null.

The full Windows workflow remains failed. No shell fix or full-green result is
claimed. Native UI Find, physical interaction, performance and exhaustive
allocation failure qualification remain unrun for this slice.
This does not close group 5's separate frame/image materialization responsibility
or reopen AI, Liquid Glass/HDR, Leyline or the native-reader experiment.
