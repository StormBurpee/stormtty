# Private native MSYS resize experiment (not a release)

## Scope and approval boundary

Only the WINDOW_BUFFER_SIZE_EVENT reader path is changed. Independent review
accepted this as a narrow experiment, **not** a complete runtime fix. The mouse
call to send_winch_maybe still has a related stale-snapshot risk; do not claim it
is fixed. Production runtime, lock and activation settings are untouched.

The patch retires the processed prefix under the existing input lock, then
calls the existing signal helper and returns without discarding twice. Signal
handler lock release/reacquire behavior is unchanged. If dimensions are
unchanged, returning the prior state preserves already-buffered input.

Each build requires and records at least 6 GiB free disk space. After securing the
copied DLL, original archive, recipe and full source diff, build.sh removes only
its own generated `OUTPUT/msys2-runtime` source/build tree. This prevents the
control tree from consuming space needed for the patched build. No broad runner
cleanup is performed.

No active build/runner/system DLL is ever replaced. Source/package output and
experimental private copies stay in newly created directories. **All modified
binaries/packages remain ephemeral inside the CI job. Upload logs, checksums,
recipe and patch provenance only; do not upload the candidate DLL/package.**
This is not a public runtime distribution or release/source-offer approval.

## Authoritative pinned inputs

- Source archive: https://repo.msys2.org/msys/sources/msys2-runtime-3.6.11-4.src.tar.zst
- Archive bytes: 128907415
- Archive SHA256: `4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204`
- Archived PKGBUILD SHA256: `3fc783146515f8882e2c1ec57c3d9029294141acf7a8edf68244bb386214d84a`
- Packaged runtime identity: `00eb8435b1c498e9211c7a7cee01584d76bd5f21`
- Archive base tag: `cygwin-3.6.11`, commit `867eb7748e6e5a0e8430ce9f46af27b4b1e8f204`.
- After all **43** official packaging patches, console.cc Git blob:
  `a7cdbed500c6235e21f41fe9d0be5075a8c471f1`
- Original console.cc SHA256: `535bd5f650122192e34e9835e33e72efd35aeecb093439716be6b770cd977d33`
- Patched console.cc SHA256: `cb902a91a09038e8fcd60bfb2b13ef13819ce6a9b3ae42d95980d7c4b5bc2cce`

The subtree .gitattributes forces LF on Windows so patch/source hashes remain stable.

The verified archive contains the Git source, recipe and packaging patches;
`makepkg --holdver` prevents updating it from the network. Original licenses,
source archive, official recipe, compiler inventory and full applied source diff
are retained beside each build. The experimental patch modifies GPL-covered
upstream source; preserve the upstream licensing and provenance.

## Build dependencies and reproducibility limit

Use a dedicated native **MSYS x86_64**, not UCRT64/MINGW64, build environment.
Install standard official MSYS2 `base-devel`, the official `zstd` CLI (used by tar), `texinfo` (makeinfo), plus the recipe's makedepends:

```text
autotools cocom docbook-xsl gcc gettext-devel git libzstd-devel
libiconv-devel mingw-w64-cross-crt mingw-w64-cross-gcc
mingw-w64-cross-zlib perl xmlto zlib-devel
```

The original binary's .BUILDINFO records GCC 15.3.0-1, binutils 2.47-1,
make 4.4.1-3, makepkg 6.1.0, cross-GCC 16.2.0-1, cocom 0.996-6,
autotools 2026.08.04-1, docbook-xsl 1.79.2-3, gettext-devel 0.22.5-1,
libiconv-devel 1.19-1, libzstd-devel 1.5.7-1, perl 5.42.3-3,
xmlto 0.0.29-2 and zlib-devel 1.3.2-1. Full transitive versions are in the
original package's .BUILDINFO. Do not pretend a latest-package install reproduces
that toolchain or the upstream DLL bit-for-bit.

For this controlled experiment, build **control and patched** with one identical
installed toolchain and no updates between them. Both inventories must match
exactly. Store package versions, `/etc/makepkg.conf`, compiler versions, full
source diff and DLL hashes. If exact replay is needed later, archive/signature-
verify those official dependency packages in an isolated toolchain cache before
its snapshot expires. Setup-action SHA and toolchain snapshot are explicit
workflow inputs, not silently selected by these scripts.

## Native CI sequence

1. Build the fixed StormTTY probe once, with the independent
   qualification-only bounded input recorder. Build the normal application for
   the link/artifact isolation check, and run qualification_input_evidence_tests. Stage one untouched official
   private Zsh bundle using the existing locked bootstrap. Download the pinned
   runtime source archive and verify it (build.sh independently verifies again).
2. In the dedicated MSYS build shell, with dependencies already installed:

