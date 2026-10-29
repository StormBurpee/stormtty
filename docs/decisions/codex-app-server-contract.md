# Codex app-server implementation contract

Implemented foundation, 2026-10-29. **No live Codex invocation has been verified.**
The only executed child in tests is `tests/fake_app_server.cpp`, authored locally;
it performs no inference, authentication, HTTP requests, tools or file changes.

## Reviewed protocol baseline

Official latest release checked on 2026-10-29: `rust-v0.165.0`, commit
`360319d8dee62d87e2d59fc0c21805d79519cfed` (released 2026-10-27).
The adapter implements a bounded subset of the stable v2 stdio JSONL contract.
It does **not** demand an old CLI version, infer compatibility from a version
number, run `codex --version`, or claim that newer versions are live-tested.
Configured runtime version is descriptive metadata. Compatibility is checked on
required response shapes, account identity and returned execution policy; unknown
request methods receive a JSON-RPC error, never a permissive fallback.

Sources (read-only; no CLI/schema-generator invocation):

- [Official app-server documentation](https://learn.chatgpt.com/docs/app-server)
- [Pinned release](https://github.com/openai/codex/releases/tag/rust-v0.165.0)
- [Pinned generated protocol types](https://github.com/openai/codex/tree/360319d8dee62d87e2d59fc0c21805d79519cfed/codex-rs/app-server-protocol/schema/typescript)

Relevant source blob identities, all at that commit:

| Generated type | Git blob SHA |
| --- | --- |
| `InitializeParams.ts` | `e48c5ee7b52c7dc346e8f851b28a26c299e1ac6c` |
| `InitializeResponse.ts` | `f1f79d173c77778057e223f5cac0e7ef2b113135` |
| `InitializeCapabilities.ts` | `ac82d743e3f06c99dbf8fb9066f1f1c085a22c05` |
| `v2/ThreadStartParams.ts` | `69280c0b38c6c45de8ca6adc1866c588252e5d97` |
| `v2/ThreadStartResponse.ts` | `c4ff08409cac6745740cf99d394aed72c07666cb` |
| `v2/ThreadResumeParams.ts` | `4da03970e9f2f0d7b5e796c4dd0ea96fda69a95e` |
| `v2/ThreadResumeResponse.ts` | `4bffeaa95569312931e684264668745cc9ad187d` |
| `v2/TurnStartParams.ts` | `e83b6818996c56bcc433d69444ed00d0b9f55c5e` |
| `v2/UserInput.ts` | `390d9dd59c931e5f1bba2bdff7078766a8f99cd4` |
| `v2/SandboxPolicy.ts` | `5575701ff2d7924e431df7b86d57947d650f2989` |
| `v2/CommandExecutionRequestApprovalParams.ts` | `0ad35b402303ddd719aba33e5d220856e63ab351` |
| `v2/CommandExecutionApprovalDecision.ts` | `c022030a1e2ba6dda977910eb3b96588110c4a18` |
| `v2/FileChangeRequestApprovalParams.ts` | `2db7be9ec49408d4a19b44486678ebc9d273daff` |
| `v2/ServerRequestResolvedNotification.ts` | `56c53cc445e7b0e1c71ac1e8c91a3e691714802b` |
| `v2/GetAccountParams.ts` | `9e82ef5e13a95682be005d91647126941dff3ca6` |
| `v2/GetAccountResponse.ts` | `e88cc6abe3a32efe04c143792387e61a5c8c1c85` |
| `v2/Account.ts` | `4897b1ff9a120df3bd3da6118b47a416267a2581` |

## Actual behavior and authority

`AgentService` is one portable, mutex-protected workspace/provider instance. It
publishes owned snapshots, not HWNDs or borrowed strings. The first adapter is
Codex-specific; this does not pretend to support another provider's semantics.
`AgentStdioTransport` owns native pipes and the child lifetime separately from the
PTY system. Its explicitly confirmed launch runs exactly the selected absolute
executable with argument `app-server`, without a shell or PATH lookup. A separate
Start confirmation is required for every thread/turn operation. No launch occurs
on service construction, deserialization, workspace restoration or reconnect.

The handshake sends initialize, awaits its response, sends initialized, then
account/read with refreshToken false. The configured account reference must be
`chatgpt:<email>` matching the returned account. Unknown, API-key and email-less
identities are unsupported in this first slice because they provide no stable
identity for safe resume. Email is metadata, not a token; do not log it. The
stable response does not identify an organization/workspace, so organization
switches sharing an email cannot be distinguished by this protocol subset.
Account update notifications invalidate the connection. No login, credential
copying, token refresh, model listing or sandbox installation is implemented.
The initialize capability requests explicit gateway OAuth rather than automatic
browser authorization; StormTTY never starts that OAuth flow.

A confirmed Start creates/resumes the workspace's thread and checks the response
before submitting text: exact requested cwd, idle thread, untrusted approval
policy, user reviewer, read-only sandbox and disabled network. Missing/mismatched
policy is an error, not an inferred grant. Text uses the current `text_elements`
field. This is an initial read-only execution mode, not an unrestricted coding
agent. Command/file approvals can allow an individual requested action; there is
no accept-for-session, policy amendment or broad-root grant in this client.
Unknown scope fields, managed-network requests, remote environments, stdin
approvals, missing item scope or a disallowed decision disable Accept. Decline
and Cancel remain available. Full item/request scope is retained for review.
A diff is display evidence only, never an apply/revert operation.

Approval identities include a process-unique provider generation plus thread,
turn, item and typed request ID. Decisions resolve once. Item completion,
serverRequest/resolved, turn completion, interrupt and disconnect retire stale
UI decisions. A duplicate server request fails closed. Requests from another
thread/turn are canceled. Interrupt acknowledgment is not terminal completion;
the UI remains interrupting until turn/completed. A disconnect during a start
or active turn marks the outcome unknown. No pending text or request is replayed.
A subsequent explicit start resumes and verifies idle state before a new turn.

## Bounds and failures

- JSONL frame: 1 MiB; read call: 4 MiB / 4096 frames; nesting: 32; JSON nodes: 32768.
- Pending outgoing requests: 32; outbound queue: 64 frames / 2 MiB.
- Pending approvals: 32; retained item scopes: 32 x 64 KiB; each displayed
  approval scope: 128 KiB. Server request deduplication: 4096 per connection.
- Transcript: 256 items / 256 KiB; each text item: 64 KiB. Display truncation is
  explicit; it never grants permission or drops a lifecycle/approval event.
- Diff: 256 KiB display cap. Metadata store: 256 workspace associations.

Malformed JSON, duplicate keys, invalid UTF-8/surrogates, excessive framing and
queue overflow fail the connection visibly and clear outbound work. Native
stderr is drained but not treated as JSON or retained in logs. Pipe write failure
never causes retry. POSIX uses nonblocking pipes/poll and a child process group;
Win32 uses a constrained inherited handle list, a kill-on-close job, and separate
read/write workers so stdout cannot deadlock behind a full stdin pipe. Stopping
kills the owned process group/job and reaps/joins workers. This is process lifetime
control, **not a sandbox**. Native Windows sandbox effectiveness is unverified.

The versioned association serializer stores only durable workspace ID, provider,
account reference and opaque thread ID. It rejects duplicate workspace IDs and
retains the caller's prior collection on invalid input. It does not itself choose
a disk path or silently add metadata to existing workspace save files.

## Evidence and remaining boundary

`agent_service_tests` covers handshake, consent, typed request correlation,
fragmented Unicode JSONL, malformed/excessive input, scoped/duplicate/stale
approvals, interrupt completion, reconnect/no replay, metadata restore and
concurrent snapshots. `agent_stdio_tests` launches only the authored fake child,
checking real local pipes, approval round-trip, interruption, malformed output,
child exit, explicit launch refusal, a 15-second stalled-handshake timeout and
destructor cancellation with a non-reading child and a full stdin pipe.

Linux core/native-pipe tests passed; Windows GNU cross-compilation passed.
ASan/UBSan core fixtures passed with leak detection disabled because LeakSanitizer
is unsupported under the execution environment's ptrace. These are separate gates.
Neither establishes native Windows execution, macOS execution, live authentication,
actual provider sandbox confinement, account entitlement, inference, accessibility
or physical UI quality. A bounded live task needs separate user approval.
