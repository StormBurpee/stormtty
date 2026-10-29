
## First native shipping run: pre-build rejection (40859498563)

The exact published tree `41243ef4a351c459c230c65a1541b6b19b482fe2`
(remote commit `7475e66248c4020e89ce73cb6e6fe3167ed70ad9`) failed the
packaging containment contract before any native build or runtime measurement.
The Windows test supplied an output inside its authored runtime, but the guard
compared a resolved runtime to an unresolved absolute output. Windows path
canonicalization (notably TEMP short/long aliases) can make those unequal.
The generator was incorrectly reached and failed on the intentionally absent
manifest. This is a real guard defect, not a performance or ACL result.

The follow-on canonicalizes both outputs after rejecting linked ancestors,
then checks containment in both directions for the package and rejects a header
inside/equal to the runtime. Regression tests require rejection before generator
invocation, including a `..` spelling. The four preflight suites pass locally
(22 tests); this is POSIX Python evidence only. No new native timing, lifecycle,
install identity, or first-Tab claim follows from this failed run. The opt-in
remains OFF; no automatic rerun or workflow-trigger edit is part of the fix.

The root-reviewed follow-on trigger now also includes the exact build-only
generator and its regression test. Source corrections on the same exact
diagnostic branch can therefore qualify without a dummy workflow edit. The
branch/job guard, all bounds, default-OFF option, and no unchanged-run retry
policy remain intact; no broad normal-CI path trigger was added.

## Corrected-source native shipping run: lifecycle blocked (40861491198)

Remote commit `77ed998f2f2e229e034e961503fc02038f443d93`, exact tree
`8516c6b43ebde69ad99dcdafffe9abd4afcc9039`, passed Windows preflight, native
application/probe compilation, exact runtime seed generation, owned no-op and
header repair, and portable installation with spaces/Unicode. The driver's full
build/install inventory, seed and binary comparison passed:

- Application SHA256: `3347ccd1f97c257d7db73cd4c6467a18be8137e075c4685d6c4355dbafeae6f0`
- Runtime manifest SHA256: `2b28817c1bb43cbfffbfee16a4162f636728682b1578678e2dd42ce157fd1854`
- Seed identity SHA256: `d816ea305da94fca848f1d0b93e072fbde5becc6bf91e09a7b11d92c3256297a`
- Seed text SHA256: `361846b3c44f1db8e82799fb28fa3bf0ae90971621a7ed6a5fd06ef3fa0d7ec7`

The lifecycle probe stopped with `INITIALIZATION_CHECKS code=1 stage=4`.
Progression to this stage establishes cross-thread contention, native event
acknowledgement, existing-config preservation, child-exit lease release and
initial lease acquisition by the subsequently departed thread. It does not
establish abandoned-owner persistence, later cross-process contention, or the
public-runtime ACL fixture. Stage 4 contained two checks; the original log cannot
distinguish them. The subsequent app smoke and all startup timings were correctly
blocked. No shipping startup improvement is measured by this run.

Artifact 12892271102 ZIP SHA256:
`37e7b8d2ee73582f7a72d0413c80a1036b07904ec921771b20b571e707310547`.
A follow-on diagnostic splits abandoned recovery and subsequent launch into
stages 41/42, retaining only boolean state/event/marker flags and a numeric error.
It changes no runtime behavior, gate, trigger, or default. Further native
qualification requires a separate root publication decision, not an unchanged
failed-run retry.

The later root-reviewed route removes the path filter entirely: every intentional
source push on `diagnostic/completion-seed-shipping` qualifies under the unchanged
exact branch/job guard. There is still no main, pull-request, or dispatch trigger.
This permits owned probe and lifecycle source corrections without dummy workflow
edits; it does not authorize retrying an unchanged failure until green.

## Stage-split run 40866040477: installed path measured, build-tree rejection

At remote `98ba115082d839c3aaec057236283600738c6a3d` / tree
`bba582cee7c57327c3152d30bf0bb622d7e36820`, all lifecycle checks passed:
`recovery_no_dump=1 recovery_event=1 state_valid=1 marker_present=1 marker_error=0`.
Installed app smoke and exact build/install identity also passed. The earlier
stage-4 failure was not reproduced; no behavioral fix was in this run.

