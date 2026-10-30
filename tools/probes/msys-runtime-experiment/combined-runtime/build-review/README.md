# Deterministic build-only linked-review preparation

Source preparation only. Nothing here has been published, downloaded, built
natively or executed against an experimental runtime. `workflow.yml.template`
is deliberately outside `.github/workflows`. Once explicitly published by root,
it triggers only on pushes to `experiment/msys-combined-build-review`, never PRs.

## Two clean builds; no candidate execution

The template runs two independent Windows jobs with no reused candidate artifacts
or build cache. Each requires `C:\stormtty-build-review` to be absent, exports the
exact checked-out commit into its fixed `source` directory, and builds:

- A `copied`: stock + unchanged 0007 + v2 observer.
- B `combined`: stock + unchanged 0007 + unchanged 0006 + the same observer.
- The reader (against the trusted installed import library) and controller.

Source archive, recipe, source/behavior/observer identities, GCC15.3.0 package
pins, GNU++17 dialect, optimization and warnings are unchanged. The original
trusted toolchain helper still performs its unrelated compiler smoke checks.
Candidate DLLs are never installed, added to PATH, or copied beside running tools;
candidate readers/controllers are never launched. The pinned package recipe has
no candidate-executing `check()` stage. All 17 pending runtime-matrix cases remain
pending; this preparation neither executes the 15-case stage nor qualifies it.

## Explicit metadata controls

The separately reviewed controls implement only proposal items 1, 2 and 4:

- The retained `cygwin-3.6.11` tag object
  `9408d4c6c8273c19d19b5a788e73155fff2e4c01` peels to upstream commit
  `867eb7748e6e5a0e8430ce9f46af27b4b1e8f204`, whose verified committer epoch is
  **1790890720**. The build checks all three identities from the archive's Git
  objects, sets that `SOURCE_DATE_EPOCH`, and carries it explicitly through
  makepkg. `00eb8435b1c498e9211c7a7cee01584d76bd5f21` remains the separate,
  unchanged MSYS package marker; it is not misrepresented as this Git commit.
- The source, compiler prefix and variant build paths are canonical and checked.
  Both compiler entry points snapshot source before compilation and reject drift
  afterward. Actual compiler/linker/tool binaries are hashed before and after;
  repeated builds must match exact hashes and configuration, not version labels.
- The runtime already conditionally uses deterministic archives and zero PE
  timestamps when the epoch is set. The reader explicitly uses
  `--no-insert-timestamp` after verifying the selected linker's help advertises it.
  This option is documented by [GNU ld](https://sourceware.org/binutils/docs/ld.html).
- The controller uses `/Brepro` for compile/link, preserves `/W4 /WX`, adds linker
  `/WX`, and requires a REPRO entry in the resulting PE headers. Unsupported
  options or missing output evidence stop the build; native tool acceptance has
  not been established here. Compiler/linker help, hashes and exact flags are
  retained for review. The hosted MSVC tools are not assumed byte-identical merely
  because both jobs use the same runner label.

No debug-prefix mapping, behavior source changes, recipe patching, timestamp
stripping, post-link normalization, relaxed hash comparison or binary rewriting
is performed. Debuglink CRCs and generated version text remain visible. These
controls are candidates for reproducibility, not proof that two builds match.

## Cold evidence and exact comparison

`inspect-built.py` parses PE/object bytes without loading code, binds actual
handler/emitter object bodies to linked DLL ranges, and records reader imports,
source identities, tool hashes, configuration and full binary hashes. It requires
fresh independent review and never creates execution approval fields.

`collect.py` exports a fixed, bounded UTF-8 text/source allowlist, rejecting
links/reparse aliases, binary-looking contents, oversized files and concurrent
size changes. DLLs, EXEs, object files, packages/source archives and HOME are never
exported. The template uploads only this collection. Failed builds retain bounded
available diagnostics; absent evidence cannot establish success.

A separate Linux comparison job downloads **only those text collections**.
`compare.py` checks each file against its collection hash, rejects incomplete or
invalid evidence, requires independent repeat markers from the same workflow
attempt/commit, verifies internally consistent source/tool snapshots, and compares
full corresponding DLL, reader, controller, configuration and source identities.
It reports `EXACT_MATCH`, `MISMATCH`, or `INCONCLUSIVE`. All outcomes explicitly
retain `approved_for_fixture_execution=false` and `native_qualified=false`.
Synthetic comparison tests are schema regressions, not fabricated native evidence.

Private binaries remain ephemeral. A separately authorized future execution job
may rebuild and compare against fresh independently reviewed full hashes. Any
mismatch must stop before launch; never rewrite a review to fit new binaries or
reuse a v1 approval. A cold match does not replace independent linked handler,
import, callback, rendezvous and controller ownership review. Full matrix and
uninstrumented application qualification remain separate gates.

## Portable validation

```
python3 build-review/tests.py --source-root /path/to/retained/prepared -v
python3 build-review/compare-tests.py -v
```

These tests cover exact retained epoch/source controls, drift, collection limits,
fixed branch-restricted build-only workflow, and synthetic comparison mutations. They do not
prove native compiler acceptance, ABI correctness, reproducibility, runtime
cleanup or physical UI behavior.
