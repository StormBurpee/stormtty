# StormTTY engineering contract

Build an actual native terminal for Windows and macOS, with Windows first. Not a webview or a mock shell.

- Shared C++20 with exact-pinned libghostty-vt C API. Windows: Win32, DirectWrite, D3D11/Direct2D. macOS: AppKit/Objective-C++, CoreText/Metal and POSIX PTY behind platform interfaces.
- Keep platform/window/process code separate from portable workspace, commands, configuration and terminal adapter.
- Own all native handles with RAII. No exception may cross a Win32, COM or C callback.
- Do not retain borrowed Ghostty pointers across mutation or threads. Publish owned immutable frames.
- Never discard PTY bytes to manage load. Bound queues, parsing work, scrollback and image decoding.
- Treat terminal titles, links, clipboard requests and image payloads as untrusted data. No automatic execution.
- Keep reference repositories read-only. Preserve upstream notices; do not relicense original application code without an explicit decision.
- Tests must distinguish Linux/core, cross-compilation, Windows runtime and physical UI verification.
- Do not claim native performance, Ink compatibility, IME, accessibility or graphics are verified unless the corresponding evidence exists.
- Use workspace-backed caches/TMPDIR; do not fill /tmp tmpfs with builds.
- Coordinate owned files. Do not rewrite another worker's implementation or shared build files without agreement.
- Root owns remote publication and CI. No external Codex task without the user's separate approval.

- Keep editor/vendor project settings out of the repository. CLion profiles and exclusions belong to the developer's local settings; maintain the shared CMake build as the source of truth.
