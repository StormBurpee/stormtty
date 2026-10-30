"""Allowlisted CI application receipts; never persist captured text or launch data."""
from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tempfile


MODES = ("smoke", "smoke-layout", "smoke-modal", "smoke-projects", "smoke-settings")
CASE_IDS = frozenset(
    ["installed-console-settings"]
    + [f"{family}-{mode}" for family in ("packaged", "bundled", "developer") for mode in MODES]
    + [f"teardown-explicit-{index}" for index in range(1, 11)]
    + [f"teardown-default-{index}" for index in range(1, 4)]
)
MAX_RECEIPT_BYTES = 2048
MAX_TAIL_BYTES = 1024 * 1024  # The existing runner's maximum, not a new capture.
WARNING = "CI_APPLICATION_RECEIPT_WRITE_FAILED"
MISSING_MARKER = b"Controlled smoke shell exited without its expected output marker."


def receipt_bytes(case_id, result):
    """Build only explicit scalar fields; absence of a signature is unknown."""
    if case_id not in CASE_IDS:
        raise ValueError("Unknown authored case")
    for name in ("timed_out", "reaped", "output_truncated"):
        if type(getattr(result, name)) is not bool:
            raise ValueError("Invalid runner flag")
    if result.alive_at_timeout is not None and type(result.alive_at_timeout) is not bool:
        raise ValueError("Invalid timeout observation")
    if result.returncode is not None and type(result.returncode) is not int:
        raise ValueError("Invalid return code")
    if type(result.exit_code) is not int or not isinstance(result.cleanup_error, str):
        raise ValueError("Invalid runner result")
    if not isinstance(result.output_tail, bytes) or len(result.output_tail) > MAX_TAIL_BYTES:
        raise ValueError("Invalid bounded output")

    receipt = {
        "schema_version": 1,
        "case": case_id,
        "returncode": result.returncode,
        "exit_code": result.exit_code,
        "timed_out": result.timed_out,
        "alive_at_timeout": result.alive_at_timeout,
        "reaped": result.reaped,
        "output_truncated": result.output_truncated,
        "cleanup_error_present": bool(result.cleanup_error),
        "signatures": {
            "missing_expected_marker_observed": True if MISSING_MARKER in result.output_tail else None,
            "malformed_paste_observed": None,
            "stale_marker_observed": None,
        },
    }
    encoded = (json.dumps(receipt, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n").encode("ascii")
    if len(encoded) > MAX_RECEIPT_BYTES:
        raise ValueError("Receipt exceeds bound")
    return encoded


def write_receipt(case_id, result, directory=Path("build/artifacts/application-receipts")):
    """Best-effort publication must never replace a test result or abort its loop."""
    temporary = None
    try:
        encoded = receipt_bytes(case_id, result)
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(mode="wb", dir=directory, suffix=".tmp", delete=False) as output:
            temporary = Path(output.name)
            output.write(encoded)
        os.replace(temporary, directory / (case_id + ".json"))
        temporary = None
        return True
    except Exception:
        # No exception message: filesystem errors may contain paths or secrets.
        try:
            print(WARNING, file=sys.stderr, flush=True)
        except Exception:
            pass
        return False
    finally:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
