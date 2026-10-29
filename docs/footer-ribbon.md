# Footer ribbon

The native Windows footer uses a portable, closed data-binding registry in
`ribbon.hpp`. Settings → Ribbon previews visibility and left-to-right order;
Save changes persists through the existing atomic settings service, and Cancel
restores the running configuration. Toggle and move buttons are keyboard
reachable. Appearance → Clock and Ribbon → Clock share `show_clock` for old
config compatibility. A hidden clock binding also suppresses the clock.

`[ribbon] order` is exactly the six registered IDs, with no duplicates. `hidden`
is an optional subset. Unknown keys, arbitrary bindings and malformed updates
fail full config validation; the existing configuration remains active. This is
a declarative foundation, not an extension runtime or package installer. It
accepts no shell text, callbacks, scripts, remote assets, paths or custom icons.
Host data stays host-owned and workspace-qualified. Agent availability/attention
enters geometry through `RibbonHostData`; it never starts or restores a task.

Width admission uses host-defined priorities: agent, background, session status,
directory, Git, clock. After admission, user order determines placement. Hidden
and omitted items have empty rectangles shared by drawing, tooltips and hit tests.
Agent attention and active session errors override hiding. Background restore remains reachable through
Commands (`Ctrl+Shift+P` → Restore background terminal), even when its ribbon
item is hidden or omitted. Agent approval sheets remain owned by the agent host,
not by customization. macOS can reuse registry/config/geometry; native macOS
footer wiring is not claimed by this change.

## Verification and native capture plan

Portable tests cover strict parsing, persistence roundtrip, settings keyboard
controls and cancellation, essential attention, hidden hitboxes and overlap/bounds
for widths 0–1499. Linux tests are not physical native verification.

On Windows, capture these states at 100% and 150% DPI:

1. Settings → Ribbon, toggle Git off and move Directory below Background; observe
   immediate footer preview. Cancel and confirm exact original order/visibility.
2. Repeat and Save, close/reopen the app, and confirm persistence.
3. Park two terminals, hide Background, then restore one using the command palette.
   Click the old chip location and confirm no hidden restore target remains.
4. Resize through 1200, 640, 400 and minimum width; inspect clipping and hit targets.
5. Hide Clock with the old appearance control; enable it through Ribbon and check
   both controls agree and the displayed minute updates, including narrow widths.
6. With a clearly labelled fake agent fixture, show attention while Agent is hidden;
   verify visible attention, keyboard access, and no provider execution on restore.
7. Exercise Tab/Shift+Tab, Enter/Space, arrow navigation, reduced motion and high
   contrast; inspect screenshots for readable labels and focus outlines.

No live Codex process, authentication or task is needed or authorized by this test.
