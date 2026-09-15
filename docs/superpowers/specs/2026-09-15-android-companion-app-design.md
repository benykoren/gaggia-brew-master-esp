# Android Companion App (MVP) — Design Spec

**Status: SUPERSEDED (2026-09-15) - not being built.** After this spec was
written, a real hardware constraint surfaced during physical-connection
planning: the ESP32 and tablet are each independently powered over the
same USB ports a Serial link would need, creating a genuine two-power-
-source conflict (see this conversation's 2026-09-15 discussion) - and
weighing that against actually needing a native app at all, the user
decided to drop the native-app approach entirely and instead connect over
**WiFi using the existing web UI** (already built, `web.cpp`'s
`index_html`), investing effort in making that web UI run well on a
KitKat-era browser instead. See
`docs/superpowers/specs/2026-09-15-web-ui-kitkat-compat-design.md` for the
spec that replaces this one. Kept for history/context, not for
implementation - do not build against this document.

**Original status:** approved, ready for implementation planning
**Date:** 2026-09-15
**Sub-project 3 of 3** in the larger "FreeRTOS task-priority refactor + dual
connectivity + Android app" initiative (see `AGENTS.md` and
`docs/superpowers/specs/2026-09-04-connectivity-protocol-design.md`). This
spec covers an MVP slice of sub-project 3 only - see §3 "Scope" for what's
explicitly deferred.

---

## 1. Context

Sub-project 1 (pump-pressure hardware bring-up) is implemented in firmware,
paused on physical bench-testing pending a replacement dimmer module.
Sub-project 2 (unified BLE/Serial connectivity protocol) has an approved
design spec and an implementation plan
(`docs/superpowers/plans/2026-09-15-connectivity-protocol.md`), **not yet
implemented in firmware as of this spec**.

This spec was written in parallel with sub-project 2's implementation
rather than after it - an explicit, accepted tradeoff (see this
conversation's brainstorming decision, 2026-09-15): the app is being
designed against sub-project 2's *spec* (field names, telemetry byte
layout, JSON shapes), not yet against a running, tested protocol. If
implementation of sub-project 2 deviates from its spec in any way that
affects the wire contract (a renamed field, a changed byte offset), this
app's implementation will need a matching update. The two share their
telemetry-frame layout and JSON field names by direct reference to the same
source spec (§4 below), not by duplication, to keep that risk as small as
possible.

## 2. Goal

A native Android app, installable and running on **Android 4.4 (KitKat,
API 19)**, that connects to the ESP32 over USB-OTG Serial (sub-project 2's
protocol), shows live telemetry with graphing, and provides the core brew
controls - **an MVP slice**, not full feature parity with the web UI yet
(§3).

## 3. Scope

**In scope (this spec, MVP):**
- Connect via USB-OTG Serial, remember the last-used device.
- Live dashboard: temperature + pressure charts, current temp/target/
  pressure/output stat tiles, Off/Brew/Steam mode buttons, shot start/stop
  with a live elapsed timer.
- Basic tuning: Brew/Steam target + Kp/Ki/Kd, live-editable.

**Explicitly deferred to a phase-2 spec, once this MVP is working
end-to-end on the real tablet:**
- **BLE transport** - the target tablet has no Bluetooth hardware at all
  (confirmed 2026-09-15), so `BleTransport` isn't built for this MVP. The
  firmware protocol (sub-project 2) still speaks BLE regardless - this is
  purely about what this particular app targets. Revisit if this app is
  ever run on a different, BLE-capable tablet.
- Named shot profiles (CRUD, apply).
- Scheduled warm-up.
- Descale/maintenance tracking.
- MQTT settings.
- Pump-pressure profile editing (ramp/decline stages).
- PID autotune trigger/monitor.
- WiFi-based OTA firmware updates from the tablet.
- Shot history log + tasting notes.

Every deferred item above already has a corresponding field set in
sub-project 2's `"update"`/`"status"` JSON commands (spec §4b) - phase 2 is
additional app-side UI and app-side local caching, not new firmware
protocol work.

## 4. Target device & toolchain

- **minSdk 19** (Android 4.4 KitKat), current `compileSdk`/`targetSdk`,
  built with current Android Studio + Gradle (not legacy Eclipse/ADT
  tooling) - AndroidX works fine down to API 14, so this is a normal,
  well-trodden "support an old device with modern tooling" setup, not a
  special one.
- **Language: Kotlin.**
- **Tablet has no Bluetooth hardware at all** (confirmed 2026-09-15) - USB-OTG
  Serial is the only transport this app targets (§3). USB-OTG host support
  itself is still worth confirming before implementation (see the
  "Recheck" item in §11), but the architecture no longer needs to hedge
  between two transports.
