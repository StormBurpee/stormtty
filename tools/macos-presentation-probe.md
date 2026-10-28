# Isolated macOS presentation baseline

This diagnostic is independent of StormTTY, PTY, CoreText, framebuffer readback,
transparency and visual-effect composition. It creates a plain AppKit-backed
`CAMetalLayer` and clears/stores directly into each drawable. It does not capture
any window/screen, request permissions, or change the production acceptance gate.

Build on macOS with the official SDK (no Ghostty or project toolchain required):

```sh
mkdir -p build/artifacts
xcrun --sdk macosx clang++ -std=c++17 -fobjc-arc -Wall -Wextra \
  -mmacosx-version-min=11.0 tools/macos-presentation-probe.mm \
  -framework AppKit -framework Metal -framework MetalKit -framework QuartzCore -framework CoreGraphics \
  -o build/macos-presentation-probe
./build/macos-presentation-probe --cametallayer > build/artifacts/cametallayer.log 2>&1
# Run separately even when the first process returns nonzero:
./build/macos-presentation-probe --metalkit > build/artifacts/metalkit.log 2>&1
```

Use an **external 30-second process timeout**; retain stdout, stderr, exit code,
runner image/architecture and source revision even on failure. The timer observes
12 seconds after application launch; `nextDrawable` can briefly block, and launch
itself is outside that observation interval. An external timeout is an ERROR, not
an inconclusive result or pass. The main AppKit event loop stays active. Frames
are paced at 100 ms, at most one GPU command is outstanding, and storage is bounded
to 256 frame records. Callback state remains owned independently of the window.

- **0 / PASS:** one frame has finite positive `presentedTime` and its matching GPU
  command completed successfully. This proves this baseline's documented onscreen
  presentation evidence, not StormTTY rendering, physical inspection or performance.
- **2 / INCONCLUSIVE:** the observation interval ended without that evidence.
  Inspect telemetry to distinguish completed GPU work, missing drawables and stalled
  work. Never translate this result into a successful native acceptance test.
- **1 / ERROR:** setup/allocation/exception/GPU failure, invalid invocation, or
  unexpected application-loop exit. A wrapper timeout must likewise be an error.

A baseline pass alongside product failure points toward product-specific lifecycle
or hierarchy differences. The same zero-timestamp symptom in both removes PTY and
terminal composition as necessary causes, but does **not** prove a hosted/headless
limitation. Frame drops, session/display conditions and framework behavior remain
possible; preserve the real macOS target and require further evidence.

## Controlled view-lifecycle comparison

No option (or `--cametallayer`) preserves the custom NSView/CAMetalLayer baseline.
`--metalkit` instead uses an MTKView-owned drawable and render-pass descriptor.
The MTKView is paused, has automatic display-triggered drawing disabled, and is
explicitly drawn by the same 100-ms timer. Its delegate uses the shared rendering
and observation code. Both paths retain identical opaque colors, direct clear/store,
pixel format, single-sample rendering, asynchronous presentation policy, outstanding
GPU limit and strict timestamp-plus-matching-GPU-completion result condition.

The dedicated workflow executes each mode in a separate owned process on the same
runner, even if the first fails or is inconclusive. `cametallayer/` and `metalkit/`
each retain their own raw log and process metadata; `comparison.json` records both.
Aggregate exit is 1 if either errors, otherwise 2 if either is inconclusive, and
0 only if both pass. No result is substituted for production acceptance.

MTKView success with baseline failure implicates custom view integration. Both
returning zero timestamps would narrow the problem to shared presentation/session
behavior, not establish that all hosted machines are unsupported. This comparison
does not capture pixels or request any recording permissions.

## API rationale

[Apple's custom Metal view sample](https://developer.apple.com/documentation/metal/creating-a-custom-metal-view)
uses `NSView.makeBackingLayer` to supply a `CAMetalLayer`.
[Presentation timestamps](https://developer.apple.com/documentation/metal/mtldrawable/presentedtime)
are zero when a drawable was not presented or its frame was dropped. Neither GPU
completion nor a callback invocation alone passes this diagnostic.
[Default asynchronous presentation](https://developer.apple.com/documentation/quartzcore/cametallayer/presentswithtransaction)
uses `presentDrawable` before command-buffer commit. Do not toggle transactional
presentation as a casual fix: it requires different scheduling and transaction
ordering. No production renderer architecture changes follow from this fixture
without comparative runtime evidence.

## Dedicated hosted diagnostic

`.github/workflows/macos-presentation-probe.yml` runs on pushes to
`diagnostics/macos-present` or explicit workflow dispatch, with a five-minute
macOS 15 job cap. It uses the owned-process runner with a 30-second deadline,
retains the raw probe output and process metadata, and leaves exit 2 failing
and explicitly INCONCLUSIVE. Timeout/abnormal exit/truncated output is ERROR.
The environment artifact includes `sw_vers`, SDK/compiler versions and selected
GPU/display properties from a time-bounded `system_profiler` call, excluding
serial numbers and EDID. Diagnostic-only changes are excluded from the full
native matrix; no native product test or acceptance criterion is altered.
