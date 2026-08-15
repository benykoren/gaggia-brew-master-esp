# Hardware Roadmap — Buy Lists & Wiring Plans

Actionable companion to `AGENTS.md` §8 ("Roadmap / future work"): for each
remaining hardware item, **what to buy, how to wire it, and what to watch
out for.** `AGENTS.md` remains the single source of truth for the *why*
(competitive research, coffee science, prior decisions) and for the running
change log (§10). This file is the *what/how*, and is kept current as each
item is planned in more detail or actually built.

Items 1-3 (shot timer, shot history, descale reminder) are already
shipped — pure software, no hardware. See `AGENTS.md` §8 and the README
roadmap. Numbering below continues from there, and is **ordered by
priority** (highest-value/most-wanted first), not by cost tier or the
order items were originally proposed in. Everything below is **not yet
built**.

## Contents

- [Summary](#summary)
- [Tools worth having first](#tools-worth-having-first)
- [Item 4 — Pump on/off control](#item-4--pump-onoff-control-control-the-button)
- [Item 5 — Bluetooth smart scale](#item-5--bluetooth-smart-scale--brew-by-weight-auto-stop)
- [Item 6 — Water tank level sensor](#item-6--water-tank-level-sensor)
- [Item 7 — Pressure transducer + live pressure graph](#item-7--real-time-pressure-transducer--live-pressure-graph)
- [Item 8 — Phase-control dimmer](#item-8--phase-control-dimmer-to-a-pressure-target-eg-9-bar)
- [Item 9 — Nextion touchscreen HMI](#item-9--nextion-touchscreen-hmi)
- [Item 10 — Milk temperature probe](#item-10--milk-temperature-probe)

---

## Summary

| Item | Adds | Depends on | Needed for your current goal? | Est. cost |
|---|---|---|---|---|
| **4** | Pump on/off relay (25s / weight auto-stop) | — | **Yes — this is the core item** | $9-13 |
| **5** | BLE smart scale (36g auto-stop) | — | Yes | scale cost |
| 6 | Water tank level sensor | — | No | $2-5 |
| 7 | Live pressure reading | — | Only if pursuing item 8 later | $15-30 |
| 8 | Phase-control pressure profiling | Item 7 | No | $12-20+ |
| 9 | Nextion touchscreen HMI | — | No | $15-40 |
| 10 | Milk temperature probe | — | No | $2-3 |

**Numbering history:** this list was reordered by priority on 2026-08-16,
continuing from the already-shipped items 1-3 rather than restarting at 1
(avoids colliding with those numbers elsewhere in `AGENTS.md`). Earlier
conversation/commit history refers to these by their original labels — item
4 was "9a," item 5 was "8," item 6 was "5," item 7 was "6," item 8 was "9b,"
item 9 was "Display," item 10 was "4." A prior item ("7," sense-only shot
detection via a current-transformer clamp) was **removed entirely** —
explicitly rejected in favor of real control instead of passive sensing;
see item 4's firmware notes for where that consideration now lives.

Current stated goal — stop the pump at **25 seconds**, or later at **36
grams** once a BLE scale is in hand — needs only **items 4 and 5**. Items
6, 7, 8, 9, and 10 are independent, optional, and can be picked up in any
order later.

---

## Tools worth having first

Two cheap, general-purpose items come up across multiple entries below.
Buying them once avoids a separate workaround per item — neither is
strictly required, and each item below also notes the no-extra-tool
alternative where one exists.

| Tool | Cost | Unblocks |
|---|---|---|
| Basic resistor assortment kit | ~$8 | Item 10's pull-up; simplifies item 7 (otherwise it needs a pre-conditioned sensor module instead of the bare part) |
| Non-contact AC voltage tester (pen-style proximity tester, **not** a multimeter — no probes touch conductors) | ~$5-10 | Item 4/8's one open wiring question: identifying the switched-Line wire at the pump's terminals |

This project has so far deliberately avoided buying resistors or a
multimeter (see `AGENTS.md` §7) — that's a fine choice for a one-off
workaround, but more than one item below independently wants the same tools.

---

## Item 4 — Pump on/off control ("control the button")

**Status (revised 2026-08-16): software half built and real-shot-tested;
hardware half (the relay itself) not yet bought or wired.** **Depends on:**
nothing. This is the core item for the current goal (stop at 25s or 36g).
(Originally "item 9a.")

The Brew gain-scheduling profile (`brewActiveKp/Ki/Kd`) and shot-start
feedforward described below are already live-tunable in the Web UI and
confirmed on the real machine to cut brew-time sag from ~11-13°C to ~6°C
(further gain tuning stopped helping once output pinned at the heater's
physical wattage ceiling - a conclusive result, not a dead end). A
configurable auto-stop timer (`shotAutoStopSec`, Settings → Shot Timer, 0 =
disabled) also now ends the *firmware's* shot bookkeeping automatically.
**None of this touches the pump yet** - "auto-stop" today means the
firmware stops tracking the shot and reverts its own tuning, not that the
pump stops flowing. That gap is exactly what the hardware below closes.

**Split from an original combined "pump dimmer" item (2026-08-16)** into
this easy on/off half and a harder profiling half (item 8) — they have very
different complexity and dependencies. Just an on/off switch in series with
the pump's existing switched wire, no phase-angle timing at all.

### What this gets you

The machine's own Brew switch still starts the pump exactly as always,
with **zero ESP32 involvement in starting a shot** — see the wiring
decision below for why. Firmware can cut it early: auto-stop by a
configured time (no other hardware needed) or by a configured weight (once
item 5's BLE scale exists). **No pressure regulation, no profiling** — full
power or nothing, same as the pump already runs today. That's item 8, a
separate, harder project.

### Buy

| Part | Spec | Why | Cost |
|---|---|---|---|
| Electromechanical relay module | 1-channel, opto-isolated 3.3V/5V logic input, mechanical relay rated ≥10A/250VAC (the common cheap Arduino-style relay module) | See "Relay vs. SSR" below — cheaper and more available than an SSR, with no downside for this specific job | $2-3 |
| Inline fuse + holder | **Mains-rated** (250V AC, e.g. a 5×20mm holder) with a 1-2A fast-blow fuse — **not** an automotive/car blade-fuse holder, which is only rated ~32V DC and unsafe on a mains circuit | The relay can also fail shorted "on"; a fuse limits the blast radius. Matches this project's safety-first pattern elsewhere (T2 reuse, physical thermal cutoff) | $2-5 |
| Wire | 18AWG silicone (mains side), 22AWG (control side) | Matches existing heater-build stock | — |
| Wago 221 + heat-shrink | Same as heater build | Mains joint insulation | Already on hand |

**Optional addition, not yet decided:** an AC-presence/zero-cross detection
module (opto-isolated, outputs a signal only when AC is present at its
input — same category of part as half of an AC dimmer module, sold
standalone) wired at this *same* splice point would let the ESP32 detect
the Brew switch being flipped automatically, closing the gap noted above —
the auto-stop timer would then control the real pump, and Start Shot would
no longer need a manual tap either. This is a different technique from the
current-transformer clamp already rejected as item "7" (senses voltage
presence on this wire, not current elsewhere on the pump's own wire) — not
a reopening of that decision, a separate one not yet made. ~$3-5.

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
why item 8 *can't* use a plain relay: phase-angle dimming needs firing
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
  to cut power. (A passive sense-only detector was considered to automate
  this tap away entirely and rejected — user wants real control, not
  passive sensing. If fully hands-off detection is wanted later, it belongs
  here as a control extension, not a separate passive sensor.)
- **Auto-stop by weight (36g):** no start reference needed — continuously
  poll the BLE scale (item 5) and energize the relay the instant the target
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
    (once item 5 exists) auto-stop by weight.

---

## Item 5 — Bluetooth smart scale + brew-by-weight auto-stop

**Status:** not started. **Depends on:** nothing (pairs with item 4 for
auto-stop-by-weight). (Originally "item 8.")

**What it's for:** brew-by-weight is more repeatable than brew-by-time
(weight is the actual outcome; time is a proxy confounded by grind/dose/
tamp). Reading the weight isn't the same as *acting* on it — auto-stop at a
target weight also needs item 4's pump control.

**Buy:** the scale itself — **Bookoo Themis or Felicita Arc**, confirmed as
the DIY-friendliest choice (open BLE protocols, both on GaggiMate's own
supported-scale list; Acaia/Decent use different, often
community-reverse-engineered protocols). No new ESP32-side hardware — BLE is
already built into the S3.

**Firmware:** BLE central role, connecting to the scale's own peripheral
service. No GPIO/wiring at all.

---

## Item 6 — Water tank level sensor

**Status:** not started. **Depends on:** nothing. (Originally "item 5.")

**What it's for:** low-water warning now; becomes a real pump interlock
once item 4 (pump on/off control) exists.

**Buy:**

| Part | Spec | Cost |
|---|---|---|
| Magnetic float switch | Normally-open or normally-closed (either works, just flip the logic in firmware) | ~$2-5 |

**The tank is removable, not fixed — this affects the wiring.** The Gaggia
Espresso Color's water tank lifts out to refill, so a float switch mounted
inside it needs a connection that survives being pulled out and reseated
repeatedly. A bare wire soldered to the switch would mean disconnecting/
reconnecting by hand every refill — the same nuisance as item 10's milk
probe. The standard fix: **spring-loaded contacts (pogo pins) at the bottom
of the tank bay**, mating with a corresponding contact on the tank's
underside when it's seated. The wire then stays fixed to the machine
chassis permanently; only the contact tips touch/separate as the tank goes
in and out, no manual plugging involved. Choose contacts rated for
corrosion resistance given the proximity to water.

**Wire:** one free GPIO (digital input, internal pull-up enabled in
firmware — `INPUT_PULLUP`), other switch leg → GND, routed through the
pogo-pin contacts above. No external resistor needed.

**Firmware:** debounce the same way the temperature sensor's fault handling
already does (see `AGENTS.md` §9 change log) — float switches chatter
mechanically, don't trust a single read.

---

## Item 7 — Real-time pressure transducer + live pressure graph

**Status:** not started. **Depends on:** nothing (item 8 later depends on
this). (Originally "item 6.")

**What it's for:** a standalone monitoring/graph feature on its own, and
the hard prerequisite for pressure profiling in item 8. **0-1.2 to
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

## Item 8 — Phase-control dimmer to a pressure target (e.g. 9 bar)

**Status:** deferred. **Depends on:** item 7 (pressure transducer) — hard
prerequisite, see below. (Originally "item 9b.")

**Split from an original combined "pump dimmer" item (2026-08-16)** as the
harder half. This is a real closed-loop control problem, not just a bigger
relay — regulating to "9 bar" requires a pressure reading to control
against, so **item 7 is a hard prerequisite here**.

### What this gets you beyond item 4

Programmable pressure/flow profiles — a gentle low-pressure pre-infusion
soak before ramping to a target (e.g. 9 bar), and/or a declining-pressure
curve near the end of a shot — matching both Gaggiuino's and GaggiMate's
profiling architecture (`AGENTS.md` §7). This replaces item 4's relay with
a TRIAC dimmer capable of partial power, and adds a PID-style control loop
using the pressure transducer as feedback (the same "measure → PID → drive
output" shape already built for temperature).

### Buy

| Part | Spec | Why | Cost |
|---|---|---|---|
| AC dimmer module | Zero-cross detection + TRIAC, opto-isolated, **3.3V-logic compatible** (e.g. RobotDyn AC Light Dimmer Module or equivalent) | Same mains position as item 4's relay, but capable of partial power via phase-angle firing | $10-15 |
| *(Everything else — fuse, wire, connectors)* | Same as item 4 | Reused, not duplicated | — |

If item 4 is already built, this is a **swap**: same splice point at the
pump's terminals, relay out, dimmer module in. The wiring decision and
splice-at-the-pump reasoning from item 4 apply unchanged — only the
NC-contact detail is moot here, since the dimmer module replaces the relay
entirely.

**GPIO:** two free digital pins instead of item 4's one — zero-cross output
(interrupt input) and TRIAC gate control (output), replacing the single
`PIN_PUMP` output.

### Firmware considerations

- **Requires item 7 (pressure transducer) already wired and reading
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

Same bench-first sequence as item 4 (steps 1-8), substituting the dimmer
module for the relay. After first live power-up confirms plain full-power
pass-through works exactly like item 4, only then start closed-loop tuning
against the pressure transducer — expect this to take real iteration, the
same way brew-temperature PID tuning did (`AGENTS.md` §10 change log).

---

## Item 9 — Nextion touchscreen HMI

**Status:** decided, not yet started. (Corrected 2026-08-16 — `AGENTS.md`
and `README.md` previously mislabeled this "in progress"; no hardware has
been bought and no driver written yet.) **Depends on:** nothing.
(Originally tracked as "Display.")

This is a different kind of item from 4-8 and 10: those are new sensing/
actuation capabilities; this is an alternative *interface* to capabilities
that already exist via the Web UI, not a new capability itself.

### What this gets you

On-machine status and basic control without needing a phone — live
temperature, current mode, shot timer, start/stop. **Deep configuration
(PID tuning, thresholds, profile editing) stays on the Web UI** — an
explicit design call, independently validated by the Gaggiuino/GaggiMate
research in `AGENTS.md` §7: both competitor projects keep profile editing
on a phone/web UI too, using their on-device screens only for live status
and basic control.

### Why Nextion over a raw OLED/color-TFT

A Nextion panel has its own onboard display controller — the GUI (buttons,
text fields, gauges) is designed in Nextion's own free editor and uploaded
to the panel directly; the ESP32 then just sends/receives simple serial
commands. This is a much smaller firmware lift than driving a raw display
buffer/graphics library from the ESP32 itself, and matches this project's
already-proven "external component over UART" pattern (the same shape as
the UART PT100 temperature module). It's also architecturally simpler than
GaggiMate's approach, which runs a full LVGL graphics stack on a *second*,
separate ESP32 board talking BLE back to its controller board — this
project keeps everything on the single existing ESP32-S3.

### Buy

| Part | Spec | Cost |
|---|---|---|
| Nextion display module | Basic or Enhanced series; size is an open decision — depends on available panel space on the machine | ~$15-40 depending on size |

**Screen size isn't decided** — pick based on how much physical panel space
is available and where it'll be mounted, not something this document can
specify without an enclosure plan.

### Wire

Nextion modules use a simple 4-wire UART interface: `5V`, `GND`, `TX`, `RX`.
This needs a **second, separate hardware UART** from the temp sensor's
(currently on `GPIO17`/`GPIO18`) — the ESP32-S3 has three hardware UART
controllers, so this is just a matter of picking two more free GPIOs,
avoiding the reserved ranges already noted in `config.h` (native USB, SPI
flash, PSRAM, strapping pins, onboard LED) and the pins already in use
(`GPIO4`, `GPIO17`, `GPIO18`).

**Check the specific module's logic-level tolerance before wiring.** Nextion
Basic-series panels are commonly documented as 5V-powered but
3.3V-logic-tolerant on TX/RX, which would allow a direct connection to the
ESP32's 3.3V GPIOs — but confirm against the specific model's datasheet
rather than assuming, the same caution as elsewhere in this project (relay
trigger polarity, pressure sensor voltage range).

### Firmware

- New `nextion.cpp` driver (already named as the plan in `AGENTS.md` §8),
  following the same "external component over UART" shape as
  `temp_sensor.cpp`.
- Either a lightweight custom parser matching this project's existing
  DIY-protocol style, or the common `ITEADLIB_Arduino_Nextion` library —
  either is reasonable; not yet decided.
- Scope stays deliberately narrow per the design call above: push live
  values (temp, mode, shot timer) to the display and read back simple touch
  events (start/stop, maybe mode switch) — no profile editing, no PID
  tuning on-device.

### Physical mounting

Cutting a panel opening for the screen is a real fabrication task, separate
from the electrical work — similar in kind to item 7's plumbing job. Budget
for it as its own step, and settle on screen size/mounting location before
ordering the part.

---

## Item 10 — Milk temperature probe

**Status:** not started. **Depends on:** nothing. (Originally "item 4.")

**What it's for:** real-time milk temperature during steaming. Dairy
science puts the sweet spot at ~55-65°C, with ~70°C as a hard
scald/foam-collapse ceiling. Neither Gaggiuino nor GaggiMate address this at
all — see `AGENTS.md` §7 competitive research.

**The wire has to leave the machine, and that's a real drawback.** Unlike
every other item on this list, this probe is only useful dipped into a
separate milk pitcher — there's no way to avoid a wire (and a probe tip)
being handled and routed externally every time it's used, which is why this
item is ranked last despite being the cheapest thing on the whole roadmap.

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

*Update `AGENTS.md` §10 (Change Log) as work on any of these items actually
begins — this file is the plan, the change log is the record of what
happened.*
