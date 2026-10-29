# Completion seed lifecycle foundation — inactive review branch

The measured diagnostic benefit is recorded in `completion-seed-native-results.md`.
This follow-on deliberately does **not** activate adoption in NativeWindow or any
application profile. It adds a controller-lifetime seam, a private initialization
lease, a no-dump fallback, and a build-anchor format for review. The optional
`managed_shell_initialization_probe` is excluded from normal builds.

## Implemented lifecycle

* A `WindowsProcessInitialization` object may be assigned to one launch's
  `ProcessOptions`. Without one, transport behavior is unchanged. Preparation,
  acknowledgement and finalization all run on the existing controller worker;
  no extra polling thread or UI join is introduced.
* The managed implementation revalidates the state owner/DACL/tree, then tries a
  named Windows mutex derived from the native volume/file identity of that state.
  The global namespace covers separate Windows login sessions sharing the same
  state. Its protected security descriptor and any existing object's owner/DACL
  are checked; a hostile precreated object cannot authorize cache use.
* Mutex ownership belongs to the controller thread. The same controller releases
  it after a private native ready-event or after child/job retirement, including
  launch failure. PTY text and shell-context OSC are never release authority.
* The loader registers a one-shot native acknowledgement as its final line-init
  hook, after initial compinit, user configuration, tools and widgets. This follows
  initial precmd processing. An app-owned sibling helper signals the random
  protected event; an error leaves the lease held until child exit. A hung shell
  does not cause a timer to release a possibly active initialization.
* Contention or untrusted mutex creation falls back to **secure `compinit -i -D`**,
  not ordinary cached compinit. It does not load/write the shared dump or `.zwc`.
  The same validated state, user configuration, history and private custom
  completion directory remain available; this is not an ephemeral-HOME fallback.
* A crashed/abandoned mutex owner is not readiness. A private uncertainty marker
  forces no-dump mode on subsequent launches rather than automatically trusting
  potentially interrupted executable state. Product recovery of that marker is
  intentionally not automatic and must be reviewed before activation.
* Seeding is attempted only under an owned lease and only if there is no user
  config/compiled config, no existing `zcompdump-*`, and no private custom
  completion. Warm/existing custom state skips seed verification/installation.
  The callback currently supplies the seed implementation; no application caller
  is wired up. Existing state is never overwritten to make it seed-eligible.
* Reserved initialization environment variables are cleared by the managed home
  launch planner before an opt-in controller supplies its own values. The loader
  consumes them without exporting them into unrelated descendants.

## Trust foundation

`prepare-completion-seed.py --trust-header NEW_HEADER` emits the generated seed
identity's SHA-256 as a native compile-time constant. `adopt_build_trusted` requires
that anchor and hashes the **same identity bytes** it parses. It is not acceptable
to get this anchor from an adjacent sidecar, environment or user config. Seed text
is now read from the same locked handle whose content hash matched, avoiding a
second path lookup after verification.

The anchored path now also validates native runtime owner/DACL policy using one
shared security context per traversal. Public read grants are permitted, but
untrusted mutation grants are rejected. Runtime ancestors are checked before
walking; each actual directory/file is opened, checked and retained before the
iterator descends. Ordered-fpath reconstruction runs only after that protected
content traversal. The unanchored original diagnostic path is unchanged and must
not be used as the production entry point.

This implementation is not yet native-qualified. The optional native probe adds
an authored ACL fixture: everyone-read is accepted, everyone-write is rejected.
Measure the additional ACL traversal cost; the earlier 0.50 s verification figure
does not cover it. Same-user administrative writers remain outside the transaction,
as with existing trusted user shell configuration.

## Tests and qualification boundary

The pinned-module POSIX test creates an existing text dump and newer compiled
sibling containing an authored execution sentinel. No-dump startup neither sources
nor changes either cache, still registers private custom completions, and preserves
user fpath/HISTSIZE/config/history. This is semantic evidence, not Windows ACL or
performance qualification.

The new optional native probe checks cross-thread and separate-process contention, owner ready-event,
custom-config seed suppression, real child-exit release through the transport and
abandoned-owner no-dump persistence, plus public-read/untrusted-write ACL policy. It is cross-compiled locally but **has not run
on Windows**. Before any application integration, execute those native checks, and qualify
normal managed children and first-ZLE signalling on both providers, not just direct
controller API calls. All participating managed launches must use the planner;
mixing opted-in seeding with older ordinary launch paths is unsupported.

Arbitrary same-user scripts can deliberately reopen/reinitialize any user-owned
cache; this feature does not sandbox user configuration or later commands. Existing
user code that explicitly chooses its own compinit/dump lifecycle remains outside
the app-owned initialization transaction. Do not describe this as protection against
arbitrary same-user writers or as a completed production rollout.

## Remaining integration gates

1. Native-qualify the new owner/DACL verification and protected traversal, including
   public-read, denied mutation, ancestor links and runtime relocation.
2. Generate the seed and trust header from the exact staged native runtime before
   native app compilation; package the seed alongside that runtime. Never add a
   runtime Python dependency or a user setup command.
3. Attach the planner to the single managed-session factory behind an OFF-by-default
   review option, with ordinary launches and contenders covered consistently.
   `window.cpp` is intentionally untouched pending ownership/integration review.
4. Define uncertainty-marker recovery and qualify normal exits, early close,
   startup failure, crash, same-process tabs and cross-process launches on Windows.
5. Re-measure complete fresh costs, including ACL verification, native acknowledgement
   and contention fallback. Do not transfer the prior prototype timing claim to
   this unqualified lifecycle implementation.
