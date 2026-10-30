# Proposal: isolated 15-case component stage

**Local preparation only. Do not publish, dispatch, or execute this proposal.**
The companion `component-stage.workflow.yml.template` is not an active workflow.
Root has authorized local preparation of an intentional exact-branch push trigger
for the narrow 15-case component stage. This is not publication or execution. Root
must review the finished workflow, identity/authorization gates and cleanup, then
authorize the exact publication and separate trust record before any candidate may
run. No build-only approval is treated as execution approval.

## Scope and evidence carried forward

The proposal covers only the existing 15-case console-record component stage:
fixed opposing A/B orders, 60 scheduled trials, no adaptive ordering or retries.
Infrastructure or coverage uncertainty stops the remaining plan. Witnessed valid
behavioral failures remain failures while the fixed opposing variant can still
run. All 17 unsupported full-matrix cases remain **INCONCLUSIVE**. Neither a clean
component result nor a completed schedule qualifies a production runtime.

Cold run `40999881932` succeeded with two independent clean builds, exact equality
between repeats and exact equality to the immutable prior identities originating
in run `40994103183`. The latter run's original comparison remains INCONCLUSIVE;
its separately reviewed offline parser correction is not rewritten as a CI pass.
Fresh linked-text review verified bounded accessor thunk/IAT/export and reader
export-name/pointer-pool evidence. Raw candidate PE files were not exported for
local rehashing: the evidence chain is the build extractor plus verified collection
hashes. The reader's UTF-16 `GetModuleHandleW` module-name literal remains source-
inferred; the evidence does not claim those literal bytes were exported. OS TLS
and accessor async-signal contracts remain explicit assumptions. Native cleanup,
rendezvous liveness and the full runtime matrix have not been demonstrated.

## Proposed workflow and authorization boundary

The eventual workflow path would be
`.github/workflows/msys-component-stage-review.yml`, byte-identical to the
inventoried template. It has only an intentional `push` trigger filtered to
`experiment/msys-component-stage-review`, with a matching job-level event/ref guard
rejecting every other event or ref. There is no dispatch-tool dependency. Read-only contents
permission and `cancel-in-progress: false` remain in place. An explicitly published
commit on that exact experiment branch would trigger the proposed job. No
pull-request, main, app smoke or promotion path is added.

The job reads the fixed file
`C:/stormtty-build-review/source/tools/probes/msys-runtime-stage-trust/authorized-review.sha256`
using `Get-Content -Raw` and `.Trim()`. It passes that value through an environment
variable to the wrapper, never interpolates it as shell code, and never generates
an approval or a replacement expected hash in CI.
The wrapper must validate the 64-hex format and record hash, all source inventories,
the active-workflow/template byte identity and the explicit authorization field
before staging or launching any candidate. There is no permissive default or
fallback record. The trust record sits outside the recursively hashed executable
source tree at `tools/probes/msys-runtime-stage-trust/authorized-review.json`; the
separate `authorized-review.sha256` file binds its exact bytes. The record pins
the full current inventory, including the wrapper and workflow template. This
avoids a self-referential hash without omitting executable dependencies.

This is one new isolated Windows rebuild, not a claim of new independent repeat
reproducibility. It reuses the exact deterministic recipe, archive/package pins,
canonical paths, fixed epoch, warning settings and toolchain controls already
reviewed. Both A and B, their reader and the controller are rebuilt. Before any
launch, the wrapper must compare their full hashes, fixture-source identities,
exact compiler/tool/configuration identities and linked-review evidence against
the immutable reviewed baseline. Missing evidence or any mismatch stops the stage;
expected hashes must not be normalized or silently updated to fit a new build.

Before initializing the compiler, a read-only check verifies the actual Windows
`git archive` extraction bytes of `tools/run-owned-process.py` and
`tools/ci-msvc-env.ps1` against their immutable prior native SHA256 identities. It
requires positive CRLF counts and no lone LF, and logs each hash/count. This is
provenance-only reconciliation to the exact previously reviewed bytes, not a
behavior change. CI performs no normalization or rewriting; a mismatch stops
before rebuilding or executing a candidate.

The native runtime gate rechecks both variants before creating a child or staging
a DLL. The proposed record binds exact evidence and preserves the stated evidence
limits; it cannot convert source inference into measured binary evidence.

## Process ownership and failure handling

The existing launcher and native controller remain unchanged. The launcher uses
strict owned-process execution (25-second timeout, 5-second reap budget). The
controller creates the reader suspended with an explicit inherited-handle list
and mandatory job-list creation attribute, then resumes it once. There is no
post-creation job-assignment fallback. Job ownership, kill-on-close and bounded
cleanup/reaping apply before shared trace/result mappings can be inspected.
A failed exposure is not retried; no input drain, signal suppression or timeout
relaxation is proposed.

The wrapper uses only canonical new paths beneath `C:\stormtty-build-review`:
`cold-stage-text` for the cold collection and `component-stage` for stage output.
Gate failure produces no stage directory; workflow logs retain its reason and
available cold text remains uploadable. The stage process's exit status is
preserved. In particular, the existing runner returns **2** for pending full-matrix
coverage, even if implemented component cases do not fail. The workflow must not
turn that result green with `continue-on-error` or status conversion. A witnessed
component failure returns **1**. The proposed 120-minute job deadline is not a
replacement for the strict per-trial process bounds.

## Artifact boundary

Only the fixed cold text collection and the new bounded component-text collector's
output may be uploaded. The latter selects results, fixture/assessment/launcher
text and decoded trace JSON after cleanup. It must not upload the raw stage
folder, `private-fixtures`, `trace.bin`, DLLs, EXEs, objects or packages. The legacy
stage collector is not used because it permits raw trace output. Collection runs
on failure where possible, but collection success does not imply stage success.

## Decision still required

Independent review must assess the finished wrapper, exact-branch push workflow,
trust record, exact linked gate and cleanup together. Root then decides whether
to authorize this bounded component stage. This document and template themselves
neither authorize execution nor activate any runtime or application default.