A concrete probe defect was found independently: fixture names used PID plus
coarse `GetTickCount64`, allowing two fast consecutive fixtures to collide.
The next fixture constructor ran before setting stage 5, so a collision could
be reported as stage 4. The correction uses a fresh GUID per authored fixture
and sets stages before construction. The old log does not prove this was its
sole cause. The separate durable journal remains pending native qualification.

All six installed fresh samples adopted; all six installed warm samples skipped
adoption. Totals include state preparation plus launch-to-ZLE, with the entire
native ACL/hash/publication cost already inside launch (never added twice):

| Installed provider / condition | Fresh min / median / max seconds | Warm min / median / max seconds |
|---|---|---|
| Inbox baseline | 4.732 / 4.821 / 4.923 | .633 / .642 / .707 |
| Inbox seed | 1.870 / 2.025 / 2.048 | .711 / .743 / .760 |
| Packaged baseline | 4.764 / 4.834 / 5.829 | .743 / .756 / .769 |
| Packaged seed | 1.919 / 1.966 / 1.980 | .766 / .806 / .844 |

Full installed verification/publication cost was 1.181–1.243 seconds. These are
fresh private states, not controlled cold OS caches. Warm candidates were slower
in this run; no warm improvement is claimed. Every actual first-Tab/module check
passed; all child runs exited/reaped without timeout/truncation/cleanup error.
The build-tree candidate rejected seed adoption in .65–.87ms and correctly fell
back to ordinary completion (fresh 4.873–5.050s); it is **not** a successful seed
sample. This failed the overall qualification despite installed-path gains.
ACL/handle/verification rejection remains to be localized without weakening it.
Artifact 12894720800 ZIP SHA256:
`9def3dc2422aef4cf9b4ef6fc15b7e892a6250fec0579aa5ca23ca890cdf1ce2`.

The rejection-localization follow-on emits only `SHIPPING_VERIFY` sample,
verification stage, ordinal object number and immediate failed-CreateFile Win32
error (0 unless such an open failed). It preserves all checks, rights, sharing,
compiled trust, hash work and fallback behavior. Stage map: 1 policy creation;
2/3 root open/ACL; 4/5 ancestor open/ACL; 6 identity read; 7 anchor hash;
8 metadata parse; 9/10/11 inventory open/ACL/hash; 12 inventory count;
13 ordered fpath; 14/15/16 seed open/hash/read; 17 private publication;
18 successfully adopted. Object is ancestor depth or inventory ordinal, never a
path/name. This localizes build-vs-install rejection without treating an
adoption failure as successful performance evidence.

## ACL-localized run 40874789913 and fresh-staging correction

Exact remote commit `8283a2c599d0bd82cf8c0597a9e826b90418ce7c`, tree
`0020f41fb6d19d41d73b813dbedcd6f60c2787c0`, passed native build, no-op/repair,
installed identity, GUID-isolated lifecycle and installed app smoke. Overall
qualification **failed**: every build-tree fresh sample reported
`SHIPPING_VERIFY stage=5 object=8 win32_error=0`. The eighth path component is
`build/runtimes/zsh`'s final root: its handle opened, then owner/DACL policy
rejected it before any inventory hashing. Rejection cost was .312–.408 ms;
ordinary completion fallback and first-Tab checks succeeded. This is not successful
build-tree seed performance evidence.

All six installed fresh samples reached stage 18/adoption 1; warm samples skipped
adoption. Complete fresh totals (min/median/max seconds) were inbox baseline
2.379/2.391/2.401 versus seed 1.036/1.053/1.125, and packaged baseline
2.347/2.370/2.447 versus seed 1.027/1.054/1.068. Included verification/publication
was 520–555 ms. Warm baseline ranges were .396–.436 s versus seeded .473–.514 s;
no warm gain is established. This opt-in diagnostic is **not main behavior** or a
cold-OS-cache comparison. The separate durable process-death journal is absent
and remains unqualified.

