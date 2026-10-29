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
