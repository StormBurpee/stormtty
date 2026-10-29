# Native text-overlay input ownership

Command palette and rename fields take keyboard and pointer ownership
immediately, not after their 100ms fade. Opening releases any terminal-captured
button while that old owner is still eligible, then cancels divider/pane/tab/
selection drags and resets the UTF-16/IME bridge. Palette motion, release and
wheel paths cannot reach a hidden terminal or mutate its layout. Handled key
presses retain their consumed-key ownership through the matching key release,
even when Enter or Escape closes the overlay.

The native `--smoke-layout` fixture enables real 1003/1006 mouse reporting in its
owned native test child (`terminal_search_tests --native-mouse-child`) with explicit
console VT input/output modes, then waits for an acknowledged terminal frame.
A cmd prompt escape alone does not establish this contract through ConPTY.
The child consumes mouse reports without echo, exits on the host sentinel, and
restores the previous console modes; completion requires its actual output row. It tests captured
button transfer, WM_MOUSEMOVE/WM_LBUTTONUP isolation, Escape and rename Enter
release handling, and opening while a real divider owns HWND capture. The
original split ratio must remain unchanged. Mouse and key ingress counters
are measured at frontend Session submission boundaries, not total transport
bytes (which include legitimate terminal protocol replies).

Native execution is required to qualify this test; cross-compilation alone
is not runtime or physical keyboard/IME qualification. No animation timings,
terminal frame interpolation or input delay are introduced.
