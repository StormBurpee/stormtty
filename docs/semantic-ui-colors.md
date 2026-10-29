# Shared chrome color roles

`resolve_ui_colors(theme)` supplies the same opaque roles to the Windows shell
and Settings: canvas, surface, navigation, raised/control states, ink/secondary,
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
ContextMenu and other legacy component-local palettes are not migrated here.
