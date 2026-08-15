# Hardware Roadmap — Buy Lists & Wiring Plans

Actionable companion to `AGENTS.md` §8 ("Roadmap / future work"): for each
remaining hardware item, **what to buy, how to wire it, and what to watch
out for.** `AGENTS.md` remains the single source of truth for the *why*
(competitive research, coffee science, prior decisions) and for the running
change log (§10). This file is the *what/how*, and is kept current as each
item is planned in more detail or actually built.

Items 1-3 (shot timer, shot history, descale reminder) are already shipped —
pure software, no hardware. See `AGENTS.md` §8 and the README roadmap.
Everything below is **not yet built**.

## Contents

- [Summary](#summary)
- [Tools worth having first](#tools-worth-having-first)
- [Item 4 — Milk temperature probe](#item-4--milk-temperature-probe)
- [Item 5 — Water tank level sensor](#item-5--water-tank-level-sensor)
- [Item 6 — Pressure transducer + live pressure graph](#item-6--real-time-pressure-transducer--live-pressure-graph)
- [Item 7 — Automatic shot start/stop detection](#item-7--automatic-shot-startstop-detection-sense-only)
- [Item 8 — Bluetooth smart scale](#item-8--bluetooth-smart-scale--brew-by-weight-auto-stop)
- [Item 9a — Pump on/off control](#item-9a--pump-onoff-control-control-the-button)
- [Item 9b — Phase-control dimmer](#item-9b--phase-control-dimmer-to-a-pressure-target-eg-9-bar)

---

## Summary

| Item | Adds | Depends on | Needed for your current goal? | Est. cost |
|---|---|---|---|---|
| 4 | Milk temperature probe | — | No | $2-3 |
| 5 | Water tank level sensor | — | No | $2-5 |
| 6 | Live pressure reading | — | Only if pursuing item 9b later | $15-30 |
| 7 | Automatic shot detection | — | **No — see decision below** | $3-5 |
| 8 | BLE smart scale (36g auto-stop) | — | Yes | scale cost |
| 9a | Pump on/off relay (25s / weight auto-stop) | — | **Yes — this is the core item** | $9-13 |
| 9b | Phase-control pressure profiling | Item 6 | No | $12-20+ |

Current stated goal — stop the pump at **25 seconds**, or later at **36
grams** once a BLE scale is in hand — needs only **items 9a and 8**. Items
4, 5, 6, 7, and 9b are independent, optional, and can be picked up in any
order later.

---

## Tools worth having first

Two cheap, general-purpose items come up across multiple entries below.
Buying them once avoids a separate workaround per item — neither is
strictly required, and each item below also notes the no-extra-tool
alternative where one exists.

| Tool | Cost | Unblocks |
|---|---|---|
| Basic resistor assortment kit | ~$8 | Item 4's pull-up; simplifies items 6 and 7 (otherwise each needs a pre-conditioned sensor module instead of the bare part) |
| Non-contact AC voltage tester (pen-style proximity tester, **not** a multimeter — no probes touch conductors) | ~$5-10 | Item 9a/9b's one open wiring question: identifying the switched-Line wire at the pump's terminals |

This project has so far deliberately avoided buying resistors or a
multimeter (see `AGENTS.md` §7) — that's a fine choice for a one-off
workaround, but several items below independently want the same two tools.

---

## Item 4 — Milk temperature probe

**Status:** not started. **Depends on:** nothing.

**What it's for:** real-time milk temperature during steaming. Dairy
science puts the sweet spot at ~55-65°C, with ~70°C as a hard
scald/foam-collapse ceiling. Neither Gaggiuino nor GaggiMate address this at
all — see `AGENTS.md` §7 competitive research.

**Buy:**

| Part | Spec | Cost |
|---|---|---|
| Waterproof DS18B20 probe | Digital, OneWire, stainless sheath | ~$2-3 |
| Pull-up resistor | 4.7kΩ | from the resistor kit above |

**Wire:** one free GPIO → DS18B20 data pin, with the 4.7kΩ resistor between
data and 3V3 (standard OneWire pull-up). DS18B20 power/GND → 3V3/GND.

**No-resistor alternative:** some DS18B20 breakout boards ship with the
pull-up already on board — check before assuming a loose resistor is needed.

**Firmware:** OneWire + DallasTemperature libraries (both common,
well-trodden on ESP32). Simple polled read, no interrupt/timing sensitivity.

---

## Item 5 — Water tank level sensor

**Status:** not started. **Depends on:** nothing.

**What it's for:** low-water warning now; becomes a real pump interlock
once item 9a (pump on/off control) exists.

**Buy:**

| Part | Spec | Cost |
|---|---|---|
| Magnetic float switch | Normally-open or normally-closed (either works, just flip the logic in firmware) | ~$2-5 |

**Wire:** one free GPIO (digital input, internal pull-up enabled in
firmware — `INPUT_PULLUP`), other switch leg → GND. No external resistor
needed.

**Firmware:** debounce the same way the temperature sensor's fault handling
already does (see `AGENTS.md` §9 change log) — float switches chatter
mechanically, don't trust a single read.

---

## Item 6 — Real-time pressure transducer + live pressure graph

**Status:** not started. **Depends on:** nothing (item 9b later depends on
this).

**What it's for:** a standalone monitoring/graph feature on its own, and
the hard prerequisite for pressure profiling in item 9b. **0-1.2 to
1.6MPa (12-16 bar)** confirmed as the right range from both Gaggiuino and
GaggiMate (`AGENTS.md` §7 competitive research).

**Buy:**

| Part | Spec | Cost |
|---|---|---|
| Analog pressure transducer | 0-1.2 to 1.6MPa (12-16 bar) range, food-safe wetted parts | ~$15-30 |
| T-fitting | Sized to match the pump-outlet plumbing | varies |

**Watch out for:** most cheap automotive-style pressure transducers are
**5V-supply, 0.5-4.5V output** — that range doesn't fit inside the ESP32
ADC's 0-3.3V window without a divider. Two ways to handle it:
- Buy a variant explicitly specified for 3.3V supply/output if available,
  avoiding the divider entirely, **or**
- Use a 2-resistor voltage divider to scale 0-4.5V down to 0-3.3V (from the
  resistor kit above).

**Wire:** analog output → an **ADC1 pin specifically** (ESP32-S3: ADC2 pins
share hardware with WiFi and become unavailable/contaminated when WiFi is
active — same lesson as elsewhere in this project). Plumbed at the **pump
outlet** via the T-fitting — a real plumbing job, not just wiring; budget
time for it separately from the electrical work.

---

## Item 7 — Automatic shot start/stop detection (sense-only)

**Status:** not started. **Depends on:** nothing.

**Decision (2026-08-16): not needed for the current goal.** Stopping the
pump at 25 seconds or at 36 grams doesn't require this item — see below.

**What it's for:** auto-triggering the existing shot timer/history/descale
count (currently a manual Web UI button) the moment the machine's own Brew
switch is used, without touching any mains wiring. **Sense-only** — this
does not let the ESP32 control the pump; that's item 9a.

**Why it isn't required right now:**
- **Weight-based stop (36g, item 8):** needs no "shot started" reference at
  all — firmware just watches the scale continuously and cuts the relay the
  instant weight crosses the target. Item 7 has nothing to add here.
- **Time-based stop (25s, item 9a):** does need a start-time reference to
  count from, but the existing manual "Start Shot" Web UI button already
  provides that the moment it's tapped — no new hardware required. Item 7
  would only remove that one manual tap, making the detection fully
  automatic (matching the physical switch alone, zero Web UI interaction).
  A convenience upgrade, not a requirement.

Revisit this item if/when fully hands-off operation (no manual Start Shot
tap at all) becomes worth the extra hardware.

**Buy (when revisited):**

| Part | Spec | Cost |
|---|---|---|
| Non-invasive AC current-transformer clamp | e.g. SCT-013 family, clamps around the pump's existing wire | ~$3-5 (bare) |

**Strongly prefer a pre-conditioned module over the bare clamp.** A bare
SCT-013-000 needs an external burden resistor plus a DC-bias network to
produce a 0-3.3V unipolar signal the ADC can read — two more resistor-kit
items and a bit of analog design. Many sellers instead offer the same clamp
pre-wired to a small PCB with the burden resistor and bias network already
built in (often sold as a complete "AC current sensor module" with a 3.5mm
jack). That version needs no extra parts — just power, GND, and signal into
an ADC1 pin (same ADC1-not-ADC2 rule as item 6).

**Wire:** clamp around one of the pump's existing conductors (non-invasive —
the conductor itself is never cut or touched), module output → ADC1 pin.

---

## Item 8 — Bluetooth smart scale + brew-by-weight auto-stop

**Status:** not started. **Depends on:** nothing (pairs with item 9a for
auto-stop-by-weight).

**What it's for:** brew-by-weight is more repeatable than brew-by-time
(weight is the actual outcome; time is a proxy confounded by grind/dose/
tamp). Reading the weight isn't the same as *acting* on it — auto-stop at a
target weight also needs item 9a's pump control.

**Buy:** the scale itself — **Bookoo Themis or Felicita Arc**, confirmed as
the DIY-friendliest choice (open BLE protocols, both on GaggiMate's own
supported-scale list; Acaia/Decent use different, often
community-reverse-engineered protocols). No new ESP32-side hardware — BLE is
already built into the S3.

**Firmware:** BLE central role, connecting to the scale's own peripheral
service. No GPIO/wiring at all.

---

## Item 9a — Pump on/off control ("control the button")

**Status:** decided, not yet built. **Depends on:** nothing. This is the
core item for the current goal (stop at 25s or 36g).

**Split from the original combined "item 9" (2026-08-16)** into this easy
on/off half and the harder profiling half (item 9b) — they have very
different complexity and dependencies. Just an on/off switch in series with
the pump's existing switched wire, no phase-angle timing at all.

### What this gets you

The machine's own Brew switch still starts the pump exactly as always,
with **zero ESP32 involvement in starting a shot** — see the wiring
decision below for why. Firmware can cut it early: auto-stop by a
configured time (no other hardware needed) or by a configured weight (once
item 8's BLE scale exists). **No pressure regulation, no profiling** — full
power or nothing, same as the pump already runs today. That's item 9b, a
separate, harder project.

### Buy

| Part | Spec | Why | Cost |
|---|---|---|---|
| Electromechanical relay module | 1-channel, opto-isolated 3.3V/5V logic input, mechanical relay rated ≥10A/250VAC (the common cheap Arduino-style relay module) | See "Relay vs. SSR" below — cheaper and more available than an SSR, with no downside for this specific job | $2-3 |
| Inline fuse + holder | Fast-blow, sized just above the pump's rated current | The relay can also fail shorted "on"; a fuse limits the blast radius. Matches this project's safety-first pattern elsewhere (T2 reuse, physical thermal cutoff) | $2-5 |
| Wire | 18AWG silicone (mains side), 22AWG (control side) | Matches existing heater-build stock | — |
| Wago 221 + heat-shrink | Same as heater build | Mains joint insulation | Already on hand |

No dimmer module, no zero-cross detection, no TRIAC — just a relay. This is
firmware-trivial: `digitalWrite(PIN_PUMP, ...)`, identical in spirit to
`PIN_SSR` for the heater (exact HIGH/LOW meaning depends on the wiring
decision below).

**Relay vs. SSR — a plain mechanical relay module is the better default
here, not just an acceptable substitute.** An SSR's real advantages (silent
switching, no mechanical wear) matter for the heater because PID control
cycles it constantly. The pump switches once per shot, a couple of times a
day — a relay's ~100,000-cycle contact life would take over a century of
daily espresso to wear out, and "silent" buys nothing on a machine whose
pump is itself called a *vibration* pump. The one real tradeoff — a relay
switches in a few milliseconds, not instantly — doesn't matter for stopping
at a time or weight threshold measured in seconds/grams. (This is exactly
why item 9b *can't* use a plain relay: phase-angle dimming needs firing
within a fraction of a millisecond of the AC zero-crossing. Plain on/off
has no such requirement.) An SSR would also work fine if reusing the same
part type as the heater is preferred — it just costs more for no benefit in
this specific role.

**Check the module's trigger polarity before wiring.** Many cheap
opto-isolated relay modules energize on a **LOW** signal (active-low), not
HIGH — confirm against the specific module's datasheet/silkscreen before
assuming any particular `digitalWrite` level energizes it.

**GPIO:** one free digital output pin. Add a `PIN_PUMP` `#define` to
`config.h`, matching the existing `PIN_SSR` pattern. Currently used:
`GPIO4` (SSR), `GPIO17`/`GPIO18` (UART temp sensor).

### Wiring decision: NC contact, spliced at the pump — not the switch

Two design choices here, both already resolved:

**1. Splice point: at the pump's own terminals, not the switch.** This
project already hit the "unverifiable panel wiring" problem once, for the
heater (`AGENTS.md` §7): the switch/thermostat bundle disappears underneath
the panel with no visual or multimeter access, so the heater was built as a
clean bypass avoiding the bundle entirely. This item doesn't need the same
bypass, because **the pump itself is a standalone, physically identifiable
component** — exactly like the heater element and Thermostat 2 were. Splice
the relay into the wire right at the pump's own terminals, not by opening
up the switch's terminal block. There's never a need to trace or resolve
what's happening inside the ambiguous bundle — only the two wires already
landing on the pump itself matter, and those are unambiguous by definition.

**2. Contact type: NC (Normally Closed), not NO.** The goal is "the switch
starts it, the ESP32 can stop it" — that requires the relay to pass power
through **by default**, with the ESP32 only acting to *interrupt* it. NC
(de-energized = closed = pass-through) is what makes that true: with the
ESP32 unpowered, crashed, or not yet booted, the pump behaves **exactly
like an unmodified machine** — the switch controls it directly, with no
dependency on the ESP32 at all. Firmware only energizes the relay
(**HIGH = stop**, opposite of a typical "off by default" actuator) to
interrupt the shot early. This is a deliberately different default-state
philosophy than the heater's GPIO4 (which defaults *off* because an
unintended heater-on is the dangerous failure mode) — here, the dangerous
failure mode would be the *opposite*: a design that made the pump unable to
start at all without the ESP32's cooperation. NC avoids that.

```
Existing Brew Switch (completely unmodified, still starts the pump as before)
  └─ existing wire, intercepted at the pump's own terminal ─┐
                                                              ├─ [inline fuse] ─ Relay COM
                                                          Relay NC ── Pump terminal A (was: switch wire directly)
                                              Pump terminal B ── unchanged (whatever it's currently wired to)

Relay IN ── ESP32 GPIO (output, HIGH = energize = open = stop)
Relay GND ── ESP32 GND
Relay VCC ── ESP32 3V3/5V (per module spec)
```

`COM` is the common/input side (fed from the fuse); `NC` is the
pass-through output side (to the pump) when the relay is de-energized.

**One thing to verify before final wiring:** at the pump's own two
terminals, confirm which one is the switched wire (only live when the Brew
switch is on) versus the other (commonly a shared Neutral, unaffected by
the switch) — only the switched one needs to route through the relay; the
other stays exactly as-is. This is what the non-contact AC voltage tester
from the tools section above is for: with the machine on mains and the pump
disconnected at its terminals, check which freed wire goes live only when
the switch is pressed. No continuity tracing, no multimeter, no opening up
the switch itself.

### Firmware

- **Auto-stop by time (25s):** needs a start-time reference. Today, that's
  the existing manual "Start Shot" Web UI button — tap it when flipping the
  switch, and firmware counts down from there before energizing the relay
  to cut power. (Item 7 would automate this reference instead of it being
  manual — see item 7's decision note above.)
- **Auto-stop by weight (36g):** no start reference needed — continuously
  poll the BLE scale (item 8) and energize the relay the instant the target
  is crossed.
- **Default state on boot: de-energized** (relay passes power through,
  switch behaves normally) — this is the fail-safe default for *this*
  component, the mirror image of the heater's "always boots off" rule, for
  the reason explained above.

### Procedure (bench-first, same discipline as the heater build)

1. **Bench test the relay module alone** on the ESP32's control side (3.3V/
   5V logic in, confirm trigger polarity — see above — and that it audibly
   clicks/switches) before it's anywhere near the pump.
2. **Verify the switched-vs-Neutral wire at the pump's terminals** (see
   above) using the non-contact AC voltage tester.
3. **Unplug the machine from mains.** Don't touch anything inside until
   confirmed unplugged.
4. Disconnect the pump's existing switched wire at the pump's own terminal
   only (not at the switch). Wire it → fuse → Relay COM; Relay NC → pump
   terminal. Leave the pump's other terminal exactly as it already was.
5. Wire the control side: 22AWG, Relay IN → ESP32 GPIO, GND → GND, VCC →
   3V3/5V per the module's spec.
6. Heat-shrink/insulate every exposed mains terminal and Wago joint.
7. Full visual inspection: no exposed conductors, nothing pinched or under
   strain, nothing touching the metal chassis.
8. Reassemble.
9. **First live power-up**: plug into mains, press the Brew switch as
   normal, briefly, supervised — confirm the pump runs exactly as before
   with the relay de-energized/pass-through. Stay ready to unplug
   immediately if anything looks, sounds, or smells wrong.
10. Only after a clean supervised run of plain pass-through, test
    firmware-triggered auto-stop (relay energizes mid-shot to cut it), then
    (once item 8 exists) auto-stop by weight.

---

## Item 9b — Phase-control dimmer to a pressure target (e.g. 9 bar)

**Status:** deferred. **Depends on:** item 6 (pressure transducer) — hard
prerequisite, see below.

**Split from the original combined "item 9" (2026-08-16)** as the harder
half. This is a real closed-loop control problem, not just a bigger relay —
regulating to "9 bar" requires a pressure reading to control against, so
**item 6 is a hard prerequisite here**, which wasn't as strict a dependency
under the old combined item 9.

### What this gets you beyond item 9a

Programmable pressure/flow profiles — a gentle low-pressure pre-infusion
soak before ramping to a target (e.g. 9 bar), and/or a declining-pressure
curve near the end of a shot — matching both Gaggiuino's and GaggiMate's
profiling architecture (`AGENTS.md` §7). This replaces item 9a's relay with
a TRIAC dimmer capable of partial power, and adds a PID-style control loop
using the pressure transducer as feedback (the same "measure → PID → drive
output" shape already built for temperature).

### Buy

| Part | Spec | Why | Cost |
|---|---|---|---|
| AC dimmer module | Zero-cross detection + TRIAC, opto-isolated, **3.3V-logic compatible** (e.g. RobotDyn AC Light Dimmer Module or equivalent) | Same mains position as item 9a's relay, but capable of partial power via phase-angle firing | $10-15 |
| *(Everything else — fuse, wire, connectors)* | Same as item 9a | Reused, not duplicated | — |

If item 9a is already built, this is a **swap**: same splice point at the
pump's terminals, relay out, dimmer module in. The wiring decision and
splice-at-the-pump reasoning from item 9a apply unchanged — only the
NC-contact detail is moot here, since the dimmer module replaces the relay
entirely.

**GPIO:** two free digital pins instead of item 9a's one — zero-cross
output (interrupt input) and TRIAC gate control (output), replacing the
single `PIN_PUMP` output.

### Firmware considerations

- **Requires item 6 (pressure transducer) already wired and reading
  correctly** — this is the feedback signal the control loop closes on.
  Without it, "control to 9 bar" has nothing to measure against.
- **Phase-angle firing timing must not use `delayMicroseconds()` in a
  simple interrupt handler** — WiFi/BT radio activity can jitter the timing
  enough to visibly flicker/mistime phase-control dimming. Use a hardware
  timer to schedule the gate-fire pulse from the zero-cross interrupt
  instead.
- A pressure-target control loop is conceptually the same shape as the
  existing temperature PID (`br3ttb/PID`, already a project dependency) —
  measured pressure as input, phase-angle/power as output, target bar as
  setpoint. Likely reuses the same library rather than needing a new one.
- **Do not buy a separate flow sensor** — per the competitive research in
  `AGENTS.md` §7, both Gaggiuino and GaggiMate estimate flow from pressure +
  pump behavior rather than metering it directly.

### Procedure

Same bench-first sequence as item 9a (steps 1-8), substituting the dimmer
module for the relay. After first live power-up confirms plain full-power
pass-through works exactly like item 9a, only then start closed-loop tuning
against the pressure transducer — expect this to take real iteration, the
same way brew-temperature PID tuning did (`AGENTS.md` §10 change log).

---

*Update `AGENTS.md` §10 (Change Log) as work on any of these items actually
begins — this file is the plan, the change log is the record of what
happened.*
