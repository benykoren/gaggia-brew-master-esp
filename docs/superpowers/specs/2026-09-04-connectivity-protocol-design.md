# Unified BLE/Serial Connectivity Protocol — Design Spec

**Status:** approved, ready for implementation planning
**Date:** 2026-09-04
**Sub-project 2 of 3** in the larger "FreeRTOS task-priority refactor +
dual connectivity + Android app" initiative (see `AGENTS.md`/this repo's
`docs/superpowers/specs/2026-09-04-pump-pressure-bringup-design.md` for the
initiative's full context). This spec covers the connectivity/protocol
half of sub-project 2 only.

---

## 1. Context

Sub-project 1 (pump-pressure hardware bring-up) is implemented in firmware
(9 software tasks complete, reviewed) but paused on physical bench-testing
(Tasks 10-13) pending the user acquiring the dimmer hardware. In the
meantime, the user has an Android device and a working ESP32-S3 with WiFi
(the existing web UI) and BLE (unused so far) — enough hardware to start
sub-project 2 and the Android app now, independent of the paused hardware
work.

Sub-project 2 was itself decomposed (brainstorming, 2026-09-04) into four
pieces, of which this spec covers the first two together:

1. **Task-priority map + shared connectivity protocol** (this spec).
2. **USB Serial transport** for that protocol (this spec covers the
   protocol; a later implementation plan wires it onto the existing
   USB-CDC link).
3. **BLE transport** for the same protocol (also covered by this spec —
   the protocol is transport-agnostic by design, and BLE is the transport
   the Android app actually needs first).
4. **Web UI authentication** — explicitly deferred to its own separate
   spec later. Currently the web UI has no authentication at all; that
   remains true after this spec ships. Not addressed here.

The Android app itself (sub-project 3) is a *consumer* of this protocol
and gets its own spec once this one is implemented.

## 2. Goal

Give the ESP32-S3 firmware a single, transport-agnostic command/telemetry
protocol that BLE and USB Serial both speak, with **full feature parity**
with the existing web UI (mode control, shot start/stop, live telemetry,
Brew/Steam/Pump-Pressure PID tuning, profile CRUD including pressure-ramp/
decline settings, eco-sleep, schedules, autotune, descale tracking, MQTT
config) — with the single explicit exception of OTA firmware updates,
which stay WiFi-only. The existing WiFi web UI is untouched and keeps
working; WiFi and BLE run simultaneously.

## 3. Transports

- **BLE**, via **NimBLE-Arduino** — chosen over the ESP32 core's built-in
  Bluedroid-based BLE stack specifically because it is much lighter on
  RAM/flash and coexists better with WiFi running at the same time, which
  this project requires (this board is already at ~68% flash with WiFi +
  PSRAM + a real-time control loop active).
- **USB Serial**, over the existing native-USB-CDC link (`ARDUINO_USB_CDC_ON_BOOT=1`,
  already enabled) — the same physical link currently used for flashing/
  monitoring, now also carrying the protocol when a debug/wired session is
  open.
- **WiFi/HTTP is unchanged** — the existing `/status`/`/update`/OTA
  endpoints keep working exactly as today, for the browser-based web UI
  and for OTA specifically.

## 4. Message shapes

Two message kinds, chosen for their very different latency/frequency
profiles (brainstorming, 2026-09-04):

### 4a. Live telemetry — compact binary frame

A fixed 21-byte little-endian frame, small enough to fit inside a single
BLE packet **even at the default, un-negotiated 23-byte ATT MTU** — no MTU
negotiation round-trip is required for telemetry to work:

| Offset | Bytes | Field | Notes |
|---|---|---|---|
| 0 | 1 | Sync marker | `0xA5` — lets a Serial reader distinguish this from a JSON command line (which always starts with `{` / `0x7B`) |
| 1 | 1 | Protocol version | `1` |
| 2 | 4 | `temp` | float32, °C |
| 6 | 4 | `pressure` | float32, bar |
| 10 | 1 | `output` | uint8, 0-100% heater duty |
| 11 | 1 | `pump_power` | uint8, 0-100% dimmer duty |
| 12 | 1 | `opmode` | 0=off, 1=brew, 2=steam |
| 13 | 1 | `shot_phase` | 0=none, 1=preinfusion_on, 2=preinfusion_off, 3=pressure, 4=extraction |
| 14 | 1 | flags bitfield | bit0 sensorFault, bit1 pressureFault, bit2 shotInProgress, bit3 autoSleeping, bit4 pressureCeilingTripped, bit5 descaleDue |
| 15 | 4 | `shot_elapsed_ms` | uint32, 0 if no shot in progress |
| 19 | 2 | CRC16 | over bytes 0-18 (CCITT) — BLE's link layer has its own CRC, but Serial does not, and a single shared encoder is simpler than two |

Pushed as a BLE notification and as a Serial line-oriented binary write, at
a configurable rate (default matching the Web UI's existing 2s poll
interval; can be raised since a 21-byte frame is cheap).

### 4b. Commands & everything else — JSON request/response

One JSON object per command, over a separate BLE characteristic pair
(Write for the request, Notify for the response) and as newline-delimited
JSON lines over Serial (mirroring the line-buffered pattern this project's
own UART temp-sensor protocol already uses in `temp_sensor.cpp`):

```json
// Request
{"id": 17, "cmd": "update", "params": {"brew_target": 93.5, "brew_kp": 30.0}}
// Response
{"id": 17, "ok": true}
// or
{"id": 17, "ok": false, "error": "brew_target out of range"}
```

- `params` reuses the **exact same field names** `/update` already accepts
  (`brew_target`, `steam_kp`, `profile_save`, `press_kp`, `sched0_en`,
  etc.) — no new vocabulary to design or keep in sync.
