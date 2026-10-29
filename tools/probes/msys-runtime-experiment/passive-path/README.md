# Passive console-path diagnostic v1 — native qualification pending

This isolated implementation advances `../PASSIVE-PATH-PLAN.md`; that document
remains the original proposal. Nothing changes the production app, runtime lock,
uninstrumented `0006-refined-prefix.patch`, or existing qualification matrix.
No diagnostic DLL is approved for installation, redistribution or default use.

## What is implemented

- Exact-source generator, separate stock/refined observation patches and locked
  identities. Each patch inserts the same recorder and observation sites **after**
  the original stock/refined behavior is selected. It neither invents retirement
  calls nor changes a return status, native queue argument, signal or wait.
- Cold explicit DLL export attaches an unnamed inherited private mapping. The
  runtime reads no environment variable and opens no logging file/listener.
- Fixed 64-byte header plus 16,384 128-byte records (2,097,216 bytes total), no
  wrap. Reservation makes at most 32 CAS attempts; overflow means inconclusive.
  Eight preallocated thread contexts, read depth four and operation depth eight
  fail closed on exhaustion. Fork/process mismatch is unsupported/inconclusive.
- Numeric counts, event types, synthetic geometry, sequence/read/peek/operation
  IDs and thread roles only. No terminal bytes, paths, handles, addresses or
  environment values are emitted by the recorder. Separate reader logs contain
  only deliberately injected synthetic `ab`/`abc` bytes. Bounded numeric post-read
  fixture logging preserves EINTR evidence before a later alarm; that cold
  between-read I/O can perturb scheduling and is not runtime hot-path logging.
- Source observations include actual partial-copy counts, read-more branch,
  dispatch index/state, existing geometry-query result, retirement API request
  and actual result, input mutex outcomes, signal request/return, handler
  entry/return and master queue read/write. No extra console query is introduced.
- Separate owned controller/reader, exact provenance staging, fixed eight-trial
  runner, conservative binary parser and negative regressions. No retries.

## Observer limitations / blockers

**No native diagnostic runtime has been built or executed yet.** Local source
patch application, Python tests, Windows cross-compilation and synthetic parser
records do not establish Windows/MSYS ABI correctness or actual path coverage.

The recorder perturbs scheduling. Lock-free instruction assertions and a
cross-compiled object are not an async-signal-safety proof. Before native runs,
review actual GCC15 **runtime console.o** code/call graph and both DLL exports.
The standalone recorder smoke object has a different include context and is
supplementary only; build.sh preserves both disassemblies before source cleanup. `errno`, Win32
thread/error accessors and cold mapping calls need their actual native imports
reviewed separately; CRT startup imports are not evidence about the hot path.
No new hot allocation, file I/O, wait or mutex is intentional. Existing native
input-mutex waits/releases are wrapped once to observe their original result.
Attach/output mutexes are **not** instrumented. Master input queue observations
are explicit, but absence/overflow cannot prove absence of a competing action.

Signal attribution supports exactly one observed synchronous request on the
reader thread. Delayed handlers, competing requests, other thread handlers,
missing handler pairs or external signals are inconclusive, never forced to
synchronize. Handler context is stack-local and does not mutate interrupted
reader bookkeeping. Every record must be committed; overflow, context errors,
missing finalization and incomplete reads reject coverage. Even an unrelated
failed input mutex operation conservatively prevents a complete-path verdict.

The parser's complete-path v1 predicates cover refined changed-mouse i0 after
partial copy and refined positive-prefix i1 with a **separate** tail read. At
VMIN=2 the positive-prefix first read returns two bytes; final `c` requires a
new VMIN=1 read. Stock i0 is supported only for a complete returning execution.
Stock positive-prefix and resize predicates are intentionally unsupported and
remain INCONCLUSIVE. A stock loss/hang can leave useful raw numeric records but
is not promoted to a complete-path claim. A separate recorded-observations
summary counts complete BEGIN/COPY/RETURN(-1, copied>0, EINTR) chains and the
maximum copied count. It never assigns mouse/resize causality or overrides a
full-path INCONCLUSIVE verdict; incomplete/overflow traces yield no such count. Byte correctness and path coverage
remain separate; any unexpected bytes/queue residue is a failure even if a
trace independently records the target prefix. The runner cannot exit all-green
while the named stock positive-prefix predicate remains unsupported.

## Fixed native qualification plan (root publication/review only)

1. Use the existing isolated official MSYS setup, exact official source archive
   and private GCC15 verification from `../toolchain.sh`. Add official `python`
   to its declared helper closure; do not change compiler/source pins or weaken
   signatures, hashes, GNU++17, `-Werror` or original packaging tests.
2. Run `bash passive-path/build.sh ARCHIVE NEW_BUILD stock` and again `refined`
   with `EXPERIMENT_TOOLCHAIN` set to the private verified compiler. Each build
   checks the behavior source, applies only its locked diagnostic patch, checks
   the result and header, builds through the original makepkg recipe, requires
   both exports and preserves native object disassembly/symbols. Compare the
   two package/environment inventories. Each build gets a 40-minute step cap;
   if insufficient, preserve failure rather than rerun unchanged.
