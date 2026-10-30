# Liquid Glass pause verification (2026-10-30)

The default-OFF `STORMTTY_EXPERIMENTAL_LIQUID_GLASS` build option hides the
Settings controls, explanatory text and extra scroll space. Host availability
reports cannot expose it. The renderer reports unavailable and the session is an
inert stub; native MSVC application builds omit capture/material sources and the
shader compiler dependency. Ordinary host-backdrop dependencies remain enabled.
The optional standalone capture diagnostic remains independently opt-in.

Source, explicit consent, self-exclusion and GPU-only/readback safeguards are
retained for an intentionally enabled developer build. No drag or HDR fixes were
implemented: the user paused those experiments.

## Evidence

- Fresh default-OFF Linux CMake build: `settings_panel_tests`, 50,799 checks passed.
- Reconfigured ON Linux CMake build: same test, 52,161 checks passed, including
  existing session-action, stale intent, consent-decline and layout coverage.
- Disabled tests cover hidden controls/text even after a host reports active and
  available; no actions/config changes; ordinary blur control remains enabled.
- Windows GNU cross-compilation passed with C++20, `-Wall -Wextra -Werror`
  for the disabled Session stub, renderer and renderer-smoke test objects. This
  is compile-only evidence, not native MSVC or Windows runtime verification.
- Native renderer smoke includes disabled-build availability, rejected Start and
  inert polling assertions alongside the existing normal-render/readback checks.
  These assertions require Windows runtime CI; they were not executed on Linux.

No desktop capture was run. Linux tests do not verify MSVC/WGC linking, Windows
runtime, physical drag latency, optical quality or HDR. Native CI and any later
explicitly authorized physical testing remain separate evidence.
