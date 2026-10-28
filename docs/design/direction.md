# StormTTY — native experience direction

**Council recommendation: Instrument.** A single carefully machined top band, a real terminal beneath it, and the minimum structure needed to arrange work. The distinctive object is the window itself—not a dashboard inside a window.

Status: design proposal, not implemented or validated on Windows. Reference inspection was read-only. This design pass wrote only council design artifacts, not the user application repository. The accompanying SVGs are clearly labeled static design concepts; terminal excerpts were captured from actual local shell commands, not invented build or test results.

## Brief and process

The user asked for a modern, minimalist native Windows terminal with Linear-level quality, custom window chrome, beautiful movable tabs and resizable panes, keyboard-first interaction, purposeful animation, and strong Ink/TUI/graphics behavior. “AI native” is interpreted here as a terminal that hosts sophisticated agent CLIs exceptionally well, not permission to place unsolicited AI controls over shell output.

I read Anthropic's actual [frontend-design skill](https://github.com/anthropics/skills/blob/main/skills/frontend-design/SKILL.md). Applied process: identify this product's real work; compare compact visual systems; critique them against the brief; revise before making the vector concept; review the rendered concept. Its useful constraint here is deliberate hierarchy and restrained, action-linked motion rather than default decorative styling. This memo and the artifacts are design work only; implementation follows the council decision.

