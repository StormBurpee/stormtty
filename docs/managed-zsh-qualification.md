# Managed Oh My Zsh qualification scope

The feature branch's baseline Windows workflow runs `managed_shell_state_probe`
with both inbox and explicitly packaged ConPTY. This is an end-to-end gate, not
an independent ACL unit test plus an environment-spoofed shell invocation:

- Fresh isolated Unicode/spaced directory under the native LocalAppData known
  folder, with the real `prepare_managed_shell_state_at` owner/DACL/reparse/link
  validator called for each managed launch.
- Exact staged Zsh executable and shipped loader/modules; private config and
  bounded history persist across fresh sessions, completion cache is reused,
  and the launch-only validated-state environment value is consumed.
- Real Session/ConPTY input checks command failure followed by subsequent prompt
  hooks, history-backed Right-arrow autosuggestion acceptance, and Tab completion.
- Timings report the harness's explicitly named observation boundary. A fresh
  state directory is not a filesystem cache flush. These CI runner measurements
  are not physical desktop latency or a universal performance guarantee.

The probe fails when required native persistence/module/editing evidence is
absent. It does not silently mark the gate successful by skipping persistence.
Logs contain fixed outcomes and numeric timings, not user config/history files,
private state paths or captured terminal transcripts. The disposable state is
not uploaded. Existing baseline tests and app screenshot steps remain intact;
those screenshots are separate evidence and do not themselves prove module
loading or history persistence.

`tools/shell-managed-tests.py` remains the narrower portable source test suite.
Its POSIX PTY fixture and simulated native path contract are not substitutes for
this Windows bridge gate. Windows-specific skips in that portable script must
never be reported as a Windows persistence pass.
