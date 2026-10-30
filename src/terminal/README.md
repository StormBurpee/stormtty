# Portable terminal boundary

`TerminalCore` consumes the exact-pinned public libghostty-vt C ABI. It has no
window, PTY, GPU, clipboard, COM, or other platform dependencies. The session
actor constructs, calls, and destroys it on one thread; wrong-thread methods
throw. Renderers only receive `FramePtr` (`shared_ptr<const RenderFrame>`).

## Ownership and incremental work

Private `TerminalFrameMaterializer` owns render-state and iteration resources,
last-frame reuse, row generations and image copying. `TerminalCore` remains the
actor-thread/mutation authority and invokes it synchronously, including inside
the permitted render-hold callback. Initial palette sampling is a specific owned
value result, not a raw render-state accessor. The materializer does not schedule
work, tick animation or decide when held output may advance.

- `feed` parses bytes without constructing a host frame on every input. A
  requested `snapshot` materializes dirty rows and shares unchanged immutable
  rows. Content generations do not advance for cursor-only row invalidations. Cheap
  color/cursor metadata is compared even when the engine dirty flags are clean;
  changed default colors re-resolve row colors before publishing.
- Row IDs come from the public API, not unpacked private cell layouts. Generation
  values are equality/cache keys, not timestamps.
- Pixel buffers are copied only for new image generations, under the same actor
  ownership as placement capture. Placement geometry is already resolved for
  fixed, Unicode-placeholder, and relative placements. Do not look up engine
  images later on a renderer thread, or issue a second storage-placement pass.
- Previous frames survive subsequent feeds, buffer switches, resizes, and core
  destruction. Session/render queues must retain a bounded number of frames;
  shared ownership does not make arbitrary historical retention memory-free.
- The adapter clears engine dirty flags only after all owned frame data has been
  constructed. A failed GPU presentation can retry the same owned frame.

## Protocol behavior

- Optional `default_foreground`/`default_background` configure actual engine RGB
  defaults, not renderer-only replacements. SGR 39/49/0 and OSC 110/111 reset to
  those defaults; OSC 10/11 queries return effective configured/overridden values.
  Explicit true-color and indexed ANSI colors remain unchanged. All configured
  colors must have alpha 255; default-background translucency stays renderer-only.
  Unspecified colors use the initial engine render defaults; both defaults are
  installed so one-sided optional themes work with the pinned engine.

- DEC 2026 begins by capturing the exact pre-hold frame *inside* the engine's
  allowed render-hold callback, even if more bytes follow in the same feed. The
  actor must honor `next_deadline()` to expire stuck holds (default one second).
- Kitty animation is caller-clock-driven. Snapshots advance it; the returned
  next deadline avoids idle polling and is capped at 120 wakeups/second. Cursor
  blinking and display refresh scheduling are renderer responsibilities.
- Key/mouse encoders refresh negotiated engine modes before encoding. Focus and
  paste use the C helpers and engine modes. No handwritten ANSI key tables.
- Key text is unmodified layout text, not C0/DEL key transformations. Platform
  code owns keyboard layout, AltGr, physical normalization, preedit, and IME
  commit deduplication. The reused C event releases its borrowed text view before
  returning to the caller.
- Clipboard reads/writes are always denied synchronously, with copied bounded
  audit effects. The C reply object cannot outlive its callback: deferred UI
  approval is not implemented. Application-initiated paste is separate and can
  explicitly approve the exact unsafe payload.
- Reply effects preserve their callback order. The session must route them to the
  PTY, not treat them as UI notifications. Exhausting the effect budget is a
  fatal, explicit session error, never silent protocol-byte eviction.

## Bounds and currently unsupported behavior

Defaults: 64 MiB Kitty storage **per engine screen**, 32 MiB approximate
scrollback (engine page-granularity, optional additional `scrollback_lines` limit), 4 MiB APC buffering, 1 MiB clipboard write,
4 MiB/4096-entry effect queue, 16 MiB copied selection, 1,048,576 viewport cells.
Captured image placements additionally cap at 65,536. Both engine screens,
current/retained frames, parser buffers, and renderer textures consume memory;
these are component budgets, not a claimed process-RSS ceiling.

Only direct Kitty transmissions are enabled. File/temp-file/shared-memory image
transports are disabled. PNG needs a process-startup platform decoder; the core
checks IHDR dimensions and the smaller of process/terminal decoded-byte budgets before calling it, verifies the
returned RGBA geometry, then allocates the engine-owned output with Ghostty's
allocator. Native decoder correctness is a separate platform test.

No SIXEL, iTerm image protocol, arbitrary URI execution, persistence/search API,
platform IME/accessibility, or native GPU/window functionality is supplied here.
Default `resize_pull_scrollback=false` is required for ConPTY; POSIX backends may
opt in. Engine protocol tests do **not** prove protocol transport through ConPTY,
WSL, SSH, or native renderer correctness/performance.
