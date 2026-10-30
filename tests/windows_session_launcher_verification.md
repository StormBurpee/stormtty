# Windows session launch boundary

`NativeWindow` constructs and owns one UI-thread-confined
`WindowsSessionLauncher`. It owns the resolved managed profile, construction-time
inherited PATH snapshot, and immutable launch overrides. Current saved/preview
configuration and project selection are borrowed only during `prepare`; returned
process/terminal options own their inputs. Read-only profile queries support the
host's existing labels and managed-shell affordances without duplicate ownership.

Preparation does not allocate an actor, start a process, register a wake callback,
write configuration, or change the parent environment. HOME discovery and native
state/DACL validation remain launch-scoped calls to the existing services. State
preparation can create validated app-owned state, exactly as before extraction.
The smoke state fixture remains host-owned and is supplied explicitly; the
launcher neither creates nor retires that fixture's root.

The host transaction is unchanged: prepare, allocate actor ID, append creation
ledger, form wake callback/start session, record smoke evidence, register session,
then assign watch token. Preparation failure precedes allocation. Existing
candidate rollback and retained-session ownership remain with the host/application.
Destroying a launcher needs no cancellation or joins; prepared values and started
actors do not refer back to it.

## Checks

- `window_geometry_tests` retains portable saved-shell marker/default policy tests.
- `windows_session_launcher_tests` characterizes saved profile authority, explicit
  runtime override, unavailable managed runtime without fallback, owned provider
  selections for new actors, trusted system cmd aliases and `/d`, profile/project
  cwd precedence, smoke overrides, opaque VT defaults and frozen inherited PATH.
- That native test exercises the existing HOME helper for explicit Windows,
  POSIX, UNC, empty and absent HOME plus fallback precedence. Full managed
  preparation is tested for empty HOME (no user state tree touched), and for
  rejection of an invalid smoke state root with nonempty HOME. It requires the
  sibling `stormtty-console-setup.exe`, supplied by its CMake dependency, but
  launches no process and downloads no runtime.
- Existing `windows_shell_state_tests` and native managed-shell probes remain the
  authority for actual DACL/state/process behavior; the extraction changes none
  of those service implementations.

Cross-compiling these sources is not native execution. Before claiming native
qualification, run the new Windows test, existing shell-state tests and affected
native settings/project/session smokes on Windows. Physical terminal/desktop
behavior is a separate evidence category, not established by these unit tests.