Artifact `12897937394` was downloaded and archive SHA256 verified:
`8310be0724b571521cc1d68f6d6140acc10b5d612f043975e8d56f8620839bce`.
Application SHA256 was `2637d3dd007247a8e7a776590fcb0d92da732e45063d376fa6234560854d8bd3`.

The staging source explains the build/install difference: it creates `new`
inside `tempfile.TemporaryDirectory`, then renames it into the build runtime.
[CPython v3.12.10's mkdir implementation](https://github.com/python/cpython/blob/v3.12.10/Modules/posixmodule.c#L5024-L5034)
uses an inheritable protected SYSTEM/Administrators/OWNER RIGHTS DACL for mode
0700, including `mkdtemp`. OWNER RIGHTS is not a concrete trusted SID in our
verifier. The build root inherits this ACL; the installed copy inherits its new
parent's ACL. Native evidence localizes rejection to the ACL policy, but does not
itself record the offending ACE; the exact source establishes the staging path.

The correction creates only the **new, owned staging runtime root** with the same
explicit current-user/SYSTEM/Administrators protected inheritable DACL as native
private state. Existing directories, source trees and system/user ancestor ACLs
are never changed. Collision fails rather than repairing permissions. Descendants
inherit concrete trusted SIDs, and rename retains their security descriptors.
Runtime validation, hash/open/sharing/security gates and default-OFF remain
unchanged. The stager source is already part of validation-input identity, so an
owned prior staging output is regenerated rather than treated as an unchanged
no-op.

Regression coverage includes creation collision, preservation of existing output,
cleanup before/after fresh-directory creation failure, and a Windows-only check
of renamed root/child/file owner and exact allowed SIDs, child inheritance and
collision ACL preservation. Diagnostic preflight now invokes the staging suite.
Local Linux results: 25 staging tests passed, native ACL test skipped; all 23
seed packaging/parser/workflow tests passed. The correction has **not run on
Windows** until a separately reviewed root publication qualifies it.
## Separate durable initialization journal hardening (pending native evidence)

The stage-4 probe retains the departed owner's mutex handle, so last-handle
object disappearance is **not an established cause** of that run's failure.
Nevertheless, a real process exit closes all mutex handles; a new process can
then create a new mutex without receiving `WAIT_ABANDONED`. The opt-in lifecycle
must not rely solely on that non-durable signal.

The separate correctness candidate creates an exclusive private
`completion-init-in-progress` file, writes its version marker and flushes it
before seed adoption or permitting shared-cache compinit. Its handle is closed
before child launch so ordinary state inspection and contenders remain possible.
A pre-existing journal or uncertainty marker suppresses adoption and selects
normal secure `compinit -i -D`, preserving config, completion paths and history.
Only a trusted native readiness acknowledgement on the owning controller can
remove the **current launch's** journal: it reopens a non-reparse, single-link
file, checks original volume/file identity and private ACL, and deletes via that
handle before releasing the mutex. Failure retains uncertainty. Child exit
without acknowledgement releases the lease but does not clear the journal.
An acknowledgement from a later uncached launch never clears an earlier journal.
There is deliberately no automatic uncertainty recovery or sweep.

The native probe adds a child that calls `ExitProcess` with the sole mutex handle
still held, then requires subsequent launches to remain uncached even after
acknowledgement. The evidence parser requires this new proof before starting
startup measurements. Existing child-exit expectations are tightened: the lease
is reusable but shared completion dumps are not. Local Windows-target compilation
and portable parser/route tests pass; actual Windows journal behavior, flush
cost and shipping latency remain unmeasured. Defaults remain OFF.

## Staging regression run 40876888614: reader error, no adoption result

Remote `75cb25c6c54baaf8c2be65664f7205623450f7db` / tree
`7efce9564f3caac5f7a3d12660d07194ad2af6a1` stopped in staging preflight.
The 25 existing/collision/cleanup tests passed. New private runtime creation and
rename succeeded, but its Windows ACL reader (`powershell.exe`) exited 1 before
returning JSON. Captured stderr was not printed by unittest's CalledProcessError
or retained in the artifact; its precise failure cause is unknown. This is a
reader failure, **not evidence that the resulting ACL passed or failed**.
Native compiler/build, lifecycle and startup were never run.

The regression reader now uses Windows security APIs directly to check token-user
identity, owners, exact allowed SID/mask sets, protected root DACL, inherited child
ACEs, and unchanged ACLs on creation collision. Native API failures report their
Win32 errors. The runtime implementation and strict verifier are unchanged by
this reader correction. Local staging suite still passes 25 tests with the single
Windows test skipped; native policy qualification remains pending.

## Integrated journal candidate (not native-qualified)

The integrated candidate combines the fresh-staging ACL correction and direct
native ACL reader with the durable journal from `37a3462` and GUID fixture/stage
ordering from `383118a`. Numeric `SHIPPING_VERIFY` diagnostics and every native
adoption/security check remain intact. The crash test now counts adoption callback
invocations explicitly (rather than throwing inside a swallowed callback), and
checks that the journal exists before the initial adoption callback runs.

The new lifecycle gate must report
`INITIALIZATION_JOURNAL code=0 process_exit_persistent=1 prior_journal_not_cleared=1`
in addition to all existing lifecycle checks. A child exits while holding its sole
mutex handle; subsequent launches must choose no-dump completion, never invoke
adoption, and never clear that previous journal even after readiness. This is
required **before** startup timing is accepted.

Local integrated validation: 25 staging tests passed (Windows ACL case skipped),
24 seed packaging/parser/workflow tests passed, and both changed native lifecycle
translation units compiled for Windows x86-64 with Zig C++20 and
`-Wall -Wextra -Werror`. This is cross-compilation, not Windows execution. Prior
installed timing and lifecycle success do not qualify this new journal or its
flush overhead. No default enablement or automatic recovery policy is included.

## Successful staging/adoption run 40877538476 (journal still separate)

Exact remote `8c9cc268be5c9b776551c5cfeaef9b32581dd59b`, tree
`2afbaeb50395c8b2a2dc365bbe19da0c9be4d854`, **passed** the full native
qualification. Native ACL regression verified concrete private grants on the
renamed runtime root and inherited children, including unchanged collision ACLs.
Build, owned no-op/header repair, identical portable installation, installed app
smoke and existing lifecycle checks passed. All nine fresh candidates (three
build, six installed) reached verifier stage 18 and adoption 1; all nine warm
candidates skipped adoption. Thus the build-root rejection is resolved without
changing runtime ACL policy, hash work or open-sharing guards.

| Condition | Fresh min / median / max seconds | Warm min / median / max seconds |
|---|---|---|
| Build inbox seed | 1.883 / 1.929 / 1.955 | .711 / .736 / .739 |
| Installed inbox baseline | 4.753 / 4.765 / 4.812 | .633 / .675 / .692 |
| Installed inbox seed | 1.993 / 2.008 / 2.035 | .712 / .728 / .770 |
| Installed packaged seed | 1.974 / 2.000 / 2.007 | .741 / .759 / .791 |
| Installed packaged baseline | 5.083 / 5.191 / 5.491 | .633 / .643 / .654 |

Totals include state preparation and launch-to-ZLE; full native verification/
publication of 1.166–1.224 s is already inside launch. First-Tab, module, native
acknowledgement and owned-child reap checks passed in every condition without
timeout, truncation or cleanup error. Fresh means private cache, not controlled
cold OS caches. Warm candidates remain slower; no warm gain is claimed.
This default-OFF diagnostic is not shipping main behavior.

Artifact `12898209727` archive SHA256 verified:
`369e09bf41cbd19b67eb94a6502b96d60b1c6f5a9afe2972fb554bf2ee906999`.
Application SHA256: `10f1df0823200cdb856dee74481c765f8350cd571b6d07554d94ceddd19867d0`.
Seed identity SHA256: `d8eb4a87f8e23aa6f6dd10ac5cf255c3b3b941f944862c27ac0cf3f0a7cede5a`.

This run predates the integrated durable journal. Sole-mutex-handle process death,
retained-journal suppression and journal flush overhead still require their own
native qualification. Successful older lifecycle checks do not establish those
new guarantees. No activation, automatic uncertainty recovery or remote retry is
implied by this result.

## Successful durable-journal run 40879470794

Exact remote `98418241f5d9032c5190bc88c1c99101d8e6f163`, tree
`537148ee03f88a5e3964a196c1b52058d99c139c`, passed native preflight, build,
repair, relocation, exact installation, application smoke and full lifecycle/
startup matrix. The new gate explicitly emitted:

```
INITIALIZATION_JOURNAL code=0 process_exit_persistent=1 prior_journal_not_cleared=1
```

This establishes that the authored child exiting with the sole mutex handle
leaves durable uncertainty: the next launch skips adoption and shared dump use,
and its acknowledgement does not erase the previous journal. The probe also
verified journal presence before adoption, custom-config preservation, successful
owner acknowledgement, child-exit release, thread/process contention, abandoned
owner handling and public-read/untrusted-write ACL policy. It is a process-exit
fixture, not a hardware power-loss simulation or a guarantee against arbitrary
same-user writers.

All nine fresh seed samples reached verifier stage 18/adoption 1. All nine warm
samples skipped adoption; all first-Tab/module/native-ack assertions passed and
children exited/reaped without timeout, truncation or cleanup errors.

| Condition | Fresh min / median / max seconds | Warm min / median / max seconds |
|---|---|---|
| Build inbox seed | 1.407 / 1.415 / 1.457 | .560 / .599 / .644 |
| Installed inbox baseline | 3.271 / 3.319 / 3.394 | .501 / .507 / .516 |
| Installed inbox seed | 1.415 / 1.470 / 1.521 | .576 / .581 / .592 |
| Installed packaged seed | 1.340 / 1.346 / 1.436 | .566 / .585 / .601 |
| Installed packaged baseline | 3.158 / 3.162 / 3.195 | .474 / .486 / .496 |

Totals include state preparation plus launch-to-ZLE. Full seed verification/
publication was .764–.823 s and is already inside launch; journal creation/write/
flush also occurs inside launch. Warm seeded medians exceeded the same-run
baseline by about **74 ms inbox / 99 ms packaged**. These are combined lifecycle,
acknowledgement and journal costs, **not isolated FlushFileBuffers timings**.
The run has no individual journal/flush timer or otherwise-identical no-journal
candidate. Different runners' earlier results cannot isolate that cost. There
is no warm speedup claim and no claim of cold OS filesystem caches.

Artifact `12900460609` archive SHA256 verified:
`55c4057e8d1ab9a78738f3d0b79807c407b8473ec9696738d17d6fd33e8230c8`.
Application SHA256: `b36bf0ca1cc584a83336234dee9f3545cfc530c54ec66e8ffb3191001919ad4d`.
Seed identity SHA256: `69124d0b429ff28b921e95936f2fb0e7f7871d58a8da5ab4dabbca413ab963fa`.

The opt-in candidate now has native process-exit journal evidence and fresh-cache
benefit. Default activation is still OFF and not authorized by qualification.
Retained uncertainty intentionally persists; there is no automatic marker cleanup
or product recovery flow. A separate promotion review must address recovery UX,
combined warm overhead and exact integration evidence rather than silently
weakening crash handling or reusing these timings as main-branch behavior.

## Bounded warm-start follow-on: scans and measurement only

Warm state does not prove completion initialization is read-only. Secure compinit
and OMZ may update the text dump or compiled sibling after runtime/fpath changes.
Therefore the private validation, cross-process lease, journal flush, compaudit,
readiness acknowledgement and same-file journal deletion remain mandatory.
The earlier no-journal successful run already showed warm median gaps of about
53 ms inbox / 116 ms packaged; the later 74/99 ms cannot be assigned solely to
journal flush. First-ZLE acknowledgement also launches a native helper process.
No helper protocol change or durability shortcut is justified by current evidence.

The narrow optimization short-circuits **only adoption-eligibility checks**:
previous uncertainty or no adoption callback skips them; existing config skips
completion/cache enumeration; the first dump entry stops the dump search and
skips private-completion enumeration. Full security tree validation still runs.
New native cases cover config, compiled config, dump and custom completion:
adoption stays suppressed, a fresh journal still exists before readiness,
readiness removes only that journal, and existing-file mtime remains unchanged.
No speed claim is made for avoiding these small redundant scans.

Qualification now requires a numeric `SHIPPING_INITIALIZATION` row for each seed
sample. Counters are opt-in to the probe, not default production clock reads:

- `prepare_ms`: real native gate preparation, including verification when fresh.
- `journal_create_write_ms`: CreateFile plus writing the fixed journal marker.
- `journal_flush_ms`: the actual FlushFileBuffers call and measurement overhead.
- `native_ready_wait_ms`: end of probe preparation through controller receipt of
  the native readiness signal; includes child creation, shell initialization,
  first-ZLE helper process and dispatch, **not just kernel wait latency**.
- `ready_cleanup_ms`: native ready handling, identity/ACL checks, journal deletion
  and lease release.

Journal timings are subintervals of preparation; verification is also inside
preparation. All are inside existing launch-to-ZLE totals, never added twice.
No paths, configuration, user text, or terminal content are logged. The parser
requires all six ordered numeric timing rows and rejects malformed/missing rows.
Actual timing and new native fixture execution await root publication.

### Recovery options and promotion criteria

**Safe action today:** keep using the terminal in uncached completion mode after
an interrupted startup. Config, history and custom completion remain untouched;
the marker is not an instruction to manually delete files. This protects safety
but can retain slower startup, so it is not a complete recovery experience.

**Recommended explicit recovery UI (design, not implemented):** offer “Rebuild
managed completion cache” with an explanation that config/history/custom
completions are preserved. Require all managed sessions across processes to close
and exclusive recovery ownership; the existing initialization mutex alone is
insufficient because it is released while older shells remain alive. Validate
private state/ancestors/identities, then atomically **quarantine**, never silently
delete, only the known versioned generated text/compiled dump and uncertainty
records in a private owned recovery directory. Preserve all other files. Require
a durable recovery transaction before the first rename; partial failure or crash
must continue no-dump mode. Rebuild through ordinary audited compinit with eager
first-Tab, and clear only the current recovery transaction after native readiness.
Never source quarantined executable caches. Unknown/unverifiable objects or
active sessions must block rebuilding, not trigger broad cleanup. Keep the backup
available until explicit user-directed removal. This needs its own crash matrix
and ownership/provenance design before implementation; matching a filename prefix
alone is not proof of disposable app-generated content.

Promotion should be a bounded decision, not endless experimentation:
1. Qualify exact scan/timing source on both providers, preserve the sole-handle
   crash and all adoption/first-Tab gates, and measure rather than assume the
   source of warm overhead.
2. Accept a documented fresh/warm tradeoff or make an evidence-based helper
   optimization without reducing readiness trust or durability.
3. Provide a truthful uncertainty notice and an explicitly authorized recovery
   flow, with interrupted recovery retaining no-dump protection and every user
   config/history/custom-completion byte preserved.
4. Integrate into the current application factory and test existing/custom/warm
   states, normal exits, early close, failed spawn and cross-process launches;
   document unsupported concurrent older/nonparticipating clients.
5. Root reviews default activation only after that exact native evidence. This
   candidate does not flip defaults, erase markers or mutate remote state.

Local follow-on validation: 57 Python/POSIX tests passed, with the separate Windows
ACL reader test skipped on Linux. Lifecycle state/probe and shipping factory/
startup-probe translation units compile for Windows x86-64 using C++20 with
`-Wall -Wextra -Werror`; factory compilation uses an authored dummy anchor only
for type checking, never native execution. New warm-preservation cases and actual
numeric overhead values are still pending native qualification.
