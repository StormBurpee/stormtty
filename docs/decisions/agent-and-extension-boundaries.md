# Agent sessions and declarative extensions

Status: proposed; implementation is not shipped by this document.
Decision date: 2026-10-29.

## Goal

Make agent work belong to a StormTTY workspace, with the same keyboard access,
visual language and background-task visibility as the terminal. Keep the terminal
usable without an agent account, network access or an extension runtime. Windows
is the first integration target; the portable model must also serve native macOS.

Workspace persistence and current build/CI reliability take precedence over this
integration. This ADR does not authorize a live agent task, authentication setup,
software installation or account changes.

## Existing boundaries

`Session` owns the asynchronous PTY/VT actor. `ProjectWorkspaceCatalog` owns
placement, not process lifetime. Configuration is bounded and declarative, with
validated replacement; commands have stable string IDs. Preserve these seams.
An agent conversation is not a terminal session, and an ANSI stream is not a
structured agent API. Shell context received through OSC remains untrusted display
data: it must never authorize a tool, start a turn or install an extension.

## Decision

Add a separate portable `AgentService`, with a Codex provider adapter first.
StormTTY owns presentation and workspace association; the provider owns its agent
harness. Do not embed Node or a browser merely to host an SDK. Do not rewrite the
harness or scrape the Codex TUI. A later Claude adapter must advertise its own
capabilities rather than pretending both providers have identical semantics.

Suggested ownership:

| Component | Owns | Must not own |
| --- | --- | --- |
| `AgentService` | State machine, bounded event queue, immutable view snapshots | HWND/AppKit objects, PTY parsing |
| `AgentProvider` | Capability interface and provider lifecycle translation | Global UI or workspace activation |
| `CodexTransport` | Child-process pipes, JSONL framing, request correlation | User consent, credential logging |
| `WorkspaceAgentStore` | Workspace/provider/thread associations | Access tokens, executable restore commands |
| Permission broker | Explicit user intent, scoped pending decisions | Automatic consent inferred from text |
| Native agent views | Transcript, approvals, changes, activity chips | Direct provider calls from paint callbacks |

Use separate agent IDs, not terminal `SessionId`. Store provider thread IDs as
opaque strings. Provider IDs and account references are scoped: do not reconnect a
stored thread under a different account silently. Transport decoding and disk I/O
stay off the UI thread; owned snapshots cross the boundary. Bound frame sizes,
transcript retention and queued work. Backpressure must not silently drop approval
or lifecycle events. A failed transport becomes visible and recoverable.

### Durable workspace association

Coordinate with the workspace-persistence implementation:
`ProjectWorkspaceMetadata::persistent_id` is an opaque 32-character lowercase hex
identity, generated when absent and serialized on save. It is distinct from the
process-local `ProjectWorkspaceId`. Duplicate persistent IDs are rejected within a
catalog/snapshot. Restore preserves the identity; cloning a saved workspace for
concurrent use assigns a new identity. This is a local association key, **not an
authorization token**. Bind agent history to it, never to pane/tab/runtime IDs.

A restored workspace may show saved agent history and disconnected status. It must
not replay prompts, restart turns, rerun commands or restore prior approvals.
Closing/parking a view does not implicitly terminate its agent; deleting a
workspace with active work requires an explicit lifecycle decision.

## Provider lifecycle and consent

Model connection state separately from conversation state:

- Connection: unavailable → disconnected → connecting → ready; failures remain
  visible, with explicit reconnect.
- Conversation: idle → starting → running → completed / interrupted / failed.
  Pending approvals are a separate keyed collection, not one global boolean.
- Key pending requests by provider instance, thread, turn, item and request ID.
  Resolve once; ignore stale UI decisions after cancellation, resolution or
  provider replacement. Never reuse a decision after reconnect.

There are two independent permission boundaries:

1. **Start a task:** this user's current rule requires approval before creating or
   starting a Codex task. Preparing UI or restoring metadata is not that approval.
   A deliberate Start action must disclose workspace, provider and execution scope.
   Do not automatically send a prompt on startup, retry or reconnection.
2. **Allow an action within a task:** show provider-requested command/file/network
   decisions with their actual scope. Never widen a grant, auto-accept a timeout,
   or map an unknown request to a permissive default. Decline unsupported requests.

An interrupted or disconnected request may have executed already. Reconcile state
before offering retry; transport retries are not permission to repeat a turn.
Display usage only when supplied, and do not present an estimate as a hard budget.
Authentication and native sandbox setup are separate, user-visible operations.
Do not copy another application's credential files or expose secrets in logs.

## Native experience

An agent panel belongs to its workspace. Its ribbon chip exposes running,
needs-attention and terminal state, with keyboard commands for focus, interrupt and
reopen. Approval sheets use StormTTY controls with accessible labels and focus
restoration. Never conceal pending approval behind an inactive workspace.