- `{"cmd": "status"}` returns the same field set as `/status` **except**
  the two large history arrays (`history`, `pressure_history`) — those are
  superseded by the higher-rate binary telemetry stream once connected;
  a client that wants a one-time backfill on connect can request a
  smaller, explicit `{"cmd": "history"}`.
- `{"cmd": "profiles"}` returns the same array `GET /profiles` already
  returns.
- The `id` field is optional; when present, it's echoed back in the
  response so a client with multiple in-flight requests (a real
  possibility over BLE's queued writes) can correlate them.
- OTA-related commands are explicitly **not** part of this protocol — the
  app calls the existing WiFi OTA endpoint directly for that one thing.

## 5. BLE GATT layout

One custom primary service, three characteristics:

| Characteristic | Property | Carries |
|---|---|---|
| Telemetry | Notify | §4a binary frame |
| Command | Write | §4b JSON request |
| Response | Notify | §4b JSON response |

Exact UUIDs are an implementation-time detail (randomly generated,
documented once chosen — not a design-level decision).

## 6. Task priority

- **`controlLoopTask`** (temperature PID + pump-pressure PID — the latter
  already lives here since sub-project 1) stays the **highest priority**,
  unchanged. This already satisfies the original "PID + pump pressure at
  highest priority" requirement; nothing in this spec touches it.
- **A new connectivity task** owns BLE event handling, Serial line
  reading, and JSON command dispatch — created at a priority **above** the
  Arduino loop task (which runs the WebServer/MQTT, i.e. lower priority)
  but **below** `controlLoopTask`, so real commands get handled promptly
  without ever being able to preempt or starve the control loop. Pinned to
  core 0 (alongside WiFi and NimBLE's own host task), leaving
  `controlLoopTask`'s core 1 undisturbed.
- The existing `stateMutex` (recursive, already used by `controlTick()`,
  `web.cpp`'s `/update` handler, and `mqtt.cpp`'s callback) is reused
  as-is by the new connectivity task for the same reason it's used
  everywhere else — no new locking primitive is introduced.

## 7. Shared command-dispatch layer (the one real refactor)

`web.cpp`'s `handleUpdate()` is already a single large function doing
arg-parsing, validation, and safety-relevant clamping (e.g.
`steam_max_safety`'s explicit "a typo shouldn't be able to set a
dangerous ceiling" comment, or the pressure-profile clamping added in
sub-project 1's final review) for every setting in this project. Building
BLE and Serial as two more independent copies of that logic would triple
the maintenance surface and risks exactly the kind of cross-transport
inconsistency sub-project 1's final review already caught once (one
transport clamping a field, another forgetting to).

**Requirement:** extract that logic into a shared, transport-agnostic
command-dispatch module (exact function signatures are an implementation-
plan-level decision, not a spec-level one) that all three transports —
HTTP's existing `hasArg`/`arg()` parsing, the new BLE/Serial JSON `params`
object — resolve down to before calling. Every safety-relevant clamp
(temperature ceilings, pressure ceiling margins, PID gain sanity, etc.)
lives in exactly one place, applied identically regardless of which
transport a command arrived over.

## 8. Error handling & connection lifecycle

- A malformed JSON command, or one referencing an unknown field, returns
  `{"ok": false, "error": "..."}` rather than silently ignoring it or
  crashing — matching how `/update` already silently accepts only
  recognized `hasArg()` names, but the new protocol should surface unknown
  fields as errors since a mobile app (unlike a browser form) needs to be
  able to detect a protocol mismatch.
- A BLE disconnect or Serial link drop stops the telemetry push and drops
  any in-flight command's response silently (the client is expected to
  detect the disconnect itself and retry once reconnected) — no special
  reconnection/resume state is kept on the firmware side.
- None of this protocol's error handling relaxes or bypasses the existing
  safety-ceiling/fault behavior in `controlTick()` — a command arriving
  over BLE/Serial that would set an unsafe value is rejected by the shared
  clamp logic (§7) exactly as it would be over HTTP today.

## 9. Out of scope (deferred)

- OTA firmware updates over BLE/Serial — stays WiFi-only.
- Web UI authentication — its own separate spec.
- The Android app itself — sub-project 3, consumes this protocol once
  implemented.
- Bonding/pairing security for BLE (encryption, passkey, etc.) — not
  addressed here; this spec assumes an open/unauthenticated BLE
  connection for now, matching the web UI's current unauthenticated state
  and consistent with deferring "connectivity security" broadly to the
  web-UI-auth spec's follow-on.

## 10. Decisions log (from brainstorming, 2026-09-04)

- Decomposition: task-priority+protocol design, then USB Serial transport,
  then BLE transport, then web UI auth as its own later spec — all folded
  into this one spec since the protocol is the shared foundation both
  transports need.
- Android app connectivity: build the unified protocol first, app is built
  against it from the start (not the existing WiFi/HTTP API).
- Message format: hybrid — compact binary for the frequent live-telemetry
  stream (latency-critical), JSON for infrequent commands/config
  (readability/debuggability matters more there, latency doesn't).
- WiFi + BLE run simultaneously (not BLE-only with WiFi disabled).
- Protocol scope: full feature parity with the web UI, except OTA (stays
  WiFi-only given BLE's poor fit for a ~1.3MB firmware transfer).
- BLE library: NimBLE-Arduino, for WiFi coexistence and lower memory
  footprint versus the built-in Bluedroid stack.
- New architectural requirement surfaced during design (not in the
  original ask): extract `handleUpdate()`'s validation/clamping logic into
  a shared, transport-agnostic layer rather than tripling it across HTTP/
  BLE/Serial.