```sh
bash tools/probes/msys-runtime-experiment/build.sh \
  /absolute/msys2-runtime-3.6.11-4.src.tar.zst /absolute/new/control-build control
bash tools/probes/msys-runtime-experiment/build.sh \
  /absolute/msys2-runtime-3.6.11-4.src.tar.zst /absolute/new/patched-build patched
cmp /absolute/new/control-build/provenance/pacman-before.txt \
    /absolute/new/patched-build/provenance/pacman-before.txt
```

Each uses the official prepare/build/package functions. It does not install the
result. `makepkg` failures remain failures; no skipped checksum or test stages.
Use separate logs and a workflow timeout (suggest 90 minutes for the build job;
actual native build duration has not yet been measured). No Linux cross-build
is represented as native validation.

3. From Windows Python, stage only new experimental private copies:

```text
python tools/probes/msys-runtime-experiment/stage.py BASE_BUNDLE CONTROL_BUILD NEW_CONTROL_BUNDLE
python tools/probes/msys-runtime-experiment/stage.py BASE_BUNDLE PATCHED_BUILD NEW_PATCHED_BUNDLE
```

The stager verifies base hashes, source/patch provenance, DLL hash and stable
build inventory; marks copies qualification-only/unverified; removes inherited
qualification claims. Old notices are retained, not claimed sufficient for a
new release. The normal application activation gate is intentionally not relaxed.

4. Run the identical immutable probe/fixture against official, rebuilt-control,
   and patched bundles, both providers, three prespecified rounds:

```text
python tools/probes/msys-runtime-experiment/compare.py --probe ABS_PROBE_EXE --fixture ABS_FIXTURE_ZSH --original BASE_BUNDLE --control NEW_CONTROL_BUNDLE --patched NEW_PATCHED_BUNDLE --provider ABS_QUALIFIED_CONPTY_DIRECTORY --output NEW_EVIDENCE_DIRECTORY --rounds 3
```

Every case requests **50** alternating phases. The existing strict first failure
stops that case; the next independent case still runs. There are no corrective
signals, delays, suppressed failures or retries conditional on failure. The overall gate also rejects missing input-evidence markers without rewriting
the process exit status. Each case
has a 180-second owned-process limit plus bounded reap, 18 cases total (54 minutes
plus reap worst case). The runner does not claim descendant cleanup; normal
Process ownership closes its own process group/job. Retain results.json, all
case logs and any input trace. A timeout or failed control stays visible and
makes compare.py nonzero. No missing case is counted as a pass.

5. Upload **only** comparison/build logs, hashes, provenance text/recipe/patch,
   and source identity/URL. Do not glob the build directories into an artifact:
   they contain experimental DLLs/packages. Do not publish release artifacts.

## Acceptance and unresolved risks

First comparison tests this specific race hypothesis. Input-pipe evidence must
establish the complete command was actually written, not merely accepted by
Session. Distinguish terminal reply/CPR interleaving from MSYS consumption and
record any trace truncation. Packaged passes do not prove that provider immune.

Before any production proposal, expand native coverage: preceding buffered keys,
unchanged-size and consecutive resize events, select(len=0), VMIN>1, SIGWINCH
handlers that read or longjmp, mouse+resize, controls/NOFLSH, Unicode/repeat input,
job control and existing managed-shell/native suites. The resize-only patch does
not resolve every input record ownership path or stale mouse snapshot.

Local validation: full archive/recipe identity and official43-patch reconstruction,
candidate apply check, three race-model tests, six staging safety tests, Bash
syntax and Python compilation. **No experimental DLL has been built or run here**;
native build and runtime results must be recorded by the dedicated CI job.


## Compiler dialect correction after first native source build

Run40705457308 stopped before comparison: the new toolchain supplied GCC16.2.0;
its default C++20 mode rejected existing dcrt0.cc volatile increment/decrement
expressions under the upstream `-Werror`. The original .BUILDINFO used GCC15.3.0.
[GCC16 release notes](https://gcc.gnu.org/gcc-16/changes.html) explicitly document
the default transition from GNU++17 to GNU++20.

Both experimental variants now use the same explicit `CXX='g++ -std=gnu++17'`
through the generated makepkg configuration. The official recipe overwrites
CXXFLAGS, so setting only those flags would not reliably select the dialect.
No source/recipe checksum or warning policy is relaxed. Preflight checks require
`__cplusplus=201703L` and compile volatile ++/-- under `-Wall -Wextra -Werror`;
compiler-mode.txt is hash-bound into staging/comparison provenance, and generated
Makefile CXX settings are retained. The existing full compiler inventory remains
required. This is still GCC16 with explicit C++17, not historical GCC15 replay.

The first build also reported missing makeinfo. The workflow explicitly installs
the official texinfo package; build preflight records its path/version rather
than waiting for a later documentation failure. Native success still must be
established by the next controlled run.
