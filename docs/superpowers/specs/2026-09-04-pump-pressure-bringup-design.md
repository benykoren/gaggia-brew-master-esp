# Pump-Pressure Hardware Bring-Up — Design Spec

**Status:** approved, ready for implementation planning
**Date:** 2026-09-04
**Sub-project 1 of 3** in the larger "FreeRTOS task-priority refactor + dual
connectivity + Android app" initiative (see "Larger initiative context"
below). This spec covers **only** the pump-pressure hardware bring-up.

---

## 1. Context

This project (`AGENTS.md`) is a PID-controlled espresso machine firmware on
an ESP32-S3. The user's actual overall goal is:

1. Restructure the firmware into a FreeRTOS task model with strict priority
   ordering — PID temperature control and pump pressure regulation at the
   highest priority, the local web UI at a lower priority, and a new dual
   USB-Serial + Bluetooth connectivity stack.
2. Build an Android tablet app that becomes the primary control surface —
   full feature parity with the web UI (profiles, schedules, auto-shutoff,
   temperature tuning), real-time graphs, older-Android backward
   compatibility, and its own OTA (Wi-Fi) update path. **This replaces the
   previously-planned Nextion touchscreen HMI (roadmap item 9)** — confirmed
   with the user 2026-09-04.

That request bundles at least four independently-buildable pieces. It was
decomposed (brainstorming session, 2026-09-04) into three sequential
sub-projects, each getting its own spec → plan → implementation cycle:

1. **Pump-pressure hardware bring-up** (this spec) — roadmap items 7
   (pressure transducer) and 8 (TRIAC dimmer) don't exist yet; there's no
   real pump-pressure control loop to assign a task priority to. Building it
   now is a prerequisite for sub-project 2 to have something real to
   prioritize, and it's mains-adjacent hardware work that deserves its own
   careful, bench-tested phase (matching this project's established pattern
   for the SSR/heater and pump-relay builds) rather than being folded into a
   software-architecture rewrite.
2. **FreeRTOS task-priority restructure + USB Serial/BLE connectivity
   stack** — builds on the real pump-pressure task this spec delivers, adds
   the dual-connectivity protocol, and secures the web UI (currently has
   **no authentication at all** — flagged as a new requirement, not
   previously true of this codebase).
3. **Android tablet app** — depends on sub-project 2's connectivity
   protocol existing to talk to.

## 2. Goal of this spec

Replace the current pump control (an NC relay, `HARDWARE_ROADMAP.md` item 4,
control-side wired but never mains-spliced, and shelved after an unresolved
brownout bug — see `AGENTS.md` change log, 2026-08-30) with a phase-control
AC dimmer (`HARDWARE_ROADMAP.md` item 8) plus a pressure transducer
(`HARDWARE_ROADMAP.md` item 7), delivering:

- Plain on/off pump control (replacing the relay's role, including
  time/weight-based shot auto-stop).
- Closed-loop pressure control to a target (e.g. ~9 bar), driven by a
  multi-phase pressure profile (pre-infusion soak → ramp → optional decline)
  integrated into the existing `ShotStage` state machine.

Both halves are covered by this one spec; implementation still lands in
verified milestones (see Section 7), but there is one design to build
against.

## 3. Hardware & pin plan

| Signal | Pin | Notes |
|---|---|---|
| Pressure transducer analog out | **GPIO1** (ADC1_CH0) | ADC1 only — ADC2 shares hardware with WiFi and becomes unavailable/contaminated when WiFi is active (same rule already followed for temp sensing elsewhere in this project) |
| Dimmer zero-cross input | **GPIO6** | Interrupt input from the dimmer module's zero-cross output |
| Dimmer TRIAC gate fire | **GPIO5** | Reused from the old `PIN_PUMP` relay pin (freed by removing the relay) |

**Buy list:**
- Analog pressure transducer, 0-1.2 to 1.6MPa (12-16 bar) range, food-safe
  wetted parts (~$15-30). Confirmed range from Gaggiuino/GaggiMate
  competitive research (`AGENTS.md` §7) and consistent with this machine's
  16-bar safety valve rating.
- T-fitting sized to the pump-outlet plumbing.
- Opto-isolated zero-cross + TRIAC AC dimmer module, 3.3V-logic compatible
  (e.g. RobotDyn AC Light Dimmer Module or equivalent), ~$10-15.

