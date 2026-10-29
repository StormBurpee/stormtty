# Isolated shell responsiveness workflow

Triggered only by a push to `diagnostic/typing-scoped`; it does not depend
on a workflow already existing on the default branch. No PR/default production
gate, shipped setting or provider pin changes. Root owns branch publication.

The integrated diagnostic is based on current main `202dd42`, preserving its
native workflow, provider/runtime pins, AI-off defaults and blur implementation.
It includes version-2 compact prelude capture with actor-ordered arm/freeze;
startup output no longer consumes detailed typing-phase record slots. The driver
requires native exact replay success for each provider, not merely a trace dump.

It restores the same pinned Ghostty cache key as native CI, builds only the normal
managed probe, isolated trace/replay evidence targets, startup probe and their
required libraries/console sidecar, and stages exact pinned shell/provider assets.
It records source/actual-payload hashes without persisting user paths or state.

Per provider, run exactly one uninstrumented normal managed control, one traced
run and one exact replay, preserving each failure independently, never retrying.
The startup driver independently runs three fresh-state and three warm-state
samples per provider in a verified disposable attribution fixture. Fresh state
is not a cold OS cache. Instrumentation perturbs timing; neither trace nor source
profiling establishes physical display latency or a visual fix.

The owned runner bounds each control/trace to 120 seconds, replay to 45 seconds,
and captured output to 1 MiB. This capture bound may be smaller than the trace's
maximum hex serialization; any truncation is explicitly invalid/inconclusive,
never accepted as complete. The source driver separately bounds its two native
invocations and sanitizes fixed numeric/function records. No stage retries.
Exact completed input fingerprints must match for the paired comparison to be
marked comparable; protocol-response differences must be investigated, not
normalized away. Successful exact replay does not prove identical display timing.

Always-upload artifacts contain independent build/validator status, source and
payload identity, sanitized numeric control/replay results, synthetic-only trace
records, and sanitized source-attribution measurements. Do not add user recordings,
frames, configuration, history, environment dumps or arbitrary profiler output.
Failed package signature/hash checks block native cases requiring that dependency.
One provider's runtime/typing/profile failure does not suppress the other provider's
case; typing failures do not suppress the separate source attribution step.

Portable driver tests cover strict expected records, no retries, independent
failures, truncation/timeout/cleanup rejection, allowlisted output, and exact push
scope. They are not a native workflow execution. Native CI remains root-owned.
