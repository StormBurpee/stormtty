#!/usr/bin/env python3
"""Owned WARP-only ABI/lifecycle diagnostic. Never hardware/physical blur evidence."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys


def classify(returncode, timed_out, reaped, cleanup_error, output):
    if timed_out or not reaped or cleanup_error:
        return "failed"
    if returncode == 3 and "PROBE_RUNTIME_UNSUPPORTED=1" in output:
        return "runtime-unsupported"
    if (returncode == 0 and
            "Owned surface Gaussian graph: constructed=1 scalar_sweep=0,8,24,48 software=1 host_pixel_proof=0" in output and
            "Host backdrop smoke: result=0 software=1 host_active=0" in output):
        return "warp-graph-lifecycle-passed"
    return "failed"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("host_backdrop_owned_runner", Path(__file__).resolve().parents[1] / "run-owned-process.py")
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    records = []
    for no_redirection in (False, True):
        variant = "no-redirection" if no_redirection else "ordinary"
        command = [str(args.exe.resolve()), "--warp", "--smoke"]
        if no_redirection:
            command.append("--no-redirection")
        try:
            result = runner.run_owned(command, timeout=30, reap_timeout=5, output_dir=args.output)
            output = result.output_tail.decode("utf-8", errors="replace")
            status = classify(result.returncode, result.timed_out, result.reaped, result.cleanup_error, output)
            record = dict(variant=variant, status=status, **result.metadata())
        except (OSError, ValueError) as error:
            output = str(error)
            record = dict(variant=variant, status="failed", start_error=str(error))
        record.update(physical_blur_verified=False, hardware_host_verified=False)
        (args.output / f"{variant}.log").write_text(output, encoding="utf-8")
        records.append(record)
        print(f"{variant}: {record['status']}; physical_blur_verified=0; hardware_host_verified=0")
        if record["status"] == "runtime-unsupported":
            print(f"::warning::Host-backdrop {variant}: hosted runtime unsupported; graph/lifecycle NOT passed.")
    (args.output / "result.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    summary = "## Isolated host-backdrop diagnostic\n\nNative build is a separate gate. No hardware host or physical blur proof.\n\n"
    summary += "\n".join(f"- {r['variant']}: **{r['status']}**" for r in records) + "\n"
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as stream:
            stream.write(summary)
    return 1 if any(r["status"] == "failed" for r in records) else 0


if __name__ == "__main__":
    raise SystemExit(main())
