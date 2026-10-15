# Install guide: HLK-PM01 power, 0-16 bar pressure sensor, water level sensor

**Status:** planned, 2026-10-15. Nothing here is installed yet. The user-facing
Hebrew version of this guide is a Claude Doc ("מדריך חיבור: חיישן מים, חיישן לחץ
16 בר והזנת ה-ESP מהמכונה"). This file is the repo copy for agents.
Background and decisions: `HARDWARE_ROADMAP.md` items 6 and 7, `AGENTS.md`
change log (2026-10-15 entries).

## Result

- The ESP32 is powered by an **HLK-PM01** (220V AC → 5V DC, 3W / 600 mA) on
  **always-on mains**, replacing the USB wall-wart. It feeds the DevKit's
  **5V pin**; the board's regulator still makes 3V3 for everything else.
- The new **0-16 bar pressure sensor** ("G41 1.6Mpa IN5V") gets 5V from the
  same rail; its 0.5-4.5 V signal goes to **GPIO1** through a **4.3k / 10k**
  divider (→ ~0.35-3.15 V).
- The **water level sensor** (non-contact capacitive) stays on **3V3**, signal
  on **GPIO7** (LOW = water present).

## Parts

| Part | Qty | Note |
|---|---|---|
| HLK-PM01 (5V 3W) | 1 | on hand |
| Pressure sensor G41 1.6Mpa IN5V | 1 | ordered |
| Capacitive water level sensor 3.3-5V | 1 | red VCC, black GND, yellow OUT |
| Resistors 4.3k + 10k | 1 each | pressure divider |
| 0.5 A fast fuse + enclosed holder | 1 | HLK live input |
| Wago 221, 3-way | 2 | live and neutral taps |
| 100 µF / ≥10 V capacitor | 1 | recommended across HLK output |
| 0.75-1 mm² wire, Dupont leads, heat-shrink, cable ties, multimeter | — | |

## Part A — HLK-PM01 on always-on mains (machine unplugged)

Tap the two **new heater-circuit wires** from the 2026-08 clean bypass
(`AGENTS.md` §7): they come straight from the mains inlet, before any switch.

```
Mains L -- new wire -- T2 (steam thermostat, on boiler) -- SSR -- heater -- new wire -- Mains N
```

1. Live: the wire **mains → T2** (T2's mains side, not T2 → SSR).
2. Neutral: the wire **heater → mains N** (not heater → SSR).
3. Splice mid-run, **away from the boiler** (too hot for the Wago and HLK).
4. Cut each wire, both ends into a 3-way Wago, third port to the HLK:
   L Wago → 0.5 A fuse → HLK AC1; N Wago → HLK AC2.
5. Mount the HLK somewhere cool (near the ESP), fully insulated
   (heat-shrink or a small box), off the chassis metal, strain-relieved.
   Rated to ~70 °C.

If the HLK fuse blows only the ESP goes down; the heater stays off because
the SSR only conducts when GPIO4 drives it.

## Part B — 5V side

| From | To |
|---|---|
| HLK +Vo | DevKit **5V** pin and pressure sensor + (red) |
| HLK −Vo | DevKit **GND** and pressure sensor − (black) |
| 100 µF | across +Vo / −Vo (long leg to +) |
| Pressure signal | **4.3k** → GPIO1 |
| GPIO1 | **10k** → GND |

- Never USB and HLK at the same time: remove the USB cable/wall-wart.
  USB flashing in future = unplug the machine first; OTA unaffected.
- Do not connect the divider midpoint to GPIO1 until check 5 passes.
- No GPIO4 pull-down: the user explicitly accepts a short heater pulse at
  boot (`AGENTS.md` §7 step 1, re-confirmed 2026-10-15).
- Pressure sensor wire colours vary - confirm the signal wire from its
  manual. Same G1/4 port at the pump outlet, PTFE tape.

## Part C — Water level sensor

| Wire | To |
|---|---|
| Red (VCC) | 3V3 |
| Black (GND) | GND |
| Yellow (OUT) | GPIO7 |

- The white JST plug does not fit the DevKit headers: male Dupont pins into
  it (then fix it), or cut it and solder to female Dupont leads (most
  reliable).
- **Only one face senses** - if it reads "no water" against a full tank but
  reacts to a finger, flip it (verified on the machine 2026-10-15). Test
  hands-off: a finger reads as water.
- Outside the tank wall at the "low" height, face flat against the plastic
  when the tank is seated; a foam pad on the chassis side keeps it pressed.
  Away from boiler and steam (rated 80 °C).
- If unstable on 3V3 (its minimum): VCC to 5V and OUT through 10k to GPIO7
  with 20k from GPIO7 to GND. No firmware change.

## Multimeter checks (in order; stop on any mismatch)

| # | Condition | Measure | Expect |
|---|---|---|---|
| 1 | unplugged, continuity | HLK AC1 ↔ AC2 | no beep |
| 2 | unplugged, continuity | any 220V wire ↔ any 5V/3V3/GND wire | no beep |
| 3 | plugged in, main switch and brew switch OFF, AC | AC1 wire ↔ AC2 wire | 230 V (proves always-on) |
| 4 | plugged in, DC | +Vo ↔ −Vo | 4.9-5.1 V |
| 5 | plugged in, no pressure, DC | divider midpoint ↔ GND | ~0.35 V; above 3.3 V → do NOT connect |
| 6 | plugged in, DC | ESP 3V3 ↔ GND | ~3.3 V |

Then unplug, connect the divider midpoint to GPIO1, insulate, plug back in.

## First power-up, firmware, calibration

1. Plug in (no USB): Web UI at `http://gaggia.local` within seconds, mode Off.
2. Heater 0%, temperature not rising.
3. Water sensor (already enabled in Settings): full tank → "Tank OK" within
   ~2 s; tank out → "Low water". Live reading in Settings → Water tank sensor.
4. Set `PRESSURE_SENSOR_IS_16BAR` to `1` in `include/config.h`, build, and
   OTA. **Not before the new sensor is fitted** - with the old 5-bar part the
   16-bar settings read below zero and fault.
5. Zero: `pressure_mv` with the pump stopped should be ~350 mV; set
   `PRESSURE_SENSOR_ZERO_MV` from the real value.
6. Pressure test: very fine coffee, heater off, 5-10 s run while watching the
   chart. Afterwards vent through the steam wand into a cup before removing
   the portafilter (no 3-way valve). Then set `PRESSURE_SENSOR_MV_PER_BAR`
   from a reference point.
7. Normal shot: pressure phases (e.g. 9 bar) are held by the closed loop;
   `PUMP_MAX_SAFETY_BAR` (12) cuts the pump on a real reading.

## Pin map after this change

| GPIO | Function | Direction |
|---|---|---|
| 5V | HLK +Vo in, pressure sensor + | power in |
| GND | HLK −Vo, all module grounds | — |
| 3V3 | PT100 module, dimmer logic, water sensor | power out |
| 1 | pressure signal via 4.3k/10k divider | analog in |
| 4 | SSR (heater) | out |
| 5 | dimmer PSM | out |
| 6 | dimmer zero-cross | in |
| 7 | water level sensor OUT | in (pull-up) |
| 17 | PT100 module RX (ESP UART TX) | out |
| 18 | PT100 module TX (ESP UART RX) | in |
