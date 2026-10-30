# Measuring managed shell startup

This opt-in Windows probe measures the actual bundled Zsh, native private-state
validator and Session/ConPTY path. It does not change the application or its shell
loader. It is excluded from normal builds and is not a CTest or timing-threshold gate.
The native workflow explicitly builds it after successful runtime staging and
collects both provider measurements as described below.

## Run

Build the explicit `managed_shell_startup_probe` target in an existing native
Windows configuration, then run it with the prepared runtime directory:

```powershell
cmake --build build --target managed_shell_startup_probe
.\build\managed_shell_startup_probe.exe .\build\runtimes\zsh
```

An optional second argument selects the already-qualified packaged ConPTY
provider directory. Omit it for the system provider. No downloads are performed
by the measurement executable.

The runtime must have been prepared from the exact source revision being
measured. Record that commit, bundle lock and machine/Windows version separately.
Use the same provider, build configuration and machine for baseline/candidate
comparisons. Native GNU cross-compilation is not runtime measurement.

## What the samples mean

The probe creates synthetic Unicode-named fixtures beneath the native local app
data folder and removes only its own fixtures when done. The first three launches
have separate fresh private state. The remaining seven reuse the third state and
must preserve the completion-cache timestamp. This does **not** flush filesystem,
OS, antivirus or executable caches.

Each sample records only:

- Native state preparation/validation time, including creation for fresh state.
- Session launch to observable first ZLE entry, including PTY, parsing and frame
  publication. A fixed ZLE status message is the readiness signal. Its rendering
  and a 5ms polling interval are included; this is not physical display latency.

Fixture creation, config writing, cleanup and module verification are outside
those two timed stages. The real managed prompt remains enabled. The synthetic
working directory is not a Git repository. First ZLE must be followed by proof of
real OMZ, autosuggestion, syntax-highlighting and fzf/zoxide functions, native
Unicode input setup, and persistent state;
a fast fallback shell does not produce a successful sample.

The probe prints fixed numeric sample/result records only. It never prints user
paths, environment, history, configuration or terminal contents. No user dotfile
is sourced or modified. Native state validation is not bypassed. The existing
managed-shell qualification probe remains the correctness gate for completion,
Right-arrow acceptance, lifecycle and persistence.

## Interpret before optimizing

Compare the three fresh samples and seven warm samples separately. Do not combine
them into one average or infer a percentile from this small sample. The observed
PR qualification runs varied from about 1.9–4.1s fresh and 0.27–0.47s warm on hosted
Windows; those individual readings are not a benchmark.

The two stages distinguish native validation from shell readiness but do not
attribute time inside OMZ. If shell readiness dominates, a separate synthetic
`zprof` investigation can attribute completion initialization and module costs;
its instrumented timings must not be compared directly with this unprofiled
baseline. Do not defer first-use completion, skip security validation or change
terminal-engine behavior merely to improve a startup number.

The probe uses the current native-console sidecar and verifies the shipped
fzf/zoxide functions as well as OMZ/autosuggestions/highlighting. Ambient module
opt-outs are cleared in its synthetic environment. It does not change user
configuration or the runtime pins. Run the existing native correctness gates
(first Tab, Right-arrow suggestions, navigation, history and ACL rejection)
alongside measurements; function presence alone does not qualify those behaviors.

## Offline source attribution (POSIX only)

```sh
python3 tools/probes/managed-shell-source-profile.py \
  --assets-cache /path/to/pinned-source-archives \
  --tools-runtime /path/to/prepared-runtime --samples 8
# Add --zprof for function attribution; do not compare instrumented runs to
# uninstrumented startup measurements.
```

This creates and deletes its own private fixture, stages the exact current addon
patches offline, and copies the real fzf/zoxide source files from the supplied
runtime. Source revisions in that runtime must match the checked-in tool lock.
It never executes the Windows tool binaries on POSIX. Module presence and `ls`
completion registration are mandatory for every sample. The fixture uses the
current loader and adapters; only its disposable copy gains stage timers. Stages
are consecutive elapsed intervals: private-state includes initial loader setup,
OMZ includes eager completion, tools/config includes post-OMZ defaults, and
context/highlighting includes function definitions. Process-to-exit includes shell
launch/teardown, not first prompt, ZLE, or physical rendering. Fresh means new
application state, **not** cold OS/filesystem caches. No stage timer is installed
in the production loader.

