# StormTTY

An owned, Windows-native terminal built around a pinned libghostty-vt engine and a purpose-built Windows application.

## Direction

- Native Win32 windowing with an integrated, restrained titlebar and tabs.
- DirectWrite text shaping and GPU-backed DirectX rendering.
- Real ConPTY sessions, keyboard-first commands, movable split panes and file-based configuration.
- Terminal correctness before decoration: Unicode, input, resize, synchronized output and graphics must be verified end to end.
- Purposeful motion, accessible interaction and reduced-motion support. No webview terminal or unsolicited AI dashboard.

## Current status

Architecture and the first native vertical slice are under development. This repository does not yet offer a production-ready application or verified Windows runtime compatibility. Build, test and performance claims will be tied to recorded evidence.

Ghostty is a separately maintained dependency, not code claimed as original StormTTY work. Its MIT notice and resolved dependency notices will accompany distributions. StormTTY application licensing has not yet been designated.
