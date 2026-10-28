# Real project workspaces

`ProjectWorkspaceCatalog` is the portable navigation model above the existing
`Workspace`. One catalog owns an ordered set of **real** project workspaces;
each contains its own existing task tabs, split trees, active pane, zoom state,
pane titles and opaque session associations. It does not duplicate the tab/split
implementation or manufacture sidebar content.

The shared implementation has no OS, filesystem, terminal or process imports.
Creating, selecting, renaming, hiding or removing a project does not launch a
shell, run a command, change a process directory, or close a session.

## Small host-facing API

Include `stormtty/project_workspaces.hpp` and link `stormtty_workspace`.

| Operation | Behavior |
| --- | --- |
| `create(metadata, workspace = {})` | Append an empty or supplied existing `Workspace`; preserve its local IDs, tab names and session associations. The first project activates automatically. Later creates preserve the current selection. Return the new project ID. |
| `entries()`, `find(id)`, `active()` | Read-only ordered project records: `id`, `metadata`, `workspace`. No accidental mutable project IDs or metadata. |
| `workspace(id)`, `active_workspace()` | Mutable/const pointers to the existing model, so native hosts retain their current tab/split operations. Null when absent. |
| `activate(id)` | Change only the selected project. All hidden workspaces remain owned and unchanged. |
| `rename(id, name)`, `update_metadata(id, metadata)` | Validate and update explicit project metadata; never modify live sessions. |
| `session_ids(id)` | Sorted nonzero bound IDs for a removal preview; empty for an absent project. Use `find` if absence must be distinguished from an unbound workspace. |
| `all_session_ids()`, `contains_session(id)` | Include **every** project, not just the visible one. The first returns a sorted union; zero is never a live association. |
| `remove(id)` | Return `RemovedProjectWorkspace {project, sessions}` containing the entire detached model and its current affected IDs. No implicit close or process destruction. |
| `validate()` | Check project selection, IDs, metadata, every nested Workspace, and session uniqueness within the catalog. |

References and pointers into a catalog may be invalidated by `create` or
`remove`. Retain IDs and reacquire records, not long-lived references. Catalogs
are intentionally noncopyable and nonmovable: copying must not duplicate their
identities, and assignment must not silently overwrite an owned set of session
associations. Detached results can be moved normally.

Successful creation preserves insertion order. Removing a nonactive row leaves
the active ID unchanged. Removing the active row selects the next surviving
row, or the preceding row when removing the last row. Removing the final project
leaves a valid empty catalog with no active workspace. The **host** decides
whether to show an empty state, explicitly create another workspace, or close a
window; the catalog makes none of those decisions. Failed operations reported
through `Status`/`Result` leave the catalog unchanged.

## Identity and lifetime invariants

1. **Project IDs are unique across catalogs in the running application.** A
   shared monotonic atomic allocator issues the strong `ProjectWorkspaceId`
   type, never zero and never reuses an issued ID after removal or catalog
   destruction. It fails on exhaustion instead of wrapping. Catalog mutation is
   nevertheless UI-thread-only; the atomic allocator is not permission for
   concurrent access to a catalog.
2. **Existing pane, tab and node IDs are workspace-local.** Two projects will
   commonly both have `PaneId{2}`. Use `ProjectPaneReference`,
   `ProjectTabReference` and `ProjectNodeReference` for any cache, selection or
   delayed action spanning projects. The helpers contain `{project, local}` and
   are strongly typed. Alternatively clear strictly view-local caches whenever
   the project changes. Never flatten different ID types into a single raw
   integer key. A project's numeric ID may equal a pane/session's numeric ID;
   these are deliberately different identity domains, not a collision.
3. **Session lifetime belongs to the native host.** Retain a single monotonic
   host SessionId allocator and registry across that host's projects. Bound
   nonzero SessionIds must be unique within one catalog. `create` rejects an
   imported duplicate, while `validate` also detects duplicates introduced
   through mutable Workspace APIs. When allocating or rebinding through those
   APIs, the host must preserve the cross-project uniqueness precondition; the
   existing `Workspace` validates only its own local associations. Zero means
   unbound, not a shell waiting to be automatically resurrected.
4. **Independent host registries need not share SessionId counters.** Separate
   catalogs may both use `SessionId{1}`. Code referring to those associations
   outside their host must use `ProjectSessionReference {project, session}` (or
   another explicit host-qualified key), not a bare SessionId. Importing a
   Workspace from another host does not transfer a process, registry entry or
   ownership; the host must explicitly arrange any valid binding.
5. **Changing visibility never changes ownership.** Project activation does not
   clone or rebuild a Workspace. A hidden build, REPL, editor or assistant
   session retains the same association and host-owned actor. Closing a pane or
   tab still uses existing Workspace APIs and requires the host's existing
   confirmation/lifetime rules.