### Local diagnostic, 2026-10-29

Linux Zsh 5.9 with pinned source modules: a baseline fresh `zprof` sample spent
312 ms in compinit (145 ms compdef, 99 ms compdump), versus about 6 ms warm.
Warm source-stage runs put fpath discovery around 3 ms and tools/config around
4 ms; neither justifies hiding modules or delaying first Tab. This motivates
keeping secure compinit unchanged rather than trading correctness for a number.

The bounded optimization removes a redundant mkdir process for an already
validated Windows cache directory and two cold-state utility invocations
(absent-dump rm and metadata tee). Host owner/DACL/tree validation, compaudit,
cache invalidation, compile locking, eager modules, and user overrides remain.
Process-spy regression tests demonstrate the removed commands; this is not a
claim about Windows milliseconds. A syscall-process census was unavailable in
the local sandbox (ptrace denied).

One exploratory eight-sample source run measured baseline fresh 425 ms, warm
median 35.4 ms, versus candidate fresh 385 ms, warm median 31.7 ms. These sequential
small samples are noisy, especially since the Windows mkdir optimization is not
exercised on Linux. They are attribution evidence, **not a performance win**.
Native Windows before/after measurements and behavior qualification remain
required; cross-compiling the probe establishes only build compatibility.

## Native CI evidence

After `staged_shell` succeeds, `native.yml` explicitly builds the optional probe
(with its native console-setup dependency), then invokes it as:

- `managed_shell_startup_probe.exe <freshly-staged-runtime>` (inbox)
- `managed_shell_startup_probe.exe <freshly-staged-runtime> <qualified-provider>`

The existing owned-process runner retains and reaps the exact probe process;
pipe EOF does not control completion. Each provider has a 240-second hard
liveness timeout plus at most 10 seconds to reap. Build timeout is 180 seconds;
the build/measurement steps also have four-/ten-minute workflow limits. The
runner does not enumerate or kill unrelated processes or claim descendant-tree
cleanup. Both providers are attempted even when one returns failure.

The immediately uploaded `shell-startup-Windows` artifact contains only
allowlisted numeric sample/result lines and numeric runner status. Arbitrary
stderr/exception text and temporary capture files are not published. Success
requires all three fresh and seven warm samples, the probe's module/cache
validation, and a clean process exit. Failure is an evidence-integrity/liveness
failure, **not** a performance regression threshold. Existing interactive/ACL/
navigation correctness gates remain separate and unchanged. A single candidate
CI run is not a before/after comparison or evidence of improvement; compare
exact baseline/candidate commits with the same runtime pins, provider and machine.

## Native source attribution (separate instrumented samples)

PR8 native Windows evidence recorded **fresh-state first-ZLE 4.25–5.08 s**, warm
0.52–0.75 s, and native state preparation around 1–3 ms on the hosted runner.
That excludes the native state validator as the dominant measured stage. It
**does not** establish which shell stage is slow. The earlier POSIX compinit
profile suggests a hypothesis, not a Windows performance finding.

The optional `--source-profile` probe mode accepts only an authored fixture
runtime with `stormtty-source-profile.json`, and runs three fresh-state samples
plus three warm samples. Normal mode rejects that marker, keeping instrumented
samples separate from the original three-fresh/seven-warm benchmark.

```powershell
python tools/probes/run-managed-source-profile.py `
  --runtime .deps/stormtty-shell --probe build/managed_shell_startup_probe.exe `
  --provider "$env:RUNNER_TEMP/stormtty-conpty-qualified" `
  --scratch "$env:RUNNER_TEMP" --output build/artifacts/source-profile
