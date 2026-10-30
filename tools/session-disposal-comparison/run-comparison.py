"""Observe each immutable head once; never retry or claim production qualification."""

import hashlib
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import traceback


ROOT = pathlib.Path(__file__).resolve().parent
WORKSPACE = pathlib.Path(os.environ["GITHUB_WORKSPACE"])
ARTIFACTS = WORKSPACE / "comparison-artifacts"
CASES = [
    (
        "baseline",
        "9b9ce10f1750b46e630f31f001281c7c3a49d0c9",
        "da6e89b8409539549a6a9d9bf045dff6b6631125",
    ),
    (
        "candidate",
        "093df896d3d57205a6945bd0b1bd9bd0391abcc4",
        "19a36327635cd6070aa297b47fe3652455f16137",
    ),
]


def git(source, *arguments):
    return subprocess.check_output(
        ["git", "-C", str(source), *arguments], text=True
    ).strip()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_record(path, record):
    path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def build_command(arguments, log):
    with log.open("ab") as output:
        output.write(("COMMAND " + repr(arguments) + "\n").encode())
        output.flush()
        subprocess.run(
            arguments, stdout=output, stderr=subprocess.STDOUT, check=True
        )


def verify_source(source, head, tree, expected_inputs, record):
    record["head"] = git(source, "rev-parse", "HEAD")
    record["tree"] = git(source, "rev-parse", "HEAD^{tree}")
    if record["head"] != head or record["tree"] != tree:
        raise RuntimeError("Immutable source identity mismatch")
    for filename, digest in expected_inputs.items():
        content = subprocess.check_output(
            ["git", "-C", str(source), "show", "HEAD:" + filename]
        )
        if sha256(content) != digest:
            raise RuntimeError("Input mismatch: " + filename)
    record["dependency_lock_sha256"] = sha256(
        (source / "tools/bootstrap.lock.json").read_bytes()
    )


def build_diagnostic(source, build, artifacts):
    engine = WORKSPACE / "baseline/.deps/ghostty-native"
    build_command(
        [
            "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
            "-DCMAKE_CXX_COMPILER=cl",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DBUILD_TESTING=ON",
            "-DSTORMTTY_BUILD_APP=OFF",
            "-DSTORMTTY_SESSION_DISPOSAL_DIAGNOSTICS=ON",
            "-DSTORMTTY_GHOSTTY_ROOT=" + str(engine),
        ],
        artifacts / "build.log",
    )
    build_command(
        [
            "cmake", "--build", str(build),
            "--target", "session_disposal_diagnostics", "--parallel", "2",
        ],
        artifacts / "build.log",
    )


def load_owned_runner(source, label):
    spec = importlib.util.spec_from_file_location(
        "owned_runner_" + label, source / "tools/run-owned-process.py"
    )
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    return runner


def run_case(label, head, tree, patch, expected_inputs):
    source = WORKSPACE / label
    build = WORKSPACE / ("build-" + label)
    artifacts = ARTIFACTS / label
    artifacts.mkdir(exist_ok=True)
    record = {
        "label": label,
        "expected_head": head,
        "expected_tree": tree,
        "overlay_sha256": sha256(patch.read_bytes()),
        "workflow_head": os.environ["GITHUB_SHA"],
        "run_id": os.environ["GITHUB_RUN_ID"],
        "attempts": 0,
        "outer_timeout_seconds": 90,
        "fixture_wait_seconds": 10,
        "scope": "diagnostic observation, not production qualification",
        "status": "preparation",
    }
    try:
        verify_source(source, head, tree, expected_inputs, record)
        git(source, "apply", "--check", str(patch))
        git(source, "apply", str(patch))
        record["instrumented_diff_sha256"] = sha256(
            subprocess.check_output(["git", "-C", str(source), "diff", "--binary"])
        )
        build_diagnostic(source, build, artifacts)
        runner = load_owned_runner(source, label)
        record["attempts"] = 1
        record["status"] = "running"
        write_record(artifacts / "provenance.json", record)

        # Exactly one invocation, with the original outer deadline and fixture.
        result = runner.run_owned(
            [str(build / "session_disposal_diagnostics.exe")],
            timeout=90,
            cwd=build,
            output_dir=artifacts,
        )
        (artifacts / "session-disposal.log").write_bytes(result.output_tail)
        write_record(artifacts / "owned-process.json", result.metadata())
        runner_failed = (
            result.timed_out
            or not result.reaped
            or result.output_truncated
            or bool(result.cleanup_error)
        )
        record["exit_code"] = result.exit_code or int(runner_failed)
        record["status"] = "passed" if record["exit_code"] == 0 else "failed"
        print(label, result.output_tail.decode("utf-8", errors="replace"), flush=True)
    except Exception as error:
        record["status"] = (
            "preparation-or-build-failed" if record["attempts"] == 0 else "runner-failed"
        )
        record["error"] = str(error)
        (artifacts / "exception.log").write_text(
            traceback.format_exc(), encoding="utf-8"
        )
    finally:
        write_record(artifacts / "provenance.json", record)
        print(json.dumps(record), flush=True)
    return record


def main():
    ARTIFACTS.mkdir(exist_ok=True)
    expected_inputs = json.loads((ROOT / "expected-inputs.json").read_text())
    patch = ROOT / "session-disposal-overlay.patch"
    # A failed baseline still permits the single candidate observation.
    results = [run_case(*case, patch, expected_inputs) for case in CASES]
    write_record(ARTIFACTS / "comparison.json", results)
    return 0 if all(result["status"] == "passed" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
