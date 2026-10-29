# Native theme library and materials

Appearance offers Graphite, Instrument, Daylight, Midnight, Evergreen, Aubergine,
Ember and Ink. Each palette supplies terminal defaults, chrome, an accent and
16 ANSI colors. Indexed colors 16–255 remain the engine's standard color cube
and grayscale; explicit truecolor output is never recolored. Application OSC
palette overrides retain precedence; OSC resets use the selected theme defaults.

The native sample and ANSI strip use the draft palette. Choose the color role
button (or use Left/Right) to reach Background, Foreground, Chrome, Accent and
ANSI 0–15. Activate its hex value, type six hex digits and press Enter. Escape
abandons a partial edit; a second Escape cancels Settings. Switching palettes
retains the current Custom draft during the open editor session. Save persists
the selected palette and all 20 colors in the existing declarative config v1;
Cancel restores the running palette. A custom palette is data, never code or a
network-loaded plugin. Older v1 files without ANSI keys remain valid.

## Material boundary

Background opacity accepts the full 0–1 interval (keyboard Home/End or drag).
It affects only the default canvas, not glyphs or explicit ANSI backgrounds.
Solid, Mica and Acrylic use the existing documented Windows system-backdrop
backend. DWM chooses the blur kernel: **this backend cannot set a blur radius**.
Opacity changes tint coverage, not blur strength. The UI therefore names the
control Background opacity, labels system blur as fixed and reports the actual
host fallback. It does not misrepresent opacity as an adjustable blur slider.
A future variable-radius blur needs a separately qualified composition backend;
the experimental probe is not a production capability.

## Verification

Portable coverage: palette parsing/roundtrip/invalid ANSI, native control
keyboard traversal and clipping, hex draft rejection/commit/Escape, retained
Custom, Save/reopen/Cancel, and Ghostty indexed color preview/rollback/OSC resets
while preserving explicit truecolor. Windows cross-build is compile-only.

Native acceptance still requires Windows runtime evidence: open Appearance at
100/150/200% DPI and at narrow sizes; scroll to every control; choose each theme;
edit Background and ANSI 1 while two panes contain SGR color output; Cancel and
verify both panes, then Save and reopen. Verify 0%, 50%, 100% opacity over a
contrasting external window, each material, Windows transparency disabled,
high contrast, and software/composition-unavailable fallback. The hex editor
must consume input without sending any keystroke to the PTY. macOS receives
startup palette values; its existing host does not yet expose this Windows
Settings panel, so no macOS live-editor claim is made.

The Windows `--smoke-settings --capture <path>.png` owned fixture also exercises
Midnight selection, partial hex Escape, native-key ANSI editing, Custom retention,
Ribbon visibility preview/Cancel and order/visibility Save/reopen. It captures
`-appearance-custom-preview`, `-appearance-custom-edit`,
`-appearance-saved-reopened`, `-ribbon-preview`, `-ribbon-before-save` and
`-ribbon-saved-reopened` PNGs via the real renderer. These are synthetic native
smoke scenes, not screenshots of the user's terminal or physical DWM blur proof.
ANSI engine resolution/OSC semantics remain covered by the portable terminal
suite; the native smoke checks host/config propagation and actor identity/defaults.


## Additional Windows blur

Appearance now has a separate **Additional blur** slider (0–64). It is persisted
as `appearance.additional_blur`, previews immediately, and follows the existing
Cancel/Save/reload lifecycle. Background opacity remains independent. At zero,
the existing system material renderer is unchanged: zero does **not** mean a
sharp desktop, because Windows already blurs its host backdrop.

On native MSVC/Windows SDK builds, positive values with Acrylic/Auto request a
Gaussian CompositionEffectBrush over CreateHostBackdropBrush. A separate sibling
visual presents the unchanged premultiplied terminal swapchain above it, keeping
text outside the effect. Mica, Solid, WARP, high contrast, transparency-off and
unsupported runtime/toolchain paths do not enable this graph. The settings status
reports unavailability instead of pretending opacity is blur. macOS exposes no
public adjustable radius here and keeps the control disabled.

No desktop capture, private API, global window alpha, additional runtime package,
or per-pane blur is used. Presentation failures retain the existing DComp/ordinary
opaque-child fallback. Resource recreation reattaches the requested graph when
possible; graph loss changes material status even if composition itself survives.
The dispatcher queue is retained across preview changes and shut down only after
the application HWND has been destroyed and its user data cleared.

Qualification: the standalone native MSVC/WARP experiment established effect ABI
and child recovery lifecycle, **not** physical blur quality. Production native
compile, renderer tests and hardware/interactive spatial-blur verification are
separate gates. Physical live-window blur, sharpness and GPU cost must still be
checked on an interactive Windows 11 hardware session; no pixel/blur claim follows
from WARP, source review or a screenshot of the slider alone.


The native `host_backdrop_backend_tests` target now exercises the **actual app
helper** against an owned WARP swapchain source (not desktop pixels): three
attach/update/resize/detach cycles and explicit final dispatcher shutdown. It
requires completion, with a 30-second external test deadline. Final shutdown
continues pumping bounded batches after a five-second warning; it does not release
a still-started controller or balance WinRT early. An exceptional shutdown failure
returns failure and retains that owner until process exit. This ownership test
still cannot prove hardware host-backdrop sampling or spatial blur quality.
