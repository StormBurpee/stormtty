# Persistent scene damage ownership

`SceneDamageTracker` owns the previous canvas scene and invalidation state on the
renderer owner thread. Its retained frames are owned immutable `FramePtr`s. It
has no HWND, GPU resource, renderer reference, presentation state, or callback.
`render_geometry` supplies the same physical-pixel bounds to damage and drawing.

## Commit and retry contract

- A new tracker requires a full surface repaint.
- Damage queries do not consume invalidation or replace the retained scene.
- An unchanged scene with no pending presentation is committed before returning.
- Successful canvas `EndDraw` commits the scene and clears invalidation **before**
  `Present1`. Busy/occluded presentation retries retain that canvas and do not
  roll back the scene. `pending_present` stays in the native renderer.
- Committing normalizes retained `force_redraw` to false.
- Exceptions from the drawing body call `EndDraw`, invalidate, and rethrow, as
  before. An error returned by `EndDraw` itself still follows the original
  `check`/device-recovery path without a newly added invalidation. A potential
  improvement for non-device-lost EndDraw errors is outside this refactor.

## Explicit invalidation mapping

All former Windows renderer `invalid = true` sites delegate to the tracker:

- stopping, starting, or updating the dormant Liquid Glass source;
- creating/recreating canvas surfaces (still clears pending presentation);
- measuring/changing fonts and clearing the shape cache;
- draw-body exceptions;
- resize, background-opacity changes, additional-blur updates/rebuilds;
- explicit device recreation.

GPU teardown, presentation, material ownership, Liquid Glass canvas taint, and
alpha diagnostics remain native and unchanged. Default feature gates remain off.

## Portable characterization

Run `scene_damage_tests` via CTest. It compares exact ordered rectangles, not just
covered area: scene/pane changes; absent frames; row identities/generations,
null/removed rows and clipping; text blink; cursor color/shape/focus/reveal,
wide tails and fallback metrics; image identity/generation contracts; chrome
generation, line padding and outward-rounded clips; and the pre-merge 128/129
fragment boundary. It also checks retention replacement and retry invalidation.

This is not Windows runtime proof. Native `renderer_smoke` (including device
recreation), renderer teardown qualification, and physical UI verification
remain separate gates described in the renderer/Windows verification documents.
