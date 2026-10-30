# Console-record component stage

Local source only. No native executable has been built or run for this stage.
Execution requires the parent runner's fresh, exact linked-binary review gate.
Passing these cases alone cannot qualify the full matrix or a production DLL.

`controller.c` is a native Windows program; `reader.c` is a separate MSYS C11
program linked against the selected instrumented runtime. Build them separately
with the pinned toolchain. The syntax declarations in `tests/windows.h` are
strictly Linux test doubles and must never appear in either native include path.

Controller arguments, in order:

`READER CASE TRACE RESULT GENERATION`

TRACE and RESULT must be new private files. CASE must match one of the 15 entries
in `cases.h`. GENERATION is a positive decimal trace identity. The controller
allocates its own console, creates unnamed inherited event/mapping handles,
uses the mandatory Windows 10+ PROC_THREAD_ATTRIBUTE_JOB_LIST creation attribute
to assign the reader atomically to its kill-on-close job, and resumes its suspended
primary thread exactly once. There is no post-creation assignment or unowned-child
fallback, including if the controller is cancelled immediately after creation.
The job handle is not inherited. Only the explicit handle list is inherited. It
never suspends a running reader/master, drains input, reads live trace records,
or sleeps to force a passing interleaving.

Each case injects one complete input batch after the reader's ready event. Ready
means setup completed, not that the runtime has entered read/select. The trace
must independently prove the required path. Mouse-gate cases service the bounded
1500ms rendezvous with a -1 width change, or -1/+1/-1 for a finite storm. Any gate
failure stops the exposure. A residual preexisting native event invalidates the
exposure; it is never discarded. Calls that merely collect subsequent bytes are
recorded individually, bounded at 16, and stop immediately on a byte mismatch.
EINTR continuation is allowed only in the explicitly changed-geometry cases.

The controller waits for the primary process and proves zero active job writers
before inspecting either shared mapping. It exports no trace/result snapshot
when writers cannot be proven stopped. Job closure is kill-on-close; cleanup
uses process waits and the job completion port with a bounded deadline, not a
polling delay. Finalized trace means writers stopped, not a passing case.

RESULT is one JSON object with schema 2. It records exact authored input,
requested/accepted batch counts, per-read counts/errors, synthetic output hex,
gate outcomes, screen geometry, native mode flags, observed termios, and cleanup
state. The reader's `mouse`/`focus` booleans describe requested mode controls after
a full successful output write; actual runtime translator modes require the
DISPATCH trace lanes. No paths, handles, environment variables, binaries or user
payloads are serialized.

Implemented cases: ordinary raw 1/64-byte requests, canonical and raw-select;
stable mouse/focus reporting off/on; unchanged resize; changed mouse at index0,
positive-prefix mouse at index1 and finite mouse storm, each reporting off/on.
Copied-prefix/zero-copy interruption, SA_RESTART, external SIGUSR1, select variants
of event controls, and surrogate cases remain absent. Parent matrix coverage
must retain these gaps as INCONCLUSIVE, not pass.

Portable check:

`python3 native/tests.py -v`

Four tests currently cover Linux declaration-based C syntax, bounded/unique case
metadata, absence of shortcut APIs, and source-level ownership/snapshot guards.
They do not simulate Windows process cleanup, prove Windows ABI compatibility,
or execute the native APIs. Those remain native review and execution gates.
