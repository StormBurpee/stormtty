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
