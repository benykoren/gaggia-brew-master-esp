# Web UI Redesign — Design Spec

**Status:** approved by user (chat), pending written spec review.
**Scope:** `src/web.cpp`'s embedded `index_html` (~2,400 lines of
HTML/CSS/JS, compiled directly into ESP32 firmware — no filesystem, no
build step, no separate asset files).

## 1. Why this exists

Session context: while getting the dashboard usable on an Android 4.4
tablet (see `AGENTS.md`'s 2026-09-15 entries — the `inset` CSS shorthand
bug, then a landscape-only 2-column layout for the Now tab, then two
`nth-of-type` bugs introduced by that same landscape work), the user asked
for a full "UI/UX + principal engineer" refactor. The first draft of that
ask was a generic React/TypeScript/Tailwind/Framer Motion prompt template
that does not match this codebase at all — see Section 2. After
discussion, the user agreed to stay on the current architecture and
explicitly asked for "the best choice for responsiveness, speed, beauty,
and more" — answered as: stay vanilla/embedded, because a framework
bundle would be *slower* to load and would break the Android 4.4 tablet
this project just fought to support.

## 2. Explicitly rejected: a React/TypeScript/Tailwind/Framer Motion stack

Not in scope, and should not be silently reintroduced later without a new
conversation:

- No build pipeline exists (no `npm`, no bundler, no `tsc`). Adding one
  means a new toolchain dependency for a single-developer hobby project's
  dashboard.
- No filesystem partition serves static assets today — everything is one
  C++ string constant in flash. A framework build output would need a new
  LittleFS partition and a second deploy artifact (filesystem image,
  separate from firmware.bin) — real new infrastructure, not a refactor.
- A modern framework bundle (React, Framer Motion) targets evergreen
  browsers. This directly conflicts with the Android 4.4 tablet
  compatibility work done this same session (see `AGENTS.md`) — shipping
  one would silently break that tablet again.
- CSS custom properties already provide the "design tokens" concept the
  template asked for; Tailwind would be a redundant second token system.

## 3. Hard constraints (must not change)

- **Delivery**: stays a single HTML/CSS/JS string embedded in
  `src/web.cpp`, served as-is. No new partitions, no external requests for
  JS/CSS/fonts.
- **Browser compatibility**: must keep working on the Android 4.4 tablet's
  Firefox build (confirmed this session to support `fetch()`, ES6
  `const`/`let`/arrow functions/template literals, and CSS custom
  properties — but NOT the `inset` CSS shorthand, which was already fixed).
  Any new CSS/JS feature added by this redesign must be checked against
  that same rough baseline (~2016-era engine) before use. When in doubt,
  prefer the older/safer syntax — this file doesn't get a real test run
  before it reaches production; the tablet is effectively the strictest
  reviewer.
- **`/status` JSON contract**: field names/shape are consumed by MQTT/Home
  Assistant auto-discovery (`src/mqtt.cpp`) and are the reference contract
  for the (separately specced, not-yet-built) Android companion app. This
  redesign only *reads* `/status`/`/profiles`/etc. — it must not rename or
  restructure any JSON field.
- **Existing endpoints**: `/update`, `/profiles`, `/settings_export`,
  `/firmware`, `/wifi_reset`, etc. — behavior unchanged; only the pages
  reading/writing them change.
- **Testability discipline**: given I (the agent) cannot open this page in
  a real browser or the actual tablet, every change ships as a small,
  individually verifiable step — matching how tonight's fixes were done
  (one commit, one OTA flash, one live check, before the next change) —
  not one giant diff landed at once.

## 4. Section A — Tab reorganization: move Auto-Tune

**Current**: the Auto-Tune button/status (`<div class="card">` containing
`#btn_autotune`/`#autotune_status`) is the last card in the **Now** tab
(`src/web.cpp`, inside `<main class="view" data-view="now">`).

