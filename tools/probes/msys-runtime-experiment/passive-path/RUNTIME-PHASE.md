# Fixed synthetic runtime candidate — root review/publication pending

This phase follows the actual native review in `NATIVE-REVIEW-40916378419.md`.
It does not broaden the uninstrumented behavior patch or any path predicate.
The original runtime bundle is copied and verified, never modified or launched.
No actual Zsh, Codex, native SSH, user command or application stage runs.

## Trigger / ordering

`.github/workflows/msys-passive-runtime.yml`: push-only exact branch
`experiment/msys-passive-runtime`, changing that workflow path, exact github.sha
checkout and read-only contents permission. It is **not** activated by the build
workflow or an API dispatch. Root reviews the candidate diff before publishing.

1. Portable regressions; stage an untouched reference bundle only to retain the
   existing provenance/isolation checks (its shell is never executed).
2. Same official source/private GCC15 verification and exact stock/refined
   diagnostic source builds. Observer/behavior patches and helper code unchanged.
3. Both variants get a linked-code preflight, even if one fails. Failure prevents
   every fixture launch. Both linked diagnostics/logs remain collected.
4. Only successful linked gates permit strict native MSYS reader/MSVC controller
   builds and experimental-copy staging. Staging and final runtime identities
   bind each linked report to the exact DLL hash; copying cannot bypass the gate.
5. Eight fixed owned trials, same two cases and two orders, then always-run
   bounded allowlisted collection/upload. No retry or continuation-as-success.

## Concrete linked gate, not a safety-name heuristic

`linked-preflight.py` does not load the DLL or call its exports. Bounded PE32+
AMD64 parsing resolves exports and file IAT records. It requires:

- The same reviewed source/observer/compiler-package identities and exact native
  console.o helper bodies from run40916378419.
- Actual linked __errno export starts with exactly
  `65 48 8b 04 25 08 00 00 00 48 2d 00 32 00 00 c3`: GS TLS load, subtract12800,
  return. No call, allocator, dispatch, lock or wait exists in that leaf. This
  sequence was also directly checked in the existing original DLL SHA
  `d3b5324eef7fe377c3412bd8d9f5c526ba1136940e34105436b15e5f1ae90717`.
- Linked handler/emitter instruction bytes match the reviewed real object code,
  except only the explicitly recorded four-byte REL32 relocation fields. Gaps,
  unknown relocation kinds or other instruction changes reject the gate.
- Relocated call/tail-jump targets resolve to that exact __errno export, the
  verified emitter, or a six-byte import thunk targeting the named KERNEL32
  GetLastError/SetLastError/GetCurrentThreadId IAT slot. Wrong DLLs, forwarded
  errno, ambiguous targets, unexpected callees or indirect helper dispatch fail.
- Actual linked errno/handler/emitter disassemblies and numeric RVA/hash reports
  are retained. Failure reports explicitly set linked_code_gate_passed=false;
  no fixtures execute on unsupported linkage/layout rather than weakening checks.

This closes the previously missing linked errno/body correspondence for this
specific code. It **does not** inspect the Windows OS DLL implementation or prove
portable/general async-signal safety; the OS TLS/identity contract assumption
remains explicit. Source observer overhead, page faults and scheduling effects
remain. Production qualification stays false in every report.

## Outcomes and bounds

The original eight-trial matrix remains unchanged, including intentionally
unsupported stock positive-prefix full-path coverage. Consequently all-eight
PATH_OBSERVED success is currently unattainable: byte/fixture failure exits1;
otherwise any unsupported/incomplete path exits2 and remains INCONCLUSIVE. Do not
rerun to chase green or mask this as continue-on-error success. Individual full
paths and independently recorded interrupted-partial-read observations remain
inspectable without converting the matrix into a production qualification.

Trace decoding now precedes fixture-log parsing. Missing/truncated/oversized
logs force byteFAIL + INCONCLUSIVE but do not erase already committed numeric
observations from a valid bounded trace. Missing/overflow/context-error traces
still cannot manufacture observations or path success.

Reader alarm8s, eight reads/256bytes maximum; controller readiness4s, queue
hint1.5s, child wait10s, process reap2s/job drain2s; outer runner25s+5s reap and
64KiB output. Matrix step8min; job180min, each source build40min. Native linked
objdump subprocesses have20s timeouts and <=16KiB inspected function ranges.
No live trace feedback schedules input. Original private bundle remains intact.

Upload is **only** a new review packet assembled from enumerated source/build
provenance plus results.json and four files in each of eight fixed trial dirs:
trace.bin, reader.jsonl, controller.jsonl, launcher.log. Trial traces cap at
2,097,216 bytes; logs64KiB; results1MiB; total packet128MiB. The private-fixtures
subtree contains DLLs/executables and is explicitly never copied/uploaded. No
DLL/object/package/keyring/HOME/history/unbounded capture is uploaded. Over-cap
or alias evidence is rejected and marked, not silently truncated into a pass.

## Local validation boundary

Synthetic PE tests cover RVA bounds, import-thunk identity, forwarded exports,
linked instruction drift and invalid relocations. Actual reviewed object text
parses as handler608bytes/emitter640bytes in both variants. The original pinned
DLL's errno leaf was inspected without loading it. These are useful preflight
checks, **not execution of the new diagnostic DLL or proof that its native
linked gate will pass**. Any first-run mismatch is preserved and reviewed.
