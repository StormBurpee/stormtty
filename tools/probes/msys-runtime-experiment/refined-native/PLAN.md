> Historical diagnostic design from e25923c; the native scenario/coverage limits
> below remain binding. The new fixed push-only orchestration and managed Session
> matrix are specified in ../REFINED-QUALIFICATION.md. Root alone publishes it.
> The original no-workflow snapshot and previous experiment remain preserved.

# Refined prefix: bounded, unqualified native diagnostic plan

New worktree based on `861e3fd`. Nothing changes run 40817519570, its branch,
source, binaries, matrix or results. No workflow is enabled here. Root alone
may publish a new explicitly diagnostic branch/workflow after review.

## Evidence carried forward, not reclassified

Prior target raw MSYS resize: stock ESC loss 6/6 trials, conditional 0/6, matched
actual resize counts. Settings-close: both 6/6 passed, inconclusive. VT-off controls
failed universally and remain failures. This refined candidate is different from
that candidate. Its native behavior has NOT been established by the prior run.

Candidate patch `0006-refined-prefix.patch` SHA256:
`ceb70e9c04be478e051f2be891409e4d7e4ad30c8654141c86f520f165b70693`.
Only stock and refined participate. No conditional/cursor/trace variant or runtime
pin/default change. Exact source hashes are enforced in build-refined.sh,
stage-refined.py and refined-identities.py. Official source, packaging recipe,
common utility compatibility patch and private GCC15 recipe remain identical.

## New diagnostic scenario matrix

Fresh owned Win32 console and reader process for each launch. Six scenarios,
two predetermined paired orders (stock/refined then refined/stock): 24 launches.
All independent launches continue after failures; no retry, threshold relaxation,
or replacement run. Reader alarm 8 seconds, injector child wait 10 seconds,
outer launcher watchdog 20 seconds. Owned job uses kill-on-close; only private
fixture child is terminated. Preserve partial logs and every failed result.

- raw-vmin2 (also mouse reporting OFF): VMIN=2, len=64, initial a; observe native
  queue emptiness if possible, shrink console window width by one, inject mouse+b.
- select: same inputs, select before read; log every select/read errno and result.
  Signal handler deliberately has no SA_RESTART. Retried EINTR is recorded.