3. Review native emitted code, imports/exports and signal-path accessors before
   enabling the fixture step. Build reader once with private native GCC15,
   `-std=gnu11 -O0 -g -Wall -Wextra -Werror -static-libgcc`; verify imports with
   existing `check-reader-imports.py`. Build controller with native MSVC C11,
   `/W4 /WX /D_CRT_SECURE_NO_WARNINGS`. Their identities are captured by runner.
4. `python passive-path/stage.py BASE BUILD NEW_DEST` for each variant. It
   retains original source/recipe/patch/header/compiler provenance and labels
   both copies `passive-path-v1`, `trace_enabled=true`, default activation false.
   Never replace the build environment DLL or the original private bundle.
5. Run `passive-path/run.py --controller ... --reader ... --original ...
   --stock ... --refined ... --output NEW_DIR`. Fixed order: stock/refined then
   refined/stock, each with partial-zero and positive-prefix = eight trials.
   No schedule chosen from live traces. Queue-empty polling is only a scheduling
   hint, not evidence of copied bytes. No application/Zsh/native SSH/Codex runs.
6. Bounds: reader alarm 8 s, at most eight reads and 256 collected bytes;
   controller readiness 4 s, external queue hint 1.5 s, child wait 10 s, owned
   process reap 2 s and job drain 2 s. Outer runner 25 s plus 5 s reap, 64 KiB
   output tail; fixed matrix maximum 240 s plus small cold setup, workflow step
   cap 8 min. Private handle allowlist is console input/output plus mapping;
   child starts suspended and joins its kill-on-close job before resume.
   Finalization follows stopped owned writers, not scenario success.
7. Always preserve all eight outcome rows, source/toolchain identities, numeric
   trace blobs and fixed synthetic logs, even after failures. Exit 1 for any
   byte/fixture failure; otherwise exit 2 for any incomplete/unsupported path.
   Only complete numeric predicates produce PATH_OBSERVED; they do not imply
   production qualification or native interoperability safety. No DLL/package,
   user HOME/history, credentials, or generic terminal captures are uploaded.

## Source and notices

The complete official source archive, packaging recipe, utility correction,
original behavior patch, diagnostic patch/new header and compiler inventories
remain together in private build provenance. MSYS-linked code/source and GPL
notice/availability obligations remain release gates; this work does not
relicense the application or constitute legal qualification. No public binary
or source-availability assertion is made.

## Local checks

`tests.py` exercises format/predicate positive and negative synthetic records;
`runner-tests.py` rejects false success from incomplete owned/controller states;
`stage-tests.py` checks isolation/hash/diagnostic provenance. `source-tests.py`
requires the preserved exact stock and refined `console.cc` paths and verifies
patch identity, application and native API count/return preservation. Build
shell syntax and Windows cross-compiles are additional non-native checks.

## Build-only native workflow

`.github/workflows/msys-passive-build.yml` triggers only an intentional push
changing that workflow on **experiment/msys-passive-build**, guarded by the exact
branch and checked-out `github.sha`. It has read-only contents permission and
no dispatch/PR trigger. It runs portable tests, verified official source/private
GCC15 setup and the two diagnostic source builds. **No fixture executable is
built or run, and no shell bundle/application bootstrap occurs.** Runtime tests
are a separately reviewed phase, not an automatic continuation of a green build.

`review-evidence.py` requires both names in the actual PE export-name table and
instruction bodies in the real console.o disassembly. It emits a structural
report with `review_required=true`, `async_signal_safety_verified=false`,
`fixtures_executed=false` and `path_coverage=NOT_EXECUTED`. These checks cannot
certify accessor/call-graph safety. A native reviewer must inspect the handler
and transitive recorder callees, atomic instructions, errno/Win32 accessor
implementations, and the instrumentation sites in both variants. The native
standalone recorder object is supplementary only. GNU objdump old/new export
formats are covered; a local cross-object format check is not native evidence.

The job cap is 150 minutes (40 minutes per source build). Always-run collection
copies only enumerated source/provenance/disassembly/build-log files to a new
review packet: maximum 32 MiB per file, 128 MiB total. Aliases/reparse points,
changed sizes or excess sizes are rejected, explicitly recorded and fail the
collector rather than silently truncating evidence. Missing build products are
listed, and absence of either structural report is explicit. Upload includes
only this bounded packet, never a DLL/object/package, keyring, HOME/history or
runtime terminal capture. Failure evidence stays available without reruns.

Because binaries are deliberately not uploaded, this build-only artifact cannot
be reused as a runtime image. After review, a controlled runtime phase must
rebuild from the same pinned sources/observer/toolchain or a reviewed correction;
never substitute guessed artifact binaries or claim successful runtime behavior
from this static phase. That necessary separation is not permission to retry an
unchanged failed matrix or broaden the behavior patch.