- **Repo layout:** a new top-level `android-app/` folder in this same repo
  (`gaggia-brew-master-esp`), alongside `src/`, `include/`, `docs/` - one
  repo, one history, keeps the app and the firmware protocol it depends on
  versioned together.
- **Physical connection:** the ESP32-S3's existing native USB-C port (the
  same one `pio run -t upload -t monitor` already uses to flash/monitor
  over the board's USB-CDC virtual serial link, `AGENTS.md` §5) connects
  to the tablet's USB port via a USB-OTG adapter cable (tablet-side
  micro-USB on this device) plus a standard USB-A-to-USB-C cable - no new
  firmware-side hardware or wiring. `usb-serial-for-android` enumerates it
  as a CDC-ACM device once attached; the app requests the one USB
  permission dialog Android shows on first attach (this is the only
  runtime permission this app needs at all - see §6).

## 5. Architecture

### 5a. Components

- **`ConnectivityService`** - a foreground `Service` (survives
  backgrounding mid-shot) that owns a `SerialTransport`, remembers the
  last-used USB device (`SharedPreferences`) for auto-reconnect on next
  launch, decodes incoming telemetry frames and JSON responses, and
  exposes current state via `ViewModel`/`LiveData` (AndroidX Lifecycle,
  works fine at `minSdk 19`). Named generically rather than
  `SerialConnectivityService` since the underlying protocol (and this
  service's role) isn't inherently Serial-only - see §3's BLE note.
- **`SerialTransport`** - wraps `usb-serial-for-android` (`mik3y/
  usb-serial-for-android`, API 14+), reading/writing over the same
  USB-CDC link the firmware's `serial_transport.cpp` speaks. Mirrors that
  file's line-buffering logic: accumulate bytes until `\n` for a JSON
  line, or treat a leading `0xA5` byte as the start of a fixed 20-byte
  binary telemetry frame; discard anything else (the same USB link also
  carries the firmware's own plain-text `Serial.println()` debug logs -
  see that file's design note).
- **`TelemetryFrame`** - a pure decoder class parsing the identical 20-byte
  little-endian layout `telemetry.cpp`'s `encodeTelemetryFrame()` produces
  (see the byte table below), including CRC16-CCITT validation (a frame
  that fails CRC is dropped, not shown). Feeds an in-memory ring buffer
  (same fixed-size-array-of-last-N-samples idea as the firmware's own
  `tempHistory`/`pressureHistory`) that `TelemetryChartView` reads from.
- **`CommandClient`** - builds `{"id", "cmd", "params"}` JSON (using
  `org.json`, already built into Android - no new JSON dependency needed
  for this app's small objects), sends it over `SerialTransport`, and
  correlates the response by `id` with a timeout (default 3s).
  Sub-project 2's protocol keeps no reconnection/resume state (spec §8),
  so a response that never arrives is a normal, expected case here, not a
  bug to work around.
- **UI:**
  - `ConnectActivity` - lists attached USB-OTG devices
    (`UsbManager.getDeviceList()` via `usb-serial-for-android`) for the
    user to pick; remembers the chosen device for next launch.
  - `DashboardFragment` - the main "Now" screen (naming matches the
    existing web UI's own "Now" view, `AGENTS.md`/`web.cpp`): stat tiles,
    `TelemetryChartView` (a custom `View` with `onDraw(Canvas)`, no
    charting library), mode buttons, shot start/stop + live elapsed timer
    read straight from the telemetry frame's `shot_elapsed_ms` field (no
    separate polling needed for the timer).
  - `TuningFragment` - Brew/Steam target + Kp/Ki/Kd. Since PID gains
    aren't in the compact telemetry frame, this screen issues
    `{"cmd":"status"}` on open to populate current values, and
    `{"cmd":"update","params":{...}}` on save.

### 5b. Telemetry frame layout (shared contract with firmware)

Copied verbatim from
`docs/superpowers/specs/2026-09-04-connectivity-protocol-design.md` §4a -
`TelemetryFrame` must decode exactly this, byte for byte, whether it
arrives over Serial (this app's only transport, §3) or BLE (other
clients of the same firmware protocol). **20 bytes, not 21** - corrected
during this app's own brainstorming (2026-09-15): a BLE notification's
usable payload at the default, un-negotiated 23-byte ATT MTU is 20 bytes
(MTU minus 3 bytes of ATT overhead), and **KitKat's `BluetoothGatt` has no
`requestMtu()` at all** (added in API 21) - so no Android client older
than that could ever negotiate a bigger MTU, even though this specific app
doesn't use BLE itself. The frame's sync-marker byte doubles as its
protocol version rather than carrying a separate version byte, to fit
exactly:

| Offset | Bytes | Field | Notes |
|---|---|---|---|
| 0 | 1 | Sync marker / version | `0xA5` = protocol v1 |
| 1 | 4 | `temp` | float32 LE, °C |
| 5 | 4 | `pressure` | float32 LE, bar |
| 9 | 1 | `output` | uint8, 0-100% heater duty |
| 10 | 1 | `pump_power` | uint8, 0-100% dimmer duty |
| 11 | 1 | `opmode` | 0=off, 1=brew, 2=steam |
| 12 | 1 | `shot_phase` | 0=none, 1=preinfusion_on, 2=preinfusion_off, 3=pressure, 4=extraction |
| 13 | 1 | flags bitfield | bit0 sensorFault, bit1 pressureFault, bit2 shotInProgress, bit3 autoSleeping, bit4 pressureCeilingTripped, bit5 descaleDue |
| 14 | 4 | `shot_elapsed_ms` | uint32 LE |
| 18 | 2 | CRC16 | CCITT, over bytes 0-17 |

`TelemetryChartView` (§5a) draws the actual temp/pressure readings as solid
lines and the current target (from the Tuning screen's last-fetched
`brew_target`/`steam_target`, or the pressure profile's ramp/decline target
once phase 2 lands) as a dashed overlay line, dual-axis (temperature and
pressure on independent scales) - a pattern worth borrowing from
`gaggiuino-fw`'s web dashboard (`reference/gaggiuino-fw/webserver/
web-interface/src/components/chart/ShotChart.jsx`), which draws its own
live shot chart the same way with Chart.js. Same visual idea, redrawn by
hand in `onDraw(Canvas)` instead of a charting library.

### 5c. Data flow

1. App launch → `ConnectActivity` (or auto-reconnect if a last-used USB
   device is remembered) → `ConnectivityService` starts, `SerialTransport`
   connects.
2. Telemetry frames arrive ~every 2s (matches firmware's
   `CONNECTIVITY_TELEMETRY_INTERVAL_MS`) → `TelemetryFrame.decode()` →
   pushed into the ring buffer + `LiveData` → `DashboardFragment`
   redraws.
3. User taps "Brew" → `CommandClient` sends
   `{"id":N,"cmd":"update","params":{"mode":"brew"}}` → awaits the
   matching response (by `id`) within the timeout → on timeout/error,
   show a `Snackbar` ("Command failed - check connection"); **no silent
   auto-retry of a `mode`/`shot` command** - this is a mains-voltage
   machine, a duplicate/late-arriving start command is a worse failure
   mode than making the user tap again.

## 6. KitKat (API 19) compatibility notes

Concrete traps, not generic caution - each of these is a real API that
does not exist yet at API 19. (BLE-specific traps - `startLeScan()` vs.
`BluetoothLeScanner`, `requestMtu()`'s API 21 floor - dropped from this
list along with BLE itself, §3; kept in the firmware-side connectivity
spec since the protocol still serves BLE clients in general.)

- **Foreground-service notification channels are API 26+** - guard that
  specific call with `Build.VERSION.SDK_INT >= 26`, not the whole
  foreground-service call (`startForeground()` itself has existed since
  API 5).
- **USB host APIs** (`UsbManager`, `UsbDevice`, used by
  `usb-serial-for-android`) have existed since API 12 - no gating needed.
  This is now the one compatibility surface that actually matters for
  connectivity, since it's the app's only transport. USB device access
  itself is gated by a one-time `UsbManager` permission dialog (not the
  API-23+ runtime permission system - this is a separate, older Android
  mechanism that's worked the same way since USB host support was added),
  shown the first time the app tries to open the attached ESP32.
- No WebView used in this design (Canvas chosen over WebView per this
  conversation's brainstorming decision), so KitKat's WebView-version
  nuances don't apply here.

## 7. Error handling & reconnection

- Connection drop (USB device detached, or a read/write failure) →
  `ConnectivityService` marks state "disconnected", `DashboardFragment`
  grays out and shows "Reconnecting…" over the last known telemetry (not
  cleared - stale-but-visible is more useful than blank).
- Reconnect attempts on a capped backoff (e.g. 3s, 3s, 5s, 10s, then
  settle at 10s) against the same last-used USB device.
- A command response that never arrives within its timeout surfaces an
  error to the user (§5c) - never retried automatically.

## 8. Testing

- **Unit-testable (JUnit, no device/emulator needed):** `TelemetryFrame`
  decode + CRC16 validation (feed known-good and deliberately-corrupted
  byte arrays, assert correct decode/rejection), `CommandClient`'s JSON
  building, the ring buffer's wrap-around logic.
- **Everything else is manual bench verification against the real
  board and tablet** - connect, watch the dashboard update, toggle modes,
  start/stop a shot, edit tuning values and confirm they round-trip via
  `{"cmd":"status"}` - matching the same "no unit-test framework for
  hardware-facing behavior, verify on the real device" discipline the
  firmware plan already follows.

## 9. Out of scope (deferred)

See §3's explicit phase-2 list. Also not addressed here:
- Multi-machine/multi-device support (this app talks to exactly one ESP32
  at a time, matching the single-user single-machine scope of the whole
  project).
- Any account/cloud sync - everything is local to the tablet and the
  ESP32's own local link (BLE/Serial), matching the project's existing
  no-cloud, local-network-only design (Web UI, MQTT are the only existing
  "remote" surfaces, and MQTT stays untouched by this app).
- Tablet-initiated OTA (§3, phase 2).

## 10. Reference-project research (2026-09-15)

Checked this repo's two vendored reference projects
(`reference/gaggimate`, `reference/gaggiuino-fw`) and prior art online
(Decent Espresso's DE1 app ecosystem, CafeHub) before finalizing this spec.

- **Neither reference project has a phone/tablet app** - both are WiFi +
  browser web UIs. No direct app-architecture precedent existed to copy;
  the useful findings were protocol- and UI-level, not app-structural.
- **GaggiMate's BLE design** (its own controller board talking to its own
  separate ESP32+LVGL display board, not a phone) is more sophisticated
  than sub-project 2's protocol: NimBLE, MTU raised to 256, Protobuf+COBS
  framing, BLE bonding/encryption with a single-peer whitelist, and a
  coalescing priority queue for outbound telemetry
  (`reference/gaggimate/lib/NanoPbComm/src/Protocol.h`,
  `BleServerTransport.cpp`). **Deliberately not adopted here**: MTU-256
  assumes a client that can negotiate MTU at all, which is exactly the
  opposite of this app's binding constraint (§5b); Protobuf/bonding would
  reopen sub-project 2's already-approved, simpler JSON+binary design for
  a single-user hobby MVP; the coalescing queue solves a backlog problem
  this app doesn't have (telemetry here is always "latest state," not a
  queue of distinct events).
- **`gaggiuino-fw`'s web chart** (`ShotChart.jsx`) draws actual readings as
  solid lines and the target as a dashed overlay, dual-axis - adopted as a
  concrete design detail for `TelemetryChartView` (§5b), redrawn in Canvas
  rather than via its Chart.js library.
- This research is also what surfaced the 21→20-byte telemetry frame MTU
  bug fixed in §5b and cross-applied to
  `docs/superpowers/specs/2026-09-04-connectivity-protocol-design.md` and
  `docs/superpowers/plans/2026-09-15-connectivity-protocol.md`.

## 11. Decisions log (from brainstorming, 2026-09-15)

- Sequencing: this spec was brainstormed in parallel with sub-project 2's
  *implementation* (not after it) - an explicit, accepted risk of rework
  if the shipped protocol deviates from its own spec.
- Scope: MVP first (connect, dashboard, mode control, shot timer, basic
  tuning), not full feature parity in one build.
- Rendering: native Canvas custom views, not a WebView reusing the
  existing dashboard HTML - trades some code reuse for better
  performance/idiomatic-Android fit on old hardware, and accepts
  maintaining chart-drawing logic in two places (ESP32 HTML + Kotlin)
  going forward.
- Persistence: `SharedPreferences` + in-memory ring buffer only for the
  MVP - no Room/SQLite yet, since there's nothing relational to store
  until phase 2's profiles/schedules/shot-history land.
- Transport: initially scoped as "support both BLE and USB-OTG Serial from
  the start," pending confirmation of the tablet's radios. **Revised
  later the same day, 2026-09-15: the tablet has no Bluetooth hardware at
  all (user-confirmed)** - BLE dropped from the MVP entirely (§3),
  `BleTransport` removed from the architecture (§5a), leaving Serial as
  the only transport this app builds.
- Repo layout: new `android-app/` top-level folder in this same repo, not
  a separate repository.
- Language/toolchain: Kotlin, current Android Studio/Gradle, AndroidX,
  `minSdk 19`.

**Recheck before/during implementation:** USB-OTG host support
(`android.hardware.usb.host`) still hasn't been explicitly confirmed on
the tablet (only Bluetooth's absence has) - worth a quick check (`adb
shell pm list features | grep usb.host`, or plugging in a USB-OTG
flash drive/mouse and confirming Android recognizes it) before writing
`SerialTransport`, since it's now the app's *only* transport rather than
one of two.