- canonical: initial a; changed dimensions, mouse+b+newline. Expect ab+newline.
- mouse-on: enable 1000/1006 modes via MSYS output. Wheel at visible cell (2,2),
  expect exactly a + ESC[<64;2;2M + b; preserve all report bytes and read boundaries.
- surrogate: UTF-8 locale, a then high-surrogate/mouse/low-surrogate/b batch;
  expect a + U+1F600 + b. This exercises stock's event-skipping surrogate lookup,
  not a redesign of that policy.
- longjmp: canonical partial line, SIGWINCH siglongjmp during a guarded
  read/select-attempt window. Expect ab+newline; log whether a jump happened.
  The guard includes instructions before the call and after it returns. A jump
  in that post-return window can skip successful bytes before the fixture logs
  them; byte loss in this case is not necessarily runtime loss. No unsafe logging
  occurs in the handler; no jump across stdio calls is requested. Mutable termios
  state has static storage so it remains defined after a later siglongjmp.

The reader drains tail bytes with VMIN=1 after its first successful read; it logs
all successful bytes, errors and final concatenation. An extra-read check catches
immediate duplicate data. Injector logs setup, queue observation, actual geometry,
exact requested/written input-record counts, timeout/exit and residual native queue.
Residual records are a strict scenario failure, possibly environmental focus/input
noise rather than proof of byte loss; retain evidence and classify honestly.

### Synchronization limits are a hard blocker to path qualification

The marker proves termios and handler setup, not read entry. Empty native queue
proves neither copied_chars=1 nor even that this reader consumed a: the master can
read/rewrite it. Geometry may be observed by the master before MOUSE_EVENT, and
Windows may synthesize additional resize events. A SIGWINCH count does not identify
its dispatch site. Native queue emptiness is transient, not an input_mutex boundary.
Window shrink and atomic mouse/key WriteConsoleInputW batch improve reproducibility
but do not prove the intended peek or exact interleaving.

Therefore run.py ALWAYS records exact_path_verdict=inconclusive. A byte-conserving
scenario pass is NOT qualification of the refined retirement branch, partial-copy
fix, select path, surrogate branch, longjmp commit point, or mouse_aware path.
Longjmp without observed jump is not coverage; even observed jump has unknown
site. True internal-path qualification would require a separately reviewed paired
diagnostic hook/trace proving copied_chars, event index/type, successful retirement,
and unlock timing, or equivalent debugger evidence. No such hook is added here.
Do not promote the candidate based solely on this harness.

## Precise next CI recipe (new workflow only, not enabled in this worktree)

Use windows-2025 and the exact pinned checkout/setup-python/setup-msys2/artifact
versions from msys-conditional-retirement.yml. Checkout exact diagnostic commit.
Use a NEW RUNNER_TEMP/stormtty-runtime-refined root, no reuse of previous output.
Keep the same strict source archive download/size/SHA256 check, package inventory,
signature verification and private GCC15 toolchain preflight. Keep makepkg checks
and GNU++17 flags; do not downgrade/install the private compiler system-wide.

1. Run portable tests: refined-tests.py, stage-refined-tests.py,
   refined-identity-tests.py, refined-native/tests.py. Build the same immutable
   raw probe (SHA256 251477c3587c25b498bf1bf099e355d47a2cf3e1b8f1dcd7649e406aca39beae)
   and qualification isolation tests using unchanged existing recipe.
2. Stage untouched original with shell-bootstrap.py; prepare same inbox/packaged
   ConPTY providers. Verify official source archive and toolchain as previously.
3. MSYS shell, after exporting EXPERIMENT_TOOLCHAIN="$root/gcc15":

```sh
bash tools/probes/msys-runtime-experiment/build-refined.sh "$root/msys2-runtime-3.6.11-4.src.tar.zst" "$root/stock-build" stock
bash tools/probes/msys-runtime-experiment/build-refined.sh "$root/msys2-runtime-3.6.11-4.src.tar.zst" "$root/refined-build" refined
cmp "$root/stock-build/provenance/pacman-before.txt" "$root/refined-build/provenance/pacman-before.txt"
export PATH="$root/gcc15/wrappers:$PATH"
mkdir "$root/native-fixture-build"
stormtty-gcc-15 -std=gnu11 -O0 -g -Wall -Wextra -Werror -static-libgcc tools/probes/msys-runtime-experiment/refined-native/reader.c -o "$root/native-fixture-build/reader.exe"
objdump -p "$root/native-fixture-build/reader.exe" > "$root/evidence/reader-pe-imports.txt"
```

   Inspect/enforce imports before execution: MSYS DLL and Windows system DLLs
   only; do not silently add unpinned gcc/third-party runtime dependencies. Reader
   must be an MSYS-linked executable, not MinGW. Archive compiler/version/import
   evidence. The reader is compiled once and identical bytes used for both DLLs.
4. Initialize the same pinned native compiler environment using ci-msvc-env.ps1.
   Build injector in NEW private fixture-build directory, not the shipped app:

```powershell
cl /nologo /std:c11 /W4 /WX /D_CRT_SECURE_NO_WARNINGS tools/probes/msys-runtime-experiment/refined-native/injector.c /Fo:"$env:EXPERIMENT_ROOT/native-fixture-build/injector.obj" /Fe:"$env:EXPERIMENT_ROOT/native-fixture-build/injector.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
python tools/probes/msys-runtime-experiment/stage-refined.py "$env:EXPERIMENT_ROOT/original" "$env:EXPERIMENT_ROOT/stock-build" "$env:EXPERIMENT_ROOT/stock"
python tools/probes/msys-runtime-experiment/stage-refined.py "$env:EXPERIMENT_ROOT/original" "$env:EXPERIMENT_ROOT/refined-build" "$env:EXPERIMENT_ROOT/refined"
```

   Each external command must gate on LASTEXITCODE. run.py validates paired
   manifest/content/source/toolchain identities before making fixture copies.
   Copies contain just reader.exe and exact variant DLL beside it, with SHA256
   equality checked. No active DLL is replaced. Bundle manifests remain untouched.
5. Execute once, keep nonzero status without skipping independent later matrices:

```powershell
python tools/probes/msys-runtime-experiment/refined-native/run.py --injector "$env:EXPERIMENT_ROOT/native-fixture-build/injector.exe" --reader "$env:EXPERIMENT_ROOT/native-fixture-build/reader.exe" --original "$env:EXPERIMENT_ROOT/original" --stock "$env:EXPERIMENT_ROOT/stock" --refined "$env:EXPERIMENT_ROOT/refined" --output "$env:EXPERIMENT_ROOT/evidence/native"
```

6. Run compare-refined.py with unchanged probe/app, --original/--stock/--refined,
   --provider and fresh evidence/raw, --workload raw. This is unchanged 3 paired
   trials per provider, not reruns until green. Run the existing fixed settings
   matrix separately with evidence/settings and --workload settings if root keeps
   that scope. Native failures must not suppress raw/settings and vice versa.
7. Always upload source/provenance, fixture source hashes/compiler/import reports,
   native results.json, */reader.jsonl, */injector.jsonl, */launcher.log, and existing
   raw/settings logs/results. Upload NO DLL/executable/object/package, private
   captures/HOME/history/keyring. Native private-fixtures must not be globbed in.

## Stopping and interpretation

Stop after these fixed cases; no retry. Root reviews failures and exact-path
inconclusiveness. Short raw reads are permitted; concatenated bytes and returns
are retained. Progress depends on eventually stable dimensions; this finite
one-resize harness does not qualify continuous-geometry liveness. Failed/partial
native discard and competing uncooperative native consumers remain untested
inherited risks, not fixed by this source patch. The original e25923c delivery had portable checks only. Updated local compile
evidence is recorded in ../REFINED-QUALIFICATION.md; native execution is pending.

## Historical local verification delivered with e25923c

- Refined raw/settings runner contracts: 8 passing tests.
- Refined private staging contracts: 20 passing tests.
- Paired source/toolchain identity contracts: 2 passing tests.
- Native-log assessment contracts: 9 passing tests (synthetic logs only).
- reader.c: Linux GCC C11 -Wall -Wextra -Werror syntax check only.
- build-refined.sh: bash -n passes.
- injector.c: Windows compiler unavailable here; NOT compiled or executed.
- No runtime or fixture binary produced; no native test run; no workflow/remote
  change. Existing conditional experiment worktree remains clean.
