# Windows managed Zsh state trust boundary

The native host prepares `%LocalAppData%\StormTTY\managed-zsh-v1` using
`SHGetKnownFolderPath`, not the ambient `LOCALAPPDATA` variable. It passes the
validated path as `STORMTTY_NATIVE_ZSH_STATE` only to managed Zsh launches,
overwriting/removing any inherited value after profile environment setup. The
loader consumes and unsets this launch-only variable. It is a host-to-managed
loader contract, **not authentication for arbitrary manually launched shells**.

This state is account-local and independent of shell HOME. Explicit empty HOME
turns persistence off. Nonempty Windows/POSIX/UNC/nonexistent HOME is preserved
without probing it. No global `.zshrc`, temp, CWD, alternate-shell environment,
existing DACL, or global security setting is modified. No PowerShell/native
startup subprocess is needed. Unavailable state means ephemeral bundled modules,
not trusting MSYS2 `noacl` synthetic POSIX permissions.

New app directories receive protected, inheritable DACLs granting full access
only to the current user, SYSTEM, and Administrators. Existing objects are never
repaired. The helper accepts only fixed local drive paths, inspects every
ancestor without following reparse points, and conservatively rejects untrusted
mutation grants or owners. The dedicated tree additionally rejects untrusted
file-read/directory-list grants to keep history private. Windows Modules Installer is also a trusted principal
for system ancestors. Ancestors may allow creating unrelated siblings but not
replacement/deletion or permission changes. Directory handles deny delete-sharing
during the bounded validation. This protects against other principals, not
against the current user, administrators, or SYSTEM changing their own files.

Every existing entry in the dedicated state tree is checked for native owner and
DACL trust; files must be ordinary disk files with one link, and no entry may be
a reparse point. Unknown effective grant ACE types fail closed. The dedicated
tree is bounded to 512 entries and four nested directory levels; oversized or
unreadable state falls back rather than recursively inspecting arbitrary HOME.
No private path, config contents, or history is logged.

## Native API basis

- [CreateDirectoryW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createdirectoryw)
  accepts a security descriptor at creation; [Creating a DACL](https://learn.microsoft.com/en-us/windows/win32/secbp/creating-a-dacl)
  documents the SDDL/SECURITY_ATTRIBUTES pattern and NULL-DACL risk.
- [GetSecurityInfo](https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-getsecurityinfo)
  reads owner/DACL from the opened handle; the returned descriptor is LocalFree-owned.
- [CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
  documents backup-semantics directory opens, open-reparse-point behavior, and sharing.
- [BY_HANDLE_FILE_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/ns-fileapi-by_handle_file_information)
  supplies attributes and link count; [file rights](https://learn.microsoft.com/en-us/windows/win32/fileio/file-access-rights-constants)
  distinguish add-child from delete-child and standard permission-change rights.

`windows_shell_state_tests` is Windows-only and registered with CTest. Its actual
native execution is required; a Linux cross-compilation is not runtime evidence.

## End-to-end native probe

Run `managed_shell_state_probe.exe <absolute-runtime-root> [absolute-provider-dir]`
with the actually staged managed runtime. Omit the second argument for inbox
ConPTY; provide the verified packaged provider directory to test that provider.
The executable uses real Session/ConPTY/libghostty, an isolated Unicode fixture
under the native LocalAppData folder, and the actual native validator before
each fresh shell launch. It never synthesizes a successful trust assertion.

The probe checks genuine module functions, config load, native-marker consumption,
HOME/CWD preservation and global-rc exclusion; a failing command followed by a
downstream prompt hook; unique filename Tab completion; persisted history with
live autosuggestion and Right-key acceptance in a second shell; unchanged
completion-dump timestamp and unchanged config. Output contains only a fixed
result code, phase, count, and two bounded timing values. No frame, path, command,
config or history bytes are printed, including on failure.

`cold_ms` means **fresh managed state**, not OS-cold: Session start to observed
fixture ZLE line-init prompt, one sample. The fixture uses the shared
`add-zle-hook-widget` mechanism and waits for the next line-init sequence before
sending each command, without replacing the packaged module widgets. `warm_ms` is the same interval for the second
launch using that state, one sample. Neither measures physical input latency,
median startup, universal first-prompt behavior, or animation performance.

Failure phases: 1 fixture/setup; 2 first startup; 3 modules/paths; 4 failure and
hook chain; 5 Tab completion; 6 history seed/exit; 7 history/cache files; 8 second
startup; 9 config persistence; 10 autosuggestion acceptance; 11 cache/config
preservation and final native revalidation. Runtime execution is required;
cross-compilation alone does not qualify any of these behaviors.