The changes view distinguishes proposed changes, applied changes and subsequent
user edits. A streamed diff is evidence, not an apply transaction. Do not promise
“approve before apply” unless execution policy actually provides that guarantee.
Never automatically revert user edits. An explicit Git-worktree execution mode can
later isolate changes; it is not required for the first provider interface.

## Extension v1: data, not arbitrary code

Use a versioned declarative package to contribute theme tokens and ribbon layout.
The following is a **proposed schema example**, not current config syntax:

```json
{
  "schema": 1,
  "id": "example.steel",
  "version": "1.0.0",
  "kind": "theme",
  "theme": {
    "background": "#11151B",
    "foreground": "#EEEDEC",
    "chrome": "#181D24",
    "accent": "#8C9EFF"
  }
}
```

Start with the existing `Theme` fields; expand the token vocabulary deliberately
for ANSI palettes and semantic chrome roles. No CSS, scripts, includes, remote
assets or filesystem traversal. Parse and validate the complete package before
preview or activation. Invalid updates retain the active theme.

Proposed ribbon configuration:

```json
{
  "schema": 1,
  "id": "example.developer-ribbon",
  "version": "1.0.0",
  "kind": "ribbon",
  "requestedBindings": ["session.directory", "session.git", "agent.status"],
  "items": [
    { "id": "cwd", "binding": "session.directory", "priority": 80 },
    { "id": "git", "binding": "session.git", "priority": 60 },
    { "id": "agent", "binding": "agent.status", "priority": 100 },
    { "id": "clock", "binding": "clock.local", "priority": 20 }
  ]
}
```

The host supplies values and layout; extensions do not inspect processes, scan
repositories or execute shell strings. Whitelist bindings and schema keys, bound
item/string counts, reject duplicate IDs, validate priority ranges and constrain
icons to the shipped icon registry. Runtime grants intersect requested bindings;
sensitive context requires explicit enablement. Bindings are workspace-qualified,
with an empty value rather than a stale value when context disappears.

Users can enable, order and hide items, with deterministic overflow and keyboard
access. A manifest cannot displace essential approval/error affordances. Actions,
when introduced, reference allowlisted registered commands and still pass their
normal availability/consent checks. No automatic command execution on load.

Agent-authored extensions are reviewable artifacts: preview → validate → explicit
enable. An agent cannot approve its own package or increase its permissions.
Themes/ribbon settings should travel with saved workspace settings without copying
credentials or machine-specific executable paths.

### Executable extensions are a later decision

Only add them with an out-of-process protocol, narrow capability grants, resource
limits, cancellation and independently verified Windows/macOS confinement. A child
process alone is not a sandbox. Avoid in-process DLL plugins and arbitrary draw
callbacks. Revocation and crashes must not take down terminal sessions. This ADR
does not select a language, runtime, marketplace or signing infrastructure.

## Implementation sequence and evidence

1. Land durable workspace identity/persistence and stabilize CI.
2. Build the portable agent model against a fake provider; test state transitions,
   duplicate/stale approvals, bounded streams, cancellation and reconnect.
3. Add declarative ribbon/theme validation and native views using clearly labelled
   fixtures. Test accessibility, keyboard paths, overflow and reduced motion.
4. Add version-pinned Codex transport/schema fixtures and compatibility rejection.
5. Obtain explicit authorization for authentication/setup and a bounded live task;
   then verify Windows runtime behavior and separately macOS behavior.

Steps 2–4 can be developed and tested without inference calls or Codex allowance.
A simulated transcript is not native agent verification. Core tests, Windows
process tests, live-provider tests and physical UI checks are distinct evidence.
No live Codex process or task was started to prepare this ADR.

## Sources and integration risks

Official documentation checked 2026-10-29:

- [Codex app-server](https://learn.chatgpt.com/docs/app-server): bidirectional
  JSONL/stdio, initialization, version-specific schema generation, thread resume,
  item events, interruption, approval requests and aggregated diffs. Stable and
  experimental fields are distinguished; WebSocket transport is experimental.
  Approval resolution events must retire pending UI. Windows sandbox setup is
  separate from starting the server. Prefer stdio and stable fields for v1.
- [Codex SDK](https://learn.chatgpt.com/docs/codex-sdk): recommends app-server for
  custom clients handling history, approvals and streamed events. TypeScript and
  Python libraries exist; neither is necessary for this native C++ integration.

Pin the supported runtime/protocol pair and validate unknown events defensively.
Documentation availability does not establish the user's account entitlement or
native sandbox readiness. Do not expose a network listener or rely on experimental
process-spawn APIs as the terminal backend. Provider capability gaps must remain
visible, not silently emulated with shell commands. Claude protocol selection and
its authentication/permission semantics require a separate documented review.
