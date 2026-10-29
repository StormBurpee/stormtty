
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
