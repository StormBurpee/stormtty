# ADR 0001: Native foundation

Status: accepted for the first implementation, subject to Windows verification.

StormTTY owns its Windows application, workspace model, configuration, process transport, renderer and interaction design. It reuses the MIT-licensed public libghostty-vt terminal state engine at exact commit 46e9dc9a927b2e7e7cba79e6902c176127dbdf2c.

Use C++20 with Win32, DirectWrite and a D3D11/DXGI surface. Direct2D is the initial correctness renderer. There is one compositor surface per window, not one swapchain per pane. No browser runtime or per-cell UI objects.

The initial visual direction is Instrument: integrated custom titlebar/tabs, restrained graphite and cold-steel focus accents, no permanent sidebar/status strip in a single-pane session. Real shell output owns the canvas. Motion follows user action and respects reduced-motion settings.

The first milestone is a real ConPTY child connected bidirectionally to the VT adapter and native renderer. Tabs and split trees are portable models, not process lifetime owners. Input, output, resize and shutdown must be bounded and ordered; no joins or blocking PTY calls inside window callbacks. Effects are copied into explicit queues. Frames own their contents.

A pinned Microsoft ConPTY distribution is the preferred provider candidate; runtime must identify the loaded provider and actual host. Inbox fallback is explicit rather than a silent compatibility claim. Exact Windows support, IME, UIA, graphics, input latency and performance remain gates, not claims.

Acceptance evidence has four tiers: portable tests; cross-compiled binaries; actual Windows CI execution; physical Windows interaction/visual QA. A lower tier never substitutes for a higher one. Performance budgets are targets until measured on recorded hardware/builds.

See the council memos for alternatives, rejected shortcuts, ownership constraints and acceptance criteria. Original StormTTY application licensing is not designated by this decision. Third-party notices must be retained.

## Platform update (user direction, 2026-10-28)

macOS is a supported product target, with Windows delivered first. Shared C++ terminal/workspace/command/configuration code must not import Windows headers or own native handles. Platform adapters own windowing, text shaping/rasterization, GPU presentation, clipboard/input/accessibility and PTY transport. macOS uses an AppKit/Objective-C++ shell with CoreText/Metal services and POSIX PTY transport; it is not a Windows compatibility layer or a webview. Command bindings and caption geometry are platform-specific, while session/workspace behavior and configuration schemas stay shared. Windows-first delivery does not constitute verified macOS support.
