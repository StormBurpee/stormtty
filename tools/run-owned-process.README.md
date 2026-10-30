# CI owned-process runner

This diagnostic distinguishes actual app termination from inherited stdout-pipe
EOF. It is not production terminal code or a descendant-cleanup solution.

```sh
python tools/run-owned-process.py --timeout 90 --tail-bytes 65536 \
  --output-dir build/artifacts -- build/stormtty.exe --smoke-modal
python tools/run-owned-process-tests.py
```

The output directory must already exist. Omit it to use the platform temporary
directory. stdout/stderr share an owned temporary **regular file**; completion is
`Popen.wait()`, not `communicate()` or an output-reader thread. Only a bounded tail
is read. A still-running descendant may append after that snapshot; this runner
neither waits for nor terminates descendants.

For existing Python workflow loops, import the hyphenated filename with
`importlib.util` (register the module in `sys.modules` before `exec_module`), then
call `run_owned(args, timeout=90, tail_bytes=65536, output_dir=...)`. The returned
`Result` exposes:

- `returncode`: actual process return code, or `None` if finite reaping failed;
- `timed_out`, `alive_at_timeout`, `reaped`, `cleanup_error`, and elapsed seconds;
- `output_tail` bytes and `output_truncated`;
- `exit_code`: 124 for deadline failure, otherwise the actual return code;
- `metadata()`: JSON-compatible diagnostics, excluding captured output bytes.

On deadline expiration, `poll()` records whether the actual owned child remains
alive. Only that retained `Popen` child is killed, followed by a separate finite
reap deadline (default 10 seconds). A race where the child exits just after the
deadline still fails as timeout. There are no broad process kills, handle scans,
privilege changes, debugger dependencies, or unbounded pipe-close waits. API
return codes retain negative POSIX signal codes; command-line exit codes follow
normal OS shell encoding. Launch/argument errors exit 125.

Portable tests cover actual timeout/reaping, true nonzero exit codes, merged
output, bounded tails, bad arguments/launch failure, CLI reporting, and a parent
that exits while a controlled short-lived descendant retains its output handle.
Windows runtime qualification of the helper remains a separate CI result.

## Compact application receipts

`native_application_receipt.py` publishes best-effort, per-case JSON for the
existing Windows installed-console, packaged/bundled/developer application and
repeated-Settings cases. Only fixed authored case IDs are accepted. The receipt
contains return/exit status, timeout/reap/truncation flags and whether cleanup
reported an error; it excludes PID, paths, argv, environment, raw output and
cleanup text. Each file is capped at 2 KiB and atomically replaced under
`build/artifacts/application-receipts/`.

The helper observes the runner's already-bounded tail; it does not launch, capture
or send anything. Only the reviewed missing-expected-marker diagnostic can set its
observation to true. No match remains null, not false; malformed-paste and
stale-marker observations remain null. These fields never diagnose a root cause.
A missing receipt means unobserved, not success. Process-start exceptions before a
Result exists do not produce a fabricated process result.

Receipt failures emit only `CI_APPLICATION_RECEIPT_WRITE_FAILED` and do not alter
the test result, loop continuation or cleanup. Existing stdout, deadlines, captures
and artifacts remain unchanged. The separate `application-receipts-windows`
artifact uploads only these JSON files, even after earlier test failures.

```sh
python tools/native-application-receipt-tests.py
```

Receipt tests use synthetic Result values and temporary files, not native input,
extra application launches or new captures.
