# Workspace persistence extraction characterization

Baseline: `f959022afbe1e90b0c11b4f2764854d577e5b92f`, `window.cpp`.
These are observed host contracts, not new grouped-persistence behavior.

- `load_workspaces` already loads the application-layout envelope and exact token
  from one guarded read. Legacy v1 migration/backup remains in native services.
- Load selects the workspace path after window construction, before HWND creation.
  Workspace-smoke options were selected by `windows_main`; Settings smoke changes
  its own config path later in `create`, with workspace persistence disabled.
- `workspace_model_changed` marks only persisted mutations. The first edit starts
  a 1500 ms timer; subsequent edits do not reset it or capture the model.
- `save_workspaces` rejects unavailable state, then defers busy requests before
  checking window count or capturing. Deferred work captures the latest model.
- Checkpoint comparison excludes presets. Equal whole-document bytes do not
  install the prepared document. A missing file is not an equal-byte no-op.
- Completion advances only the exact token; it never installs its saved document
  over later presets or close history. Every save failure clears the token.
- Final flush records the individual close and clean marker before joining the
  previous worker, drains its pending retry, explicitly saves, then joins/drains.
- Existing edge: final capture/record failure clears the token, but an older
  successful completion reinstates it. This extraction does not silently fix it.
- Native thread wake is only a hint; mailbox polling after dispatch is authoritative.
  The host reports errors to registered surviving views. All joins are outside
  WndProc. A worker retains copied input/services/mailbox, never a window/model.
- Single-persistent-window creation/save/retirement gates remain. Unsupported
  selected recovery groups and missing crash checkpoints disable writes rather
  than flattening, dropping or overwriting layouts. No second-process ownership.

## Verification scope

Portable deterministic owner tests qualify sequencing and copied inputs. They do
not qualify Win32 timers, filesystem security or native UI. Existing Windows-only
`application_layout_services_tests` and `workspace-native-smoke.py` remain the
native security/migration/restart qualification. No guarded service/codec change.

### Explicit fail-closed prerequisite

Prerequisite commit `e1516c0` acknowledges file completion before fallible UI
reporting: failed saves invalidate the writable token and consume the pending
retry flag first. Failed `record_closed` validation also invalidates first. This
is deliberate hardening, not hidden as a behavior-preserving extraction. Host
capture failure and unsupported-restore ordering remain unchanged, including the
older-success token-reinstatement edge. Tests throw during caller-side error
presentation after completion and verify that neither failure kind permits writes.

### Local extraction checks (2026-10-30)

- Fresh-source Linux `g++ -std=c++20 -O0 -g0 -pthread -Wall -Wextra
  -Wpedantic`: owner **1255 checks passed**, no compiler diagnostics.
- Fresh-source portable regressions: application-layout codec **183**, workspace
  presets/signal **101469**, application workspaces **1811** checks passed.
- Direct Windows GNU `clang -cc1 -fsyntax-only`: owner implementation, native
  service adapter, owner tests and complete `window.cpp` passed, no diagnostics.
- Thread-start failure is source-reviewed, not fault-injected: candidate document
  adoption and owned copies precede `busy`; thread construction/capture failure
  resets `busy` and propagates, leaving document timing as before.
- Successful token adoption is a move before constructing the public outcome;
  the completion contains no token/document copy. A failed token is invalidated
  before returning its error, so presentation allocation cannot leave it writable.
- Production service exception behavior is unchanged: guarded services return
  failure outcomes. The service seam does not add a generic exception/retry policy.
- Native timer, modal-loop wake consumption, HWND survivor presentation, guarded
  filesystem runtime and separate-executable restart remain native qualification,
  not inferred from portable tests or cross-platform syntax checks.
