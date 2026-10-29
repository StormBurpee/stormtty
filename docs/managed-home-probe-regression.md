# Managed HOME qualification protocol regression

Base: local `914b59f86825e1b8c122d1a22951ec80e21c54a6` (published main
`3577` prefix). Failed native runs `40910203129` and `40914085681` are retained;
Windows job `123589546561` reports the first absent-HOME case failing before
shell invocation with empty captured stdout/stderr. The old driver did not
record the helper's exit code, so the historical Windows exception code is
unknown. No unchanged rerun or MSYS-flakiness explanation is justified.

The actual invocation is `bundled_shell_probe --managed-home-environment`,
not `stormtty-console-setup`. Its early argument branch serializes the shared
wide-API `managed_shell_home_environment()` overrides. Foundation commit
`c5975f6` added two `std::nullopt` removal entries for inherited initialization
controls, including with completion seed OFF. The probe still unconditionally
dereferenced each optional. This is a concrete C++ undefined-behavior bug in
the qualification serializer, not a change to HOME resolution or a missing
console setup dependency. A synthetic reproduction of that dereference aborts
under checked libstdc++ (exit 134); this demonstrates the source defect but is
not a claim about the missing historical Windows exit code.

The qualification-only serializer now emits `NAME=-` for deletion and
`NAME=` plus UTF-8 hex for set operations, including the distinct empty value.
The Python consumer requires both reserved controls to be deleted, rejects
unexpected/duplicate overrides, and applies deletion rather than dereferencing
or pretending it is an empty value. Every HOME fallback/explicit/empty,
Unicode CWD, drive/temp mapping, and dotfile-isolation contract is unchanged.
The fixture additionally inherits hostile values for both reserved controls
in mixed casing and verifies their case-insensitive removal. Set overrides also
replace all differently-cased aliases, matching Windows semantics. Failure diagnostics contain numeric exit (signed
and unsigned32) and output byte counts, without private environment values.

Local checks:
- C++ protocol test passes with checked STL: nullopt deletion, empty and Unicode.
- Four Python protocol tests pass (including failure diagnostics and required
  deletion enforcement).
- Actual bundled-shell probe and C++ protocol regression strict Windows GNU
  cross-compilation pass. This is compile evidence, not Windows execution.
- `git diff --check` passes.

Required native gate before claiming repaired: the new `environment_protocol_tests`
CTest plus the existing complete `Verify managed shell HOME and drive paths`
step on the exact corrected SHA. Both native workflows run parser tests then
all real resolver/MSYS path cases without retries or bypasses. Root owns
publication and native execution; native result remains pending at this commit.
No application, console setup, runtime dependency, ACL, trust, journal, HOME
resolver, default provider, completion default or release/license gate changed.
