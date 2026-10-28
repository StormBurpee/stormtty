# Bundled terminal fonts

Unmodified Microsoft Cascadia Mono variable fonts from the official
[Cascadia Code v2407.24 release](https://github.com/microsoft/cascadia-code/releases/tag/v2407.24).

- `CascadiaMono.ttf`: SHA-256 `08026f617eea315caca59898784e999f6cdb2d0c34d2040deeb042bcdc359fa2`
- `CascadiaMonoItalic.ttf`: SHA-256 `d9d23dfb03c6844448a24cf0337a657ac99965262874d2e6fb5b82ac0ac08c4d`

Copyright Microsoft Corporation. Distributed under the SIL Open Font License
1.1; the complete upstream notice is in `Cascadia-LICENSE.txt`. Keep it with the
fonts in application packages. No system-wide installation is performed.

The Windows renderer uses a private DirectWrite collection when Cascadia Mono
is selected. Explicit user font choices retain system-font resolution and
fallback. Native smoke tests verify private collection selection, not merely
that a similarly named font happens to be installed on the runner.