Linear's [March 2026 interface account](https://linear.app/now/behind-the-latest-design-refresh) reinforces the value of compact navigation, predictable action placement, and reduced competing visual weight. We take those principles, not its branding, palette, icons, or page composition. Ghostty's [feature documentation](https://ghostty.org/docs/features) supplies the appropriate terminal benchmark: native arrangement controls, a capable cell renderer, and application protocols. Neither is a license to replace terminal semantics with a web interface.

## Three real directions, then critique

### A. Instrument — recommended

**Palette:** carbon `#17191D`, cast graphite `#22252B`, raised graphite `#30343D`, chalk `#ECEEF2`, mist `#A4ADB9`, cold steel `#A7C4D2`.

**Type:** Segoe UI Variable Text for native chrome; Cascadia Mono for terminal cells. No display typeface. Density comes from optical alignment, not tiny type.

**Layout:** tabs inhabit the custom titlebar; pane headers exist only when necessary; the terminal fills every remaining pixel. Controls and text are left aligned; caption controls retain the Windows edge.

```
[menu] [source review ×] [PowerShell] [+]    drag space    [commands] [— □ ×]
[ghostty-reference                        ][stormtty                    ]
[                                         │                            ]
[              terminal                   │          terminal          ]
[                                         │                            ]
```

**Identity:** an exact relation between tabs, cell edges, pane seams, and window frame. Muted cold steel is used only for focus and manipulation. Terminal ANSI colors supply the visual energy.

**Critique:** initially this was a generic dark-purple developer tool. Revised the accent from periwinkle to desaturated steel; removed persistent footer, colored profile badges, redundant pane chrome in single-pane mode, and any ambient glow. Dark is justified by long terminal sessions and existing TUI palettes—not by “AI” styling. This direction meets the brief with the least obstruction.

### B. Column — credible alternate, not the default

**Palette:** deep slate `#172226`, slate `#253238`, pale slate `#AFBDC2`, white `#EEF2F3`, line `#495B62`, pale cyan `#A9D5D5`.

**Type:** Segoe UI Variable Text; IBM Plex Mono as a user-selectable terminal face, not a bundled dependency at this stage.

**Layout:** a 152 DIP collapsible session rail at left, quiet custom window band above, edge-to-edge terminal at right.

```
[StormTTY                         drag                         — □ ×]
[Source review  ][                                               ]
[PowerShell     ][                   terminal                    ]
[WSL            ][                                               ]
[+              ][                                               ]
```

**Strength:** excellent named-session scanning for users with many environments; genuinely different navigation, not a recolored A.

**Critique:** takes columns from the terminal, biases toward IDE/workspace-manager behavior, and makes the app look more like the dashboards the user rejected. Keep as a later optional layout if real usage demands it. Do not build a left rail by default.

### C. Daylight — light-native alternative

**Palette:** paper white `#FAFBFC`, silver `#EDF0F3`, dark ink `#232B33`, slate `#586572`, divider `#C4CBD2`, deep blue `#315E78`.

**Type:** Segoe UI Variable Text and Cascadia Mono; same mechanical type scale as A.

**Layout:** slim light tab band joined to a light terminal surface, no dark island, no cream-and-serif editorial styling.

```
[menu] [source review ×] [+]                 drag               — □ ×
┌──────────────────────────────────────────────────────────────────┐
│ dark shell text on an even, near-white canvas                     │
└──────────────────────────────────────────────────────────────────┘
```

**Strength:** exceptional daylight clarity; recognizably Windows without nostalgic console styling.

**Critique:** not all terminal apps negotiate light mode correctly; their hard-coded colors can become unreadable. It should be a first-class shipped theme and tested mode, not the first visual north star. Never silently recolor arbitrary truecolor TUI output.

### Decision

Choose **A**, ship **C** as a peer theme, defer **B** pending demonstrated need. The memorable quality is the precision of the native object and its behavior, not a gimmick. This is a terminal design, not a SaaS kit with a monospace center.

## Exact initial token contract

All sizes below are **DIP** unless marked physical pixels. Design values are targets, not claims that platform metrics can be hard-coded. Platform caption bounds, resize borders, system text scale, and accessibility settings win where necessary.

### Color and roles

| Token | Dark value | Use |
|---|---|---|
| `terminal.background` | `#17191D` | Default terminal; user theme may replace |
| `chrome.background` | `#22252B` | Integrated top band and split headers |
| `surface.raised` | `#30343D` | Menu/palette, active hover |
| `text.primary` | `#ECEEF2` | Active labels and default cell foreground |
| `text.secondary` | `#A4ADB9` | Inactive tab text and secondary labels |
| `focus.accent` | `#A7C4D2` | Keyboard outline and active pane marker |
| `line.subtle` | `#464E5C` | Nonessential structural separators |
| `line.interactive` | `#748391` | Idle resizable seam; accessible edge |
| `selection.background` | `#3A4F5B` | Default terminal selection |
| `selection.foreground` | `#ECEEF2` | Explicit selection foreground |
| `find.match` | `#69542F` | Noncurrent match background |
| `find.current` | `#E4C685` | Current match outline; distinguish by outline, not color alone |
| `danger.foreground` | `#F0A6AA` | Destructive text, not permanent close-button red |
| `disabled.foreground` | `#78828E` | Disabled only; never explanatory copy |

Measured sRGB contrast: primary/canvas **15.15:1**; secondary/chrome **6.77:1**; secondary/raised **5.50:1**; focus/chrome **8.39:1**; selected text/selection **7.38:1**; interactive seam/canvas **4.52:1**. `line.subtle` is decorative, not a sufficient interactive boundary; use the stronger seam token or explicit focus outline for operable edges. Validate all component states, including light and high-contrast modes, rather than treating these ratios as whole-app accessibility certification.

ANSI palette belongs to the terminal theme and remains independent from chrome. Do not map red/green or truecolor program output into brand colors. Users can pick matching or deliberately different terminal/chrome themes. Default terminal is opaque; optional translucency never changes selected-text legibility and is disabled by contrast settings.

### Typography

- Chrome: **Segoe UI Variable Text**, 13 DIP regular; selected tab 13 DIP semibold; menu rows 13 DIP; supporting metadata 12 DIP. Native system font fallback, no webfont fetch.
- Settings: 14 DIP body / 20 DIP line; section headings 18 DIP semibold / 24 DIP line. Sentence case, no tracked all-caps labels.
- Terminal: **Cascadia Mono**, initially 14 DIP, measured font metrics determining advance and line height; target line box about 19 DIP, adjustable. Do not force a browser-like line-height that clips combining marks. User font selection is first-class.
- Cascadia Mono deliberately preserves distinct ASCII symbols instead of enabling programming ligatures by default. Cascadia's packaging and OFL information are documented by [Microsoft](https://learn.microsoft.com/en-us/windows/terminal/cascadia-code). Confirm the exact license and subset distributed before shipping. Do not bundle Segoe; use Windows' installed UI face.
- Native UI has a different role from the monospace cell surface. Do not render menus or tab labels in the terminal font.

### Geometry

- Spacing scale: **2, 4, 6, 8, 12, 16, 24, 32**. 4 DIP base; 2/6 reserved for optical corrections.
- Desktop top band **40** high, touch/large-control option **48**. If the chosen native framework requires a platform height, use that metric rather than fighting it. No extra stock titlebar above it.
- Tabs **32** high; preferred width **160**, min **88**, max **220**. 8 DIP top-band side inset around app controls; 8–12 DIP text padding. Label ellipsis at the end; full title in tooltip and accessible name.
- Radius: **6** for tabs and controls, **8** for menus/palette; window radius belongs to Windows/DWM. No independent rounded “cards” around panes.
- Icons **16** viewbox, 1.5 DIP nominal stroke, optically pixel-aligned; desktop hit targets **28–32** min; touch mode **40–44**. Caption geometry uses system metrics (concept approximates 46-wide buttons).
- Pane header **24** high when split; terminal padding **12** horizontal / **10** vertical. Single-pane mode removes the header; padding stays. Compact mode may reduce padding, not glyph size.
- Divider visible line **1 physical pixel**, pointer hit strip **7 DIP**, touch strip **12 DIP**. Hover increases edge contrast; never a thick permanent gutter. Keyboard resize provides a nonpointer equivalent.
- Palette width **560**, max available minus **32**; search field **44** high; rows **36** for one line / **52** with a secondary description; max list **8** rows; 8 DIP outer padding. At narrow widths use window width minus 16 and wrap secondary descriptions.
- No persistent bottom status bar by default. A shell-owned status line stays shell-owned.

### Motion

| Transition | Duration | Curve | Limits |
|---|---:|---|---|
| Button hover/press | 80 ms / 0 ms | linear | Background only; press never delayed |
| Tab reorder settle | 120 ms | cubic-bezier(0.2,0,0,1) | Translate adjacent tabs, not terminal pixels |
| Menu/palette opening | 100 ms | cubic-bezier(0.2,0,0,1) | Opacity + at most 3 DIP travel |
| Menu closing | 60 ms | linear | Focus restoration immediate |
| Split insertion | 140 ms | cubic-bezier(0.2,0,0,1) | Final grid geometry commits immediately; animate boundary/header reveal only, no scaled terminal texture |
| Drop target change | 80 ms | linear | Outline/opaque tint, no spring |
| Resize drag | 0 ms interpolation | direct | Surface size follows pointer; cell grid updates safely |
| Pane/tab focus | 0 ms | immediate | Input target must be unambiguous now |

Reduced motion removes travel and layout transitions; use immediate state changes or brief opacity only. No startup choreography, pulsing output, breathing focus rings, animated gradients, or automatic scrolling chrome. Respect OS caret blink preference independently; never animate terminal contents to “smooth” application output.

## Native window contract

**Custom appearance must not mean counterfeit window behavior.** Paint one coherent band across the full width; never place a conventional titlebar above custom tabs. Preserve caption semantics with platform caption controls wherever they can visually integrate. If native Win32 owner-drawn caption buttons are chosen, that is a real implementation obligation, not three SVG icons.

- Empty band space is draggable; tabs, tab close buttons, new-tab, overflow, command button, and caption buttons are explicit interactive exclusions. Keep at least **48 DIP** draggable gap at normal widths; at tiny widths preserve a small deliberate drag zone while collapsing tabs to an active-tab picker.
- Recompute regions after final layout, tab movement, caption-metric changes, DPI changes, maximize/restore, and RTL changes. No stale rectangles; no draggable overlay atop an interactive control. Microsoft's [titlebar customization guidance](https://learn.microsoft.com/en-us/windows/apps/develop/title-bar) documents physical/logical coordinate conversion and platform-reserved caption bounds.
- Double-click blank band maximizes/restores; right-click opens the Windows system menu; Alt+Space works; Alt+F4 closes; Win+arrow, Win+Z, taskbar restore, Aero Snap, and system move/resize work. Do not intercept reserved OS shortcuts.
- For fully custom Win32 captions, route nonclient behavior correctly; the maximize/restore region must answer `HTMAXBUTTON`. Microsoft's [Snap guidance](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-snap-layout-menu) also warns that oversized minimum widths prevent snapping. Target **330 DIP minimum window width**, with a compact one-pane layout rather than refusing common Snap zones. This target still needs device validation.
- Minimize, maximize/restore, close retain standard order and recognizable glyphs. Close becomes system red on hover/press only. No traffic lights or playful replacements. Maximize swaps to restore in the maximized state.
- Invisible resize border follows system metrics; edges/corners remain usable at every DPI. Maximized windows do not retain a fake rounded outer frame or overscan into taskbar bounds. Fullscreen hides chrome, with a keyboard command and discoverable exit.
- Inactive window: mute chrome emphasis, not terminal content. Keep selection distinguishable. Do not reduce the whole app opacity.

## Tabs, panes, dragging, resizing

**Hierarchy:** window → tabs → split tree → terminal surfaces. A tab contains one split arrangement, not an artificial workspace with hidden extra state. Tab title defaults to active process/CWD information where trustworthy; user rename wins. Terminal-controlled titles are untrusted text, never markup.

**Tabs:** click activates; close only on its explicit button/middle click/configured key. Active tab's brighter surface and lower-edge continuity communicate selection; keyboard focus adds a separate 2 DIP outline. Inactive tabs have no pill-shaped outline until hover. Close button remains visible on active/hover/focus states, with fixed space so labels do not jump. Overflow appears before tabs become unreadably narrow. Keyboard next/previous and a searchable tab picker expose all tabs.

**Reorder/tear-off:** begin drag only beyond system drag threshold; show a narrow insertion indicator. The drag preview is a tab silhouette, not a blurry screenshot of live terminal output. Drag outside the band offers a new window; returning cancels. Do not kill/restart the process to move it. Capture loss, Escape, canceled cross-window drop, and source-window closure must leave the session in one recoverable owner. Across windows, only move when supported by the underlying session ownership model; otherwise disable explicitly rather than fake a transfer.

**Panes:** no rounded card borders. A thin seam and a 24 DIP header identify each leaf when split. Header contains concise process/location label, pane drag handle area, and a small actions menu on hover/focus. The active pane has a short steel marker beside its label and stronger label weight; focused terminal has its native cursor. Never blanket-dim inactive output: both panes may be relevant.

**Split discoverability:** new-tab dropdown and command palette list “Split right” and “Split below.” A pane actions menu exposes the same verbs. When dragging a pane header, reveal four edge drop zones and a center “Swap panes” target only after intentional drag. A preview shows the exact future rectangle; Escape cancels. No always-visible plus signs floating over terminal content. Center is explicitly **swap**, not ambiguous nest/replace.

**Resize:** pointer gets the correct resize cursor over the 7 DIP seam target. Drag updates the split ratio; each leaf sends the correct cell size to its PTY. No fractional-cell drift or stretched glyph texture. Double-click a seam equalizes the neighboring split. Keyboard “Resize pane…” offers directional steps and exact percentage; announce dimensions. Minimum sizes should prefer a useful grid, but at narrow whole-window sizes offer temporary focus/zoom instead of preventing the OS snap. Never silently close hidden panes. Pane zoom keeps the tab identity and shows a compact “Return to split” command.

**Close safety:** when a process is active and risk can be reliably detected, name the process and ask before killing it according to user preference. Do not invent process-idleness certainty. Close confirmation is local to the affected pane/tab/window; typed commands are never replayed on reopen without explicit consent.

## Commands, find, and configuration

- **Command palette:** Ctrl+Shift+P, also a quiet top-band command glyph. Search app actions only; show exact shortcut to the right, scope/current pane context in secondary text, and disabled reasons. No “Ask AI” field. The first empty-state suggestions are useful terminal actions, not onboarding marketing.
- **Key routing:** reserve only documented app shortcuts. Ctrl+C remains interrupt without a selection; copy uses Ctrl+Shift+C by default (optional conventional copy-on-selection behavior must be explicit). Ctrl+V/Ctrl+Shift+V policy configurable, preserve bracketed paste, and do not shadow AltGr or IME composition. Palette/menus temporarily own keys; closing restores exactly the prior terminal focus.
- **Candidate defaults:** Ctrl+Shift+T new tab, Ctrl+Shift+W close pane/tab as scoped, Ctrl+Tab/Ctrl+Shift+Tab cycle tabs, Ctrl+Shift+P palette, Ctrl+Shift+F find, Ctrl+, settings. Split/resize directional bindings must be validated against keyboard layouts and terminal applications; do not prematurely hard-code AltGr-hostile combinations. Every action is remappable and discoverable through the palette.
- **Find:** a compact anchored strip at top-right of the active pane; query, previous/next, count, case/regex toggles in an overflow if narrow. Search actual retained terminal text; preserve original scroll position on cancel. Current match has an outline in addition to fill. Scope is explicit: “This pane”; future all-pane search is a deliberate mode, not silent aggregation. Find should not resize the PTY or cause a TUI redraw.
- **Settings:** a native searchable settings surface opened intentionally, not a permanent left dashboard rail. Organize Appearance, Profiles, Keyboard, Terminal, Window, Accessibility. Show defaults and user overrides; preview fonts/themes using a clearly labeled sample separate from live output. “Open config file” complements the GUI. Invalid values report location and repair guidance; apply safe changes immediately and identify changes requiring a new session. Never reset working config after a parse error.
- Errors speak plainly: “Couldn’t start PowerShell. The executable was not found.” Provide “Choose executable” and “Copy details.” No motivational empty-state slogans, counters, or synthetic activity feeds.

## First-class Ink TUIs and graphics

The TUI is an application in the terminal, not a subordinate panel that StormTTY restyles. Accurate cells, Unicode, input and redraw behavior are the design. Required visual QA includes full-screen Ink lists and forms, alternate-screen applications, rapid progress redraw, wide glyphs, combining marks, emoji, box drawing, hyperlinks, mouse reporting, and split resize under load.

- Do not parse application output into cards. Do not synthesize fake success ticks or agent activity.
- Input echo, selection, scrollback and copy are based on terminal state, not a DOM snapshot.
- Native UI overlays (palette/find) remain separate from the grid; menus do not get clipped into a terminal's scissor rect. Do not partially cover active IME candidate UI.
- Protocols such as synchronized output and kitty graphics are capability/compatibility decisions for engineering. Claim support only after conformance tests. Design images to remain anchored/clipped to their terminal position through scroll, resize and alternate-screen transitions; never float them as UI attachments.
- Character/pixel geometry is reported consistently. An image must not alter row metrics, selection geometry or native caption hit testing.
- Focused-pane cursor, title updates and bell state are the only baseline signals outside terminal output. Bell may add a small static attention marker; no flashing whole window by default. No AI sidebar unless the user explicitly opts into a future feature.

## Input, accessibility and hardware quality

**IME:** use the chosen native stack's composition integration / TSF path, not only raw keydown events. A composition string is a provisional native input overlay at the true caret, visually underlined, committed once; never send preedit fragments as shell commands. Candidate window tracks screen-space caret geometry after scrolling, split resize and mixed-DPI moves. Test Japanese, Chinese and Korean composition, dead keys, AltGr, emoji input and focus transfer while composing. Microsoft's [TSF overview](https://learn.microsoft.com/en-us/windows/desktop/TSF/text-services-framework) is a platform reference; engineering must select the actual stack-specific integration, not assume UWP APIs fit Win32.

**UI Automation:** expose window, tab list, tabs, pane labels, buttons and menus with meaningful names/roles/states. Provide terminal text/ranges/selection and input semantics through an appropriate UIA provider, not just accessible chrome around an opaque bitmap. Screen-reader output must be bounded and user-controllable under a log flood. Tab traversal is for app controls when chrome is active; ordinary Tab in the terminal belongs to the process. Provide a documented way to enter/leave chrome keyboard navigation.

**Contrast and focus:** system contrast themes override app chrome colors using system roles, not a handcrafted approximate theme; selection and focused controls retain distinct system foreground/background pairs. See [Microsoft contrast guidance](https://learn.microsoft.com/en-us/windows/apps/design/accessibility/high-contrast-themes). Offer an explicit terminal accessibility palette override for ANSI output, explaining that arbitrary application truecolor may be changed; do not silently rewrite output. Focus is not color-only: use outline plus shape/weight/position. Keyboard focus must never disappear against a selected tab.

**Motion and size:** honor reduced motion, caret-blink preferences, system text scale, and per-user terminal font size independently. Allow full keyboard operation and touch-size controls. No important tooltip-only action or drag-only workflow.

**Touchpad/mouse:** high-resolution wheel deltas accumulate correctly; application mouse-reporting takes precedence when enabled with a documented override modifier for local selection/scrollback. Scrollback follows finger direction/settings; no CSS-style inertial overscroll or bounce on terminal lines. Ctrl+wheel font zoom is optional and reversible; normal wheel never changes fonts. Pointer selection remains cell-accurate during autoscroll. Right-click behavior is configurable with a safe default context menu, not an unexpected paste.

**DPI:** per-monitor-v2-aware native window; remeasure fonts, cells, chrome and hit regions on DPI transition. Snap visible hairlines to device pixels while keeping hit areas in DIP. Use native event handling such as [WM_DPICHANGED](https://learn.microsoft.com/en-au/windows/win32/hidpi/wm-dpichanged), including suggested placement as appropriate. Test 100/125/150/175/200%, monitors with different scale, negative screen coordinates, maximize, docking, and sleep/wake. Never independently round neighboring pane sizes so total dimensions drift.

## Acceptance gate before calling this beautiful

1. At 1280×800 one pane reads as almost entirely terminal: one custom top band, no redundant toolbar/footer/marketing.
2. Every interactive top-band region works after reorder, maximize, monitor change and RTL changes; blank regions drag; Snap hover and Win+Z work.
3. 330 DIP-wide window is usable through active-tab picker and pane zoom/compact behavior; hidden sessions remain alive and discoverable.
4. Tab/pane moves preserve process identity; dragging out then pressing Escape never loses or duplicates a session.
5. One-pixel seams are visible enough to discover, with accessible hit area and keyboard alternative.
6. No input lag is introduced by transitions. Resizing does not scale a stale framebuffer or flicker alternate-screen content.
7. Ink stress sample, vim, PowerShell, WSL, CJK IME, emoji, graphics, selection and mixed-DPI cases all pass documented visual+behavioral tests.
8. Narrator can discover and operate controls and read terminal content; contrast and reduced-motion settings visibly take effect.
9. Theme customization does not silently damage arbitrary TUI colors; default text and all interactive-state contrast are measured.
10. The final Windows screenshots match the intended hierarchy, not merely this vector mockup. No native-runtime quality is claimed until a native build is inspected.

## Artifacts and provenance

- `design/instrument-concept.svg`: main two-pane native-window concept with actual shell text excerpts, clearly labeled not a screenshot.
- `design/instrument-interactions.svg`: command palette, pane-drop, and chrome hit-region detail; all proposed UI, not a running implementation.
- PNG renders sit alongside SVGs for review. The Linux renderer uses locally available DejaVu stand-ins where Windows fonts are absent; final Windows glyph metrics must be revalidated with Segoe UI Variable/Cascadia Mono.
- Terminal specimens were captured read-only from `/workspace/shared/ghostty-reference` and `/workspace/shared/stormtty` on 2026-10-28. They are evidence of shell output content only, not evidence that StormTTY can run it.

No app implementation, remote task, external publication, or reference-repository write was performed for this design pass.

## Render review and remaining proof

The initial PNG pass exposed an unsupported SVG shadow primitive that hid the background; it was replaced and both final renders inspected. Final checks: no cropped shell text, no collisions in the tab/caption band, readable palette rows, explicit drag targets, and concept labels outside the app bounds. The empty terminal space is intentional: it contains no invented productivity widgets or fake test results. The interaction sheet uses enlarged descriptive rows to make behavior reviewable.

The remaining risk is not the static composition but native execution: Windows font metrics, actual Snap behavior, real UIA/IME support, live terminal resizing, mixed-DPI hit regions and TUI protocol correctness are unproven until the chosen stack is running on Windows. The concept does not certify them. An implementation should preserve the grid first and only then spend polish on subtle transitions.

`design/instrument-tokens.json` records the selected initial values for implementation handoff; it is a design artifact, not application implementation.
