# One-shot paired Session disposal observation

Diagnostic branch only. Neither the overlay nor this workflow is a production fix
or a replacement qualification gate. Root publishes after review.

The workflow checks out immutable baseline 9b9ce10f (tree da6e89b8) and candidate
093df896 (tree 19a36327). Both use the same verified pinned engine. A common patch
adds isolated Process/Session libraries and a diagnostic fixture, OFF by default.
Shipping sources on the control branch are untouched; the patch is applied only
to the two disposable native runner checkouts. Each complete fixture executes
once, in baseline/candidate order, with unchanged 10-second bounded waits,
30,000-line workload, assertions, first-failure short circuit, flushes and cleanup.
The owned runner retains the existing 90-second outer deadline for each case.
Baseline failure does not suppress candidate. No retry or rerun-to-green.

The failure reporter remains noexcept with the existing fallback. Metadata adds
no terminal payloads, paths, process identifiers, environment values or free-form
provider errors. Mapping setup failure means unavailable diagnostics, not a
transport failure. The original wait result remains authoritative even if state
advances before sampling. Producer stage 4 is function return, not OS exit.
Counters/phases are independent observations, not a barrier or proof of a hang.
The extra SYNCHRONIZE-only process handle and counter stores can perturb timing.

Read/callback/fed/queued counters distinguish delivery boundaries. OS process
signaled plus controller phase distinguishes teardown from unfinished producer
work. Actor final-capture/final-publish/final-effects/retire-live-batch/
publish-process-result/destroy-terminal/retire-commands labels distinguish
finalization and retirement boundaries without instrumenting terminal ownership.
An unavailable transport after Session retires it is expected. A single phase
only locates the sample. Both passing produces no failure snapshot and leaves
the original cause unresolved. One-side failure is not causal proof.

Artifacts retain both source commit/tree identities, overlay and instrumented
diff hashes, dependency-lock hash, build logs, owned-process metadata, exact
attempt counts, output and statuses. Setup failure can prevent execution and is
reported separately; neither a build failure nor outer timeout is a clean pass.

Local preparation checks: Windows-target GNU syntax for Process/Session/fixture;
macro-OFF whitespace-normalized preprocessing equality against original sources;
patch applicability for both local trees; Python syntax and workflow structure.
These checks do not claim MSVC linkage or Windows runtime qualification.