**Voltage matching:** many cheap pressure transducers are 5V-supply,
0.5-4.5V output, which doesn't fit the ESP32 ADC's 0-3.3V window without a
2-resistor divider. Exact resistor values are an implementation-time detail
once the specific transducer part is chosen; a transducer sold with native
3.3V output avoids the divider entirely and should be preferred if
available at similar cost.

**Splice point:** the dimmer's mains-side splice reuses the same point
already identified and bench-verified for item 4 — the pump's own White
wire (switched, sourced from the Brew Switch), confirmed against the OEM
service diagram (Gaggia SAE0486) and the user's own hand-tracing (see
`AGENTS.md` §7 "Update" note). The Blue (unswitched) wire is untouched.

**Decommissioning the old relay:** its control-side wiring (GPIO5/VCC/GND)
is physically disconnected from the SONGLE SRD-05VDC-SL-C relay module. The
relay was never mains-spliced, so there is no mains-side undo required.
`setPumpRelay()` and all `PIN_PUMP`-relay-specific code paths are removed
from `main.cpp`/`config.h` rather than kept as a fallback (explicit user
decision, 2026-09-04 — the relay's brownout bug and its fail-open dependency
on the pump's own switch are not being preserved; the dimmer has no
equivalent passive pass-through state, so **this reintroduces the fail-off
dependency already accepted and documented for item 8** in
`AGENTS.md`/`HARDWARE_ROADMAP.md`: the pump will not run at all if the
ESP32 isn't running firmware, even with the Brew switch held).

## 4. Firmware control design

- **Zero-cross ISR** on GPIO6 schedules a **hardware one-shot timer** at a
  phase-delay computed from the current target power percentage. This is
  the only piece that is genuinely timing-critical (mains half-cycle is
  ~8.3-10ms), so it is the only piece that bypasses the normal ~50ms control
  cadence — per `HARDWARE_ROADMAP.md` item 8's existing warning,
  `delayMicroseconds()` inside a plain interrupt handler is not acceptable
  here (WiFi/BT radio activity can jitter it enough to visibly mistime the
  firing); a hardware timer callback is not subject to that jitter.
- **Gate-fire callback** (from the hardware timer) pulses GPIO5 to fire the
  TRIAC, then disarms until the next zero-cross.
- **Pressure read + fault detection** joins the existing 50ms
  `controlTick()` cadence, reusing the same EMA smoothing
  (`TEMP_EMA_ALPHA`-style) and rolling-window fault-rate pattern already
  built for the temp sensor (`SENSOR_FAULT_WINDOW` /
  `SENSOR_FAULT_MIN_SAMPLES` / `SENSOR_FAULT_RATE_THRESHOLD`), applied as an
  independent instance for pressure.
- **Pressure PID:** a second `PID_v1` instance (Input = measured bar,
  Output = 0-100% power, Setpoint = the active stage's pressure target) —
  same "measure → PID → drive output" shape already used for temperature.
- **`ShotStage` gains a `PRESSURE_TARGET` type** (in addition to the
  existing `PUMP_ON` / `PUMP_OFF` / `EXTRACTION`) carrying a target bar
  value alongside the existing duration field. A shot can move: `PUMP_ON`
  (low-pressure pre-infusion soak) → one or more `PRESSURE_TARGET` stages
  (ramp to ~9 bar, then an optional declining curve as further
  `PRESSURE_TARGET` stages) → `EXTRACTION`.
- **Backward compatible:** existing profiles with no pressure data keep
  working exactly as today — `PUMP_ON` / `PUMP_OFF` / a plain `EXTRACTION`
  with no pressure target just drive the dimmer at 100%/0% duty,
  reproducing the old relay's on/off behavior exactly. No profile migration
  is needed; `profiles.cpp`'s JSON storage already gracefully handles
  optional/missing fields.
- **Safety ceiling:** a fixed `PUMP_MAX_SAFETY_BAR = 12.0` (comfortable
  margin under the 16-bar safety-valve rating, above the ~9 bar working
  target) forces 0% duty immediately if measured pressure exceeds it, or if
  a pressure-sensor fault is active — independent of whatever the PID or
  active profile is requesting. This mirrors the existing rule that a
  safety cutoff always wins over PID/profile output (`activeMaxSafety` for
  temperature). Not Web-UI-configurable initially, matching how
  `BREW_MAX_SAFETY` (fixed) is treated rather than `STEAM_MAX_SAFETY`
  (configurable) — there's no equivalent tuning need identified yet.
- **Bring-up gate:** on/off duty (100%/0%) must be verified reproducing
  plain pass-through on the real pump *before* any closed-loop pressure
  tuning is attempted — matches `HARDWARE_ROADMAP.md` item 8's stated bring-up
  order.

## 5. Web UI / API changes

- `/status` gains `pressure` (bar) and `pressure_fault` (bool) fields, plus
  a pressure history ring buffer mirroring the existing
  `TEMP_HISTORY_LEN`/sparkline pattern, surfaced as a second live chart on
  the dashboard alongside the temperature sparkline.
- The profile editor (`profiles.cpp`, JSON-based) gains optional per-stage
  pressure-target fields. Profiles that omit them fall back to today's
  duty-only behavior.
- `PUMP_MAX_SAFETY_BAR` is a fixed constant, not exposed in the Web UI
  initially.

## 6. Safety

Per `AGENTS.md` §6 (mains voltage rules), this is mains-adjacent work and
follows the same discipline already established for the SSR/heater and
pump-relay builds:

- Bench-test the dimmer module on a low-voltage/lamp load first, verifying
  zero-cross detection and gate-firing logic thoroughly, before ever
  connecting the pump's real AC line.
- Insulate every mains joint (dimmer AC-side terminals, splice point,
  connectors) before closing the machine back up.
- First live power-up: confirm 100% duty reproduces unmodified pass-through
  behavior before testing anything else.
- Supervise the first closed-loop pressure-tuning session on the real
  machine, the same way first PID-autotune and first mains-heating runs
  were supervised.

## 7. Bring-up procedure (verified milestones)

1. Bench-test the dimmer module on a lamp load — confirm zero-cross
   detection and gate-firing logic in isolation, no mains pump connection
   yet.
2. Splice the dimmer at the pump's White (switched) wire, following the
   same procedure already documented for item 4/8 in `HARDWARE_ROADMAP.md`.
   Disconnect the old relay's control-side wiring at the same time.
3. **Milestone A:** verify 100% duty reproduces the machine's normal
   pass-through pump behavior, and 0% duty stops it — this alone delivers
   everything the old relay would have (time/weight-based auto-stop).
4. Plumb the pressure transducer at the pump outlet via the T-fitting; wire
   to GPIO1 (with divider if needed). **Milestone B:** verify live pressure
   readings track a manually-observed pressure change (e.g. a rough
   correlation while pulling a shot) before trusting them for control.
5. **Milestone C:** closed-loop pressure-target tuning — expect real
   iteration, the same way brew-temperature PID tuning took multiple rounds
   against the actual boiler (`AGENTS.md` §10 change log).

## 8. Out of scope (deferred to later sub-projects)

- Formal FreeRTOS task-priority assignment for the pump-pressure control
  loop (sub-project 2) — this spec keeps it inside the existing
  `controlLoopTask`/`controlTick()` cadence except for the ISR/hardware-timer
  gate-firing path, which is inherently outside any task's cadence.
- USB Serial / BLE connectivity (sub-project 2).
- Web UI authentication/security (sub-project 2).
- Android app integration (sub-project 3).
- Any change to the existing temperature PID, autotune, eco-sleep, or shot
  auto-stop logic beyond what's needed to add the `PRESSURE_TARGET` stage
  type.

## 9. Decisions log (from brainstorming, 2026-09-04)

- Both halves (on/off dimmer control + closed-loop pressure PID) built in
  one spec, one implementation plan — not split into stage-1/stage-2 specs.
- Nothing bought yet — this spec includes the buy list.
- Old NC relay removed entirely, not kept as a fallback.
- Multi-phase pressure profile (integrated into `ShotStage`), not a single
  fixed target.
- Gate-firing architecture: zero-cross ISR + hardware timer, with the
  pressure PID loop staying in the existing control task rather than
  getting its own FreeRTOS task now (that's sub-project 2's job).
- Android app confirmed to replace the previously-planned Nextion
  touchscreen HMI (roadmap item 9).
