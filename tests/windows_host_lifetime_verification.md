# Windows host lifetime module boundary

This moves existing owners, not application/view transactions. The implementation
baseline is f8c27fab. NativeApplication and NativeWindow remain in window.cpp;
their eventual separation is still outstanding.

## Modules

- application_actors.hpp/.cpp: ActorLifetimeRegistry and ApplicationActors.
- native_wake.hpp/.cpp: WakeState and RendererReadyWake, with the existing
  WM_APP + 41 wake identifier.
- WorkspaceSaveWake remains in window.cpp and still posts its distinct,
  noncoalesced workspace-save notification. It is not folded into WakeState.

These private Windows modules know no NativeWindow, NativeApplication, Renderer,
live Config or placement model. There is no pimpl allocation, new registry,
factory, generic lifecycle hook or host-context facade. The existing borrowed
const registry range remains allocation-free; asynchronous/queued consumers
must explicitly retain a Session. The native endpoint is a shared synchronized
notification/mailbox record, not an HWND owner.

## Exact ordering

Registration remains handle insertion followed by incarnation issuance/insertion.
It is deliberately not made atomic; map-allocation failure retains the inherited
partial-registration behavior. IDs remain application-scoped and nonrecycled.

Disposal with a completion endpoint binds the route before request_close, then
erases registered ownership, forgets identity and erases route discoverability.
ApplicationActors next forgets observation metadata, then attention metadata.
A callback-only failed launch follows the same path without a registered handle.
Callbacks retain their route, and routes hold only weak endpoint ownership.

Declaration order is unchanged:

- ApplicationActors: observations, attention, registry. Registry closes first.
- ActorLifetimeRegistry: identities, sessions, wake router. After destructor-body
  close requests, members unwind router, sessions, identities.
- RendererReadyWake: endpoint, wait, borrowed armed handle, fired flag. Its body
  cancels/drains then closes the threadpool wait before endpoint destruction.

Neither registry operation adds an explicit actor join. Releasing the last shared
Session reference retains Session's destruction behavior; externally retained
actor tests are not a universal prompt-destruction claim.

WakeState preserves HWND/thread/fallback precedence, atomic memory orders,
coalescing and reset-on-failed-post. Clearing the HWND does not clear a pending
mailbox or invent a second delivery protocol. Clipboard/error ownership is still
exchanged under one mutex in the host; callbacks never call native view methods.

RendererReadyWake preserves fired acknowledgement before same-handle comparison,
one-shot rearming, and cancellation via SetThreadpoolWait followed by
WaitForThreadpoolWaitCallbacks(TRUE). Its callback touches only its fired flag and
endpoint. The wait never closes the renderer's borrowed latency handle.

## Unchanged host integration

A mechanical comparison of window.cpp against f8c27fab, removing only the moved
class definitions and substituting imports/aliases, produces exact equality.
Therefore the creation ledger, failed-candidate destruction/rollback, completion
rebinding during retirement, persistence flush, message loop and all renderer
wait cancellation sites are unchanged. NativeWindow member order is unchanged.
No navigator state or input route is modified.

The extracted source expands dense control bodies and fields for readability
without changing operations or declaration order.

## Native tests and limitations

windows_host_lifetime_tests uses message-only HWNDs, authored events and this
test executable as a finite child. It does not capture source/desktop pixels.

- HWND/thread/fallback routing, coalescing, acknowledgement, expired weak target,
  real PostMessage failure and clipboard/error mailbox ownership.
- Event-backed readiness firing, same-handle rearm, repeated cancel, destruction,
  and proof that the caller still owns the event.
- Callback-only failed-registration cleanup, survivor metadata, repeated disposal,
  completion rebind and callback survival beyond registry/original endpoint.
- Real registered Session lookup/snapshot/identity and explicitly retained actor
  ownership after disposal; registry destruction requests closure of another
  externally retained actor. Registered identities in distinct application owners
  use distinct host identities.

The authored child waits at most 15 seconds for its named event. Exception cleanup
signals that event, requests close and performs bounded fixture-only wait_closed.
Production registry code gains no waits. Readiness fixtures declare the wait
inside the endpoint/window/event lifetimes so assertion unwind drains it first.

Not directly fault-injected: allocation failure between handle and incarnation
insertion, CreateThreadpoolWait resource exhaustion, or every callback scheduling
interleaving. Those retain source-order evidence, not a native fault-injection
claim. The endpoint still has the inherited HWND-clear/post race; this extraction
does not add new delivery guarantees.

Required integration gates remain session identity/wake-router/attention/
observation tests, native_windows_ownership_smoke (both creation failure points,
late completion and delayed Settings retirement), workspace persistence, projects,
Settings and modal scenarios, and renderer recovery/final teardown. Existing
malformed shell paste and stale layout-marker failures must be classified using
their exact rows rather than hidden behind longer deadlines or waived tests.

Local direct Windows cc1 syntax checks are not MSVC/link/native runtime evidence.
Record exact-tree native CI separately before accepting this module boundary.
