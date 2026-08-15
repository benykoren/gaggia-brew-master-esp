# GaggiaBrewMasterESP — AGENTS.md — Project Doc & Agent Change Log

> **Read this first.** This is the single source of truth for this project and a
> running log of every change agents make. **Every agent that works in this repo
> MUST:**
> 1. Read this whole file before doing anything.
> 2. Keep it accurate — if you change hardware, pins, build flags, or behavior,
>    update the relevant section.
> 3. Append an entry to the **Change Log** at the bottom (newest at the top of
>    that section) describing what you did and why.

---

## 1. What this project is

**GaggiaBrewMasterESP** — a smart controller platform for a **Gaggia Espresso
Color** espresso machine, running on an **ESP32-S3**. Started as a closed-loop
**PID brew-temperature controller** replacing the machine's stock bang-bang
thermostat, and has grown well beyond "just PID": Brew/Steam gain-scheduled
modes, eco/auto-sleep, PID autotune, OTA updates, WiFi reconfiguration - with
a local Wi-Fi Web UI (plus optional MQTT / Home Assistant), a Nextion
touchscreen HMI in progress, and a phased roadmap toward pressure profiling,
water-level safety, pump flow control, and Bluetooth scale integration (see
[Roadmap](#8-roadmap--future-work)).

**Project name note (2026-08-15):** renamed from the working name "Gaggia
PID" - that undersold the scope once eco-sleep/autotune/OTA/display work
started. `gaggia.local` (mDNS hostname) is kept unchanged for practical
continuity (existing bookmarks/muscle memory); only display branding (page
title, manifest, WiFi AP name, this doc) changed.

### Origin
This project was **cloned from [`petoz/gaggia_pid_esp32-c6`](https://github.com/petoz/gaggia_pid_esp32-c6)**
(originally targeting the ESP32-**C6**) and **retargeted to the ESP32-S3**.
The upstream repo's README states "MIT License" in text but has **no actual
LICENSE file** (confirmed via GitHub's own repo metadata: no license
detected) - so this project adds a proper MIT `LICENSE` file of its own
while crediting the origin, rather than assuming the gap away. Since then,
the sensor hardware turned out to be entirely different from the original
(UART module, not SPI MAX31865 - see Section 9 change log) and the feature
set has grown substantially beyond the original clone; **this file
(`AGENTS.md`) is the authoritative doc for this build**, not the upstream
README.

---

## 2. Hardware

| Part | Model / spec | Notes |
|------|--------------|-------|
| Machine | Gaggia Espresso Color | Aluminum boiler, vibration pump |
| Controller | **ESP32-S3-DevKitC-1 (N16R8)** | 16MB flash, 8MB OPI PSRAM, WiFi+BLE |
| Temp sensor | **PT100, 3-wire**, PTFE cable, M4 thread | Replaces the original boiler thermostat |
| Temp module | **UART PT100 + relay module** (silkscreen says "MAX31865" but it is NOT SPI — it's an ASCII AT-command UART board; see Section 3/4) | 9600 8N1, `AT+T` command |
| Heater switch | **Fotek SSR-DA** (DC control → AC load) + heatsink | Switches the boiler heater |
| PSU | **External USB wall-wart** (5V), routed into the case | Powers the ESP32; HLK-PM01 module dropped from the build 2026-08-14 |
| Wiring | Wago 221 (mains), silicone wire (18AWG power / 22AWG signal), Dupont, PTFE tape | |

---

## 3. Pin map (ESP32-S3-DevKitC-1)

Defined in [`include/config.h`](include/config.h). Chosen to avoid reserved S3
pins (USB `GPIO19/20`, flash `GPIO26-32`, OPI PSRAM `GPIO33-37`, strapping
`0/3/45/46`, RGB LED `48`).

| Function | GPIO |
|----------|------|
| SSR (heater) control | **4** |
| Temp module UART RX (ESP receives, module TX) | **18** |
| Temp module UART TX (ESP sends, module RX) | **17** |

The temp module also needs **3.3V** and **GND** from the DevKit (module is
5V-rated but works powered from 3V3 — see Section 9 change log for why).

### Temp module protocol (also in `config.h`)
- `SENSOR_BAUD = 9600` (8N1)
- Command: send ASCII `AT+T` terminated with **CRLF** (`\r\n`).
- Reply: `+T=<value>\r\n` then `OK\r\n` on success, `ERROR_1\r\n` on a bad/
  unrecognized command. `<value>` is the PT100 reading in °C.
- Driver: [`src/temp_sensor.cpp`](src/temp_sensor.cpp) / `include/temp_sensor.h`
  (`tempSensorInit()` / `tempSensorRead(float&)`). Full reverse-engineering
  process is in the Section 9 change log — read it before touching this
  protocol again, it explains several non-obvious parser quirks (the module
  is line-buffered, only processes on `\n`, and unterminated bytes from a
  prior send get prepended to whatever's sent next).
- A standalone diagnostic build (`env:esp32-s3-sniffer`, `src/sniffer.cpp`)
  and `tools/serial_capture.py` are kept in the repo for any future protocol
  spelunking on this or another UART module — see Section 5 for how to run
  them.

---

## 4. Software / architecture

- **Framework:** PlatformIO + Arduino (`pioarduino` platform fork).
- **Libraries:** `br3ttb/PID`, `tzapu/WiFiManager`, `knolleary/PubSubClient`.
- **Files:**
  - `src/main.cpp` — setup + main loop: reads the temp module every 250 ms,
    runs PID, drives the SSR with **1000 ms time-proportioned slow-PWM**.
    Implements the `OpMode` (`OFF`/`BREW`/`STEAM`) state machine (see
    "Brew/Steam mode architecture" below), the eco/auto-sleep timer, and the
    PID autotune relay-feedback state machine (see their own subsections).
  - `src/temp_sensor.cpp` / `include/temp_sensor.h` — UART driver for the
    temperature module (see Section 3).
  - `src/web.cpp` / `include/web.h` — `WebServer` on port 80, WiFiManager captive
    portal, `/status` JSON, `/update` settings, `/firmware`+`/update_fw` OTA,
    `/wifi_reset`, mDNS `gaggia.local`. The UI (`index_html`) is a
    self-contained dark-themed responsive dashboard (no external assets); it
    polls `/status` every 2 s.
  - `src/mqtt.cpp` / `include/mqtt.h` — MQTT + Home Assistant auto-discovery.
  - `include/config.h` — pins, sensor UART config, brew/steam gain-scheduling
    profile defaults, per-mode safety ceilings, eco-sleep default, autotune
    relay-feedback parameters.
  - `src/sniffer.cpp` — standalone UART protocol prober, NOT part of the main
    app (its own env, excludes main/web/mqtt). Kept for future diagnostics.
- **Persistence:** settings (per-mode target/Kp/Ki/Kd, MQTT, eco-sleep
  timeout) saved in NVS via `Preferences`. `OpMode` itself is NOT persisted —
  the board always boots into `OFF` regardless of what was active before a
  reset (safety: never auto-resume heating unattended).

### Brew/Steam mode architecture
- `enum class OpMode { OFF, BREW, STEAM }` (`config.h`). Each non-OFF mode has
  its own setpoint + PID gains ("gain scheduling", `main.cpp`:
  `brewSetpoint/brewKp/Ki/Kd`, `steamSetpoint/steamKp/Ki/Kd`) and its own
  safety ceiling (`BREW_MAX_SAFETY` (fixed) / `steamMaxSafety` (configurable,
  see below) → `activeMaxSafety`).
  `setOpMode()` applies the new profile and does a **bumpless PID reset**
  (cycle `MANUAL`→`AUTOMATIC`) so a big setpoint jump like brew→steam doesn't
  inherit a stale integral accumulator from the other regime.
- Mode is selected **purely from the Web UI** (`/update?mode=off|brew|steam`)
  — by explicit user decision, this does **not** read any physical
  brew/steam switch on the machine. No wiring changes to switches or
  thermostats were made or are planned for this feature.
- **`STEAM_SETPOINT_DEFAULT` and `STEAM_MAX_SAFETY_DEFAULT` (125°C, since
  2026-08-15 — was 135°C) in `config.h` are still PLACEHOLDERS**, not
  verified against this specific machine's real thermal limits.
  `steamMaxSafety` itself is now **configurable live from the Web UI**
  (Steam tuning card, persisted, clamped server-side to
  `[STEAM_MAX_SAFETY_MIN, STEAM_MAX_SAFETY_MAX]` = [100, 150]°C so a typo
  can't set a dangerous ceiling) — but the right *value* still depends on
  the still-open question in Section 7 step 4 (does the machine have an
  independent high-limit safety fuse, separate from the thermostats? what's
  the *actual* steam thermostat's rated trip point?).
  Treat any steam-mode test as attended/supervised until that's resolved.
- **Important unconfirmed assumption baked into this design:** the SSR is
  the sole heating actuator in every mode. Since the machine's original
  switch/thermostat wiring is untouched, whether "Steam" in the Web UI
  produces any real physical effect depends on the SSR genuinely being the
  only thing controlling the heater regardless of switch position — not yet
  confirmed (same open thermostat-photo dependency as above).

### Eco / Auto-Sleep
- `lastActivityTime` (`main.cpp`) advances only on an explicit `/update` call
  (mode change, tuning edit, wake) — deliberately NOT on passive `/status`
  polling, so a browser tab left open silently doesn't block sleep.
- `ecoTimeoutMin` (persisted, default 30, `0` = disabled): if this many
  minutes pass with no activity while `BREW`/`STEAM` is active,
  `checkEcoSleep()` force-switches to `OFF`, remembers the mode it interrupted
  (`modeBeforeSleep`), and sets `autoSleeping = true`. The Web UI shows a
  sleep banner + "Wake Up" button (`/update?wake=1` → `wakeFromSleep()`,
  which restores `modeBeforeSleep` and resets the activity timer).
- Autotune (below) is exempt from eco-sleep while running — they don't fight
  each other.

### PID Autotune (relay feedback / Ziegler-Nichols)
- Hand-built in `main.cpp` (`AutotuneState` enum in `config.h`) — no external
  autotune library; none was already a dependency, and the relay-feedback
  (Astrom-Hagglund) method is a well-established, moderate-complexity
  technique to implement directly.
- While `RUNNING`, it **owns `Output` directly** (PID is forced to `MANUAL`):
  toggles between `AUTOTUNE_RELAY_HIGH`/`_LOW` with `AUTOTUNE_HYSTERESIS_C`
  hysteresis around the active profile's setpoint, averages the resulting
  oscillation period/amplitude over `AUTOTUNE_MIN_CYCLES` cycles, then derives
  `Ku`/`Pu` → `Kp = 0.6 Ku`, `Ki = 1.2 Ku/Pu`, `Kd = 0.075 Ku Pu` (classic
  Z-N relay-tuning formulas; Ki/Kd are in the same per-second units
  `PID_v1::SetTunings()` already expects, matching how manual Web UI tuning
  has always worked). Result is applied to and persisted for whichever
  profile (Brew/Steam) was active when triggered.
- **Safety:** `runAutotuneStep()` independently re-checks sensor fault and
  `activeMaxSafety` every cycle (since bypassing `PID.Compute()` also
  bypasses its normal enforcement), plus a hard `AUTOTUNE_MAX_RUNTIME_MS`
  (30 min) abort ceiling. The loop's own unconditional safety-ceiling
  force-off check still applies underneath this regardless.
  **`AUTOTUNE_RELAY_HIGH` defaults to 500 (50% duty), not full power** —
  deliberately conservative for a real boiler's first autotune runs.
- Triggered via `/update?autotune=start` (Web UI: "Start Auto-Tune" button,
  with a confirm() prompt warning the heater will cycle repeatedly for
  several minutes) — **supervise the first run on real hardware.**
- **Stoppable, two ways** (`/update?autotune=stop` → `stopAutotune()`):
  (1) the same Web UI button turns into a red "Stop Auto-Tune" control while
  running; (2) clicking **any** mode button (Off/Brew/Steam) during an
  autotune run interrupts it first, before applying the new mode - this
  wasn't true in the first version shipped (mode changes didn't actually
  stop the relay-driven `Output`) and was fixed same-day before the first
  real-hardware autotune attempt.

### Interfaces
- **Web UI:** connect to the ESP's IP or `http://gaggia.local` — shows temp,
  target, output, mode (Off/Brew/Steam), eco-sleep status, autotune status,
  and live per-profile Kp/Ki/Kd + target editing.
- **Wi-Fi setup (captive portal):** on first boot it creates an AP
  **`GaggiaPID_Setup`** — join it and enter your home Wi-Fi credentials.
  **`/wifi_reset`** (Web UI "Reset WiFi Settings" button) clears stored
  credentials and reboots into this same captive portal on demand, without
  needing USB — reuses this existing, proven path rather than a second
  custom WiFi-config UI.
- **MQTT / Home Assistant:** optional, configured from the Web UI form.
- **OTA:** `http://<ip>/firmware` (upload form) → `/update_fw` (upload
  handler). **First real-world test (2026-08-15) initially failed silently**
  (empty HTTP response, board rebooted back into the still-working previous
  firmware — the ESP32 OTA partition scheme's safe fallback behavior when
  `Update.end()` doesn't complete cleanly, not a bricking risk). A second
  attempt succeeded. Root cause not confirmed (likely a WiFi hiccup or
  large-upload timing issue with the synchronous `WebServer` during the
  ~1.1MB transfer) — if OTA fails again, just retry; if it fails repeatedly,
  fall back to USB/serial flashing.

### Safety behavior (current)
- Safety ceiling is **mode-aware**: `BREW_MAX_SAFETY = 105.0°C`,
  `STEAM_MAX_SAFETY = 135.0°C` (placeholder, see "Brew/Steam mode
  architecture" above), tracked at runtime as `activeMaxSafety`. SSR is
  forced **OFF** on sensor fault (persisting > ~1 s), when temp ≥ the active
  ceiling, or when temp reads invalid (`-999`).
- Board always boots into `OpMode::OFF`, never resumes a previous mode.
- Brew PID default gains: `Kp=50, Ki=0, Kd=0`. Steam PID default gains:
  `Kp=30, Ki=0, Kd=0` — **both placeholders, must be tuned** (steam is a
  different thermal regime and will very likely need its own tuning pass).

---

## 5. Build / flash / monitor

PlatformIO Core is installed in a venv at
`C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe`.

```powershell
$pio = "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe"

# Build (first build downloads the S3 toolchain — several minutes)
& $pio run -e esp32-s3-devkitc-1

# Flash + monitor (board enumerates on the native-USB port)
& $pio run -e esp32-s3-devkitc-1 -t upload -t monitor
```

- **Board port:** ESP32-S3 native USB shows up as a `USB Serial Device`
  (VID:PID `303A:xxxx`). Last seen on **COM5**. Use the DevKit port labeled `USB`.
- Build flags enable native USB CDC (`ARDUINO_USB_CDC_ON_BOOT=1`) so Serial and
  flashing both use that port. If upload fails: hold **BOOT**, tap **RESET**,
  release **BOOT**.
- A second env `esp32-c6-devkitc-1` is kept in `platformio.ini` for reference.

---

## 6. Safety rules for agents (mains voltage!)

- This machine runs on **220V AC**. **Never** instruct the user to power the
  heater/SSR wiring until the firmware is verified on the bench over USB.
- **Bench-first workflow:** flash → verify Web UI + a sane temperature reading
  with the board on USB only (mains disconnected) → only then wire the SSR/heater.
- Do not weaken the overtemp cutoff or fault handling without explicit user
  approval and a clear reason logged below.
- **SSR line at boot:** `GPIO4` floats for a few ms during reset before
  `setup()` drives it `LOW`. Before wiring to mains, add a **~10k pull-down**
  from the SSR control pin to GND so the heater can't glitch on during boot.

---

## 7. Mains wiring plan (current phase)

> Active step-by-step plan for moving from bench-verified (USB only) to the
> machine wired to real mains. Follow Section 6's safety rules throughout.
> Update this section as steps complete; log outcomes in Section 10.

1. **Pull-down resistor on GPIO4 — SKIPPED, explicit user decision
   (2026-08-01).** User doesn't have a 4.7k-47k resistor on hand and accepted
   the risk after being told: the boot-glitch window is only a few ms
   (thermally/mechanically negligible for a boiler element built for
   sustained heating), but it recurs on **any** reset, not just first
   power-on (WiFi hiccup, watchdog timeout, brownout, OTA reboot) — so it's a
   small, recurring, unsupervised risk rather than a one-off. **Do not
   silently re-add this as a blocker without checking whether the user has
   changed their mind.**
2. **ESP32 power source — external USB wall-wart (reverted 2026-08-14,
   was internal HLK-PM01 2026-08-01→2026-08-14).** User decided against the
   HLK-PM01 integration. Power the ESP32 the same way as on the bench: a
   standard 5V USB charger/cable routed into the case through a grommeted
   hole, plugged into a regular wall outlet (can share a power strip with
   the machine, or be independent). Consequence: the ESP32 stays powered
   independently of the machine's own power cord — unplugging the machine
   does not power down the controller, only unplugging the USB wall-wart
   does. The HLK-PM01 module is no longer part of this build.
3. **Bench pre-wiring (before opening the machine):**
   - SSR: pigtail wires on the DC control terminals (→ GPIO4 + GND, 22AWG
     signal wire) and on the AC switched terminals (→ heater circuit, 18AWG
     power wire). Leave far ends unconnected/insulated until step 4 confirms
     routing.
   - Heat-shrink over all exposed SSR screw terminals as a first insulation
     pass.
   - Confirm the physical `GPIO4` pin location on the actual board against
     its silkscreen.
   - Identify where the USB cable will pass through the case (grommeted
     hole, away from heat/steam) for the wall-wart power.
4. **SUPERSEDED — no longer trying to understand or reuse the panel
   switch/thermostat wiring (decided 2026-08-13).** The full circuit
   (thermostats, Brew/Steam switches, LEDs, pump) turned out to be
   unverifiable — wiring disappears into a bundle underneath that the user
   cannot see, trace, or test (no multimeter access, no visual access to the
   bundle). Rather than build mains wiring on unverified guesses, the plan
   now uses a **clean-bypass heater circuit** (step 6) that doesn't depend
   on any of that wiring at all. See "Heater-circuit wiring — final approach"
   below. The historical investigation is kept further down for context but
   is **no longer a blocker** and no longer needs resolving.
5. **Boiler mechanical prep** (machine unplugged from mains): remove the
   **brew** thermostat (the lower-temperature one) from the M4 boiler port,
   thread the PT100 probe into that same port. Only its physical *location*
   matters here — its wiring doesn't need to be traced, just capped/retired,
   since the clean-bypass circuit doesn't reuse it.
6. **Heater/SSR wiring — clean bypass, does not depend on the old panel
   wiring at all.** See "Heater-circuit wiring — final approach" below for
   the full circuit. In short: disconnect the heater element AND the Steam
   Thermostat (T2) from whatever they're currently wired to, and feed the
   heater fresh from mains through T2 (reused as the overheat cutoff,
   re-terminated with new wires) and the SSR. The UART temp module's own
   onboard relay is unrelated to any of this — it stays unwired; only used
   for the PT100/UART reading.
7. **USB power routing**: route the USB cable/wall-wart into the case per
   step 3, plug into the ESP32's USB-C port.
8. **Reassembly & insulation check**: Wago connectors/heat-shrink on every
   mains joint, full visual inspection, machine still unplugged.
9. **First live power-up**: only then plug into mains. Watch the Web UI
   temperature climb, confirm the safety cutoff and fault handling behave,
   stay ready to unplug immediately if anything looks off.
10. **PID tuning**: once heating for real, tune the placeholder gains
    (Kp=50, Ki=0, Kd=0) against the boiler's actual thermal response.

### Heater-circuit wiring — final approach (decided 2026-08-13, revised 2026-08-14)

The panel wiring below (thermostats, Brew/Steam switches, LEDs, pump) turned
out to be **unverifiable** — it disappears into a bundle underneath that the
user has no visual or electrical (no multimeter) access to. Rather than keep
trying to resolve that ambiguity, the design **doesn't depend on any of it**.
Only three points in the whole machine are needed:

1. **The incoming mains terminal block** — where the wall power cord lands,
   before any switch or thermostat. The one place "which wire is mains" is
   never in question.
2. **The boiler heating element's own two terminals** — a standalone,
   physically distinct component, not something requiring tracing through
   any switch.
3. **Thermostat 2's ("steam") own two terminals** — also a standalone,
   physically distinct component mounted on the boiler. Its *existing*
   wiring into the ambiguous bundle is irrelevant and gets disconnected;
   only the component itself (re-terminated with fresh wires) is reused, as
   the overheat cutoff.

**Circuit (revised 2026-08-14 — reuses T2 instead of a new thermal fuse):**
```
Mains (Line) -- Thermostat 2 (reused, re-terminated with new wires) -- SSR terminal 1
SSR terminal 2 -- Heater element terminal A
Heater element terminal B -- Mains (Neutral)
SSR terminal 3 (+) -- ESP32 GPIO4          SSR terminal 4 (-) -- ESP32 GND
```
- Both the heater element's AND Thermostat 2's existing wires (wherever
  they currently go into the ambiguous bundle) are disconnected/capped —
  retired, not reused. Only the two components themselves are kept, wired
  fresh directly into the new circuit.
- **Why reuse T2 instead of buying a new thermal fuse** (2026-08-14 change
  of mind from the original plan): user wants real overheat protection and
  prefers reusing hardware that's already correctly mounted on the boiler
  over sourcing a new part. This works without reopening the wiring-
  ambiguity problem specifically because T2 is a discrete, physically
  identifiable component — its own two terminals are visible on the part
  itself, independent of whatever the rest of the bundle does.
- T2's exact rated trip temperature is still unknown (nice to confirm if
  visible on the part, but not blocking — it's presumably in the normal
  steam-thermostat range, i.e. comfortably above both brew and the current
  placeholder `STEAM_MAX_SAFETY`).
- Wire spec unchanged from Section 3/5: 18AWG silicone + Wago 221 on the
  mains side, 22AWG + Dupont on the control side.
- **Untouched by this change:** the pump, the Brew/Steam switches, and the
  panel LEDs keep doing whatever they currently do — none of it is part of
  this circuit anymore. Any LED that was specifically wired to reflect the
  old thermostats' state will simply go dark (expected, not a fault) —
  acceptable since Web-UI-only monitoring was already the explicit decision
  for steam-state feedback.

**How to do it, step by step:**
1. Unplug the machine from mains. Don't touch anything inside until confirmed unplugged.
2. Locate: the incoming mains terminal block (where the power cord's Line/Neutral land, before any switch), the boiler heating element's own two terminals, and Thermostat 2's own two terminals — all standalone, physically distinct components.
3. Disconnect the heater element's AND Thermostat 2's existing wires from wherever they currently attach. Cap each freed wire end (wire nut or heat-shrink + tape) so it can't short against anything — the old wiring is retired, only the two components themselves are reused.
4. Wire the mains side with 18AWG silicone + Wago 221 at every joint: Mains Line → Thermostat 2 (new wires) → SSR terminal 1; SSR terminal 2 → heater terminal A; heater terminal B → Mains Neutral.
5. Wire the control side with 22AWG + Dupont: SSR terminal 3 (+) → ESP32 GPIO4; SSR terminal 4 (−) → ESP32 GND.
6. Route the USB power cable into the case (grommeted hole, away from heat/steam) to the ESP32's USB-C port; plug the wall-wart end into a wall outlet.
7. Heat-shrink/insulate every exposed mains terminal (SSR AC side, Thermostat 2's new terminals, heater terminals, Wago connectors) before closing the machine back up.
8. Full visual inspection: no exposed conductors, no wires under strain or touching the metal chassis, nothing pinched by the case.
9. Reassemble.
10. First power-up (Section 7 step 9): plug into mains, confirm the Web UI shows a live reading immediately, briefly test Brew mode and watch the heater/temperature respond as expected — stay by the machine, ready to unplug immediately if anything looks, sounds, or smells wrong.
11. Only after a clean supervised first run, move to PID tuning (step 10).

### Heater-circuit wiring investigation (historical, no longer blocking)

> Kept for context only — **superseded by "final approach" above, do not
> use this for actual wiring.** This is the user's own text description of
> the panel wiring, recorded as-is for continuity — **not confirmed**, and
> specifically flagged by the user as having at least one likely error (see
> below).

**As described (two separate messages, not yet reconciled with full
confidence):**
- **Thermostat 2 ("steam"):** Side 1 → steam button/switch AND Thermostat 1;
  Side 2 → described as "mains power."
- **Thermostat 1 ("brew"):** Side 1 → an indicator LED AND directly to the
  boiler element; Side 2 → Thermostat 2's Side 1.
- **Boiler element:** powered from Thermostat 1's Side 1, and from "mains"
  on its other terminal.
- **Steam Switch:** wire → Steam LED; wire → Brew Switch; wire (light grey)
  → Steam Thermostat.
- **Steam LED:** wire → Steam Switch; wire (red) → Brew Thermostat.
- **Brew Switch:** wire → Brew LED; wire → Steam Switch; wire (white) →
  "routed downwards, likely the pump"; wire → described as "mains power."
- **Brew LED:** wire → Brew Switch; wire (blue) → "routed downwards to
  mains power."

**Known error in the above:** the user flagged that essentially every wire
described as going to "mains power" is uncertain — those wires are bundled
with the pump's wiring out of sight, so any of them could actually terminate
at the **pump** instead of at mains. This affects at least Thermostat 2's
Side 2, the Boiler element's other terminal, Brew Switch wire 4, and Brew
LED wire 2 — i.e., most of the claimed mains connections in this whole
writeup are suspect.

**Two more open questions from the agent's read of this, unresolved:**
1. Is the Steam Switch upstream or downstream of Thermostat 2? (Working
   assumption, unconfirmed: downstream — it just gates whether T2's already
   thermostat-controlled output continues on to the Steam LED, since T2.Side
   2 supposedly already has constant "mains" presence independent of any
   switch.)
2. Is the Steam LED in series (carrying real current toward the heater) or
   a parallel indicator (far more common in appliances — bridges two points
   just to show "this section is powered")? Text description doesn't
   settle this.

**No further action needed on this** — the "final approach" above sidesteps
the whole ambiguity instead of resolving it. Only revisit this if a future
agent/user decides to reuse the panel wiring after all (e.g. to restore the
Steam LED's function, or to make Thermostat 2 do real work again).

---

## 8. Roadmap / future work

- **Tune PID gains** for the Gaggia aluminum boiler — brew is largely dialed
  in (see Section 10 change log), steam still needs real testing.
- **Physical local display (in progress, separate from the phased roadmap
  below):** Nextion touchscreen HMI chosen over OLED/color-TFT — see
  Section 10 for the decision and procedure. Will need its own `nextion.cpp`
  driver once hardware is in hand, following the same "external component
  over UART" pattern as `temp_sensor.cpp`.

### Competitive research (2026-08-16)

Before committing further to the phased plan below, researched two mature
open-source projects solving the same problem — **Gaggiuino**
([github.com/Zer0-bit/gaggiuino](https://github.com/Zer0-bit/gaggiuino),
2,600+ stars) and **GaggiMate**
([github.com/jniebuhr/gaggimate](https://github.com/jniebuhr/gaggimate),
~900 stars) — plus the actual coffee-science parameters behind espresso/
cappuccino extraction (SCA guidelines, Barista Hustle, etc.). Findings that
change/confirm the plan:

- **Both use a pressure transducer at the pump outlet, 0-1.2 to 1.6MPa
  range** (12-16 bar) — concrete spec confirmation for Phase 1 below.
- **Neither uses a dedicated flow sensor** — flow is estimated from pressure
  + pump behavior, not physically metered. Simplifies Phase 2 - no separate
  flow meter to source.
- **Both structure pressure/flow profiles as ordered phases** (pump power/
  pressure/flow target + time/volume/pressure stop conditions per phase),
  editable on the **web UI, not the on-device touchscreen** - independently
  validates the exact call already made for the Nextion display (deep
  config stays on phone/web, screen is for live status + basic control).
- **GaggiMate's software autotune is reported buggy** (e.g. returning
  `0,0,0,0`, GitHub #672). Worth noting: **our own hand-built relay-feedback
  autotune worked cleanly on the first real-hardware run** (Section 10,
  2026-08-15) - a genuine point of confidence in this project's approach.
- **GaggiMate's confirmed BLE scale list** includes Bookoo, Felicita, Acaia,
  Decent, and others - reconfirms Bookoo/Felicita as the DIY-friendliest
  choices already favored in Phase 3 below.
- **Both use a K-type thermocouple, not PT100** - this project intentionally
  stays on PT100 (already working, more stable at espresso temps); not a
  reason to switch, just noting the divergence.
- **Coffee science take**: 9 bar is a historical convention, not a physical
  optimum - real "pressure profiling" is about **pre-infusion** (a gentle
  low-pressure soak before ramping up, reduces channeling) and sometimes a
  **declining pressure curve** near the end of a shot, not "hold flat at 9
  bar." This is exactly why Phase 2 below is phase-based, not a single fixed
  target. Brew-by-weight is also confirmed as more repeatable than
  brew-by-time (weight is the actual outcome; time is a proxy confounded by
  grind/dose/tamp) - reinforces Phase 3's priority.
- **A genuine gap in both competitors**: neither addresses **milk
  temperature for steaming/cappuccino** at all - they focus entirely on
  brew pressure/flow. Real dairy science says ideal steaming range is
  ~55-65°C with ~70°C as a hard scald/foam-collapse ceiling. Added as a new
  near-term item below since it's cheap, low-complexity, and a real
  differentiator.

### Phased future roadmap, sequenced by risk/dependency

#### Can be done now — pure software, zero new hardware
1. **Shot timer** - elapsed time display during Brew mode (target window is
   ~25-30s per shot, per SCA-referenced extraction guidelines).
2. **Shot history log** - duration + peak temp per shot now; extends
   naturally once pressure/weight data exist later. Both competitors treat
   this as core.
3. **Descale / maintenance reminder** - shot count or days-since-reset,
   surfaced in the Web UI (matches Gaggiuino's "Service Log").

#### Needs new hardware, but cheap + low-voltage (no mains wiring)

4. **Milk temperature probe for cappuccino/steaming.**
   **Hardware:** one waterproof **DS18B20** digital temp probe (~$2-3),
   one free GPIO (OneWire protocol, needs a 4.7kΩ pull-up resistor). Dipped
   into the milk pitcher during steaming.
   **Why:** neither competitor addresses milk temperature at all - real
   dairy science says the sweet spot is ~55-65°C with ~70°C as a hard
   scald/foam-collapse ceiling. Cheapest, simplest hardware item on this
   whole list, and a genuine differentiator.

5. **Water tank level sensor.**
   **Hardware:** one magnetic float switch (~$2-5), one free GPIO (digital
   input, internal pull-up).
   **Why:** low-water warning now; becomes a real pump interlock once
   item 9a (pump on/off control) exists.

6. **Real-time pressure transducer + live pressure graph.**
   **Hardware:** one analog pressure transducer, **0-1.2 to 1.6MPa
   (12-16 bar) range** (confirmed spec - matches both competitor projects),
   plumbed at the **pump outlet** via a T-fitting (a real plumbing job, not
   just wiring) - roughly $15-30 for the sensor itself. Wired to an **ADC1**
   pin specifically (ADC2 is WiFi-contaminated, same lesson as elsewhere in
   this project).
   **Why:** the prerequisite for any real pressure profiling in item 9, and
   useful as a pure monitoring/graph feature even before that exists.

7. **Automatic shot start/stop detection (sense-only).**
   **Hardware:** a non-invasive AC current-transformer clamp (e.g. SCT-013,
   ~$3-5) around the pump's own wire, read via one ADC1 pin - no mains
   wiring touched at all, since it only senses the magnetic field around an
   existing conductor. Detects "pump is drawing current" i.e. the physical
   Brew switch is on.
   **Why:** today items 1-3 (shot timer/history/descale count) start and
   stop from a manual Web UI button, because the ESP32 has no visibility
   into the machine's own Brew switch (left untouched by design - the panel
   wiring couldn't be fully verified without a multimeter, see Section 7).
   This closes that gap without any of the risk of tapping the switch's own
   (mains-carrying) wiring directly. Sense-only - does not let the ESP32
   control the pump, just observe it. Full active control (auto-*stop*, not
   just auto-*detect*) needs item 9a - though note item 9a keeps the
   physical Brew switch as the *start* trigger, so this item's detection
   isn't strictly required for auto-stop to work, only for a fully
   button-free shot.
   **Decision (2026-08-16): not needed for the current goal** (stop at 25s
   or at 36g). Weight-based stop needs no start-time reference at all
   (firmware just watches the scale continuously); time-based stop gets its
   start reference from the existing manual "Start Shot" Web UI button, no
   new hardware required. This item would only remove that one manual tap.
   Full reasoning in `HARDWARE_ROADMAP.md` item 7.

#### Needs new hardware, wireless purchase (no wiring at all)

8. **Bluetooth smart scale + brew-by-weight auto-stop.**
   **Hardware:** buy the scale itself - **Bookoo Themis or Felicita Arc**
   confirmed as the DIY-friendliest choice (open protocols, both appear in
   GaggiMate's supported-scale list; Acaia/Decent use different, often
   community-reverse-engineered protocols, so "any BLE scale" isn't
   realistically one firmware). No new ESP32-side hardware - BLE is already
   built into the S3.
   **Why:** brew-by-weight is confirmed more repeatable than brew-by-time
   (weight is the actual outcome; time is a proxy confounded by grind/dose/
   tamp). Pairs naturally with item 1's shot timing once both exist. Reading
   a weight is not the same as *acting* on it - actually cutting the pump at
   a target weight still needs item 9a's active pump control.

#### Needs new hardware, a second real mains-voltage subsystem (serious)

**Split 2026-08-16** (was one combined "item 9") into an easy on/off half and
a harder profiling half, since they have very different complexity and
dependencies. Full buy lists, wiring diagrams, and step-by-step procedures
for both in [`HARDWARE_ROADMAP.md`](HARDWARE_ROADMAP.md) (which also covers
items 4-8 above in the same buy-list/wiring format).

9a. **Simple on/off pump control ("control the button").** **Decided:** the
    physical Brew switch keeps starting the pump exactly as today; the
    ESP32 sits between the switch and the pump (spliced at the pump's own
    terminals, not the switch's terminal block, to avoid reopening the
    unverified panel-wiring problem from Section 7) so firmware can cut it
    early. **Hardware (revised 2026-08-16): a plain electromechanical relay
    module, not an SSR** - the pump switches once per shot, so relay
    contact-life/cycle-count is a non-issue, and SSR's silent/no-wear
    advantages don't matter here the way they do for the constantly-cycling
    heater; a relay is also cheaper and more available. No zero-cross
    detection, no phase-angle timing either way, firmware-trivial
    (`digitalWrite`, like `PIN_SSR`). **Uses the relay's NC (Normally
    Closed) contact, not NO** - de-energized/pass-through is the default,
    so the switch controls the pump with zero ESP32 involvement unless
    firmware actively energizes the relay to interrupt it; this is the
    mirror image of the heater's "always boot off" rule, and deliberately
    so - the dangerous failure mode for the pump is being unable to start
    without the ESP32's cooperation, not the reverse. Full reasoning in
    `HARDWARE_ROADMAP.md` item 9a. **Why:** turns auto-stop by a
    configured **time** (no other hardware) or **weight** (with item 8) into
    real closed-loop features, without needing item 9b's much harder
    profiling work first. Judged the easier of the two halves.

9b. **Phase-control dimmer to a pressure target (e.g. 9 bar).** **Hardware:**
    a zero-cross detection + TRIAC module (e.g. RobotDyn AC dimmer,
    ~$10-15), swapped in at the same splice point as item 9a - **the same
    category of seriousness as the original SSR/heater build**, bench-test
    on low voltage first, verify zero-cross/firing logic thoroughly before
    ever connecting the pump's real AC line, insulate every mains joint. No
    dedicated flow sensor needed (see competitive research above - both
    competitors estimate flow from pressure + pump behavior instead).
    **Hard prerequisite: item 6 (pressure transducer)** - you cannot
    regulate to a bar target without a pressure reading to control against;
    this wasn't a strict dependency in the old combined item 9, but is once
    "control to 9 bar" is the explicit goal rather than open-loop dimming.
    **Why:** enables a phase-based profile system (pump power/pressure/flow
    target + time/volume/pressure stop conditions per phase, matching both
    competitor projects' architecture) - real pre-infusion and
    declining-pressure-at-shot-end, not a flat 9-bar target, per the
    pressure-profiling research above.

   **Migrating to [GaggiMate](https://github.com/jniebuhr/gaggimate) instead
   of building this: ruled out (2026-08-16).** Checked their actual source,
   not just the README - `lib/GaggiMateController/src/ControllerConfig.h`
   hardcodes GPIO pinouts for six specific PCB revisions, autodetected via a
   voltage divider that only exists on their board. Temperature sensing is
   MAX31855 thermocouple (SPI) or NTC thermistor only (`peripherals/`) - no
   driver for this project's UART AT-command PT100 module. Pressure/dimming
   need their Pro PCB specifically. It's also two ESP32s talking BLE (a
   separate LilyGo T-RGB display board + their controller board), not one.
   So this is a **hardware swap** (buy their PCB, rewire, replace the
   sensor) and not a firmware migration onto the current board - explicitly
   decided against. Item 9 stays a from-scratch build on this hardware.

---

## 9. Current status

- ✅ Cloned + retargeted C6 → ESP32-S3-DevKitC-1 (N16R8).
- ✅ **First S3 build passes** (RAM 15.7%, Flash 56.4% of the 1.97MB app partition).
- ✅ **Flashed & verified on the board** (bench/USB only). Chip: ESP32-S3 rev v0.2,
  16MB flash, 8MB PSRAM, MAC 48:27:e2:ed:f6:30.
- ✅ **Boot confirmed** — the `GaggiaPID_Setup` Wi-Fi AP appears, so `setup()` ran
  through `setupWeb()`/WiFiManager successfully.
- ✅ **Connected to home Wi-Fi + Web UI verified.** `http://gaggia.local/status`
  returns 200 with live JSON (target 93, Kp 50, mode heat).
- ✅ **Sensor hardware turned out to be a UART AT-command module, not SPI
  MAX31865** (silkscreen is misleading) — protocol reverse-engineered on the
  bench: `AT+T\r\n` → `+T=<value>\r\nOK\r\n`. Full process in Section 10.
- ✅ **Firmware rewritten** (`src/temp_sensor.cpp`) to read the module over
  UART (GPIO18/17, 9600 8N1) instead of SPI; `main.cpp`/`config.h` updated;
  MAX31865 library dependency removed from the S3 env. PID/fault-handling/
  safety-cutoff logic unchanged.
- ✅ **Real PT100 reading confirmed on the bench**: ~24.7 °C at room temp,
  moving slightly as expected (not a stuck fault value).
- ✅ **Main firmware flashed with the real UART sensor driver, verified
  end-to-end via the Web UI** (`http://gaggia.local/status` → live `temp`
  matching the bench reading).
- ✅ **Sensor smoothing (EMA), fault-type distinction, temp history sparkline,
  fault banner, last-updated indicator, and PWA icon/manifest** added to the
  Web UI and backend, verified end-to-end.
- ✅ **Brew/Steam mode support added**: `OpMode` (OFF/BREW/STEAM) state
  machine with gain-scheduled PID profiles and mode-aware safety ceilings,
  purely Web-UI-controlled (no physical switch wiring). Verified end-to-end
  on the bench (full off→brew→steam→off cycle via `/update?mode=...`,
  correct setpoint/output per mode). Steam gains/ceiling are placeholders —
  see "Brew/Steam mode architecture" in Section 4.
- ✅ **Machine wired to real mains and live-heating verified** (Section 7's
  clean-bypass circuit: mains → reused Thermostat 2 → SSR → heater; USB
  wall-wart powers the ESP32). First live brew-mode test confirmed real
  temperature rise/overshoot behavior end-to-end on the actual boiler.
- ✅ **PID tuning in progress via live testing** — iterative manual
  adjustments against the real boiler (not just bench simulation) have
  already cut brew overshoot from ~1.8°C (default `Kp=50,Ki=0,Kd=0`) down to
  ~0.6-0.7°C. Current live values as of 2026-08-15: brew
  `target=92, Kp=5, Ki=0, Kd=2`; steam `target=115, Kp=40, Ki=0.10, Kd=60` —
  **still being refined, not final.**
- ✅ **Eco/Auto-Sleep, PID Autotune (relay feedback), and WiFi-reset added**
  (see Section 4 subsections) — built, flashed via a real **OTA update over
  WiFi** (first attempt failed safely and fell back to the previous
  firmware, second attempt succeeded — see the OTA note under Interfaces),
  and verified live: new `/status` fields present, all previously-tuned
  brew/steam values survived the update intact (NVS persists independently
  of the firmware binary).
- ⬜ Continue PID tuning (brew nearly there; steam largely untested at real
  steam temps — remember `STEAM_MAX_SAFETY`/steam gains are still
  placeholders per Section 4).
- ⬜ Autotune not yet run on the real machine — first run should be
  supervised (Section 4, "PID Autotune").

> Serial-monitor note: the S3 USB-Serial/JTAG can drop the CDC link on `RST`, so
> the boot banner is often missed by an already-attached monitor. Use the
> `GaggiaPID_Setup` AP as the proof-of-boot signal instead.

> Note: PlatformIO reports the board as N8/8MB by default; the `min_spiffs.csv`
> 4MB partition layout is used. Fine for now; revisit for a 16MB layout if more
> flash/OTA space is needed.

---

## 10. Change Log

### 2026-08-16 — Claude Code (Sonnet 5) — Competitive research (Gaggiuino/GaggiMate) + coffee science; roadmap refined
- **Researched two mature sibling open-source projects** (parallel background
  agents): [Gaggiuino](https://github.com/Zer0-bit/gaggiuino) (2,600+ stars)
  and [GaggiMate](https://github.com/jniebuhr/gaggimate) (~900 stars), plus
  the actual coffee-science parameters behind espresso/cappuccino extraction
  (SCA guidelines, Barista Hustle, dairy-science sources on milk texturing).
  Full findings recorded in Section 8's new "Competitive research" subsection
  - key takeaways: both use a 0-1.2/1.6MPa pressure transducer at the pump
  outlet with **no dedicated flow sensor** (estimated instead), both
  architect profiles as ordered phases edited on a **web UI** (not the
  on-device screen - validates the Nextion design already chosen), and
  GaggiMate's own autotune is reported buggy where **ours already worked
  cleanly on the first real-hardware run**. Coffee science confirmed 9 bar
  is a historical convention (not a physical optimum - real technique is
  pre-infusion + declining pressure curves) and brew-by-weight beats
  brew-by-time for repeatability. Found a genuine gap neither competitor
  covers: milk temperature for steaming/cappuccino.
- **Rewrote Section 8 (Roadmap)** around this research, reorganized
  explicitly by hardware cost/complexity tier: pure-software items (shot
  timer, shot history, descale reminder), cheap low-voltage sensors (milk
  temp probe - new item, water level, pressure transducer with confirmed
  spec), wireless-only (BLE scale - Bookoo/Felicita reconfirmed as the
  DIY-friendly choice), and the one remaining mains-voltage subsystem (pump
  dimmer, now explicitly phase-based per both competitors' architecture).
- **Mirrored a condensed version into `README.md`'s Roadmap section**,
  organized the same way (by hardware tier) for public-facing readers, with
  a pointer back here for full sourcing/reasoning.
- Documentation only in this entry - no firmware changes. **Not yet
  committed/pushed to the public repo** - ask before doing so.

### 2026-08-15 — Claude Code (Sonnet 5) — Project renamed to GaggiaBrewMasterESP; LICENSE added; README rewritten
- **Renamed the project from the working name "Gaggia PID" to
  "GaggiaBrewMasterESP"** - user's own choice, made after noting "PID"
  undersold the scope once eco-sleep/autotune/OTA/WiFi-management/planned
  display work started. Updated: this doc's title/Section 1, the Web UI's
  `<title>`, H1, PWA manifest `name`/`short_name`, `apple-mobile-web-app-title`,
  and the WiFi setup AP name (`GaggiaPID_Setup` → `GaggiaBrewMasterESP_Setup`).
  **Deliberately did NOT change the mDNS hostname** (`gaggia.local` stays as
  the actual URL) - that's a practical-continuity call, not an oversight;
  don't "complete" the rename by changing it later without asking.
- **Local project folder rename to `gaggia-brew-master-esp` is INCOMPLETE** -
  blocked by Windows reporting the directory in use (something has a handle
  open inside it, likely the user's own editor). Deferred; either retry once
  it's confirmed closed, or the user renames it manually via Explorer. Not a
  functional blocker for anything else.
- **Checked the actual legal status of reusing/publishing this before doing
  any of the above**: the upstream `petoz/gaggia_pid_esp32-c6` README states
  "MIT License" in text, but has **no actual LICENSE file** - confirmed via
  `gh api repos/petoz/gaggia_pid_esp32-c6` showing `license: null` and a 404
  on `/contents/LICENSE`. Given how far this project has diverged (different
  sensor hardware entirely, full feature set built on top), concluded it's
  legitimate to publish as the user's own public project, provided proper
  attribution + a real LICENSE file - which is exactly what was done:
  **added a formal MIT `LICENSE` file** (crediting the origin project in its
  footer) rather than leaving the same gap the upstream repo has.
- **Fully rewrote `README.md`** (previously stale - still described the
  original C6/SPI-MAX31865 hardware, never updated for this project's actual
  UART sensor or any feature built since). New version covers: what it does,
  a hardware table, the heater-circuit wiring summary + safety section,
  build/flash/first-WiFi-setup steps, a usage walkthrough of every Web UI
  feature, a developer-facing architecture summary, the full phased roadmap,
  and project history/credits - all pointing back to `AGENTS.md` as the
  authoritative deep-technical source, not duplicating it.
- No firmware behavior changes beyond the branding strings already pushed
  live via OTA in the same session (see the entry below/this session's
  earlier work) - this entry is documentation + licensing only.

### 2026-08-15 — Claude Code (Sonnet 5) — First real autotune run; tuning-field refresh fix
- **First-ever PID autotune run on the actual machine, Brew profile**:
  `Ku=58.86, Pu=60.0s → Kp=35.32, Ki=1.1782, Kd=264.68`. Applied and
  persisted automatically per the existing design; **user reviewed and
  explicitly asked to keep these values as-is** - do not revert or "fix"
  them without being asked again.
- **Worth future agents watching for**: `Kd=264.68` is notably higher than
  the `Kd=180` that caused the settle-below-target stall diagnosed earlier
  in manual tuning (at a similar `Kp≈35`). Live data right after this
  autotune run showed the opposite symptom instead - overshoot to ~94.2°C
  against a 92°C target (~2.2°C, worse than the best manual result of
  ~0.6-0.7°C) - plausibly from the much larger `Ki=1.18` (vs `0-0.1`
  previously) causing integral windup that outweighs the larger Kd's
  damping. Not reverted since the user hasn't asked to and wants to observe
  it further - but if a stall or excessive oscillation shows up on a later
  brew cycle, this combination is the first thing to suspect.
- **Fixed a real UX gap**: after autotune completes, the Kp/Ki/Kd *input
  boxes* in the tuning form didn't refresh to show the new values (only the
  status message text did) - because `setVal()`'s "sync once per page load"
  guard (which protects a field being actively edited from being clobbered
  by the next `/status` poll) also blocked it from ever picking up
  autotune's changes. Fixed with a one-shot force-write the moment
  `autotune_state` transitions to `done_ok` (tracked via `lastAutotuneState`
  in the page's JS), targeting whichever profile (`brew`/`steam`) was
  active when autotune ran.
- Pushed live via OTA, verified present in the served page and via a real
  autotune-completion `/status` readout.

### 2026-08-15 — Claude Code (Sonnet 5) — Fixed long-standing "output %" display bug; finer Ki step
- **User caught a real bug**: the "Heater output" percentage (and its
  progress bar) has displayed the raw `Output` value (0-1000 scale, ms
  within the 1000ms SSR window) with a bare `%` appended, instead of
  converting to an actual 0-100% duty cycle - i.e. every duty percentage
  ever shown in this UI has been **10x too high** (e.g. an earlier
  `output:75.11` reading displayed as "75%" was really 7.5% duty). Autotune
  driving `Output` to 500 (its 50% relay level) made this impossible to miss
  ("500%"). This bug predates all of this session's work - inherited from
  the original cloned project, never previously noticed.
- **Fixed** in `web.cpp`'s status-poll JS: introduced `outputPct = output /
  10`, used for both the numeric display and the output bar's width.
- **Loosened the Ki input step** from `0.1` to `0.01` on both Brew and Steam
  tuning forms - browsers enforce `step` strictly on `<input type=number>`,
  so `0.1` silently rejected values like `0.05`. Kp/Kd left at `0.1` (not
  reported as an issue, and their typical magnitudes don't need finer
  granularity the way Ki's small values do).
- Pushed live via OTA, verified: board rebooted cleanly, all tuned values
  intact, both fixes confirmed present in the served page.

### 2026-08-15 — Claude Code (Sonnet 5) — Configurable steam safety ceiling; autotune stop fix; button polish
- **`STEAM_MAX_SAFETY` changed from a fixed `#define` (135°C) to a live-
  configurable `steamMaxSafety` variable, default 125°C** (`config.h`:
  `STEAM_MAX_SAFETY_DEFAULT`/`_MIN`/`_MAX` = 125/100/150). Persisted in NVS,
  editable from the Steam tuning card, clamped server-side to [100,150]°C so
  a typo can't set a dangerous ceiling. `BREW_MAX_SAFETY` stays a fixed
  constant (not asked to change).
- **Found and fixed a real gap before the first hardware autotune attempt**:
  the initial autotune implementation had no way to stop it once running -
  clicking Off/Brew/Steam mid-run didn't actually interrupt the relay-driven
  `Output`, since `runAutotuneStep()` took priority unconditionally in
  `loop()`. Fixed by exposing `stopAutotune()` and calling it from the
  `/update` handler (a) whenever `autotune=stop` is sent explicitly, and (b)
  automatically whenever any `mode` change is requested while autotune is
  `RUNNING`, before the new mode is applied. Web UI button now flips to a
  red "Stop Auto-Tune" state while running instead of just disabling.
- **Restyled the autotune button** - was a small, plain, easy-to-miss
  control ("hangs there" per user feedback); now a full-width gradient
  button matching the app's existing submit-button visual language, with a
  lightning-bolt icon, its own separated section (divider) in the Boiler
  card, and centered status text below it.
- All changes pushed live via OTA (no USB) - board remained reachable
  throughout, all previously-tuned brew/steam values and the new
  `steam_max_safety` survived each update.

### 2026-08-15 — Claude Code (Sonnet 5) — Live mains heating + PID tuning; Eco-Sleep/Autotune/WiFi-reset added; first OTA update
- **Machine now actually wired to mains and heating for real** — the
  clean-bypass circuit from the previous entries (mains → reused Thermostat
  2 → SSR → heater; USB wall-wart for the ESP32) is built and working. First
  live brew-mode test (target 45°C, default `Kp=50,Ki=0,Kd=0`) showed a
  real, expected 1.8°C overshoot from thermal lag - not a bug, confirmed by
  `output:0` already at the moment temp crossed target. This kicked off
  genuine iterative PID tuning against the real boiler across several
  rounds, each checked via live `/status` polling:
  - Added `Kd=150` → overshoot dropped to ~0.6-0.7°C at a 48°C target -
    confirmed the derivative term anticipating the thermal lag works as
    expected.
  - Raising target to 50°C with `Kd=180` **stalled the approach** (settled
    below target and started drifting back down with `output` still low,
    despite positive error) - diagnosed as Kd too high relative to Kp,
    over-damping the climb. Backed off to `Kd=90`.
  - As of this entry, live values: brew `target=92, Kp=5, Ki=0, Kd=2`; steam
    `target=115, Kp=40, Ki=0.10, Kd=60` - **user-driven, still being
    refined, not a final recommendation.** Future agents: don't assume
    these are correct without checking current `/status` first.
  - General lesson worth keeping: with `Ki=0`, `output` reaching exactly 0
    the instant temp crosses setpoint means the *controller* did the right
    thing - any overshoot after that point is the physical boiler's thermal
    lag, and the fix is `Kd`, not blaming the P-term or panicking.
- **Added three requested features, all directly in the existing codebase**
  (synchronous `WebServer` + `PID_v1` + the UART sensor module - deliberately
  NOT switched to `AsyncWebServer`/`ElegantOTA`/a `PID_AutoTune` library
  despite a couple of prompts suggesting that stack, since none of it
  matches what's actually built and verified here):
  - **Eco/Auto-Sleep**: `ecoTimeoutMin` (persisted, default 30 min, 0
    disables), activity tracked only on explicit `/update` calls (not
    passive `/status` polling) so a browser tab left open doesn't block
    sleep. Forces `OFF` on timeout, remembers the interrupted mode, Web UI
    shows a sleep banner + "Wake Up" button. See Section 4.
  - **PID Autotune**: hand-built relay-feedback (Ziegler-Nichols) autotuner,
    not a library integration - full design in Section 4. Bypasses
    `PID.Compute()` while running (owns `Output` directly) but independently
    re-checks the safety ceiling and sensor fault every cycle, plus a hard
    30-minute abort cap. **Not yet run on the real machine** - next agent/
    user should supervise its first live use closely.
  - **WiFi reset**: `/wifi_reset` clears credentials and reboots into the
    existing `GaggiaPID_Setup` captive portal - reused proven
    WiFiManager infrastructure instead of building a second WiFi-config UI.
    OTA upload (`/firmware` + `/update_fw`) already existed from the
    original cloned project; just linked more prominently.
- **First-ever real test of the OTA upload path, right after building this**:
  pushed the new firmware via `curl -F "update=@firmware.bin" .../update_fw`
  since the board was only reachable over WiFi at the time (USB was
  connected to a different computer). **First attempt failed silently** -
  empty HTTP response, board rebooted back into the still-running previous
  firmware (confirmed by `/status` missing the new fields) - this is the
  ESP32 OTA partition scheme's safe fallback when `Update.end()` doesn't
  complete cleanly, NOT a bricking risk. **Second attempt succeeded**
  cleanly - new fields present, all previously-tuned brew/steam values
  survived intact (NVS is independent of the firmware binary). Root cause
  of the first failure not confirmed (suspect a WiFi hiccup or a timing
  issue with the synchronous `WebServer` during the ~1.1MB transfer) -
  documented here so a repeat isn't mistaken for something worse; the fix
  so far has just been "retry once."
- Build stayed clean throughout (RAM ~15.4%, Flash ~56.8%).

### 2026-08-14 — Claude Code (Sonnet 5) — Dropped HLK-PM01 for USB power; reuse T2 instead of a new thermal fuse
- **ESP32 power source reverted to external USB wall-wart**, undoing the
  2026-08-01 "internal HLK-PM01" decision — user changed their mind. Same
  power setup as the bench: USB cable routed into the case through a
  grommeted hole, wall-wart in a regular outlet. Removed the HLK-PM01
  module, its pre-wiring steps, and its dedicated wiring step from Section 7
  entirely; updated the Section 2 hardware table accordingly. Consequence
  worth remembering: the ESP32 now stays powered independently of the
  machine's own power cord (unplugging the machine ≠ powering down the
  controller).
- **Heater-circuit safety element changed from "buy a new thermal fuse" to
  "reuse Thermostat 2 (the steam thermostat), re-terminated fresh."** User
  wants real overheat protection and prefers reusing hardware already
  correctly mounted on the boiler over sourcing a new part. This does NOT
  reopen the wiring-ambiguity problem from the previous entry: T2 is a
  discrete, physically identifiable component (like the heater element) —
  only its *existing* wiring into the ambiguous bundle is unverifiable and
  gets disconnected/capped; the component itself is kept and wired fresh
  with new wires directly into the clean-bypass circuit, exactly like the
  heater already was in the previous plan.
- Updated circuit: `Mains(L) → T2 (reused) → SSR terminal 1 → terminal 2 →
  Heater → Mains(N)`. Updated Section 7 steps 2/3/6/7 and the "How to do it"
  procedure accordingly.
- T2's exact rated trip temperature is still unconfirmed — not blocking,
  but worth checking if it's printed on the part before finalizing
  `STEAM_MAX_SAFETY`.
- No firmware changes in this entry — wiring plan only.

### 2026-08-13 — Claude Code (Sonnet 5) — Heater-circuit wiring finalized: clean bypass, no panel-wiring dependency
- **The panel-wiring investigation from the previous entry turned out to be
  a dead end**: the user has no visual or electrical (no multimeter) access
  to the wire bundle underneath the switches/thermostats, so the ambiguity
  (mains vs. pump on several wires, Steam Switch position relative to T2,
  Steam LED series-vs-parallel) can't actually be resolved. Rather than keep
  trying, **pivoted to a design that doesn't depend on any of that wiring**.
- **New circuit**: `Mains(L) → [new inline thermal fuse] → SSR terminal 1`,
  `SSR terminal 2 → heater element`, `heater → Mains(N)` — built from only
  two unambiguous points in the whole machine (the incoming mains terminal
  block, and the heater element's own two terminals), instead of trying to
  splice into the panel switch/thermostat network. Heater's existing wires
  get disconnected/capped, not reused.
- **Trade-off, explicit**: this drops Thermostat 2's potential role as a
  free independent mechanical safety backstop (never confirmed anyway) in
  favor of a **new, cheap, independently-verifiable inline thermal fuse**
  (common $1-3 one-shot cutoff, ~172-184°C, rating TBD) mounted directly
  against the boiler. Reasoning: a safety device you can't verify isn't
  actually a safety guarantee — better to install something new and known
  than trust unverifiable legacy wiring.
- Section 7 step 4 changed from "BLOCKING — get thermostat photo" to
  "SUPERSEDED — no longer needed"; step 5 narrowed to "remove the *brew*
  thermostat, only its physical location matters, not its wiring"; step 6
  rewritten around the new clean-bypass circuit; added a full "How to do
  it, step by step" procedure (unplug → identify the 2 unambiguous points →
  disconnect/cap heater's old wires → mount fuse on boiler → wire mains
  side → wire control side → wire HLK-PM01 → insulate → inspect →
  reassemble → supervised first power-up). Old investigation subsection
  kept below, explicitly marked historical/superseded, not deleted (in case
  a future agent wants to revisit reusing the panel wiring, e.g. to restore
  the Steam LED's function).
- No firmware changes in this entry — wiring plan and procedure only.

### 2026-08-13 — Claude Code (Sonnet 5) — Heater-circuit wiring investigation recorded (unverified)
- User provided two separate text descriptions of the panel wiring
  (thermostats + steam button, then switches + LEDs in more detail).
  Recorded verbatim as a new subsection in Section 7 ("Heater-circuit wiring
  investigation") rather than merged into a confident diagram, because:
  (1) the user themselves flagged that most of the connections described as
  going to "mains power" are actually uncertain — they're bundled with the
  pump's wiring out of sight, so any could really terminate at the pump
  instead; (2) two more ambiguities surfaced when trying to reconcile the
  two messages (whether the Steam Switch sits upstream or downstream of the
  Steam Thermostat; whether the Steam LED carries real current in series or
  is just a parallel indicator).
- **Deliberately did NOT produce a "before/after" wiring diagram this
  round** — a wrong diagram at mains-voltage stakes is worse than no
  diagram; drawing one now would just dress up unverified guesses as
  confidence. Recommended next step: identify the pump's two actual
  terminals directly and compare wire colors against the writeup to
  separate real mains connections from pump connections, ideally via
  multimeter continuity trace with the machine unplugged (or a single photo
  showing the whole bundle together, since the ambiguity partly comes from
  stitching together separate per-component descriptions).
- No firmware changes in this entry — documentation only, and explicitly
  provisional. **Future agents: do not build the Section 7 step 4/6 heater
  wiring instructions on top of the "as described" subsection without first
  confirming the pump/mains distinction has actually been resolved.**

### 2026-08-13 — Claude Code (Sonnet 5) — Brew/Steam mode support (software-only)
- **Design discussion first covered a physical brew/steam switch integration**
  (GPIO-sensed switch position as `activeProfile`, independent of a Web UI
  `heatingEnabled` toggle) — documented for reference but **not built**: user
  chose a simpler, fully software-driven approach instead, explicitly with
  **no wiring changes to switches or thermostats**.
- **Added `enum class OpMode { OFF, BREW, STEAM }`** (`config.h`) with two
  independent gain-scheduling profiles: `BREW_SETPOINT_DEFAULT/KP/KI/KD` and
  `STEAM_SETPOINT_DEFAULT/KP/KI/KD`, plus per-mode safety ceilings
  `BREW_MAX_SAFETY` (105°C) and `STEAM_MAX_SAFETY` (135°C, **placeholder**).
- **`main.cpp`**: `currentMode`, `brewSetpoint/Kp/Ki/Kd`,
  `steamSetpoint/Kp/Ki/Kd`, `activeMaxSafety` globals; `applyActiveProfile()`
  applies the active profile's setpoint/gains/ceiling; `setOpMode()` switches
  modes with a **bumpless PID reset** (`MANUAL`→`AUTOMATIC` cycle, per
  PID_v1's built-in re-seed behavior) so a big setpoint jump doesn't inherit
  a stale integral term from the other regime; `refreshActiveProfileIfChanged()`
  lets a live edit to the *active* profile's own tuning apply immediately
  without a full mode-switch reset. All `MAX_TEMP_SAFETY` references replaced
  with `activeMaxSafety`. **Board always boots `OpMode::OFF`** regardless of
  what was active before a reset — deliberate, so a WiFi hiccup/watchdog/OTA
  reboot never silently resumes unattended heating.
- **`web.cpp`**: `/update` now accepts `mode=off|brew|steam` plus
  `brew_target/kp/ki/kd` and `steam_target/kp/ki/kd` (each persisted to NVS
  independently); `/status` reports `"opmode"` and both profiles' full
  values so the UI can show/edit both regardless of which is active. Web UI's
  old 2-way Heat/Off control became a 3-way Off/Brew/Steam selector (new
  `--steam` blue accent color); the single PID-tuning form became two
  stacked forms (Brew / Steam) in the same card.
- **Verified end-to-end on the bench**: full `off→brew→steam→off` cycle via
  curl against `/update`/`/status` — correct `opmode`, `target`, and PID
  `output` at each step (e.g. brew: target 93, output 1000 while cold; off:
  output forced to 0).
- **Explicitly documented, unconfirmed assumption this design rests on**: the
  SSR is the sole heating actuator in every mode. Since the machine's
  original switch/thermostat wiring is untouched, Steam mode's real-world
  effect (or lack thereof) depends on that being true — won't be known until
  the machine is actually opened and the heater circuit inspected (Section 7
  step 4, still blocking).
- No changes to the sensor driver, fault handling, or bench-verified UI
  polish from the previous entry — purely additive on top.

### 2026-08-01 — Claude Code (Sonnet 5) — Mains wiring plan documented (Section 7, new)
- Bench work is fully done (sensor solved, UI polished, verified end-to-end)
- and the project moved into planning the actual mains/machine wiring. Added
  a new **Section 7 "Mains wiring plan (current phase)"** — a numbered,
  living checklist — and renumbered the old Sections 7-9 to 8-10
  accordingly (updated the internal roadmap anchor link in Section 1 too).
- Recorded two explicit user decisions so future agents don't re-litigate or
  silently "fix" them: (1) proceeding **without** the GPIO4 pull-down
  resistor (user doesn't have one, accepted the small recurring boot-glitch
  risk after being told the tradeoff), (2) powering the ESP32 via the
  **internal HLK-PM01** rather than an external USB supply, with the
  specific caution to feed its 5V into the DevKitC-1's `5V` pin (not `3V3`)
  and never have USB connected at the same time.
- Documented the bench pre-wiring steps (SSR + HLK-PM01 pigtails, terminal
  insulation, pin verification) and flagged the still-open, blocking
  question: no photo yet of the machine's second (steam/milk?) thermostat or
  how it's wired — heater/SSR wiring must not proceed without it.
- No firmware changes in this entry — documentation only.

### 2026-08-01 — Claude Code (Sonnet 5) — Sensor smoothing, fault distinction, sparkline UI, PWA icon
- **`temp_sensor.h`/`.cpp`**: `tempSensorRead()` now returns a `TempSensorStatus`
  enum (`OK`/`TIMEOUT`/`ERROR_REPLY`) instead of a bare bool, so the two fault
  modes get logged distinctly. Briefly tried tightening `READ_TIMEOUT_MS` from
  200→100ms as a "faster" tweak; reverted after it caused real, repeated
  timeouts in normal operation (the module doesn't always reply as fast as the
  handful of earlier sniffer tests suggested) — **do not lower this again
  without measuring actual worst-case reply latency over a long run first.**
- **`main.cpp`**: added an exponential-moving-average filter
  (`TEMP_EMA_ALPHA`, `config.h`) on top of each good read to cut sensor noise
  reaching the PID; skips blending right after boot/a fault (no history to
  blend against). Added a `tempHistory` ring buffer (`TEMP_HISTORY_LEN=60`,
  sampled every `TEMP_HISTORY_SAMPLE_INTERVAL_MS=2000` — a 2-minute window)
  for the Web UI sparkline; sampling is skipped while `sensorFault` is true or
  before the first real reading lands (an early version recorded a stray
  `0.0` at boot before any reading existed — fixed by gating on
  `currentTemperature > 0`).
- **`web.cpp`**: `/status` now includes `"fault"` (bool) and `"history"`
  (array, oldest-first) fields. UI gets a live sparkline canvas (last 2 min),
  a red fault banner (shown instead of a raw `-999`/garbage number), and a
  "Xs ago" last-updated indicator that turns red past 6s stale. Added a
  self-contained PWA manifest + inline SVG coffee-cup icon (`/manifest.json`,
  `/icon.svg`) for add-to-home-screen support — no external assets, matches
  the existing "no CDN/external resources" constraint.
- **Debugging note for future agents**: after this change, the sensor started
  failing 100% of reads and the Web UI became completely unreachable at the
  same time. Root cause was NOT the code — a physical wire to the temp
  module had come loose during bench handling. The two symptoms are
  correlated by design: when every sensor read times out, the main loop
  spends the full 200ms of every 250ms cycle blocked waiting on the UART
  reply, starving `handleWebLoop()` enough that the web server effectively
  stops accepting connections too. If both symptoms appear together again,
  check the physical UART wiring FIRST before suspecting firmware — ping the
  board's IP to confirm WiFi/network is fine (it was) before assuming a
  bigger failure.
- Verified end-to-end after reseating the wiring: real reading, `fault:false`,
  clean history array (`{"temp":24.44,...,"fault":false,"history":[24.5,...]}`).

### 2026-08-01 — Claude Code (Sonnet 5) — Protocol solved (AT+T), firmware rewritten, doc cleaned up
- **Prober v5** (long passive listen on fresh boot + `\n`/`\r`/no-terminator sweep):
  no auto-stream; confirmed the module only processes a line on bare **LF**
  (`\n`) — `\r`-only or unterminated bytes just sit in its buffer unprocessed
  and get prepended to whatever's sent next once a `\n` finally arrives (this
  is the actual root cause of the old "Too much enter" / concatenated-echo
  confusion from v3/v4, which only ever tried CRLF).
- **Prober v6** (flush-then-send: force-clear the buffer with a lone `\n`
  before every command, so each test is provably isolated): every clean
  command tried (`READ`, `read`, `GET`, `TEMP`, `VER`, `STATUS`, `AT`, several
  `AT+...` guesses) still came back `ERROR_1`.
- **User independently found the real command** using their own PC serial
  terminal: `AT+T` → `OK` / `+T=<value>`, values drifting downward as the
  probe cooled — confirmed this is a live PT100 reading, not a canned reply.
- **Prober v7** confirmed the exact framing over our own ESP32 relay:
  `AT+T` + **CRLF** (not bare LF) → `+T=<value>\r\nOK\r\n`, 100% repeatable
  (16/16 across two capture runs). Bare-LF `AT+T` still gave `ERROR_1` even
  fully isolated — so CRLF specifically is required for this command, despite
  LF alone being sufficient to get *some* line processed on other inputs.
- **Root-caused a fixed nonsense reading** (`+T=422.333`, never varying even
  after reseating leads): not a wiring-order problem after all — after the
  user re-terminated the PT100 leads in the module's green `B`/`B`/`R` block,
  the reading became a plausible, slowly-drifting room temperature
  (`+T=24.76` → `24.73`). Probe is a genuine 3-wire PT100 (white PTFE,
  silver-plated conductors, M3/M4 thread, -50~200°C) — red lead is the "odd"
  wire, the two same-colored (blue) leads are the internally-joined pair;
  which specific color plays which role is manufacturer-dependent, only the
  "2 matching + 1 different" topology matters.
- **Rewrote the sensor layer for real**: added `src/temp_sensor.cpp` /
  `include/temp_sensor.h` (`tempSensorInit()`, `tempSensorRead(float&)`)
  implementing the confirmed `AT+T\r\n` → `+T=<value>\r\nOK\r\n` protocol on
  UART1 (GPIO18 RX / GPIO17 TX, 9600 8N1). Removed all
  `Adafruit_MAX31865`/SPI code from `main.cpp`, removed the now-unused
  `RTD_WIRES`/`RTD_NOMINAL`/`RTD_RREF`/`PIN_SPI_*` defines and the MAX31865
  library dependency from the `esp32-s3-devkitc-1` env in `platformio.ini`
  (left untouched in the reference `esp32-c6-devkitc-1` env). PID loop, fault
  debounce counter, `-999` error sentinel, and `MAX_TEMP_SAFETY` cutoff logic
  in `main.cpp` are unchanged — only how the raw temperature number gets read
  in changed.
- **Doc cleanup**: Sections 2-4 (Hardware/Pin map/Software architecture)
  rewritten to describe the real UART setup instead of the stale MAX31865
  SPI description; `src/sniffer.cpp` + `esp32-s3-sniffer` env +
  `tools/serial_capture.py` are kept in the repo (not deleted) as reusable
  diagnostic tooling for any future unknown-protocol hardware.
- Build verified clean (RAM 15.2%, Flash 55.9%). **Flashed and verified
  end-to-end**: board rejoined home Wi-Fi on its own (no manual reset
  needed), `http://gaggia.local/status` returns a live real reading
  (`temp: 24.31`, matching the bench PT100 value) through the full app —
  PID, Web UI, and safety logic all working on the real UART sensor.

### 2026-07-31 — Cursor agent (Opus 4.8) — Command discovery in progress; seeking official docs
- Prober v3 (clean-ish per-command) result: **`READ\r\n` returned `OK\r\n`** (only
  recognised command found); everything else `ERROR_1`. Also saw `Error Reason:Too
  much enter` → the parser counts CR and LF as "enters" and dislikes extras. v3's
  bare-CR / no-terminator variants polluted the module's line buffer (echoes showed
  concatenated leftovers), so single-command interpretation is unreliable.
- Prober v4 (only ever sends `<cmd>\r\n`, flush between) — module answered a boot
  burst then **went silent to everything, incl. READ**. So the earlier `OK` may have
  been a buffered-echo artifact, OR the module enters a stuck/parser state after
  repeated input, OR it needs a specific terminator/sequence we haven't hit.
- **Seller identified: "EC Buying"** (AliExpress; watermark on the listing photos the
  user shared) = yourcee / Shenzhen Jixin. Its ASCII firmware prints English:
  `Received:`, `ERROR_1`, `OK`, `Error Reason:Too much enter`.
- **Launched a browser research subagent** to find the official serial-command doc
  (exact READ command + response format + correct terminator + auto-stream option).
  Waiting on that + the user's product-page URL.
- Open question for next agent: if docs can't be found, do a PURE-passive long listen
  right after a clean boot (module may auto-stream), and try single-terminator lines
  (`\n` only, or `\r` only) since CRLF triggers "Too much enter".

### 2026-07-31 — Cursor agent (Opus 4.8) — Module TALKS: 9600 8N1, ASCII line protocol
- **BREAKTHROUGH.** After a clean power-cycle, sniffer v2 (both-orientation sweep)
  captured real replies. Findings, now certain:
  - **Baud = 9600 8N1.** (Other bauds returned garbled versions of the same bytes.)
  - **Wiring correct as-is:** module TX → ESP **GPIO18** (ESP_RX=18/TX=17). The
    swapped orientation produced only noise → do NOT swap.
  - **NOT Modbus.** The module is an **ASCII line-command interface**: to any input
    it replies  `Received:"<echo>"\r\n`  then  `ERROR_1\r\n`  (its "unknown command").
  - Module silk = **MAX31865 + "temperature sensor"** (yourcee / Shenzhen Jixin);
    bundled PDF is a generic safety sheet with **no protocol** — command set is
    undocumented, so we must discover the command empirically.
- **Added sniffer v3 (`src/sniffer.cpp` rewritten as a "prober"):** fixes UART at
  9600/RX18/TX17, does a 12 s passive listen (auto-stream check), then fires a
  battery of likely ASCII commands (READ/TEMP/T/GET/AT/… each with CRLF, CR, and
  bare) plus the classic `A0 01 01 A2` serial-relay frame, printing every reply as
  hex+ASCII. Goal: find the command that returns a number instead of `ERROR_1`.
- **Flashing note:** auto-reset into download mode now fails while firmware runs;
  user must do hold-BOOT + tap-RST to flash. And the S3 USB-CDC only streams after a
  **clean power-cycle** (unplug/replug), not after an esptool reset — capture only
  after power-cycling.

### 2026-07-31 — Cursor agent (Opus 4.8) — DEFINITIVE serial-read method for this setup
- **Sniffer re-flashed OK** (COM6, 100% verified) with the module wired and powered at
  3.3V (user reports **2 red LEDs lit on the module → it runs at 3.3V**, good).
- **Terminal-jam pattern now fully characterized (READ THIS, future agents):**
  - `pio run ... upload` (foreground, opens COM6 briefly, then closes) = FINE.
  - `pio device monitor` **as a BACKGROUND job** (`block_until_ms: 0`, own shell id) +
    reading its terminal file = FINE, **as long as you never kill it**.
  - Any **foreground** command that holds COM6 open for a long read (a live monitor, or
    the `tools/serial_capture.py` run in the foreground) **jams the agent's persistent
    PowerShell** — every later command returns "no exit status". Requires a window
    reload to recover.
  - **`taskkill`/`Stop-Process` on a serial reader also jams the shell.**
  - A stray reader can **survive a window reload** and keep holding COM6 (then
    `upload` fails with "Access is denied"); clear it via Task Manager → end
    `python.exe`/`pio.exe`, or a PC restart.
- **THE method that works:** run `tools/serial_capture.py` (it self-closes after N
  seconds, so nothing lingers and nothing needs killing) **as a BACKGROUND job**
  (`block_until_ms: 0`), then READ ITS TERMINAL FILE for the output. Background +
  self-terminating + read-the-file = no jam, no kill.
- Added `tools/serial_capture.py` (pyserial; opens port with DTR/RTS de-asserted so it
  won't reset the board; reads for a fixed duration; prints; exits).

### 2026-07-31 — Cursor agent (Opus 4.8) — Hardware confirmed from clear photos
- **Temperature module fully identified (clear photos):** green PCB, silk
  "temperature sensor", SONGLE **SRD-05VDC-SL-C** relay (5V coil, 10A/250VAC).
  Front **green 3-screw terminal labeled `B B R`** = PT100 input (R = red lead,
  the two `B` = the other two leads). Back 4-pin header labeled **`5V · TX · RX ·
  GND`** (pin1=5V … pin4=GND) = TTL-UART. "OPEN/COM/CUT" solder-jumper config on back.
  → Confirms the UART-decode path is correct; **module is designed for 5V** (5V relay
  coil), so 3.3V power is a best-effort first try that may leave it silent.
- **SSR confirmed:** Fotek-style **SSR-40 DA**, input **3–32 V DC** (so a 3.3V ESP
  GPIO on `PIN_SSR` can trigger it, though 3.3V is near the low end), output
  24–380 VAC 40A. This is the heater switch for the later mains stage.
- **PT100 probe:** 3-wire, PTFE lead, M4 thread — matches the boiler thermostat port.
- **HLK-PM01:** 100–240VAC → 5V 3W — the in-machine PSU for the final build.
- **Plan:** bench sniffer test. Wire module 5V→ESP **3V3** (safe first try), GND→GND,
  module TX→GPIO18, module RX→GPIO17; PT100 red→`R`, other two→`B`,`B`. Flash sniffer,
  open monitor in BACKGROUND and READ THE TERMINAL FILE (never kill it from the shell),
  decode protocol. If silent at 3.3V → module likely needs 5V → needs a level shifter
  or 2-resistor divider on its TX (user has neither yet).

### 2026-07-31 — Cursor agent (Opus 4.8) — Root cause of shell hang; sniffer output confirmed working
- **ROOT CAUSE of the repeated terminal hang IDENTIFIED:** killing the background
  `pio device monitor` (started via the same persistent shell) with
  `taskkill`/`Stop-Process` **takes down the Cursor persistent shell itself** — every
  subsequent command returns "no exit status". This happened twice. **Rule for future
  agents:** do NOT `taskkill`/`Stop-Process` a `pio device monitor` that you launched
  from the agent shell. To stop it, have the USER reset the terminal (trash icon /
  Reload Window), or avoid opening a lingering monitor in the first place.
- **After a fresh PC restart, the sniffer monitor WORKED** (COM6, `--filter direct`):
  the board transmits a Modbus read probe every ~1.5 s across all baud rates
  (2400/4800/9600/19200/38400/115200). Confirms the USB-CDC serial path is fine when
  a monitor is opened on an already-running board (no reset after attach).
- **Module was NOT wired during that test** (user had only connected the board to USB),
  so "silence" was expected — no conclusion about the module's protocol yet.
- **User chose Path 1: restore the Web UI.** Plan: free COM6, flash
  `esp32-s3-devkitc-1` (main firmware), verify via `GaggiaPID_Setup` AP — **no serial
  monitor needed**, which avoids the hang entirely.
- **DONE: re-flashed `esp32-s3-devkitc-1` main firmware to COM6, 100% verified**
  (Flash 56.7%, RAM 15.7%; esptool connected on COM6 without manual download mode;
  hard-reset into the app). No monitor opened → no shell hang. The Web UI controller
  is back on the board. Since WiFiManager previously stored the home Wi-Fi creds, the
  board should rejoin the home network on boot and serve the UI at
  `http://gaggia.local` (falls back to the `GaggiaPID_Setup` AP only if creds fail).

### 2026-07-31 — Cursor agent (Opus 4.8) — Sniffer flashed; serial silent; terminal hung
- **Flashed `esp32-s3-sniffer` to COM6 successfully** (esptool connected without a
  manual download-mode this time; hard-reset into the sniffer). Board: ESP32-S3
  rev v0.2, 16MB, MAC 48:27:e2:ed:f6:30.
- **Problem: the USB-Serial/JTAG monitor captured ZERO bytes** — not even the
  sniffer's own startup banner / 1.5s probe lines — both before and after a manual
  `RST`. Consistent with the known S3 CDC-drop-on-reset quirk AND with the monitor
  going stale when the port re-enumerates on reset. We have in fact **never** seen
  serial output from this board (boot was previously verified via the Wi-Fi AP).
- **Next-agent plan to read the sniffer:** open a FRESH `pio device monitor -p COM6`
  on the already-running board and do NOT reset afterward — the sniffer prints every
  ~1.5s, so a clean monitor should catch probe lines within ~2s. If still silent,
  suspect the S3 native-USB CDC path; consider routing sniffer debug output to a
  second UART on a USB-TTL adapter, or lowering to a known-good monitor filter.
- **Tooling incident:** after flashing, attempts to kill/restart the background
  serial monitor left the Cursor shell backend hung — every new terminal command
  (including a bare `echo`) returned "no exit status". File tools still worked.
  Resolution needs the user to restart the terminal/agent shell. Sniffer firmware
  is already on the board, so no re-flash is required after the restart.

### 2026-07-31 — Cursor agent (Opus 4.8) — Web UI redesign (professional dark theme)
- **Rewrote the `index_html` page in `src/web.cpp`** into a modern, responsive
  espresso-themed dashboard: dark card layout, large boiler temperature hero,
  live target/heater-output progress bars, Heat/Off toggle with active-state
  styling, and grouped PID + MQTT forms with proper labels.
- **All endpoints, form field names, element IDs, and JS behavior preserved** —
  `/status`, `/update`, `/firmware`, `setMode()`, and the field names
  (`target`, `kp/ki/kd`, `mqtt_*`) are unchanged, so the backend handlers and
  MQTT/OTA flows are untouched.
- **Efficiency verified (user requested):** no external resources at all — no
  CDN, web fonts, or `<link>`/`<script src>` (system font stack + a Unicode ☕
  glyph). Page is still one inline HTTP GET served from flash; `/status` polling
  interval unchanged at **2000 ms**; bars use cheap CSS transitions (no canvas,
  no animation loop, no charting lib). Added ~6–8 KB of text in flash, RAM
  unchanged (`const char*` in flash).
- **Known pre-existing note (not changed):** the `/status` handler opens NVS
  `Preferences` on every 2s poll to return MQTT fields. Harmless at this rate;
  could be cached in RAM at boot if desired.
- **Reason:** user asked to make the UI look professional without hurting
  responsiveness.

### 2026-07-31 — Cursor agent (Opus 4.8) — UART reverse-engineering path (no new parts)
- User confirmed: only has the **UART+relay module** (no bare MAX31865), and has
  **no resistors and no multimeter** → can't build a 5V→3.3V divider.
- **Workaround chosen:** power the module from the ESP32 **3V3** pin (not 5V) so
  its TX can't exceed 3.3V and is safe to wire directly to an ESP RX pin. Relay
  won't actuate at 3.3V (we don't use it); sensor/serial expected to still work.
- Added `src/sniffer.cpp` + `[env:esp32-s3-sniffer]` (build_src_filter excludes
  main/web/mqtt) — a diagnostic that cycles baud rates, passively logs the
  module's TX (hex+ASCII), and sends Modbus read probes. Main env now excludes
  `sniffer.cpp`.
- **Planned UART pins:** ESP RX = GPIO18 (<- module TX), ESP TX = GPIO17
  (-> module RX).
- Sniffer firmware **builds OK**.
- **User reports the module is now WIRED** (module 5V->ESP 3V3, GND->GND,
  TX->GPIO18, RX->GPIO17, PT100 in the green terminal). Next: flash the sniffer
  and read the serial monitor to identify the protocol, then write the real UART
  sensor driver and swap it into `main.cpp` in place of the MAX31865 SPI reads.

### 2026-07-31 — Cursor agent (Opus 4.8) — Sensor hardware mismatch found (DECISION PENDING)
- **User's temperature board is NOT a MAX31865 SPI breakout.** Photos show a
  self-contained module: onboard SONGLE SRD-05VDC-SL-C relay, silk "temperature
  sensor", green 4-pos terminal (PT100 in), blue 3-pos terminal (relay NO/COM/NC),
  and a back header labeled **`5V / TX / RX / GND`** = **TTL-UART interface**, plus
  an `OPEN/COM/CUT` config area. It's the common "PT100 acquisition + relay,
  Modbus-RTU over UART" module (likely 9600 8N1, temp at register 0x0000 in 0.1°C).
- **Impact:** current firmware uses `Adafruit_MAX31865` over **SPI** and CANNOT
  read this board. The PT100 probe itself (3-wire, M4) is fine.
- **Options presented to user:** (A) buy a real MAX31865 SPI breakout — matches
  firmware, best accuracy; (B) rewrite the sensor layer to poll this module over
  UART/Modbus (needs a level shifter/divider on the module's 5V TX → ESP RX, and
  protocol confirmation; ~1% accuracy, extra latency); (C) abandon ESP32 PID and
  use the module as a standalone thermostat (loses PID + Web UI — not recommended).
- 3-wire hookup for this module family (from vendor manuals, for later): red → P+,
  the two same-color wires → P- and GND, and REMOVE the on-board jumper.
- No firmware changes yet — awaiting user's choice.

### 2026-07-31 — Cursor agent (Opus 4.8) — Boot + Web UI verified end-to-end
- Confirmed the board runs the firmware: `GaggiaPID_Setup` AP came up; after
  WiFiManager captive-portal setup it joined home Wi-Fi.
- Verified `http://gaggia.local/status` returns 200 with live JSON
  (`target 93, Kp 50, mode heat`). No sensor connected → `temp < 0` → `output 0`
  (correct fail-safe, heater stays off).
- No code changes this entry — verification only.

### 2026-07-31 — Cursor agent (Opus 4.8) — First successful flash
- Cleared orphaned `pio`/`esptool` processes from an interrupted upload that had
  file-locked `firmware.bin` (Windows "used by another process" build error) and
  held COM6 open.
- Re-flashed successfully over the S3 USB-Serial/JTAG (COM6) with
  `PYTHONIOENCODING=utf-8` + `chcp 65001` and no `Tee-Object` pipe. All flash
  sections verified; board hard-reset into the firmware.
- **Flashing note for future agents (Windows):** always set UTF-8 first and do
  NOT pipe esptool progress through `Tee-Object` (cp1252 crash). Put the S3 in
  download mode with hold-`BOOT` + tap-`RST`; it then enumerates as `303A:1001`
  (last seen COM6). If a build fails with a file-lock on `firmware.bin`, kill
  stray `python`/`pio`/`esptool` processes first.

### 2026-07-31 — Cursor agent (Opus 4.8) — Pre-flash verification + fault-counter fix
- **Fixed a latent bug** in `src/main.cpp` sensor fault handling: the `if`/`else`
  branches each declared their own block-scoped `static faultCounter`, so a good
  read reset a *different* variable than the fault path incremented. Faults would
  accumulate indefinitely and eventually latch a false `-999` sensor error during
  normal operation. Now uses one shared counter.
- Added SSR boot-state safety note (pull-down on `GPIO4`) to Section 6.
- Diagnosed the flash failure: **not hardware** — esptool connected fine and
  auto-detected 16MB flash, but PlatformIO's output printer hit a Windows
  `cp1252 UnicodeEncodeError` on progress-bar chars. Fix: set
  `PYTHONIOENCODING=utf-8` + `chcp 65001` before uploading, and don't pipe the
  progress output through `Tee-Object`. Board needs one clean re-flash (the
  interrupted upload left the bootloader region partially written).

> Newest entries on top. Format:
> `### YYYY-MM-DD — <agent/model> — <short title>` followed by bullets of
> **what changed** and **why**.

### 2026-07-31 — Cursor agent (Opus 4.8) — Retarget C6 → ESP32-S3 + project doc
- Cloned `petoz/gaggia_pid_esp32-c6` into `Desktop\gaggia_pid_esp32s3`.
- `platformio.ini`: added primary env `esp32-s3-devkitc-1` (16MB flash, `qio_opi`
  PSRAM, native USB CDC); kept original C6 env for reference; set it as default.
- `include/config.h`: remapped pins to S3-safe GPIOs (SSR=4, CS=10, MOSI=11,
  SCK=12, MISO=13); added `RTD_WIRES` / `RTD_NOMINAL` / `RTD_RREF` defines so the
  reference resistor and wire mode are configurable in one place.
- `src/main.cpp`: replaced hardcoded `MAX31865_3WIRE` and `temperature(100.0, 430.0)`
  literals with the new `config.h` defines.
- Added this `AGENTS.md` as the project doc + agent change log.
- Verified the S3 firmware **builds successfully** (`pio run -e esp32-s3-devkitc-1`,
  264s; RAM 15.7%, Flash 56.4%).
- **Reason:** user has an ESP32-S3-DevKitC-1 + PT100/MAX31865; petoz repo is the
  closest hardware match (Gaggia + PT100 + MAX31865 + Web UI) but shipped for C6.