Project IDs are stable for the model lifetime, **not disk-persistent UUIDs**.
This increment adds no catalog serialization, terminal-state persistence,
command replay, or session resurrection. Importing a detached Workspace through
`create` issues a fresh project ID; it is not restoration of its previous host
identity. Existing Workspace metadata serialization remains unchanged and still
restores unbound panes only.

## Honest metadata and navigation

`ProjectWorkspaceMetadata` contains:

- `name`: a user-facing nonblank name, at most 256 UTF-8 bytes. Names are not
  identity keys and need not be unique. Only entirely ASCII-space names are
  additionally treated as blank; valid non-ASCII names are retained verbatim.
- `working_directory`: explicit UTF-8 launch context, at most 32,768 bytes.
  Empty means the host's default launch directory. This is **not** a claim about
  the current directory of any running shell. The host must resolve/check the
  directory using its platform rules before explicitly launching a session.
- `environment_label`: optional descriptive text, at most 256 UTF-8 bytes.
  It does not establish WSL, SSH, a container, Codex or Claude connectivity.

All fields reject malformed UTF-8 and ASCII control characters, including NUL,
newlines and terminal escapes. The model neither probes paths nor shell-parses
metadata. Updating launch context affects only future host launches, never
existing sessions. Preserve meaningful existing tab titles; derive untitled
tabs from their real active shell/session in the host. Do not add sample
projects, invented activity, fake assistant status or placeholder integrations.

Sidebar expanded/collapsed state, row hover and scrolling are presentation
state. Collapsing the navigation must not remove project records or their
sessions. A future native macOS sidebar can use the same catalog with its own
AppKit view state.

## Native-host integration checklist

1. Keep one catalog per owning host/window and one host session registry across
   its projects. Create the initial project from the actual configured launch
   context, not invented example data. The catalog itself starts empty.
2. Replace direct active-model ownership with `active_workspace()` access.
   Handle the null case deliberately. Use `create` and explicit `activate` when
   adding a project, then explicitly launch/bind any requested real shell using
   the project's validated host launch context.
3. Enumerate all entries for background actor/title updates. Retire a session
   only after checking **all** catalog associations with `contains_session`,
   not merely the active Workspace's panes. Existing actor/process ownership
   stays in the host.
4. On project activation, end view-local drag/rename operations and clear or
   project-qualify pane/tab/divider hover, animation, renderer and delayed-action
   keys. A stale local `PaneId` from the previous project must not accidentally
   target the same numeric ID in the next one. Only render and focus the active
   project; do not terminate hidden ones.
5. Before project removal, call `session_ids(id)` and use the host's actual
   session registry to decide what confirmation is needed. Cancel means do
   nothing. If a modeless/reentrant confirmation permits intervening changes,
   compare the current associations and relevant metadata with what was
   authorized and prompt again when necessary. Do not mistake a preview for
   automatic permission to terminate a newly added session.
6. After authorization, call `remove(id)`, inspect the returned `.sessions`, and
   deliberately close/retain the corresponding host-owned sessions according to
   that authorization. `.project.workspace` remains available in the result;
   no opaque association is silently discarded by the removal API. Continue
   handling final-project/window exit in the host. Treat the removal result as
   required; the API is `[[nodiscard]]`.

This layer intentionally does not provide shell launching, terminal actor
management, confirmation dialogs, directory selection, or native sidebar
widgets. Those stay in the existing platform boundary rather than creeping
into the portable task/split model.

## Portable verification

`tests/project_workspaces_tests.cpp` covers:

- Empty, first-create and subsequent-create behavior; const/mutable access.
- Repeated activation preserving hidden tab/split/zoom/focus/session state.
- Hidden-session updates and complete catalog session enumeration.
- Process-wide project ID uniqueness across catalogs, removal, recreation and
  catalog destruction; qualified local ID keys and independent host registries.
- Complete detached removal reports, canceled-preview nonmutation,
  deterministic selection fallback and final-project removal.
- Malformed/control-character/oversize metadata, duplicate session imports,
  host binding invariant detection, project count limits and unbound panes.
- A deterministic 1,500-operation trace checking validity, session retention,
  selection, failed-operation atomicity and never-reused project IDs.

The tests can run without a terminal adapter, renderer or process backend:

```sh
g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Iinclude \
  tests/project_workspaces_tests.cpp src/core/project_workspaces.cpp \
  src/core/workspace.cpp -o project_workspaces_tests
./project_workspaces_tests
```

Portable test success is not Windows or macOS runtime, UI, accessibility,
terminal-session performance or native renderer verification.
