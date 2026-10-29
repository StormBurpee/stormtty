# Data-only theme exchange

StormTTY has eight built-in palettes (`graphite`, `instrument`, `daylight`,
`midnight`, `evergreen`, `aubergine`, `ember`, `ink`) plus an editable custom palette.
The portable `theme_exchange` service lets native hosts exchange palettes without
importing profiles, commands, key bindings, appearance settings, or experimental
feature flags. It does not discover files, download themes, or execute anything.

## Formats

The caller must select the format explicitly; filenames do not establish trust.

**StormTTY palette** is a `[theme]` fragment with quoted `#RRGGBB` values for
`background`, `foreground`, `chrome`, `accent`, and `ansi0` through `ansi15`.
All twenty colors are required; optional `name` must be `"custom"`. Export includes
that name and all colors, not the original built-in identity. To edit the full app
configuration manually, replace its existing `[theme]` section with this fragment;
do not append another section. The exported fragment is not a full app config.

**Ghostty colors** accepts `background`, `foreground`, and repeated
`palette = N=#RRGGBB` assignments, where N is 0–15. Six-digit hex without `#` is
also accepted. Missing colors retain the explicitly supplied current palette,
including StormTTY chrome and accent. Cursor and selection color fields
(`cursor-color`, `cursor-text`, `selection-background`, `selection-foreground`)
are validated but not applied: the result includes line-numbered warnings.
Ghostty export contains the eighteen supported terminal colors only, not chrome
or accent. This is deliberately a subset, not a general Ghostty config importer.

Both formats accept blank lines, full-line `#` comments, CRLF, and an optional UTF-8
BOM. Inline comments, duplicate fields, unsupported directives, named colors,
256-color indices, control characters and malformed values are rejected.
Input is bounded to 64 KiB with 1024-byte lines. Errors never return a partially
imported palette. Export validates every color before generating output.

## Windows workflow

In Settings → Appearance, **Import…** opens the native file picker. Select either
StormTTY palette or Ghostty colors in the file-type selector. Import changes only
the draft and previews through the existing settings path: **Cancel** restores
the opening configuration and **Save** persists it. Unsupported cursor/selection
colors are disclosed inline. A rejected import leaves the draft untouched.

**Export…** saves the current draft palette through the native save picker, with
overwrite confirmation. The Ghostty terminal-colors filter and inline result disclose omission of chrome
and accent. Files are restricted to regular local-drive files, bounded on read, and
exported through a flushed sibling temporary file before replacing the target.
Errors retain the prior export. No settings/profile data are included.

The Windows native dialogs still require Windows runtime qualification; a
cross-build is not proof of dialog behavior. macOS has the portable parser but
no picker buttons yet. There is no persistent multi-palette library in this change.

## Native host integration contract

Import into the **Settings draft**, not the running or saved config directly.
Present errors and unsupported-color warnings inline without native alert popups. The
existing Settings preview/Cancel/Save flow must remain authoritative. Retain the
previous custom palette when an import is canceled or rejected. File selection
must be explicit, bounded on read (do not allocate from an unchecked file size),
and must not follow URLs or use a shell. Export must use the native save dialog's
overwrite confirmation and must not silently replace a library file. These pure
APIs do not themselves provide a native picker or persistent theme library.

## Why a restricted importer?

[Ghostty's official theme documentation](https://ghostty.org/docs/features/theme)
notes that a theme can carry other configuration, not just colors. We therefore
parse only the explicit allowlist; we never pass imported files to a terminal
process. [Windows Terminal's color-scheme documentation](https://learn.microsoft.com/en-us/windows/terminal/custom-terminal-gallery/custom-schemes)
also separates color schemes from other settings. Windows Terminal JSON is not
yet an accepted interchange format here.

Tests cover all built-ins, native lossless color roundtrips, Ghostty preservation
of absent colors, unsupported-color warnings, strict format separation, invalid
and executable-looking input, bounds and hostile-byte parsing. Linux tests and
Windows cross-compilation are distinct from native file-picker/runtime evidence.

## Exact-branch native gate

`.github/workflows/theme-native.yml` runs on `feature/theme-exchange`, checks out
`${github.sha}`, builds with MSVC, and retains SHA-stamped screenshots and test
logs. Owned native fixtures exercise real bounded file reads, atomic replacement,
locked-destination failures and temporary-file cleanup; repeated exports; malformed
and oversized files; and relative, UNC and alternate-stream rejection. The native
STA dialog fixture opens and cancels the real import and export dialogs using only
its own thread's owned windows; it also checks null/destroyed owner rejection.
It does **not** automate file selection or prove interaction with a user desktop.
CTest bounds each native fixture to 30 seconds. The app capture uses the existing
owned Settings smoke process and proves the two actual buttons are visible and
enabled, with authored palette preview and saved/reopened captures. Artifact
existence is not visual approval: inspect the images after the exact native run.
