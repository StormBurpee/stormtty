# Native Windows teardown qualification

`--smoke*` launches emit flushed, fixed-stage `Native CI teardown` records. Normal
launches do not. Records contain uptime/thread IDs and fixed stage names, not
process memory, command arguments, environment values, or file contents.

The owned-process runner remains the authority for completion: a loop-exit or
last destructor marker is not a passing process exit. No teardown is skipped,
no forced-exit API is added, and all owned resources are released.

## Single-drain candidate (not yet runtime-qualified)

The retained failure from commit `64e99b6`, run `40736011873`, Windows job
`122984642100`, explicit-runtime repeat 8, reached the second renderer
`ClearState begin` after the first ClearState/Flush and D2D releases returned.
The owned host remained alive at the unchanged 30-second timeout and was killed
and reaped. The other twelve repeats passed; they do not erase that failure.

Full GPU teardown now unbinds the D2D target, releases canvas/image resources and
the D2D contexts/device, then releases presentation resources and performs one
ClearState/Flush before releasing the D3D context/device. Standalone swapchain
replacement retains its ClearState/Flush. This consolidates two drains, rather
than bypassing teardown. Microsoft documents releasing references followed by
[ClearState/Flush before replacing a flip-model swapchain](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush).
Flush is asynchronous: a returned call does not prove completed GPU work, and
the documentation does not establish that the previous second ClearState was
invalid. The ordering change is a controlled hypothesis, not a confirmed causal
fix, and must pass the native repeat batch below with complete host exit.

Keep the failing record alongside the new batch. Require every original smoke
assertion and owned-process result, plus complete renderer/application teardown
markers; do not accept only the earlier smoke-success or UI-loop-exit message.
Also exercise existing renderer/device-recreation tests so the preserved
swapchain-replacement path is checked. If a stall recurs, retain its exact stage
and obtain sanitized native stacks before cleanup rather than lengthening the
timeout or silently retrying it away.

## Repeatable probe

On the native Windows CI runner with the packaged runtime, run ten independent
`StormTTY.exe --shell-runtime <absolute bundled root> --smoke-settings --capture
<unique image>` launches through `tools/run-owned-process.py --timeout 30`.
Retain each launch's output, `OWNED_PROCESS_RESULT`, and frame. Continue the
bounded ten-launch batch after failure, but fail its aggregate if any launch
fails/times out. Use three no-override `--smoke-settings` launches as controls.
Do not reuse application processes, relax timeouts, or accept a later success as
a replacement for the first failing record.

Read the last entered/completed pair:

- Loop exit before `window class unregister end`: class cleanup.
- Window destructor body: close requests, HWND destruction, settings worker or
  smoke settings file cleanup (each has boundary records).
- Renderer destructor: per-resource `Release`, `SetTarget`, `ClearState`, `Flush`,
  image-cache clear, wait-handle close, opaque HWND destruction, and apartment
  unwind are distinguished. The unchanged implicit cache/font destruction
  follows `release_gpu end`; DWrite/D2D final releases have their own pairs.
- Renderer end before `native application locals destroyed`: remaining window
  members (including session references), not a GPU release.
- `UI CoUninitialize begin` without end: main STA uninitialization.
- `wWinMain locals destroyed; returning to CRT` followed by an owned-process
  timeout: investigate CRT/static/DLL process teardown, not the UI loop.

A Windows GNU cross-build checks compilation only. Reproduction and attribution
require native runtime evidence. If markers are insufficient, obtain narrowly
sanitized thread/module/stack summaries rather than uploading raw process dumps:
stack and dump memory can contain CI credentials even for smoke processes.
