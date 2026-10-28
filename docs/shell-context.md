# Shell context protocol, provisional v1

This slice is a portable parser and latest-state model only. It is not connected
to the terminal, session actor, shell preset or native footer yet. OSC number
7400 and the `stormtty-context` namespace are application conventions, not a
registered standard or a claim of ownership of that number. Other namespaces
and protocol versions are ignored. Document any future wire change as a version.

The producer sends the ordinary PWD/OSC7 update first. Its exact raw payload is
the `directory_key` below. Ghostty deliberately returns the raw OSC7 URI, not a
decoded local path. Matching uses exact bytes, without filesystem access, URI
canonicalization, case folding, host inference or path resolution. Equivalent
but differently encoded URIs do not match. This conservative rule can hide
context; it cannot carry stale Git state into a different directory.

## Wire format

The unknown-OSC callback receives content without `ESC ]` or BEL/ST. Pass that
complete content and the callback's `truncated` flag to
`parse_shell_context_osc`. Do not feed raw PTY chunks or strip delimiters with a
second parser. Ghostty owns framing, cancellation, split reads and terminators.

```
7400;stormtty-context;1;set;<directory_key>;<display_directory>;<repository_root>;<kind>;<head>
7400;stormtty-context;1;clear
```

Exactly five fields follow `set`. The four string fields use UTF-8 with percent
encoding for `%`, `;` and any byte that cannot safely appear in an OSC payload.
Both hexadecimal cases are accepted. Decode exactly once. For example, raw PWD
`file:///c/a%20b` must be encoded as `file:///c/a%2520b` in `directory_key`.
Never interpolate values into a printf format, command, eval or executable
configuration. The future Zsh encoder must use builtins, not one subprocess per
prompt, and enforce these bounds before writing control bytes.

- Entire callback content: at most 8,192 bytes. Truncated matching packets are
  rejected, even if the visible prefix happens to look valid.
- Decoded directory key, display directory, repository root: at most 2,048 bytes
  each. Decoded head: at most 256 bytes. Wire and field limits both apply.
- Directory key and display directory are required. Never truncate the key to
  make it fit; send clear if identity cannot be represented safely.
- `kind` is the literal `none`, `branch` or `detached`, not percent-encoded.
- `none` requires empty root and head, explicitly clearing previous Git data.
- `branch` requires nonempty root and head. Names are data, including literal
  percent signs, quotes, dollar signs, backticks and spaces.
- `detached` requires a root and a 7–64-character hexadecimal commit identifier.
  The field is not a command or an object to fetch.
- Strict UTF-8 rejects overlong encodings, surrogates, out-of-range scalars and
  incomplete sequences. C0/C1 controls, DEL, Unicode line/paragraph separators
  are rejected after decoding. Bidi formatting controls are visibly replaced
  with U+FFFD in display fields; the opaque key is never rendered.

`display_directory` is presentation only, e.g. `~/src`, `C:/src` or a remote
shell's path. It does not authorize chdir, opening a folder, copying arbitrary
terminal data to the clipboard or executing a host command. A `.git` indirection
file can represent a submodule as well as a linked worktree, so v1 does not
invent a worktree flag. `repository_root` is likewise a display string.

## Consumer integration contract

1. In `TerminalCore`, install Ghostty's existing unknown-sequence callback and
   set `GHOSTTY_TERMINAL_OPT_UNKNOWN_MAX_BYTES` to the wire limit. Handle only
   the OSC tag. Its content pointer is borrowed until callback return; parse
   synchronously into owned strings. Use the existing noexcept callback wrapper
   so allocation failures never escape C/COM/Win32 callbacks.
2. Keep one `ShellContextState` in the terminal actor. At the PWD callback, call
   `observe_directory` **in stream order**, before converting PWD to a queued
   effect. Updating it later in `Session::take_effects` would wrongly reject a
   following context packet from the same PTY read.
3. Pass only parser-produced messages to `apply`. A valid snapshot matching the
   latest nonempty PWD replaces the whole state. A matching v1 malformed packet,
   explicit clear or directory mismatch clears prior context. Unknown namespace
   and future-version packets leave it untouched.
4. `apply`, `observe_directory` and `reset` return whether the exposed optional
   context changed, not whether internal directory bookkeeping changed. Repeated
   equal packets return false. No event queue or intermediate-history retention.
5. Copy `context()` into the thread-safe `SessionInfo` publication, alongside
   existing `CommandSnapshot`. The reference getter itself is actor-local and
   must never cross threads. Command phase/exit status remains OSC133-owned;
   no duplicate lifecycle fields are added to this protocol.
6. RIS and new-session replacement call `reset`. At EOF the host may retain a
   copied final context as explicitly exited-session information; do not label
   it live. Parking/restoration keeps the same actor. Pane/tab/project placement
   cannot transfer context between unrelated SessionIds.

A matching directory is correlation, **not authentication**. Any PTY program can
spoof both protocols. The model intentionally has no filesystem, network,
process, clipboard or UI-action APIs. No host-side automatic Git calls occur.

## Verification

Standalone portable test, without Ghostty or platform dependencies:

```
c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I include -I tests \
  src/core/shell_context.cpp tests/shell_context_tests.cpp -o shell_context_tests
./shell_context_tests
```

CMake registers `shell_context_tests` and includes the model in
`stormtty_terminal`. Parser tests cover wire bounds, strict decoding, controls,
bidi display neutralization, literal shell-looking data, full Git clearing,
directory mismatch/clear, reset, separate session models and duplicate floods.

The terminal adapter uses Ghostty's unknown-OSC callback with an 8192-byte
capture limit. It observes PWD synchronously, before the next OSC is parsed in
that feed. All accepted fields are copied before the callback returns; no
borrowed pointers or cross-thread VT calls escape. `TerminalCore::shell_context`
returns an owned optional value. Session publication updates that value and the
working-directory effects under the same lock, including metadata-only updates.
RIS clears correlation; EOF retains a frozen final snapshot, not live status.

Real Ghostty tests cover every two-chunk split point for BEL/ST packets, CAN/SUB
cancellation, unrelated OSC, engine truncation, changed/empty PWD, explicit clear,
RIS, final output retention and independent terminals. A real child process test
covers Session publication, cwd invalidation, non-repository replacement and
immutable final copies. These are Linux core/PTY checks, **not** native Windows
runtime or UI verification. Managed-shell emission and native footer presentation
remain separate integration steps.
