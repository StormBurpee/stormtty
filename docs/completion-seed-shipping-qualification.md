# Opt-in shipping completion seed qualification

Branch: `diagnostic/completion-seed-shipping`, based on current local main
`202dd426` (remote main equivalent `24a36e1`). No runtime pin or default feature
activation changes. `STORMTTY_EXPERIMENTAL_COMPLETION_SEED` defaults **OFF**.

## Actual application path

With that native build option explicitly ON, the existing managed session factory
attaches `completion_seed_launch` to **every** validated managed launch: fresh,
warm and existing custom state. The hook is confined to `new_session`; palette/UI
code is untouched. Native controller initialization performs the existing lease,
private state and build-anchored runtime ACL/content checks before creating the
child. Existing config/completion/cache state skips adoption, not user settings.
Contenders initialize secure completions without loading/writing shared dumps.
Normal compaudit and eager first Tab remain enabled. See the lifecycle document
for abandoned-owner handling and limits on arbitrary same-user cache writers.

The verifier now lives in the Windows platform implementation, not a probe-only
copy. The factory's trusted identity is a generated C++ constant, never an
ambient environment variable or a checksum read beside an untrusted seed.

## Consistent build and install identity

The ordinary pinned runtime stager runs first. Build-only native Zsh then generates
its pristine text seed from that **final staged tree**, including the staging
receipt. A generated trust header is a prerequisite of compiling the native
factory. There is no runtime Python dependency, launcher download or user setup
step. Native generation is rejected in cross builds.

The build helper refuses unowned output directories and symlink/reparse outputs,
checks real runtime content on no-op builds, and repairs an owned missing header
or seed. Changed content rebuilds the seed/anchor. With the option ON, installation
copies the same final runtime tree and seed. With the option OFF, existing build
and installation behavior is unchanged. This avoids the earlier mismatch between
`_shell_source` and a final tree containing an additional staging receipt.

## Bounded native route

`.github/workflows/completion-seed-shipping.yml` runs only on a push changing that
workflow on the exact branch, with the same exact-ref job guard and checkout SHA.
Root owns one intentional publication; no retries-until-green or automatic PR
workflow. Existing native CI workflows/gates remain unchanged.

One engine cache/build and one native application build produce the application,
startup probe and lifecycle probe. Build and install directories are authored
beneath LOCALAPPDATA, modelling a per-user installation rather than trusting a
possibly broadly writable CI workspace. Actual native ACL checks remain enabled;
no existing directory permissions are repaired or weakened to make adoption pass.

The job verifies:

1. Build helper no-op and owned deleted-header repair; anchor bytes unchanged.
2. Relocated portable install with spaces/Unicode: every runtime file/directory,
   seed and app/helper binary matches the build outputs.
3. Native lifecycle probe: thread/process contention, private event, custom-config
   suppression, child-exit release, abandoned-owner handling and ACL policy.
4. Installed application's real settings smoke, exercising NativeWindow's factory.
5. Uninstrumented shipping factory measurements: one build-tree inbox candidate,
   then installed baseline/candidate for inbox and packaged providers. Each fixed
   condition has 3 fresh-private-state + 3 warm samples, with no repeats.

The measurement executable uses exactly the app's factory and the installed
console helper. Its only authored user config is the existing non-replacing ZLE
observation hook; for a fresh candidate it is created **after** the factory's
freshness/adoption decision but before child creation. This avoids misclassifying
the observer as pre-existing user customization. The runtime is not modified or
source-profile-instrumented. First-Tab and module assertions still run.

For shipping samples, verification/publication happens on the controller **inside**
`launch_to_zle_ms`. The total is therefore **state_prepare_ms + launch_to_zle_ms**.
`verification_publish_ms` is an included subinterval, not an extra cost to add
again. Candidate readiness requires both the ZLE marker and native initialization
acknowledgement. All three fresh candidates must actually adopt; warm samples must
skip adoption. Failed adoption cannot be counted as a successful seeded sample.

The driver bounds the lifecycle probe to 120 s, installed app smoke to 120 s, and
five measurement children to 240 s each, all with owned process/reap checks. The
workflow adds a 1,800 s outer driver timeout and always uploads allowlisted numeric
logs/hash identities plus the authored settings capture. No executable seed,
runtime tree, HOME/history or raw profiler output is uploaded. There is no speed
pass/fail threshold.

## Evidence limits before this run

The earlier native diagnostic showed fresh-private-cache totals improving from
roughly 4 s to 1.0–1.3 s including hashing. That result does **not** qualify this
new shipping lifecycle/ACL implementation or its extra costs. Local validation
includes pinned-module semantic tests, build-helper/parser/contract tests and
Windows cross-compilation (with an authored dummy anchor solely for compilation).
Actual native build graph, ACL compatibility, lifecycle, installation and timing
results remain pending root publication. Default enablement is a separate review
and is not authorized by preparing this branch.
