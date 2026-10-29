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

Cleanup is a harness concern, not an application qualification result. The
runner checks the freshly created root identity and exact ownership marker,
rejects links/reparse points and multiply linked or special files before any
deletion, and retains uncertain process roots. For a Windows access-denied
unlink of a verified readonly loose Git object in the owned sample repos only,
it clears that file's readonly attribute and retries once. It never changes
ACLs, repairs directories, or retries other permission/sharing failures. The
marker remains until child cleanup succeeds; failure is reported and the
remaining fixture is retained. This assumes the private fixture has no other
writers after the owned processes exit; it is not a hostile concurrent-writer
sandbox. The existing captured frames remain valid if later harness cleanup
fails; the overall CI step still fails and must not be reported green.

The attribute-only repair follows Python's Windows
[`rmtree` example](https://docs.python.org/3.12/library/shutil.html#rmtree-example)
and [`chmod` contract](https://docs.python.org/3.12/library/os.html#os.chmod), with
stricter ownership, regular-file and object-path checks.
