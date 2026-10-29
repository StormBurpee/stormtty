# Pristine completion seed: feasibility, not a production change

## Motivation and finding

Native run `40819617232` attributed fresh initialization to 639 `compdef`
calls (2.47–2.54 seconds inclusive), within 3.38–3.46 seconds of `compinit`.
This supports investigating a pre-generated dump, **not** a measured speedup.
The separate fpath-index candidate is unchanged by this diagnostic branch.

A text dump can in principle avoid fresh registration while retaining ordinary
`compinit -i -d` and its eager first-Tab setup and `compaudit`. Shipping a dump
is substantially less invasive than modifying upstream registration semantics.
It is not safe to copy an arbitrary build cache into user state, however.
No loader, runtime pin, staging default, or native host change is made here.

## Audited upstream behavior

The staged functions inspected were:

| Function | SHA-256 |
| --- | --- |
| compinit | `e784ec2f3089d12dbeb61b4f058df00d4cffbaab04b77ea342fbfeb1d1834501` |
| compdump | `474a41680c54eb1d75ed24259a9c2fdd9fcee972b2662451041ccab3dacbb40f` |
| compaudit | `d41820035eaa692eea358900f6d9f0882055191a15d1cffae654c0ac1e9b4d24` |

* `compdump` writes `_comps`, `_services`, pattern maps, `_compautos`, selected
  underscore ZLE widgets/key bindings, name-only autoload registrations and
  `_comp_assocs`. It does not serialize `autoload -r`'s resolved absolute paths.
  Its temporary filename contains host/PID; those are not dump contents.
  This supports relocation, but does not prove arbitrary plugins produce
  path-free or environment-independent maps. Generate before user code.
* Normal `compinit -i` calls `compaudit` **before** considering the dump. Its
  validity check is version and completion-file count, not hashes, mtimes,
  directory order or individual filenames. Excluding insecure files can change
  that count and force regeneration. Do not use `-C`, `-u`, or disable compfix.
* OMZ additionally requires a revision line and `#omz fpath: $fpath`, with the
  exact effective ordered, absolute fpath. This includes relocated runtime
  directories and the private cache/completions directory. A build's metadata
  cannot be reused verbatim. OMZ removes a mismatched dump before compinit.
* Source loading may prefer a newer adjacent `.zwc`. A shipped compiled dump
  creates an unnecessary architecture/version/mtime/relocation ambiguity.
  Ship **text only**; require neither destination dump nor `.zwc` exists before
  adoption. Let unchanged OMZ compile its adopted private dump. Do not alter its
  freshness/lock behavior or claim compilation disappears.
* The managed loader resets inherited fpath, initializes OMZ, then sources the
  opt-in user configuration. Existing user config overrides remain later and
  must remain unchanged. A user adding fpath there still needs the same explicit
  completion initialization they need today; seeding must not pretend to
  automatically detect future shell mutations. Any additions already present
  when deciding adoption must reject the pristine seed, including private cached
  completion files. Seed eligibility does not extend stock compinit's generic
  cache validity promises after arbitrary user code runs.

## Executable semantic evidence

```
python3 tools/probes/completion-seed-semantics-tests.py \
  --functions-root PATH/TO/STAGED/usr/share/zsh/functions/Completion -v
```

Six tests passed using the staged functions above and local POSIX Zsh 5.9:

1. Moving a function directory retains the map from the unchanged path-free
   dump; zprof still reports compaudit, without fresh compdef registration.
2. Same-count completion content replacement leaves a stale map: negative proof
   that file count alone is insufficient.
3. Same-count duplicate-function fpath precedence reversal also leaves a stale
   map: exact directory order is required.
4. Making the completion directory insecure causes audit exclusion before dump
   loading and removes that registration.
5. Adding another completion forces regeneration.
6. A newer compiled sibling overrides altered plaintext until removed.

Tests use authored temporary completions, copy only four upstream functions,
never source user configuration, and bound each child to 20 seconds. They are
**POSIX semantic evidence**, not MSYS Windows timing, native ACL evidence, or a
complete shipped-module first-Tab qualification. Windows execution is explicitly
skipped. No native seed has yet been generated or benchmarked.

## Bounded native prototype contract

A next diagnostic fixture (not a production default) should:

