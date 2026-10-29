# Native static review — run 40916378419

## Decision and boundary

**Supports a subsequent bounded synthetic diagnostic phase with the conditions
below. Does not establish general async-signal safety, correct runtime behavior,
input preservation, actual internal-path coverage, or production suitability.**
Root owns publication and review of the subsequent controlled phase. No fixture
was executed in this run. No runtime patch broadening follows from its success.

Exact commit `65b8801c4df3a21f49ab1593a4bae497d3c8aefb`, job123597586300.
Artifact12917871915 SHA256
`14a7680e08e205c8ffbfeeb026b2755f8efdbb51bbe1f927767ae773a4dee62c`.
Downloaded archive and all 95 manifest entries were rehashed locally: 7,992,998
uncompressed bytes, no missing/rejected files, both static reports present.
Prior failed run40912823466 remains retained; this corrected run fixed only CRLF
text parsing, not the source observer or runtime behavior patch.

## Actual native evidence reviewed

- Both source builds used verified GCC/G++15.3.0, GNU++17, explicit diagnostic
  macro and strict warnings. Package and private-compiler inventories match.
- Exact official archive, behavior patch, separate observer patches/header and
  before/after console source hashes match the locked inputs.
- Both real DLL export-name tables contain exactly the two diagnostic exports.
  Review used the **actual runtime console.o**, not the standalone smoke object.
- All seven emitted recorder/helper function bodies are identical between stock
  and refined. The combined init/handler disassembly SHA is
  `af8d17554e8db80442ef39214d70384921a199f3e43d318e1492cbeb0aa40b8d`.
- `stormtty_path_handler_v1` uses a stack-local context, cleared with `rep stos`.
  Its external relocations are GetCurrentThreadId, GetLastError, SetLastError,
  __errno and the local emit_context helper. No allocator, file/network I/O,
  sleep/wait, mutex acquisition, indirect dispatch or generic atomic helper is
  present in that function body. Context does not access interrupted mutable
  thread-context bookkeeping.
- emit_context calls only GetLastError, SetLastError and __errno. The actual
  sequence at offsets0xce/0xe4/0x251 checks slot<=0x3fff, uses hardware
  `lock cmpxchg`, and exits after 0x20 failed attempts. Offset0x270 marks overflow;
  offset0x203 publishes the committed word after payload stores. There is no
  wrap/overwrite or unbounded software reservation loop.
- Remaining out-of-line helpers are begin, read_end, signal_begin, signal_end
  and release. Their extra callees are current process/thread IDs and emitter;
  release also calls the **original** ReleaseMutex being observed. It is not
  reachable from the diagnostic signal-handler path. These functions and the
  generator/source diff were checked separately from the existing console code,
  which legitimately contains allocations and waits unrelated to the observer.
- Cold init contains MapViewOfFile/UnmapViewOfFile. It runs before measurement;
  neither is reachable from handler/emitter. No mapping is polled by controller
  while the reader runs, and no recorder operation gates/schedules input.

`native-review-40916378419.json` records exact code/helper/provenance identities
and relocation targets. DLL/object hashes there are **recorded build identities**;
the artifact deliberately excludes binary files, so they are not local binary
rehashes. Text/source/disassembly hashes were directly recomputed.

## TLS/accessor dependency and remaining uncertainty

The official source archive was independently rehashed as
`4b178dfb5b77bce293706dcfabea673b77d8da061a4725bdf4cd94f17533c204`.
Its bare Git pack/index were compared byte-for-byte by SHA256 with the preserved
source repository, avoiding reliance on a mutable prepared console file. Source
revision867eb7748e6e5a0e8430ce9f46af27b4b1e8f204 (cygwin-3.6.11) and original
packaging patch list show the errno/TLS files below are unchanged by packaging.

- `newlib/libc/errno/errno.c` returns the address of errno in `_REENT`; SHA
  `f6aa3747ded9ca8441730918e5bd97794d6c0b55e999cd132c670b74f4d32cc1`.
- `winsup/cygwin/include/cygwin/config.h` defines dynamic reentrancy and the
  internal __getreent computation from x86_64 GS stack/TLS location; SHA
  `0a664264d08bf08ed787b3065d04b2cec14e1edf98cd6d64ef9b02d4128e69cc`.
- `winsup/cygwin/thread.cc` external __getreent returns `_my_tls.local_clib`; SHA
  `c580fac87c0df31016fc91587114ee5010b85e3703925230a4e4e933f96bc707`.

This source supports a nonallocating TLS accessor interpretation, but the current
artifact does **not** include linked __errno instruction bytes. Do not silently
turn source review into a claim about that missing linked body.

Microsoft documents [GetLastError](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-getlasterror)
and [SetLastError](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-setlasterror)
as per-thread error accessors, and [GetCurrentThreadId](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getcurrentthreadid)
as returning the calling thread ID. Those contracts do not promise POSIX
async-signal safety. No Windows OS DLL implementation was captured/reviewed here.
Their ordinary TLS/identity behavior is an explicit assumption for an isolated,
kill-bounded synthetic diagnostic, not a portable/general safety certification.
Page faults and observer overhead can still perturb scheduling. Thread-ID reuse,
late/competing handlers, fork, nested/overflow contexts and unsupported paths
remain rejected or inconclusive as documented; absence of a trace is not proof.

## Required next-phase controls

1. Preserve the same source/observer/toolchain identities or separately review
   any correction; rebuild privately because no binary reuse artifact exists.
2. Before launching fixtures, capture linked __errno and recorder code, inspect
   their resolved call targets and reject unexpected allocation, locking,
   dispatch or atomic helpers. Preserve this evidence on failure. Do not mark
   the current static report's async_signal_safety_verified field true merely
   because the job passed or this limited diagnostic is considered acceptable.
3. Compile the fixed reader/controller with strict native warnings and verify
   reader DLL imports. Retain the exact eight predeclared trials, owned job,
   alarms/timeouts, mapping bounds, numeric-only runtime trace and no retries.
4. All missing/incomplete/overflow/ambiguous traces stay INCONCLUSIVE. Stock
   positive-prefix remains an unsupported complete-path predicate. Byte loss,
   reader alarm, residue or cleanup failure remains a failure, not a pass based
   on observed numeric prefixes. Retain all trial rows and source identities.
5. No actual Zsh/Codex/native SSH/user command, production installation/default
   change, binary upload or release/source-notice waiver. Any eventual behavior
   proposal requires actual copy/dispatch evidence, not the earlier external
   EINTR hypothesis or this build-only result.
