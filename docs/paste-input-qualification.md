# Windows paste input qualification

`paste_input_probe` is built only with `STORMTTY_TEST_SHELL_PROBE=ON` on Windows. It links the existing instrumented qualification Session/Process libraries, not the application's transport ABI.

Run with an absolute bundled runtime path and, optionally, the qualified packaged ConPTY directory:

```text
paste_input_probe.exe <bundled-zsh-runtime> [packaged-conpty-directory]
```

The probe compares a fixed, non-executable payload at two boundaries:

- Completed Windows `WriteFile` calls on the real Session input path.
- Bytes returned to an isolated child by native `ReadFile` or bundled MSYS Zsh `sysread` in raw mode.

Each provider runs native console input with VT input disabled/enabled and an MSYS raw reader. Each child runs 20 rounds with no extra activity, 35 focus reports, four resizes, or both. This recreates input-related settings activity; it does **not** drive the Settings UI or qualify ZLE's key parser.

Only recognized focus reports are removed for comparison. Bracketed-paste delimiters, payload, and final Enter must match exactly. The first mismatch prints bounded authored-input writer evidence and consumed hex. Successful rounds report numeric read gaps, including the maximum gap immediately after ESC. These include fixture-reader overhead and are not production typing latency measurements.

Every child has a fresh temporary working/home directory. No user configuration or history is sourced, and the payload is read as bytes, never executed. The CI runner owns the process tree and keeps both providers' logs. Any mismatch remains a failure; there are no retries, longer deadlines, disabled modules, or relaxed payload assertions.

Native VT-input-disabled (`kind=0`) is a diagnostic control, not the managed shell's claimed input contract. Its strict byte comparison remains visible and counted separately; a mismatch alone is not a production defect diagnosis. Cases catch exceptions independently so one failed launch/read does not suppress the rest of the matrix. After child consumption, the host observes writer callback publication for up to 250ms before judging the record incomplete; this does not retransmit input or extend any application smoke deadline.

The writer comparison classifies standalone, complete numeric cursor-position replies (`ESC [ row ; column R`) separately and logs their exact bytes. ConPTY resize can request these after an accepted paste; they are protocol traffic, not extra user input. Mixed/malformed records are not excluded. The child comparison never removes cursor replies and still requires exact paste delimiters, payload and Enter.