1. Verify the freshly staged bundle against its full inventory and exact lock
   identities. In an owned native process with authored private HOME/state,
   generate the dump using the exact shipped Zsh executable and modules, same
   loader options/keymap and effective fpath, stopping immediately after the
   ordinary secure compinit/compdump operation, before OMZ libraries, tools or
   user configuration can add state. No inherited functions, plugins, history,
   compiled dump or startup files. Fail if compaudit excluded anything.
2. Record Zsh version/build and executable hash, addon/tool/loader identities,
   exact ordered logical fpath (runtime-relative tokens plus one empty-private-
   completions token), every relevant directory entry and file content hash,
   and seed text hash. Include duplicate directory precedence, empty directories
   and compiled function archives in identity; reject links/reparse points and
   unrecognized paths. A conservative full bundle fingerprint is acceptable.
   Bound directory/file counts and total bytes; overflow means ordinary startup,
   not relaxed verification. Never treat mtimes as content identity.
3. At adoption, validate the **actual** staged tree against that identity, not
   just trust matching JSON strings. Match the current pre-compinit effective
   ordered fpath, including modules and empty private completions. Any drift,
   version mismatch, user addition, unavailable validation or existing cache
   falls back to unchanged initialization. Rechecking every bundled byte might
   eat the gain; measure it separately rather than assuming it is free.
4. Only adopt into host-validated private state, with no existing dump, compiled
   sibling, symlink/reparse point or conflicting startup. Use exclusive native
   file creation/atomic no-replace publication and inherited protected DACL;
   never overwrite user state. Revalidate ancestors/handles rather than a
   check-then-follow shell copy. Concurrent startup must either win once or
   abandon seeding and use existing normal behavior. Seed errors are optional
   optimization failures, not terminal startup failures.
5. Produce current OMZ revision/fpath metadata from validated runtime values;
   append as literal comment data (reject newline-containing paths), not shell
   interpolation/evaluation. Keep the seed body byte-identical. Never rewrite
   strings throughout executable dump text. Preserve private umask and do not
   ship/copy `.zwc`. The native host currently does not offer complete per-launch
   runtime content verification; adding only a fingerprint environment variable
   would not fulfill step 3.
6. Run unchanged secure compinit/OMZ and preserve first-Tab, suggestions, history
   and later user overrides. Qualify both inbox/packaged providers with baseline
   and seeded fresh private state, 3 samples each, plus 3 warm each; separately
   record integrity validation, seed installation and source/ZLE times. Check
   corrupted seed/inventory, equal-count edits, order changes, insecure dirs,
   version mismatch, user completion additions, stale compiled siblings,
   relocation, concurrent creators and initial interactive completion behavior.
   Any native timing claim must include validation/installation overhead.

## Decision

**Feasible upstream semantics, incomplete adoption trust/transaction design.**
The small tests establish the crucial fast-path and rejection behavior without
patching compinit. They also expose why a manifest-only seed copy is not yet a
sound production optimization. The next implementation should be a deliberately
isolated native generation/adoption fixture following the contract above. Do not
merge a loader seed hook or enable a default based solely on these POSIX tests.

## Implemented diagnostic prototype (not enabled in application launches)

The optional `managed_shell_startup_probe` now has these CI-only modes:

* `--completion-seed PACKAGE RUNTIME [PROVIDER]`: 3 fresh + 3 warm normal-source
  samples. This is a developer measurement probe, not an end-user startup step.
* `--completion-seed-profile PACKAGE RUNTIME [PROVIDER]`: the same with the
  existing authored source-profile fixture and compaudit/compinit attribution.
* `--completion-seed-checks PACKAGE RUNTIME`: 8 bounded native adoption checks.

`prepare-completion-seed.py` runs only on a Windows build/qualification machine.
It creates a new external package from the exact runtime's Zsh and secure
compinit, with authored empty HOME/cache, no rc files and managed fpath/keymap.
It verifies inventory hashes and records every actual file and directory,
version, pristine dump hash and ordered logical fpath. It rejects compiled loader,
reparse entries and changes during generation. No Python is used by the shell or
native adoption path. The seed package/identity is trusted build output in this
prototype, **not an authenticated arbitrary download format**.

