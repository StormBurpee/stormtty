# Private Windows console backend preview

## User contract

Settings → Shell → Console backend offers **System** and **Bundled (preview)**.
System is the default for both new and pre-existing configurations. Merely
including the runtime does not select it. Save applies to new tabs/panes/projects;
existing sessions keep their original provider and process identity. Preview,
Cancel, failed Save and merely opening Settings never switch transports.

The saved `[terminal] windows_console_backend` value is `system` or `bundled`,
not a version, absolute path or shell command. Non-Windows configuration edits
preserve the preference but do not expose/use it. Shell selection and the VT
engine remain independent of console transport.

The bundled pair is found only beside the executable at
`runtimes/conpty/1.25.260930003/x64`. A missing/corrupt pair creates a failed session
with a visible repair message, not a silent System fallback or an early app exit.
Settings remains available: choose System, Save, then open a new terminal. No
failed session is automatically respawned. `--console-runtime <absolute directory>`
is retained for diagnostics; it overrides the saved preference for that launch
and Settings explicitly displays the override. Remove the flag to honor Settings.

## Build and redistribution contract

Native AMD64 Windows app builds include the runtime automatically. Disable with
`-DSTORMTTY_BUNDLE_CONPTY=OFF`; cross-compilation must explicitly use that option.
Core-only and macOS builds do not load the module or acquire this dependency.
Native AMD64 means both the app and host OS, **not x64 emulation on ARM64**. Other
architectures keep System; support requires separate host layout/pins/tests.

`STORMTTY_CONPTY_CACHE` selects the private build cache and
`STORMTTY_CONPTY_OFFLINE=ON` requires the exact cached archive. The application
never downloads anything. The build helper uses Python plus built-in Windows
PowerShell Authenticode validation. It neither needs `dotnet` nor changes or
bypasses PowerShell execution policy. Trust failure stops the build/install.
Linux tests exercise archive handling only; they cannot validate Windows trust.

The lock file records the official NuGet exact-version URL, package SHA256 and
catalog SHA512, exact DLL/host hashes, source commit and notice hashes. Extraction
is bounded and allowlisted; deployment checks Microsoft signatures. Every actual
packaged session rechecks the pinned binary bytes with held file handles, loads
only an absolute DLL path with restricted search flags, and confirms the actual
OpenConsole child identity before launching the shell. No global PATH or Windows
component is changed. Build repair never copies an unverified runtime over a
working bundle.

### Interrupted publication and recovery

Runtime publication and its external ownership receipt are **not crash-atomic**.
The helper writes the new receipt in its private staging directory before moving
anything. If either the runtime rename or receipt replacement raises an ordinary
I/O error, it restores the prior runtime (if any), keeps the prior receipt intact,
and leaves unrelated files untouched. A first-install receipt failure therefore
leaves no unowned installed directory, and a subsequent build can retry.

If rollback itself is obstructed, the helper deliberately keeps its active guard
and named `.stormtty-conpty-*` recovery directory and reports their location;
it does not delete the saved `previous` runtime. A process/machine crash also
leaves an active guard and can leave that staging directory. Builds stop rather
than guess which directory to adopt. After confirming no build/app is using the
files, inspect the **exact reported helper directory** and pinned provenance:

- Before the first rename, the old destination/receipt are unchanged.
- Between directory renames, the old runtime is `previous` and destination may
  be absent; restore `previous` to the destination before removing the guard.
- After publishing the new directory but before its receipt, the old receipt is
  unchanged (or absent on first install). Preserve `previous` if present; move
  the new candidate aside and restore the old runtime, or on a first install
  remove only the verified helper-created candidate so a clean build can retry.
- After receipt replacement, destination and receipt are committed; verify them
  with `--verify-only` before removing stale helper artifacts and the guard.

Never remove/adopt an unrelated directory or treat a stale guard as permission
to overwrite unowned content. Automated crash recovery is not implemented.

Normal builds do **not** claim to revalidate the NuGet repository signature:
pinned archive hashes plus executable Authenticode are the developer-build
contract. A dependency-pin update/release intake must additionally run full NuGet
signature verification, review official catalog provenance and licenses, record
results, and rerun native qualification. Never update to an unpinned latest
version or waive trust checks because an offline trust chain is unavailable.

The actual nupkg declares MIT but contains no license/third-party notice text.
`tools/conpty-notices` therefore preserves the **complete** LICENSE and NOTICE.md
from Microsoft Terminal commit `dc8ae365847d41f62d1ebca93815bb00959bc12f`, alongside
package metadata/provenance in the shipped runtime. Do not prune notices based on
an assumption that an absent package notice means no third-party obligations.
This backend does not relicense StormTTY or waive other bundled-shell obligations.

## Acceptance and evidence

Portable config/Settings tests verify strict roundtrip/defaults, unavailable
choice and Cancel/Save semantics. `tools/conpty-bootstrap-tests.py` exercises
bounded allowlist extraction, hashes, path rejection and notice inventory.

On a native AMD64 Windows app build with the bundle enabled, CTest also registers
`console_backend_tests`: real processes and actual provider identity, saved
System→Bundled→System, independent application-like restart reading saved config,
no-CLI executable-relative discovery, retained original process identity, missing
host/corrupt DLL failures followed by explicit System recovery, and lifecycle.
The native Settings smoke also drives both saved choices through owned controls
and verifies that old System and bundled actors remain intact. With `--capture`,
it also writes a `-console.png` sibling showing the owned backend selector. Both tests must
run on the final commit; compiling them is not a native pass. Physical Settings
layout/keyboard/accessibility inspection remains separate from automated smoke.

Native experiment40716243943 isolated the static Ink Unicode-border defect to
inbox ConPTY and found this pinned pair preserved the border. **Compact-alt
resize remains unresolved.** Do not infer full Ink, graphics, Kitty input, IME,
performance or OS-matrix qualification from this backend choice. System stays
default until a separate reviewed decision backed by native evidence.
