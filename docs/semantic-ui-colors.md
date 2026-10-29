# Shared chrome color roles

`resolve_ui_colors(theme)` supplies the same opaque roles to the Windows shell
and Settings/context menus: canvas, surface, navigation, raised/control states, ink/secondary,
divider, accent/selection, and contrasting primary-action text. Navigation mixes
surface toward canvas; selection mixes 16% accent into surface and hover mixes
6% ink. No role depends on the theme's display name or fixed dark/light blues.

The user's four base theme colors and terminal ANSI entries remain unchanged.
Material opacity and finite reveal animations still apply at their existing
paint boundaries; these colors do not change terminal glyph alpha or backdrop
behavior. Windows shell contrast mode retains its OS colors. Settings retains
stable default semantic tokens before its existing OS-color remapping, including
HighlightText and disabled text; custom colors cannot alias those mappings.

Portable coverage checks every built-in theme, custom/renamed themes, invalid
hex fallback, opaque roles, primary-label contrast for 4096 accent samples, and
actual Settings scene roles plus exact OS-palette-only contrast output. Existing
Settings interaction/preview/cancel/save tests remain required. Native same-theme
shell and Settings captures are required before declaring visual qualification.

ContextMenu now uses the shared surface, divider, ink/secondary/subdued,
accent, selected and selected-border roles. `set_theme` is palette-only and
idempotent for equivalent colors; name/ANSI-only edits do not change menu
geometry, selection, scrolling, pending presses or actions. The Windows host
refreshes it in `settings_changed`, shared by startup, live Appearance preview,
Cancel, Save, Reload and accessibility refresh. The OS high-contrast colors
still win exactly, including HighlightText for selected destructive items and
GrayText for disabled labels/reasons. Toggling contrast off reveals the latest
configured theme, not the old Graphite palette.

The new `danger` role is UI-only, never ANSI red. It retains the familiar warm
red where readable and adjusts toward the strongest black/white endpoint to
reach 4.5:1 against both surface and selected backgrounds. If an adversarial
custom pair cannot share that contrast, the strongest endpoint is used. This
is **not** a repair of arbitrary custom foreground/accent readability. Disabled
menu items use subdued ink and stronger secondary ink on hover; all built-ins
retain at least 3:1 for those inactive labels on their respective backgrounds.

`ui_colors_tests` checks actual menu scenes for every built-in, light/custom,
renamed and invalid palettes; exact OS colors; normal/selected destructive
contrast; disabled hover; submenu/scroll roles; a live press and submenu across
a palette change; and Settings preview/Cancel/failed Save/retry/reopen/Reload.
The owned Windows `--smoke-settings` fixture additionally checks the palette
already applied by the host during light/dark/custom preview and Cancel/Save/
Reload. It captures real system menus after Cancel, saved Custom, and Reload,
each at normal/narrow window sizes, without opening a menu over Settings or
bypassing its input gate. A scoped reentry guard protects capture/resize work;
config, actor identities, frontend key/mouse input counters and restored focus
are checked. Normal focus/protocol and resize controls still reach the actors;
the fixture does not claim zero actor traffic.
These WARP captures are native scene evidence, not physical desktop/OS
accessibility qualification. Other legacy component-local palettes remain
outside this migration.