```

The driver verifies the source manifest, rejects links/uninventoried code,
checks the exact addon/tool locks and current loader, then copies the bundle
into a new disposable directory. It verifies every copied inventoried file.
Only that copy's `.zshrc` gains timestamps and `zprof`; no executable or module
bytes, production loader, runtime pins, user configuration or default behavior
are changed. The fixture manifest records the loader change and forbids default
activation/release. The original and instrumented loader hashes plus original
manifest hash are saved, never filesystem paths.

Nine consecutive source intervals use Zsh's builtin `EPOCHREALTIME`:

- bootstrap (including native console setup and state-function definitions);
- private-state setup; fpath discovery; eager OMZ/completion;
- defaults/private authored config; real fzf/zoxide integration;
- autosuggestion source; context/hook definitions; syntax-highlighting source.

Module loading for the profiler happens before the first timestamp. Writing
private diagnostic files and producing zprof occur **after** all timed source
intervals. First precmd/ZLE hooks and shell/DLL startup before `.zshrc` are outside
those intervals. They remain inside launch-to-ZLE, together with instrumentation
cost, so that difference is **not** a clean measurement of any one stage.
`EPOCHREALTIME` is wall clock and profiler overhead is nonzero. Do not compare
instrumented launch-to-ZLE directly to uninstrumented performance samples.

Zprof function totals/self time distinguish compinit, compaudit, compdef,
compdump, zrecompile, OMZ metadata checks/libraries, private-state setup and
highlighting loader work. Only fixed function names and numeric fields from the
first summary table are published. Raw call graphs (which can contain synthetic
fixture paths) stay in private fixture files and are deleted; they are never
printed or uploaded. First-Tab completion is not deferred, compaudit is not
skipped, and all modules remain eager. The probe checks completion registration
and module/state validity; existing interactive first-Tab/suggestion gates still
provide the behavior proof.

CI preserves the existing baseline, then runs this diagnostic separately on both
providers. Each owned process has 240 seconds plus a 10-second reap bound, the
driver has a 660-second outer bound, and the workflow step has 12 minutes. No
latency threshold or speedup claim is introduced. Missing stages, missing
compinit/compaudit attribution, truncated output, module/cache failure or cleanup
failure makes evidence invalid. Only sanitized logs and source identity hashes
are uploaded; no raw profile, runtime copy, history, config or frame is uploaded.

Interpretation is prespecified: if Windows OMZ is dominant, compare compinit
inclusive time against its compdef/compdump/audit components; if bootstrap or
fpath dominates, investigate native helper/filesystem work instead. If source
intervals are small relative to first-ZLE, attribute shell launch and first-prompt
hooks next. Do not optimize based on the POSIX numbers or disable audit/modules
to manufacture improvement. Native profiled results remain pending until the
reviewed workflow runs; local fixture/parser tests and cross-compilation are not
native timing evidence.

## Diagnosing repeated slow launches in the actual app

From PowerShell, run the same installed/built executable you normally use:

```powershell
& .\StormTTY.exe --diagnose-shell-startup
```

This does not select a different shell, provider, configuration, runtime or seed
policy. If your saved profile is not managed Zsh, select **Zsh · managed** in the
app. Keep this instance open and open a second managed tab: compare the two
`STORMTTY_STARTUP schema=1` lines in the terminals. No external Codex task,
user-dotfile change, diagnostic file or environment-variable setup is needed.
Only share those fixed diagnostic lines, not surrounding terminal contents.

Each managed launch reports:

- `native_state=ready|disabled|unavailable`: native private-state validation result.
  `disabled` means explicitly empty HOME; `unavailable` means persistence could
  not be safely prepared. Normal launches also show an app notice for unavailable
  persistence. Nothing repairs permissions, deletes state or weakens validation.
- `state_prepare_ms`: native preparation elapsed milliseconds (GetTickCount64
  resolution); excludes shell launch. `persistent` says whether the loader actually
  selected persistent state, not whether every subsequent file write succeeds.
- `dump_before` / `dump_after`: readable regular completion dump present before
  OMZ / after OMZ, before user configuration. Presence **does not prove a valid cache or cache hit**.
  OMZ can rebuild a present dump when revision, absolute runtime/fpath metadata,
  or completion inventory changes. Switching output/runtime directories can
  therefore produce another cold launch without deleting private state.
- `shell_source_ms`: owned `.zshrc` entry (after loading the builtin clock module)
  to its final diagnostic setup; `omz_ms`: inclusive OMZ source interval;
  `source_to_line_init_ms`: same source start to this one-shot ZLE line-init hook.
  These use a wall clock, include diagnostic overhead, and can be affected by
  clock adjustments. They are not comparable to uninstrumented benchmark samples.

The last field is **not** process-spawn time, first transport byte, full app
startup, physical display latency, or proof that all possible hooks finished and
input is usable. User hooks can replace/cancel line initialization; no line means
no observation, not zero latency. The normal prompt, modules and user hooks remain
active. The diagnostic uses no subprocess or executable cache and prints only a
fixed schema, bounded state enumeration, booleans and numeric times.

If every launch has `persistent=0`, investigate unavailable/disabled state before
optimizing completion. If the first launch has `dump_before=0` and the next has
`dump_before=1` with much lower OMZ time, the difference is consistent with cold
completion initialization. If both have a dump but OMZ is still slow, presence
alone cannot distinguish invalidation from user/module work. If owned-source
intervals are short while the observed app delay is long, this diagnostic has
not measured that missing time; investigate host/transport startup separately.

### Native actual-app qualification (separate from timings)

The dedicated `shell-startup.yml` workflow runs only on root-published
`diagnostic/shell-startup` commits. The completion-seed workflow and running
qualification are unchanged. It stages the existing pinned runtime and builds
the default-OFF actual app plus engine observation test, then invokes:

`tools/probes/run-shell-startup-app.py --build <default-OFF-build> --output <evidence>`
launches the actual app with the existing owned-process runner. Its explicit
`--smoke-shell-startup off|ready|disabled|unavailable` selector is fixture-only,
rejects mixed smoke modes and requires the diagnostic flag to match the mode.
It cannot load user configuration. All state and hardlinks live inside a unique
app-created smoke root removed by the existing owned cleanup.

The four System-provider cases cover:

- **off:** hostile inherited diagnostic values, but no flag and zero diagnostic
  records after a real authored input round-trip;
- **ready:** fresh and then sequential warm sessions in the same validated owned
  state, expected dump presence changing from absent to present;
- **disabled:** explicit empty HOME reaches the real home resolver and validator;
  persistence is disabled, without the unavailable-state notice;
- **unavailable:** an authored hardlinked history file triggers actual native
  validation rejection. No injected status, permission repair or weakened check.
  The expected notice must exist with nonzero layout and in the presented native
  scene. Ordinary smoke validation failures still fail outside this exact case.

Each case requires actual subsequent command input/output independently of the
line-init observation. The host uses the existing actor-owned selection formatter
to unwrap soft wraps while preserving hard newlines, then applies a full-line
fixed grammar to bounded in-memory fixture text. Selection is cleared without
writing the system clipboard. No frame text is logged in this smoke mode. The driver publishes only
allowlisted `STARTUP_APP_CHECK`, `STARTUP_APP_EXIT`, `STARTUP_APP_CLEANUP` and
`STARTUP_APP_RUN` numeric records. Malformed, missing and duplicate observations
fail. Every shell receives authored `exit 0`, must actually exit successfully,
and is closed before reuse or success. The driver also requires actor teardown,
owned-root removal and successful app process exit/reaping.
Each app process keeps the existing 120-second bound plus 10-second reap. The
engine-only formatter test runs first with 30 seconds plus 10 seconds to reap;
the separate
workflow step has a 600-second outer bound and 11-minute workflow limit. Existing
unprofiled startup/seed measurement commands and their timeouts remain untouched.
No diagnostic timing values enter the baseline matrix.

This new native gate is **not yet executed** merely because parser/source tests
or Windows cross-compilation pass. It currently targets default-OFF builds only.
Before enabling the completion seed with this loader, require a separate actual
ON-app ready-path coexistence check with the exact rebuilt seed identity; a
seeded fresh dump is already present before OMZ, unlike this OFF fixture's first
sample. Do not relabel an OFF success as ON evidence or modify the timing baseline
to obtain that check. This fixture verifies native scene submission, not physical
display latency or human input feel.

The engine-backed observation test covers widths 20, 40, 80 and 120 columns,
rejects the same record split by a hard newline, and rejects malformed/extra text.
It is registered in CTest, including native Windows builds. Viewport row
concatenation is never diagnostic-record authority.
