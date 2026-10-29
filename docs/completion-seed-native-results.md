# Native completion seed qualification: 2026-10-29

## Identity and outcome

[Run 40838444672](https://github.com/StormBurpee/stormtty/actions/runs/40838444672)
completed successfully on Windows Server 2025, both inbox and pinned packaged
ConPTY providers, with no retries. Remote commit
`abb400c9b22bd54c92cefea7890133deb22d3b75` has the prepared tree
`a0827299801c82491261ed7eb6d30cb91a0a8edc` (local `e7581ce`).

Artifact `12882071614` was downloaded and its archive SHA-256 verified:
`7b091fc01f8da88b9fb7e6577917b3184e35d5f5ea35a939bfbc934ccbf330ff`.
It contains all four valid measurements, adoption/fallback checks and hash-only
identities. Original staged manifest SHA-256:
`bb77f3d8f8c62b8bb0f05f2e9699e9a35313603373bf98a199a2cdf2db5d3f71`.
The generated text seed was 44,173 bytes, SHA-256
`361846b3c44f1db8e82799fb28fa3bf0ae90971621a7ed6a5fd06ef3fa0d7ec7`.
No executable seed/runtime or private state was uploaded.

## Actual measured totals

Each row has three fresh-state and three warm-state samples. **Total** below is
`state_prepare_ms + verification_publish_ms (if any) + launch_to_zle_ms`.
It includes complete bundle hashing and native private publication, rather than
reporting only the reduced shell portion. This is an instrumented source-profile
fixture, not an uninstrumented application startup benchmark. Fresh means a new
private completion cache, **not** a cold OS filesystem cache or first-ever app run.
The same runner's bundle was already staged, verified and used in adoption checks.

| Provider / variant | Fresh range (s) | Fresh median (s) | Warm range (s) | Warm median (s) |
| --- | ---: | ---: | ---: | ---: |
| Inbox baseline | 4.006–4.123 | 4.064 | 0.568–0.629 | 0.612 |
| Inbox seeded | 1.026–1.174 | 1.070 | 0.585–0.981 | 0.586 |
| Packaged baseline | 4.170–4.730 | 4.379 | 0.647–0.678 | 0.655 |
| Packaged seeded | 1.121–1.275 | 1.211 | 0.631–0.689 | 0.649 |

Full verification/publication cost **496.8–526.1 ms** on the six successful fresh
adoptions; warm shells performed no adoption. The observed fresh median reduction
was 2.994 s inbox and 3.168 s packaged in this fixture. No warm improvement is
established: one seeded inbox warm sample was 0.981 s, and provider ranges overlap.
The fixed order was inbox baseline then seeded, packaged seeded then baseline;
three samples per condition do not establish broad hardware/filesystem behavior.

## Attribution and correctness evidence

* Fresh baseline `compinit`: **3503–4095 ms**; seeded: **80–132 ms**.
* Fresh baseline `compdef`: **639 calls**, 2570–2964 ms; seeded: **2 calls**,
  0.11–0.16 ms. Seed loading removes the initial registration scan, not security.
* `compaudit` remained in every measured sample. Fresh seeded inclusive audit
  totals were **44.7–92.7 ms** (zprof reports two calls because of its internal
  invocation structure; this is not an assertion of two independent audits).
* All six fresh seed adoptions succeeded. All six seeded warm samples reused
  state without adoption; the probe also asserted unchanged dump mtime on warm
  samples.
* All eight native adoption/rejection checks passed: initial/repeated adoption,
  existing dump, compiled sibling, custom completion, competing temporary,
  corrupted seed, incorrect actual-content hash and reordered fpath (initial
  adoption/repeated adoption share one test case).
* The separate corrupted-seed fallback rejected adoption after **479.2 ms**,
  then used normal secure compinit: **3548.1 ms**, 639 compdef calls and 48.1 ms
  compaudit. Total fallback measurement was **4557.3 ms** including rejection
  cost, state preparation and launch-to-ZLE. It passed first-Tab/module checks.
* Every baseline/seeded sample and the fallback exercised the first actual Tab
  against an authored unique filename before module assertions. Assertions also
  required OMZ, completion registration, autosuggestions, highlighting, fzf,
  zoxide, Unicode console setup and private persistent state.
* The relocated runtime path contained spaces and Unicode; private state did too.
  Exact OMZ fpath metadata therefore worked with those paths, not just ASCII
  paths without spaces. All owned measurement children exited/reaped without
  timeout, truncated evidence or cleanup error.

## Decision and remaining limits

The prototype has now demonstrated a substantial **fresh-private-cache** benefit
on real Windows even after its conservative whole-bundle verification overhead.
That supports further production lifecycle design; it does **not** activate the
feature or establish production readiness. Default loader/runtime behavior remains
unchanged.

Keep the constraints from `completion-seed-feasibility.md`: trusted build-produced
package, owned immutable diagnostic runtime, no ordinary shell starting during
publication, and no arbitrary same-user writer. Package-ancestor protection,
pre-guard enumeration and coordination against concurrent ordinary `.zwc` creators
still need a reviewed production solution. Do not bypass integrity verification,
compaudit, user override ordering or eager completion to reproduce these numbers.
A rejected seed must retain the ordinary completion path even though rejection
adds measured overhead. No further run/retry or remote mutation was performed.