The native probe independently reconstructs the managed fpath ordering and
hashes actual runtime contents with Windows SHA-256 before each **fresh** adoption.
The check is once per adopted private state, not every warm shell. It holds runtime
ancestor/object handles against replacement and file writes during verification
and publication. Limits are 65,536 entries, 1 GiB total content, 256 fpath entries,
8 MiB identity and 1 MiB text dump. Rejecting a changed bundle, file/order/version,
seed, existing cache/compiled sibling or private completion falls back to ordinary
startup, not a relaxed cache check. Complete-bundle hashing is intentionally
conservative; its cost could erase the benefit.

Private publication reuses the native state owner/DACL/tree checks, holds ancestor
and state handles, creates protected cache directories, requires empty private
completions, and uses exclusive temporary creation plus handle-based atomic
**no-replace** rename. It refuses all preexisting zcompdump variants and a competing
seed temporary file. It never changes config/history or overwrites a cache. Native
checks cover initial adoption, repeated adoption, existing text/compiled dump,
custom completion, competing temporary file, corrupted seed, wrong content hash
and reordered fpath. The pristine seed remains text; unchanged OMZ creates any
private `.zwc` and still invokes secure compaudit/compinit before user configuration.

The lifecycle is deliberately restricted to the probe's owned fresh fixture:
**no shell starts in that fixture until publication returns**. The native helper
is not called by application launch. Before production use, coordinate ordinary
shell creators as well as seed adopters: no-replace publication protects the text
file, but independent user/shell creation of a compiled sibling after its absence
check is not a transaction. Likewise an arbitrary same-user writer is not a
security boundary supplied by this diagnostic. Do not broaden this helper into a
production launch hook without solving that lifecycle.

### Bounded native measurement for root review

Build the existing optional target, then run (under a 25-minute outer timeout):

```
python tools/probes/run-completion-seed.py --runtime STAGED_RUNTIME \
  --probe BUILD/managed_shell_startup_probe.exe --provider PACKAGED_CONPTY \
  --output NUMERIC_ARTIFACT_DIRECTORY --scratch OWNED_SCRATCH
```

The driver copies/verifies an authored source-profile fixture, generates its seed
under an owned 120-second process bound, runs native adoption checks under 240
seconds, then runs baseline/seeded 3-fresh/3-warm measurements for each provider
under four 240-second bounds (+10 seconds reap each). Provider order is baseline
then seeded for inbox, seeded then baseline for packaged. No repeat-until-green
or timing threshold. It requires successful adoption for all three seeded fresh
samples, none on warm samples, and independently valid stage/audit measurements. Each sample also exercises
first Tab against an authored unique filename before its module assertions.
Only allowlisted numeric/name logs and hash-only source identity are retained;
seed/raw profile/runtime/private state are disposable. The CI wrapper must retain
its outer timeout for preparation/copying as well.

`verification_publish_ms` measures complete native verification plus private
publication. Include that **and** `state_prepare_ms` plus `launch_to_zle_ms` when
comparing fresh totals; the ordinary startup figure alone excludes seed cost.
Source intervals are instrumented, not uninstrumented interactive latency claims.
Existing Windows source attribution is the motivation only. This prototype has
cross-build/portable tests but no Windows execution or speedup evidence yet.

### Isolated integration route

Branch `diagnostic/completion-seed-native` integrates the required diagnostic
source profiler and seed prototype atop `5c640df`; existing workflows, loader,
addon patches and runtime defaults are unchanged. Its only new workflow is
`.github/workflows/completion-seed-native.yml`. One root-authorized push changing
that workflow on the exact branch starts qualification; source-only pushes do not.
There is no dispatch/PR trigger and no retry-push-until-green policy. Root owns
publication and interpretation. No need to merge this experiment into main.

The driver deliberately relocates the runtime beneath `runtime with spaces é`;
the private state already uses spaces and Unicode. This exercises the exact OMZ
joined-fpath metadata rather than assuming paths contain no spaces. The native
check phase additionally corrupts an owned seed copy and actually starts the
normal inbox shell after rejection, requiring secure audit attribution, first-Tab
and all module assertions. Both measured providers retain unchanged baseline and
seeded source-profile runs. Evidence remains instrumented and includes full hash
cost; there are no native results or speedup claims before qualification runs.

The generation/package directories are owned immutable diagnostic fixtures.
Package ancestors are not protected by a production authenticity/handle protocol,
and ordered-path reconstruction enumerates before all individual object guards
are retained. These are additional reasons the prototype is **not** production
adoption code. Same-user arbitrary writers and ordinary-shell concurrency remain
out of scope until an explicitly reviewed production lifecycle is implemented.
