# Metal compositor ownership extraction

## Boundary and lifecycle

`MetalCompositor` owns the weak view and native device/queue/pipelines, layer,
composition target/white texture, serial, and shared callback-generation state.
`MetalFrame` is a noncopyable/nonmovable synchronous paint borrow: concrete
fill/row/image/BGRA operations upload through `MetalTextureResources` and encode
immediately, without a raw device/queue/encoder getter. Renderer retains scene
policy, previous-scene comparison, font/DPI policy and public counters.

Renderer stores the compositor in an early optional slot, emplaced after external
font/texture owners have been constructed but before option validation. This keeps
initial shared-state allocations in their original phase without an extra heap
allocation. Normal destruction and constructor catch first release only the
owner's presentation/error/count references, in that order. Texture/font owners
then unwind before the native graph in the early slot. A failed emplacement
unwinds its partially constructed shared states before the external members.
Callbacks retain their own generation; releasing owner references invokes no
callbacks, native cleanup, completion waits or encoding.

Reconstruction is intentionally not transactional: begin upload batch, replace
count/error/presentation state sequentially, acquire device/queue/pipelines,
upload white, attach layer, then clear texture residency and old target. Failure
before that late clear leaves existing target/cache progress as before. Serial is
not reset. The view's installed layer is not detached on destruction/failure.

## Admission, submission and readback

Order remains: suspended target, scene bounds, GPU failure, occlusion, unchanged
scene, two-in-flight limit, drawable acquisition, serial/upload reset, pre-command
presentation diagnostics, command/encoder allocation. Busy/occluded/unchanged do
not reset accounting. The renderer retains its original autorelease-pool scope.
`nextDrawable` can wait; this is not a claim of nonblocking presentation.

Explicit submit ends encoding, installs generation-only handlers, copies the
composition target into an onscreen drawable, presents, increments in-flight and
commits. Renderer commits history/counters afterward. Frame destruction closes
an active render encoder before releasing ARC references, but never submits,
presents, increments in-flight, waits or rolls back uploads. The ended state is
tracked separately from submission so failure after encoding ends cannot end it
twice.
Ordinary command buffers retain encoded resources through completion.

Readback retains 256-byte row alignment, shared Apple-family versus managed
Intel/AMD storage and managed synchronization. It waits on its own buffer without
joining the render in-flight accounting. Only onscreen readback requires positive
`presentedTime`, using the original three-second observation and error details;
zero-time callbacks and GPU completion alone remain insufficient. Host capture
continues to grant exclusive worker access while UI renderer operations defer.

## Tests and evidence limits

`macos_metal_compositor_tests` builds separate renderer objects with the explicit
`STORMTTY_METAL_COMPOSITOR_TESTS` definition. Production objects have no test
checkpoints, held-command injection, test state or private-access helpers.

Native tests cover:

- offscreen composition, clipping, transparent replacement padding and aligned
  straight-RGBA readback;
- no-target and unattached-view status without serial/upload resets;
- two commands held behind a shared event, third-admission rejection, recreation
  before old completion and generation-independent completion;
- unconditional RAII signaling of every held event on failure/unwind;
- admitted paint abandonment after upload: no submission, no budget rollback,
  followed by successful fresh-frame rendering;
- failure after render encoding ends: no double-end, no submission or rollback,
  followed by successful fresh-frame rendering;
- pre-command/pre-encoder failure progress, post-acquisition constructor unwind
  with subsequent successful rendering, and partial reconstruction state;
- option/acquisition/late-font failure and normal release-boundary traces.

The trace records the owner-reference release and native-owner unwind boundaries;
C++ member order/source review establishes the intervening texture/font unwind.
It does not pretend to instrument every driver release. Existing CPU/font/texture,
renderer and PNG tests remain required, including native retained-texture tests.

Source review and whitespace checks are not native execution. A supported Apple
SDK plus the project's matching LLVM/libc++ toolchain and actual Metal device are
required to run these gates. These offscreen tests do not qualify real onscreen
presentation, physical UI, IME/accessibility, Intel/AMD hardware, or performance.
Held footer/presentation investigations and paused features remain unchanged.

## Explicit exceptional-cleanup correction

This cleanup is a separate robustness change, not pure ownership extraction.
Native diagnostic run `41089324910` on macOS 15 passed composition/readback and
held-generation fixtures, then aborted immediately after the abandonment
fixture's upload/throw marker: Metal asserted `Command encoder released without
endEncoding`, before the catch marker. The original pre-extraction renderer also
ended its local encoder only on the normal path; CPU exceptions could bypass it.

The correction ends only a still-active render encoder during frame destruction
or constructor catch after encoder acquisition (where no frame destructor runs).
It preserves advanced serials and consumed upload bytes, leaves scene history and
public counters untouched, and does not commit abandoned commands. All original
abandonment assertions and real next-frame recovery remain mandatory; a separate
post-end failure checkpoint covers double-end prevention. At the correction's
source-review stage, native recovery was still pending; the later evidence below
supersedes that pending status without treating the diagnostic failure as success.

## Later native recovery evidence

Run `41091838736`, macOS job `124190006594`, built remote source
`df357d6dc86eb53e07367b4db9debd07543c1707` at merge checkout
`c6fbae8ca13304c9d38c0de4d4a3634d9fc72815`, tree
`5d0944522dc7d39ab661e1cf694ed61410fbbb7b`. The actual macOS 15 runner used
Clang/Objective-C++ 20.1.8 with the official Xcode 16.4 macOS 15.5 SDK.
All 57 blocking native baseline tests passed, including fonts, CPU row parity,
texture residency, renderer and PNG integration.

Baseline artifact `12994902773` contains all eight compositor fixture passes:
composition/readback, held generations, active-frame abandonment, post-end failure,
post-acquisition constructor failure, failure ordering, renderer unwind and
unattached-view status. Abandonment reached its catch and drained its pool before
passing the unchanged no-submit/no-rollback and real next-frame recovery checks.
The two new failure fixtures also passed subsequent rendering/readback.

This qualifies the tested native/offscreen boundary, not onscreen capture,
physical presentation, GUI interaction or Intel/AMD hardware. The wider workflow's
Windows tail is separate; this receipt makes no full-workflow-green claim.
Later integration preserves these macOS source/target bytes while adding the
independently reviewed navigator changes; it is not whole-tree identity.

### Wider workflow result

Run `41091838736` ultimately failed its Windows job; Linux and macOS succeeded.
The Windows bundled-Zsh layout case reported that a controlled shell exited
without its expected marker. Bundled artifact `12996036152` records all five
actors finished with exit code 0, but actor 5 lacked the marker; the same log
includes `history: is a directory`. That diagnostic is recorded, not established
as the cause. The application reached its final CRT-return log. The other four
bundled smoke logs, including Settings, passed. These application logs do not
establish runner-level reap results.

The separate repeated Settings step also failed. Its exact cases, output and
runner reap metadata remain unclassified because the job-log connector repeatedly
failed. The available final archive exceeds the supported transfer cap; it was
not downloaded, and no repeat result is inferred from the bundled application
logs. This macOS slice changes no Windows production source. Its native macOS
qualification remains valid for the tested boundary, but the complete workflow
is not green and repeated Windows teardown is not claimed as verified.