**Change**: move that card to the **Tune** tab, placed after the Brew
card and before the Steam card (it tunes Brew gains first; Steam gets its
own separate autotune-eligible run using the same button/state machine —
placement right after Brew reflects that Brew is tuned first in normal
use, per `AGENTS.md` §4's autotune section).

**Rationale**: Auto-Tune is a calibration action run occasionally, not a
live-glance stat — it doesn't belong beside the temperature gauge/shot
controls. It directly populates the Brew/Steam PID gain fields on the
Tune tab, so it belongs next to them. This also matches the landscape
layout's existing decision to hide Auto-Tune from the compact Now view
(`src/web.cpp`'s landscape media query) — moving it out of Now entirely
makes that hidden-in-landscape special case unnecessary; **that landscape
override rule (`.view[data-view="now"] > .card:nth-of-type(4) { display:
none; }`) gets deleted** as part of this change, since Now will no longer
have a 4th card at all (hero, shot, stats — 3 cards).

**No JS changes needed beyond this**: `startAutotune()`/`stopAutotune()`
and `#btn_autotune`/`#autotune_status` element IDs are unchanged, just
relocated in the DOM. `showTab()`'s tab-switching logic already works
generically per-`data-view` and needs no changes.

## 5. Section B — Design tokens & typography scale

**Add to the existing `:root` block** (`src/web.cpp`, currently holding
color/radius/shadow/spacing tokens only):

```css
--text-xs: 11px;   --text-xs-lh: 1.4;
--text-sm: 13px;   --text-sm-lh: 1.45;
--text-base: 15px; --text-base-lh: 1.5;
--text-lg: 18px;   --text-lg-lh: 1.4;
--text-xl: 22px;   --text-xl-lh: 1.3;
--text-2xl: 28px;  --text-2xl-lh: 1.2;
```

The gauge's own large numerals stay on their existing `clamp()`-based
sizing (Section headers below don't touch `.gauge-value`/`.gauge-unit`) —
this scale is for everything else: card titles, labels, hints, table
text, stat values.

**Sweep and replace ad-hoc font-sizes** currently scattered as raw pixel
values (found via grep: `22px`, `19px`, `14px`, `11px`, `10.5px`, and
others across `.tab-section-title`, `.hint`, `.metric-row`, `.tab-icon`,
`.stat-label`, `.history th/td`, etc.) with the nearest token above, each
paired with its matching `-lh` line-height variable rather than a bare
`line-height` value.

**Recurring inline colors**: grep for repeated literal `rgba(...)` values
outside the token system (e.g. the `.gauge-fill.heating/.ready/.over`
`drop-shadow` colors, `.tabbar` gradient stops) — pull ones that repeat
more than once into new tokens (e.g. `--shadow-glow-steam`,
`--shadow-glow-green`, `--shadow-glow-red`) alongside the existing
`--shadow-sm`/`--shadow-md`. One-off colors used exactly once stay
inline — this is about removing *duplication*, not eliminating every
literal value.

## 6. Section C — Accessibility & semantic structure

- **Tab bar** (`<nav class="tabbar">`): add `role="tablist"` on the
  `<nav>`, `role="tab"` + `aria-selected="true"/"false"` on each
  `<button class="tab">`, and `aria-controls="view-<name>"` pointing at
  each `<main class="view" data-view="...">` (give each `<main>` a
  matching `id="view-now"` etc.). `showTab()` gets one small addition:
  when it sets `.active`, also set `aria-selected` to match, and toggle
  `tabIndex` (0 for the active tab, -1 for others) per standard ARIA tab
  pattern.
- **Icon-only controls**: the sleep-banner dismiss button (`&times;`,
  already has `aria-label="Dismiss"` — good, no change) and the Wake Up
  button are fine (has visible text). Check `.btn-chip-sm` and any other
  symbol-only button for a missing `aria-label` — grep for `&times;`,
  `&#`, and buttons whose only content is an HTML entity.
- **Focus visibility**: add one rule —
  ```css
  :focus-visible { outline: 2px solid var(--copper-light); outline-offset: 2px; }
  ```
  This is a real, if narrow, browser-compatibility question:
  `:focus-visible` shipped later than `:focus` (Chrome 86, Firefox 85,
  both 2020-2021) — **check whether it works on the Android 4.4 tablet's
  Firefox build before relying on it alone.** If it doesn't apply there,
  add a plain `:focus { outline: 2px solid var(--copper-light); }`
  fallback first, then a `:focus-visible` rule after it that removes the
  outline for mouse/touch clicks on browsers that do support the
  distinction (`:focus:not(:focus-visible) { outline: none; }`) — an
  unsupported browser simply never matches the second rule and keeps the
  plain `:focus` outline, which is the safe direction to fail in.
- **Landmarks**: `<header class="topbar">` should be a real `<header>`
  (already is). Each `<main class="view">` is already a real `<main>` —
  but HTML only allows one visible `<main>` at a time in the accessibility
  tree; since 3 of the 4 are `hidden`, this is already correct as-is (the
  `hidden` attribute removes them from the accessibility tree too, so
  there's no "multiple mains" conflict) — no change needed here, just
  confirming it's already right.

## 7. Section D — States & feedback

**Form save confirmation**: every settings form currently does a plain
`<form action="/update" method="GET">` full-page reload-via-navigation
(GET request, page reloads with query string, `/status` poll then
re-populates fields). Change to: intercept `submit`, `fetch()` the same
URL+query in the background, and on success show a small inline "Saved"
confirmation (a `<span>` next to the button, fades in/out via CSS
transition, not a `Framer-Motion`-style animation library) instead of a
full navigation. This is a real behavior change (no more full page
reload on save) — call this out explicitly when implementing, since a few
forms may rely on the reload incidentally (e.g. MQTT save, which reboots
the controller — that one should probably keep its current
reload-after-delay behavior, or show a distinct "Restarting..." message
instead of "Saved", since the device actually goes away for a few
seconds).

**Loading state**: `#temp`/`#target`/`#output`/etc. currently show `--`
until the first `/status` response. Replace the very first render (before
any successful fetch) with a subtle pulsing skeleton — a CSS
`background: linear-gradient(...); animation: pulse 1.5s ease-in-out
infinite;` block matching each stat's shape — swapped for the real value
the instant the first `/status` resolves. This is cosmetic-only (doesn't
change any data flow) and should respect `prefers-reduced-motion` (skip
the pulse animation, just show a static muted block instead).

**Error/empty states already partially exist** (`#shot_history_empty`,
`#fault_banner`) — audit these for consistent visual treatment against
the new type scale/tokens, but no new error states are being invented
here; this section is about consistency, not new functionality.

## 8. Section E — New feature: target overlay on live charts

Extend `drawSparkline(data)` (temp chart) and `drawPressureSparkline(data)`
(pressure chart), both in `src/web.cpp`'s inline `<script>`, to accept a
second parameter — the current target value (a plain number, not an
array) — and draw it as a horizontal dashed line across the chart at that
value's y-position, reusing the same `ctx.setLineDash([3, 2])` pattern
`drawSparkline` already uses for its phase-marker vertical lines (see
`src/web.cpp` around line 876).

- **Temp chart**: target = whichever of `brew_target`/`steam_target` is
  active for the current `opmode` (already available in the polled
  `/status` response — no new endpoint or field needed).
- **Pressure chart**: target = the active profile's ramp/decline bar
  target while `shot_phase` indicates a pressure stage is active (fields
  already in `/status`: `press_ramp_bar`, `press_decline_bar`,
  `shot_phase`); no target line when no pressure profile is active
  (`press_enabled: false`) — don't draw a misleading flat line at 0.
- Call sites (`drawSparkline(tempHistoryArray)` /
  `drawPressureSparkline(pressureHistoryArray)`, wherever the existing
  `/status` poll handler calls them) pass the extra argument; the
  function's own `min`/`max` autoscale calculation (`Math.min.apply` /
  `Math.max.apply` over `data`) must also factor in the target value so
  the dashed line is never drawn outside the visible y-range — e.g.
  `var min = Math.min.apply(null, data.concat([target ?? data[0]]))` for
  vanilla-ES5-safe code, `??` isn't ES5 — use
  `target === null || target === undefined ? data[0] : target` instead,
  consistent with this file's existing ES5-only baseline.

## 9. Rollout / testing plan

Each section above ships as its **own commit**, in this order (each
lower-risk than the next, so an early problem doesn't block later,
unrelated work):

1. Section A (tab move) — pure DOM relocation, easiest to visually verify.
2. Section B (tokens/type scale) — CSS-only, no behavior change.
3. Section C (accessibility) — mostly additive attributes/CSS; the
   `:focus-visible` fallback needs a real check on the Android 4.4 tablet.
4. Section D (states/feedback) — the only section with a real behavior
   change (form submission via `fetch` instead of full navigation) —
   gets the most scrutiny/testing.
5. Section E (chart overlays) — additive drawing logic, isolated to two
   functions.

After each commit: build via PlatformIO, OTA-flash
(`http://192.168.1.73/update_fw`), and the user verifies on both a modern
browser and the Android 4.4 tablet before the next section starts —
same discipline as tonight's `inset`/landscape-layout fixes.

## 10. Explicitly out of scope

- No React/TypeScript/Tailwind/Framer Motion (Section 2).
- No LittleFS/filesystem-served assets.
- No changes to `/status` JSON shape, MQTT discovery, or any endpoint
  contract.
- No wholesale re-grouping of tabs beyond the single Auto-Tune move
  (Section A) — the user had no other specific complaints about tab
  grouping.
- No new hardware-facing features (this is a UI-layer redesign only;
  Milestone B/C pressure-plumbing work in `HARDWARE_ROADMAP.md` is
  unrelated and unaffected).
