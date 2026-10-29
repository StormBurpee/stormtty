# Owned native presentation captures

`native-presentation.yml` is isolated from normal CI and the native craft
qualification workflow. It runs only on its named feature branch or an
explicit dispatch. Its images are **owned demonstrations**, not user projects,
performance measurements, paste qualification, or fabricated terminal logs.

The runner creates fresh temporary `Theme Lab` and `Notes` Git repositories,
commits authored sample files locally, then modifies a tracked palette file
and adds real untracked notes. Git configuration, HOME and application-data
paths are private to that run. No remote repository or user checkout is used.

`--smoke-presentation` requires the explicit marker-validated local fixture,
an actual managed Zsh runtime, and a capture path. The app creates real actors
and displays their observed `git status --short --branch` and `git diff --stat`
output. The fixed printable command is submitted through normal text/key
input, **not the paste path**. Existing paste/typing qualification is unchanged.

The app waits for the real Git branch context, output, restored shell input
mode, and an actual presented frame. It captures the workspace and both
Navigator views using the native renderer, then requires all owned shells to
exit successfully. The runner refuses success for missing images, truncation,
timeout, failed exit, or uncertain reaping. It leaves the fixture intact when
process ownership is uncertain (including timeout, cleanup errors, or failed exit); it never enumerates or kills unrelated trees.

Normal/narrow functional captures remain in `native-craft.yml`; the demo does
not replace them. High contrast, physical accessibility, IME, DWM materials,
and frame pacing still require their own evidence. Images must be shared as
native owned-demo frames without pixel alteration.
