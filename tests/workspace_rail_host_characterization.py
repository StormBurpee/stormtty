#!/usr/bin/env python3
"""Compile the actual host sampling block against the frozen legacy environment.

No NativeWindow, native runtime or physical presentation is instantiated. The
fragment is taken from the tested checkout, not a second copy of host sampling.
Use an existing output directory/compiler; this is a small standalone build.
"""
import argparse
import pathlib
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--output-dir", type=pathlib.Path, required=True)
parser.add_argument("--compiler", default="g++")
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "src/platform/windows/window.cpp").read_text()
scene = source.index("WindowScene NativeWindow::scene()")
start = source.index("    if (navigation_.rail.width > 0) {", scene)
end = source.index("    for (const auto& placement : layout_.panes)", start)
fragment = source[start:end]
assert fragment.count("WorkspaceRailRenderer{}.append_scene(") == 1
args.output_dir.mkdir(parents=True, exist_ok=True)
fragment_path = args.output_dir.resolve() / "workspace_rail_host.inc"
fragment_path.write_text(fragment)
output = args.output_dir.resolve() / "host-characterization"
command = [args.compiler, "-std=c++20", "-O0", "-Wall", "-Wextra", "-Werror",
           "-I" + str(root / "include"),
           '-DSTORMTTY_RAIL_HOST_FRAGMENT="' + str(fragment_path) + '"',
           str(root / "tests/workspace_rail_renderer_tests.cpp"),
           str(root / "src/core/workspace_rail_renderer.cpp"), "-o", str(output)]
subprocess.run(command, check=True)
subprocess.run([str(output)], check=True)
