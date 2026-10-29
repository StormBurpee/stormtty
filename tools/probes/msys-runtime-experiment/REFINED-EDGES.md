# Remaining refined external-edge matrix: bounded new source run

## Why previous binaries cannot be reused

Read-only GitHub artifact inventory for run40881120249 contains exactly one
artifact12903300924, ZIP SHA-256
`12d57f062da56955f4489ddb3ed930bad69d30391f502c2c430d643cc79df37a`.
It was independently downloaded/hash-verified and inspected: provenance and logs
only, no runtime DLL, native reader/injector EXE or package. The original workflow
explicitly excludes those binaries and saves no runtime build-output cache.
Its observed DLL hashes are identities, not recoverable byte payloads. Therefore
an exact prior binary reuse workflow is impossible with the retained artifacts.
Do not replace it with an official unmodified DLL, an unrelated cached build,
reconstructed binary assumed identical, or a successful-run-only source choice.

## Narrow new workflow

`.github/workflows/msys-refined-edges.yml` is push-only on exact branch
`experiment/msys-refined-edges`, only when that workflow path changes. Root owns
publication. No dispatch/PR route, remote mutation or unchanged workflow retry
is performed here. The current full qualification workflow and evidence remain
preserved; this new run is explicitly the previously unexecuted external matrix.

Use normal verified source rebuild, not guessed binary reuse:

- Same complete official runtime source SHA, strict signed/hash-pinned private
  GCC15.3.0, GNU++17, common utility-only patch, strict `-Werror`, and original
  preparation/source before-after hash checks as the full workflow.
- Same braced refined patch SHA
  `1afc3483da5dcc8848f336619f858c0ffcfbef608d1446464e6566135b2aff66`.
  No runtime semantics/source pin changes. Build stock/refined once each,40min
  each, and enforce identical package inventory and all non-DLL bundle bytes.
- Same reader/injector source and source hashes, same private per-variant copies,
  exact DLL/reader hash equality, MSYS/system import allowlist, owned Job and
  read/reap bounds. Case-safe system-root lookup fixes the proven prelaunch
  orchestration failure; it is not a runtime change.
- Same6 scenarios,2 prespecified stock→refined/refined→stock paired orders,
 24 fresh launches. No scenario/threshold/order alteration, no repeat-until-pass.
  Each outer launch20s plus5s reap,64KiB capture,12min matrix step. Reader alarm8s,
  injector child wait10s plus bounded cleanup. Overall job180min covers explicit
  step ceilings (and overhead), without pretending OS setup itself is atomic.
- Skip Ghostty/app build and managed/raw/settings workloads that already produced
  complete retained evidence; this is not a replacement success classification
  for those strict VT-off control failures. No packaged ConPTY download needed:
  the external injector owns an ordinary fresh Windows console, not ConPTY.

## Required interpretation and preservation

Initial native results.json declares planned24 and an empty trial list before
launch. Every independent case runs after earlier case failure. Preserve all
reader/injector JSONL, launcher tails, strict timeout/reaped/truncation/cleanup
metadata and source/toolchain/runtime identities. Failure is never erased by a
later pass. Upload provenance/logs only; do not newly distribute DLLs/EXEs,
packages, HOME/history, captures or keyrings.

All external `exact_path_verdict` values remain **inconclusive**, even if exact
byte conservation succeeds. Queue emptiness cannot prove copied_chars, changed
mouse discovery, actual retired count, or signal/lock ordering. A positive result
would qualify these external scenarios only. The passive internal-observation
plan is separate, unimplemented, and cannot be inferred from this workflow.
No default change, production hook or public runtime release is authorized by
this matrix. Existing matching-source/notices and broad compatibility gates stay
open. If build/fixture failure blocks execution, record zero or partial coverage,
not24 passing cases or an observed runtime regression.

## Local verification

All75 portable tests pass (existing72 plus3 edge-workflow scope/bounds tests).
YAML and embedded Bash syntax parse, fixed reader/injector/runtime patch hashes
are unchanged, and git diff whitespace checks pass. This new workflow has not
run natively. PASSIVE-PATH-PLAN.md is design-only and adds no instrumentation.
