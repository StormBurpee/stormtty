#!/usr/bin/env python3
"""CI-only owned-child runner: process termination, never pipe EOF, is completion."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from typing import Optional, Sequence


@dataclass(frozen=True)
class Result:
    pid: int
    returncode: Optional[int]
    timed_out: bool
    alive_at_timeout: Optional[bool]
    reaped: bool
    elapsed_seconds: float
    output_tail: bytes
    output_truncated: bool
    cleanup_error: str = ""

    @property
    def exit_code(self) -> int:
        # A process exiting just after the deadline does not turn timeout green.
        return 124 if self.timed_out else self.returncode if self.returncode is not None else 125

    def metadata(self):
        return {name: value for name, value in self.__dict__.items() if name != "output_tail"}


def run_owned(command: Sequence[str], *, timeout: float = 90, tail_bytes: int = 65536,
              reap_timeout: float = 10, cwd=None, output_dir=None, env=None) -> Result:
    """Run one child without a shell; kill/wait only its retained Popen handle.

    Descendants are neither enumerated nor terminated. A descendant retaining
    stdout cannot delay this runner's wait or output read. TemporaryFile owns
    the regular-file capture (including delete-on-close semantics on Windows).
    No communicate(), output pipe, reader thread, or process-group operation.
    """
    if not command or isinstance(command, (str, bytes)):
        raise ValueError("command must be a nonempty argument sequence")
    if not math.isfinite(timeout) or timeout <= 0 or not math.isfinite(reap_timeout) or reap_timeout <= 0:
        raise ValueError("timeouts must be finite and positive")
    if not isinstance(tail_bytes, int) or not 1 <= tail_bytes <= 1024 * 1024:
        raise ValueError("tail_bytes must be between 1 and 1048576")
    started = time.monotonic()
    with tempfile.TemporaryFile(mode="w+b", dir=output_dir) as capture:
        child = subprocess.Popen(command, cwd=cwd, stdin=subprocess.DEVNULL,
                                 stdout=capture, stderr=subprocess.STDOUT, close_fds=True, env=env)
        timed_out = False
        alive_at_timeout = None
        cleanup_error = ""
        try:
            try:
                child.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                alive_at_timeout = child.poll() is None
                if alive_at_timeout:
                    try:
                        child.kill()  # This exact process handle/PID only, never its tree.
                    except OSError as error:
                        cleanup_error = "owned-child kill: " + str(error)
                try:
                    child.wait(timeout=reap_timeout)
                except (subprocess.TimeoutExpired, OSError) as error:
                    cleanup_error += ("; " if cleanup_error else "") + "owned-child reap: " + str(error)
        except BaseException:
            # Do not strand our child if the runner itself is interrupted.
            if child.poll() is None:
                try:
                    child.kill()
                    child.wait(timeout=reap_timeout)
                except (OSError, subprocess.TimeoutExpired):
                    pass
            raise
        returncode = child.poll()
        # Read a bounded snapshot. A surviving descendant may append later;
        # there is deliberately no EOF wait or claim of descendant cleanup.
        size = os.fstat(capture.fileno()).st_size
        capture.seek(max(0, size - tail_bytes))
        tail = capture.read(min(size, tail_bytes))
        return Result(child.pid, returncode, timed_out, alive_at_timeout,
                      returncode is not None, time.monotonic() - started,
                      tail, size > tail_bytes, cleanup_error)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--reap-timeout", type=float, default=10)
    parser.add_argument("--tail-bytes", type=int, default=65536)
    parser.add_argument("--output-dir", type=Path, help="Existing directory for owned temporary capture")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    try:
        result = run_owned(command, timeout=args.timeout, reap_timeout=args.reap_timeout,
                           tail_bytes=args.tail_bytes, output_dir=args.output_dir)
    except (OSError, ValueError) as error:
        print("OWNED_PROCESS_START_ERROR=" + str(error), file=sys.stderr, flush=True)
        return 125
    print(result.output_tail.decode("utf-8", errors="replace"), end="", flush=True)
    print("\nOWNED_PROCESS_RESULT=" + json.dumps(result.metadata(), sort_keys=True), flush=True)
    return result.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
