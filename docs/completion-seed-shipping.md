
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
