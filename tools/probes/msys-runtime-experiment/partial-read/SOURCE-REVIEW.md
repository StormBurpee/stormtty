# Positive partial read at signal exit — isolated proposal

This proposal responds only to run40924737063's eight directly observed reads:
copy1, resize dispatch, SIGWINCH handler, input_winch, return-1/EINTR. See
`../passive-path/RUNTIME-REVIEW-40924737063.md`. Settings leading-ESC attribution,
mouse dispatch/master rewrite coverage, and production qualification remain
unproven. No shipped pin, default, prior refined patch, or CI workflow changes.

## Exact scope

`0007-preserve-copied-on-signal.patch` applies **alone to stock** prepared console
source from the exact official archive. `identities.json` locks archive, input,
output and patch SHA256. It adds one positive-count return at `sig_exit`, before
`set_sig_errno(EINTR)`. It does not change signal dispatch, waiting, retirement,
copying, console input modes, VMIN, SA_RESTART, cancellation, or the `err` label.
It never resubmits bytes. Refined-prefix is neither modified nor implicitly
combined with this patch. Any combined candidate needs a separate review.

## Every signal-exit ingress and ownership

There are exactly two `goto sig_exit` sites in `fhandler_console::read`:

1. `cygwait(get_handle(), timeout)` returns WAIT_SIGNALED. This wait precedes
   `acquire_input_mutex`; the reader does not hold input_mutex at the jump.
   If copied_chars is positive, the preceding copy iteration already released
   input_mutex and called fix_input_mode_if_necessary before `goto read_more`.
   Returning the existing positive count requires no additional release/fix.
2. `process_input_message` returns input_signalled or input_winch. That shared
   arm releases input_mutex, fixes input mode, then checks the existing signal's
   SA_RESTART flag. Restart still continues unchanged. Only non-restarting flow
   reaches sig_exit, with no input mutex held. The signal may already have run;
   this patch does not defer, repeat, or suppress it.

The function's ordinary `buflen=copied_chars` return precedes `err` and returns;
`err` also returns, so neither falls through into sig_exit. WAIT_CANCELED retains
pthread cancellation. Background checks and unrelated input/handle errors retain
their exact old results, including existing post-copy error behavior; they are
outside this proposal. `push_process_state` retains the same stack lifetime on
return. No new borrowed pointers, native handles, locks or mutation are added.

## Accounting and errno

copied_chars starts at0, advances only as bytes are written to this call's caller
buffer, and survives the existing read_more loop. Existing copy bounds limit it
to requested buflen. A positive short result reports that already-consumed prefix
once; the caller can then read the untouched remainder. Zero-copy interruption
still calls set_sig_errno(EINTR) and returns(size_t)-1 exactly as before.
Positive return does not clear errno or set it to EINTR; successful reads do not
need errno=0, and the existing success path also does not clear it. No attempt is
made to reverse signal-handler errno changes or to restore old error state.

## Limits and verification

Portable tests model both signal ingresses, zero/positive copies, errno,
restart, cancellation, unrelated errors, existing timeout/nonblocking EAGAIN
exits, canonical/VMIN/capacity read_more boundaries and exact prefix partition. The optional
pinned-source gate actually applies the one-hunk patch and checks the full output
hash plus byte-for-byte unchanged code outside its insertion. These are not
native execution or a proof of all signal/concurrent reader behavior. No new
full build is performed in the disk-constrained executor. Independent source
review and the qualification recipe are required before native publication.

## Independent review

The separate Settings lifecycle reviewer independently applied the locked patch
and confirmed the two signal ingresses' lock/input-mode reasoning. The reviewer
also inspected `exceptions.cc` set_sig_errno (updates errno_addr and saved_errno),
`syscalls.cc` read (returns the fhandler-updated length), and `cygwait` default
handler/restart flow. The new positive branch intentionally sets/resets neither
errno field; it does not request wrapper retry. SA_RESTART behavior remains
outside the change, even where broader POSIX semantics might warrant discussion.
Neither the existing background-check early return nor the generic err label is
repaired: this proposal must not be described as fixing all partial-read losses.

Independent reviewer recorded these prepared-source SHA256 identities for that
supporting wrapper/errno/restart audit (not new native execution):

- exceptions.cc: `5ed7f5bc5eb622bed929de96d22daf5705d60b3f2ecfe181f197cd88ee18c063`
- syscalls.cc: `c1300a542b13337e55c2a25cd5757ec7a8cc0e09d4ae16e5d6eddb65cb274e3f`
- cygwait.cc: `921b698deb9c2fa02f5b2894e78d570e1bd35c0245458c7d9bb561ee7cd5b95f`
