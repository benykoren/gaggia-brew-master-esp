#include "config.h"
#include <Arduino.h>
#include <PID_v1.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <time.h>

#include "dimmer.h"
#include "pressure_sensor.h"
#include "profiles.h"
#include "profile_codec.h"
#include "pump_calibration.h"
#include "shot_planner.h"
#include "shot_log.h"
#include "water_level.h"
#include "mqtt.h"

extern float currentTemperature;
extern bool sensorFault;
extern double Setpoint, Input, Output;
extern PID myPID;
extern PID pressurePID;
extern float tempHistory[];
extern int tempHistoryHead;
extern int tempHistoryCount;

extern float currentPressure;
extern float lastShotAvgBar, lastShotTargetBar, lastShotPeakBar;
extern long long lastShotTs;
extern bool pressureFault;
extern double pressureKp, pressureKi, pressureKd;
extern float pressureHistory[];
extern int pressureHistoryHead;
extern int pressureHistoryCount;

extern OpMode currentMode;
extern double brewSetpoint, brewKp, brewKi, brewKd;
extern double brewActiveKp, brewActiveKi, brewActiveKd;
extern double steamSetpoint, steamKp, steamKi, steamKd;
extern double steamMaxSafety;
extern void setOpMode(OpMode mode);
extern void refreshActiveProfileIfChanged();

extern unsigned long ecoTimeoutMin;
extern unsigned long steamAutoOffMin;
extern bool autoSleeping;
extern OpMode modeBeforeSleep;
extern void noteActivity();
extern void wakeFromSleep();

extern AutotuneState autotuneState;
extern String autotuneMessage;
extern void startAutotune(OpMode forMode);
extern void stopAutotune();

extern bool shotInProgress;
extern unsigned long shotStartMillis;
extern void startShot();
extern void stopShot(bool byUser);
extern bool lastShotStoppedByUser;
extern String lastShotProfile;
extern bool waterSensorEnabled, waterActiveLow, waterInterlock;
extern void applyWaterSettings();
extern const char *shotBlockedReason;
extern unsigned long shotBlockedAtMillis;
extern ShotPhase currentShotPhase;

extern unsigned long shotCount;
extern time_t lastDescaleTime;
extern unsigned long descaleShotThreshold;
extern unsigned long descaleDayThreshold;
extern void markDescaled();

extern String activeProfileName;
extern bool selectProfile(const char *name);
extern PlannerOutput plannerOut;
extern float estPressureHistory[];
extern float targetPressureHistory[];
extern int estHistoryHead;
extern int estHistoryCount;


extern bool schedEnabled[SCHED_MAX_COUNT];
extern int schedHour[SCHED_MAX_COUNT], schedMin[SCHED_MAX_COUNT];
extern bool schedModeSteam[SCHED_MAX_COUNT];
extern int schedTzOffsetMin;
extern void resetSchedFired(int i);

// See config.h "Shared-state lock" - this handler runs on the AsyncTCP task,
// a different task than controlTick(), so mutating PID/mode/shot globals
// needs the same lock controlTick() holds during its own tick.
extern void lockState();
extern void unlockState();

AsyncWebServer server(80);

// ============================================================================
// Web dashboard (self-contained: no external assets, no CDN, no build step -
// this string IS the shipped frontend, served straight from flash). Design
// system: dark-mode-only, coffee/steel palette as CSS custom properties
// ("tokens") at the top of <style>, so future features (pressure graph, water
// level, scale weight - see AGENTS.md roadmap) can reuse the same --steam/
// --green/--red/--amber semantics and .stat-tile/.chart-card components
// instead of inventing new colors/components per feature.
//
// Layout: mobile-first single page, no reload. The "Now" view (default,
// deep-linkable via #now/#tune/#history/#settings) carries only what needs to
// be legible at a glance from across the kitchen - temperature, mode, shot
// timer - everything else (PID tuning, network, MQTT, shot log, descale) is
// tucked behind the bottom tab bar. All four views share one /status poll
// and one <script> - switching tabs is just toggling `hidden`, no re-fetch.
//
// API contract is unchanged from the previous UI: same /update query params,
// same /status and /shots JSON field names - this is a frontend-only
// replacement, no firmware logic touched.
// ============================================================================
const char *index_html = R"rawliteral(
<!DOCTYPE HTML><html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <meta name="theme-color" content="#0e0f11">
  <link rel="manifest" href="/manifest.json">
  <link rel="icon" href="/icon.svg" type="image/svg+xml">
  <link rel="apple-touch-icon" href="/icon.svg">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
  <meta name="apple-mobile-web-app-title" content="BrewMaster">
  <title>GaggiaBrewMasterESP</title>
  <style>
    /* ---- Tokens (2026-10-15 redesign: matte charcoal, copper = heat,
       cyan = pressure/water, white = primary data). ---- */
    :root {
      --bg: #0e0f11;
      --surface: rgba(255,255,255,.035);
      --surface-2: rgba(255,255,255,.06);
      --border: rgba(255,255,255,.07);
      --border-soft: rgba(255,255,255,.05);
      --border-strong: rgba(255,255,255,.12);
      --text: #f5f5f3;
      --text-2: #a1a4a8;
      --text-dim: #80838a;
      --copper: #e39a62;
      --copper-deep: #d98c55;
      --copper-ink: #1c0f06;
      --cyan: #46e3f0;
      --danger: #f0716a;
      --warn: #e8b04b;
      --ok: #5fd39a;
      --radius: 22px;
      --radius-sm: 14px;
      --sp-1: 4px; --sp-2: 8px; --sp-3: 12px; --sp-4: 16px; --sp-5: 20px; --sp-6: 24px; --sp-8: 32px;
      --font: -apple-system, BlinkMacSystemFont, "SF Pro Text", "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
    }
    * { box-sizing: border-box; }
    html, body { margin: 0; background: var(--bg); color: var(--text); }
    body {
      font-family: var(--font); font-size: 15px; line-height: 1.4;
      -webkit-font-smoothing: antialiased; -webkit-text-size-adjust: 100%;
      font-variant-numeric: tabular-nums;
    }
    a { color: var(--text); }
    button, input, select { font-family: inherit; }
    :focus-visible { outline: 2px solid var(--cyan); outline-offset: 2px; }
    [hidden] { display: none !important; }
    @media (prefers-reduced-motion: reduce) { * { transition: none !important; animation: none !important; } }

    .label { font-size: 11px; font-weight: 600; letter-spacing: .16em; text-transform: uppercase; color: var(--text-dim); }
    .hint { color: var(--text-2); font-size: 13px; line-height: 1.5; margin: var(--sp-2) 0 0; }
    .hint-link { display: inline-block; margin-top: var(--sp-3); color: var(--text-2); font-size: 13px; }

    /* ---- Shell: phone = bottom tab bar, wide screens = left rail ---- */
    .app { min-height: 100vh; padding: 0 var(--sp-4) 104px; max-width: 1400px; margin: 0 auto; }
    .app.degraded main { opacity: .55; }
    .tabbar {
      position: fixed; left: 0; right: 0; bottom: 0; z-index: 20;
      display: flex; justify-content: center; gap: 48px;
      padding: 8px 0 10px;
      padding: 8px 0 calc(10px + env(safe-area-inset-bottom));
      background: rgba(14,15,17,.94); border-top: 1px solid var(--border-soft);
    }
    .tab {
      display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 4px;
      min-width: 76px; min-height: 52px; border: 0; border-radius: 16px; background: transparent;
      color: var(--text-dim); font-size: 11px; font-weight: 500; cursor: pointer;
    }
    .tab svg { width: 22px; height: 22px; }
    .tab.active { color: var(--text); background: var(--surface-2); }
    .tab .rail-logo { display: none; }

    /* ---- Header ---- */
    .topbar { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: var(--sp-3); padding: 28px 0 var(--sp-4); }
    .brand-status { display: flex; flex-direction: column; gap: 4px; min-width: 0; }
    .status-line { display: flex; align-items: center; gap: 10px; }
    .status-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--text-dim); flex: none; }
    #mode_status { font-size: 22px; font-weight: 500; letter-spacing: -.02em; }
    .pill.brew .status-dot, .pill.steam .status-dot { background: var(--copper); box-shadow: 0 0 0 4px rgba(227,154,98,.15); }
    .pill.ready .status-dot { background: var(--ok); box-shadow: 0 0 0 4px rgba(95,211,154,.15); }
    .pill.shot .status-dot { background: var(--cyan); box-shadow: 0 0 0 4px rgba(70,227,240,.16); }
    .pill.fault .status-dot { background: var(--danger); box-shadow: 0 0 0 4px rgba(240,113,106,.16); }
    .last-updated { font-size: 11px; color: var(--text-dim); }
    .top-controls { display: flex; align-items: center; gap: var(--sp-2); width: 100%; }
    .top-controls .mode-switch { flex: 1 1 auto; }
    .top-controls .mode-btn { flex: 1 1 0; min-width: 0; }
    .tank-pill {
      display: inline-flex; align-items: center; gap: 8px; height: 44px; padding: 0 16px;
      border-radius: 22px; background: var(--surface); border: 1px solid var(--border); font-size: 14px; color: var(--text-2);
    }
    .tank-pill svg { width: 12px; height: 16px; }
    .tank-pill.ok { color: var(--text); }
    .tank-pill.low { color: #1a0d0c; background: var(--danger); border-color: var(--danger); }
    .tank-pill.low svg path { stroke: #1a0d0c; }
    .mode-switch { display: flex; padding: 3px; border-radius: var(--radius-sm); background: var(--surface); border: 1px solid var(--border); }
    .mode-btn {
      min-width: 82px; height: 38px; border: 0; border-radius: 11px; background: transparent;
      color: var(--text-2); font-size: 14px; cursor: pointer;
    }
    .mode-btn.active { background: rgba(255,255,255,.09); color: var(--text); font-weight: 600; }
    .mode-btn.mode-off.active { color: var(--text); }
    .mode-btn.mode-steam.active { color: #ffd2a6; }

    /* ---- Banners ---- */
    .alerts { display: flex; flex-direction: column; gap: var(--sp-2); }
    .alerts:not(:empty) { margin-bottom: var(--sp-2); }
    .banner {
      display: none; align-items: center; justify-content: space-between; gap: var(--sp-3);
      padding: 12px 16px; border-radius: var(--radius-sm); font-size: 14px; line-height: 1.4;
      background: var(--surface); border: 1px solid var(--border);
    }
    .banner a { color: inherit; font-weight: 600; }
    .banner-error { background: rgba(240,113,106,.12); border-color: rgba(240,113,106,.45); color: #ffd6d3; }
    .banner-warn { background: rgba(232,176,75,.10); border-color: rgba(232,176,75,.40); color: #ffe6b8; }
    .banner-info { background: rgba(70,227,240,.06); border-color: rgba(70,227,240,.25); color: #d4f8fb; }
    .btn-chip, .btn-chip-sm {
      border: 1px solid var(--border-strong); background: transparent; color: var(--text);
      border-radius: 999px; cursor: pointer; white-space: nowrap;
    }
    .btn-chip { height: 36px; padding: 0 14px; font-size: 13px; font-weight: 600; }
    .btn-chip-sm { height: 32px; min-width: 32px; padding: 0 12px; font-size: 12px; }
    .btn-chip-sm.danger { color: var(--danger); border-color: rgba(240,113,106,.4); }
    .btn-chip-sm.armed { background: var(--danger); color: #1a0d0c; border-color: var(--danger); }

    /* ---- Cards ---- */
    .card {
      background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius);
      padding: var(--sp-5); box-shadow: inset 0 1px 0 rgba(255,255,255,.035);
    }
    .card + .card { margin-top: var(--sp-3); }
    .tab-section-title { font-size: 17px; font-weight: 600; letter-spacing: -.01em; margin-bottom: var(--sp-2); }
    .metric-row { display: flex; align-items: center; justify-content: space-between; gap: var(--sp-3); padding: 10px 0; border-bottom: 1px solid var(--border-soft); font-size: 14px; color: var(--text-2); }
    .metric-row b { color: var(--text); font-weight: 500; margin-left: auto; }
    .empty-hint { color: var(--text-dim); font-size: 14px; }

    /* ---- Brew view ---- */
    .brew-grid { display: flex; flex-direction: column; gap: var(--sp-4); }
    .brew-col { display: flex; flex-direction: column; gap: var(--sp-4); min-width: 0; }

    .dial { position: relative; width: 100%; max-width: 400px; margin: 0 auto; }
    .dial::before { content: ""; display: block; padding-top: 100%; }
    .dial-glow {
      position: absolute; top: 6%; right: 6%; bottom: 6%; left: 6%; border-radius: 50%; pointer-events: none;
      background: radial-gradient(circle, rgba(227,154,98,.13) 0%, rgba(227,154,98,0) 68%);
      transition: opacity .6s;
    }
    .dial.off .dial-glow { opacity: 0; }
    .dial svg { position: absolute; top: 0; left: 0; width: 100%; height: 100%; overflow: visible; }
    .dial-track { fill: none; stroke: rgba(255,255,255,.05); stroke-width: 3; stroke-linecap: round; }
    .dial-temp { fill: none; stroke: url(#dialCopper); stroke-width: 3; stroke-linecap: round; transition: stroke-dasharray .8s ease; }
    .dial-temp-glow { fill: none; stroke: var(--copper); stroke-opacity: .55; stroke-width: 4; stroke-linecap: round; filter: url(#dialBlur); transition: stroke-dasharray .8s ease; }
    .dial-temp.over, .dial-temp-glow.over { stroke: var(--danger); }
    .dial-press-track { fill: none; stroke: rgba(70,227,240,.09); stroke-width: 2; stroke-linecap: round; }
    .dial-temp.zero, .dial-temp-glow.zero, .dial-press.zero { opacity: 0; }
    .dial-press { fill: none; stroke: var(--cyan); stroke-width: 2.5; stroke-linecap: round; transition: stroke-dasharray .5s ease; }
    .dial-press.ready { stroke: var(--cyan); }
    .dial-press.heating { stroke: rgba(70,227,240,.6); }
    .dial-press.over { stroke: var(--warn); }
    .tick { stroke: #fff; stroke-opacity: .16; stroke-width: 1.2; stroke-linecap: round; transition: stroke .4s, stroke-opacity .4s; }
    .tick.major { stroke-opacity: .32; stroke-width: 1.6; }
    .tick.lit { stroke: var(--copper); stroke-opacity: .55; }
    .tick.major.lit { stroke-opacity: .95; }
    .dial-marker { transition: transform .8s ease; }
    /* At working temperature (2026-10-16): a soft green halo along the arc up
       to the target marker, a pulse at the temp dot, the marker turns green
       and the caption reads "At temp" - so "ready" is visible from across
       the kitchen, not only as a number. .at-temp is set by updateDial(). */
    .dial-ready-halo { fill: none; stroke: var(--ok); stroke-width: 10; stroke-linecap: round; opacity: 0; filter: url(#dialBlur); transition: opacity .6s ease; }
    .dial.at-temp .dial-ready-halo { opacity: .5; }
    .dial-pulse { fill: none; stroke: var(--ok); stroke-width: 1.5; opacity: 0; }
    .dial.at-temp .dial-pulse { opacity: 1; animation: dialPulse 1.8s ease-out infinite; }
    @keyframes dialPulse { 0% { r: 6px; stroke-opacity: .9; } 100% { r: 20px; stroke-opacity: 0; } }
    .dial.at-temp #dial_target polygon { fill: var(--ok); }
    .gauge-target .ready-mark { display: none; color: var(--ok); font-weight: 600; }
    .dial.at-temp .gauge-target .ready-mark { display: inline; }
    .dial.at-temp .gauge-target .target-word { display: none; }
    .dial.at-temp .gauge-target b { color: var(--ok); }
    .dial-center {
      position: absolute; top: 0; right: 0; bottom: 0; left: 0; display: flex; flex-direction: column; align-items: center; justify-content: center;
      text-align: center; pointer-events: none;
    }
    .dial-temp-value { display: flex; align-items: flex-start; margin-top: 2px; }
    #temp { font-size: 84px; font-weight: 200; letter-spacing: -.05em; line-height: 1; }
    .deg { font-size: 26px; font-weight: 300; color: var(--copper); margin: 8px 0 0 2px; }
    .gauge-target { margin-top: 6px; font-size: 14px; color: var(--text-2); }
    .gauge-target b { color: var(--text); font-weight: 600; }
    .dial-press-line { margin-top: 18px; display: flex; align-items: center; gap: 10px; font-size: 13px; color: var(--text-2); }
    .dial-press-line .dot { width: 6px; height: 6px; border-radius: 50%; background: var(--cyan); }
    .dial-press-line b { color: var(--text); font-weight: 500; }
    .dial-press-line .sep { width: 1px; height: 12px; background: rgba(255,255,255,.14); }

    .section-head { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 10px; }
    .section-head a { font-size: 13px; color: var(--text-2); text-decoration: none; }
    .preset-row { display: flex; flex-wrap: wrap; gap: var(--sp-2); }
    .btn-preset {
      flex: 1 1 22%; min-width: 72px; min-height: 80px; padding: 10px 6px;
      display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 8px;
      border-radius: 16px; background: var(--surface); border: 1px solid var(--border);
      color: var(--text-2); font-size: 13px; font-weight: 500; cursor: pointer;
      transition: border-color .2s, background .2s, box-shadow .2s;
    }
    .btn-preset .profile-mini { width: 64px; height: 20px; }
    .btn-preset.active {
      color: var(--text); background: rgba(70,227,240,.07); border-color: rgba(70,227,240,.45);
      box-shadow: 0 0 24px -8px rgba(70,227,240,.5);
    }

    .card-shot { display: flex; flex-direction: column; gap: var(--sp-3); }
    .shot-head { display: flex; align-items: flex-end; justify-content: space-between; gap: var(--sp-4); }
    .shot-time { font-size: 56px; font-weight: 200; letter-spacing: -.05em; line-height: 1; transition: color .3s; }
    .shot-time.in-window { color: var(--ok); }
    .shot-time.over { color: var(--warn); }
    .shot-sub { margin-top: 6px; font-size: 13px; color: var(--text-2); }
    .shot-progress { height: 4px; border-radius: 2px; background: rgba(255,255,255,.07); overflow: hidden; }
    .shot-progress span { display: block; height: 100%; width: 0; border-radius: 2px; background: linear-gradient(90deg, #b8703f, #eaa672); transition: width 1s linear; }
    .shot-progress span.in-window { background: var(--ok); }
    .shot-progress span.over { background: var(--warn); }
    .btn-shot {
      position: relative; width: 100%; height: 72px; border: 0; border-radius: 36px; cursor: pointer;
      display: flex; align-items: center; justify-content: center; gap: 12px;
      font-size: 18px; font-weight: 600; letter-spacing: -.01em;
      color: var(--copper-ink); background: linear-gradient(180deg, #eaa672 0%, #d98c55 100%);
      box-shadow: inset 0 1px 0 rgba(255,255,255,.35), 0 14px 36px -12px rgba(227,154,98,.65);
      transition: background .2s, color .2s, box-shadow .2s;
    }
    .btn-shot::before {
      content: ""; width: 0; height: 0; border-style: solid; border-width: 8px 0 8px 13px;
      border-color: transparent transparent transparent var(--copper-ink);
    }
    .btn-shot.running {
      color: var(--text); background: var(--surface-2); border: 1px solid var(--border-strong);
      box-shadow: inset 0 1px 0 rgba(255,255,255,.06);
    }
    .btn-shot.running::before { width: 13px; height: 13px; border: 0; border-radius: 3px; background: var(--text); }
    .btn-shot.blocked { color: var(--text-2); background: var(--surface); border: 1px solid var(--border); box-shadow: none; }
    .btn-shot.blocked::before { border-color: transparent transparent transparent var(--text-dim); }
    .shot-block-msg { display: none; padding: 10px 14px; border-radius: var(--radius-sm); background: rgba(240,113,106,.12); border: 1px solid rgba(240,113,106,.45); color: #ffd6d3; font-size: 14px; }
    .shot-block-msg.show { display: block; }

    .chart-card { padding: 16px 18px 12px; }
    .chart-head { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; }
    .legend { display: flex; gap: 14px; font-size: 11px; color: var(--text-2); }
    .legend i { display: inline-block; width: 12px; height: 2px; border-radius: 1px; margin-right: 6px; vertical-align: middle; }
    .chart-wrap { position: relative; }
    .chart-wrap canvas { display: block; width: 100%; height: 170px; }
    .chart-foot { display: flex; justify-content: space-between; margin-top: 6px; font-size: 11px; color: var(--text-dim); }

    /* Live shot block under the dial: phase steps + big readouts. */
    .shot-live { display: none; flex-direction: column; gap: var(--sp-4); }
    .app.shooting .shot-live { display: flex; }
    .phase-steps { display: flex; flex-wrap: wrap; align-items: center; justify-content: center; gap: 6px 8px; }
    .phase-step { font-size: 13px; color: var(--text-dim); padding: 5px 0; }
    .phase-step.done { color: var(--text-2); }
    .phase-step.current { color: var(--bg); background: var(--cyan); font-weight: 600; padding: 5px 12px; border-radius: 999px; }
    .phase-sep { width: 14px; height: 1px; background: rgba(255,255,255,.18); }
    .live-reads { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); }
    .live-read { display: flex; flex-direction: column; align-items: center; gap: 4px; }
    .live-read + .live-read { border-left: 1px solid var(--border); }
    .live-read b { font-size: 34px; font-weight: 300; letter-spacing: -.03em; line-height: 1.05; }
    .live-read b small { font-size: 13px; color: var(--text-dim); margin-left: 3px; letter-spacing: 0; font-weight: 400; }
    .live-read.press b { color: var(--cyan); }
    .live-read.temp b { color: var(--copper); }

    /* -/+ steppers for the two everyday temperatures; save automatically. */
    .stepper { display: flex; align-items: center; gap: var(--sp-3); margin-top: var(--sp-3); }
    .step-btn {
      flex: none; width: 56px; height: 56px; border-radius: 28px; border: 1px solid var(--border-strong);
      background: transparent; color: var(--text); font-size: 26px; font-weight: 300; line-height: 1; cursor: pointer;
    }
    .step-btn:active { background: var(--surface-2); }
    .stepper input[type=number] { flex: 1 1 auto; min-width: 0; height: 56px; text-align: center; font-size: 30px; font-weight: 300; letter-spacing: -.02em; -moz-appearance: textfield; }
    .stepper input::-webkit-outer-spin-button, .stepper input::-webkit-inner-spin-button { -webkit-appearance: none; margin: 0; }
    .stepper-note { display: flex; justify-content: space-between; align-items: center; min-height: 20px; margin-top: 8px; font-size: 12px; color: var(--text-dim); }

    .last-shot-sum { display: flex; flex-wrap: wrap; align-items: baseline; gap: 6px 14px; color: var(--text-2); font-size: 14px; }
    .last-shot-sum b { color: var(--text); font-weight: 400; font-size: 22px; letter-spacing: -.02em; }
    .taste-row { display: flex; gap: var(--sp-2); margin-top: var(--sp-3); }
    .yield-row { display: flex; align-items: center; gap: 10px; margin-top: 12px; }
    .yield-row label { font-size: 13px; color: var(--text-2); white-space: nowrap; }
    .yield-row input { flex: 1; min-width: 0; height: 44px; }
    .yield-row button { height: 44px; padding: 0 18px; border-radius: 12px; border: 1px solid rgba(255,255,255,.12); background: rgba(255,255,255,.06); color: var(--text); font: inherit; font-size: 14px; cursor: pointer; }
    .taste-btn {
      flex: 1 1 0; height: 48px; border-radius: var(--radius-sm); cursor: pointer;
      background: var(--surface); border: 1px solid var(--border); color: var(--text-2); font-size: 14px;
    }
    .taste-btn.active { color: var(--text); font-weight: 600; background: rgba(227,154,98,.12); border-color: rgba(227,154,98,.55); }
    .taste-tip { min-height: 20px; margin: 10px 0 0; font-size: 13px; color: var(--text-2); }
    .dialin { margin: 12px 0 2px; padding: 10px 12px; border-radius: 12px; background: rgba(255,255,255,.035); border: 1px solid rgba(255,255,255,.07); font-size: 14px; line-height: 1.45; color: var(--text); }
    .dialin b { font-weight: 600; }
    .dialin.act { border-color: rgba(227,154,98,.45); background: rgba(227,154,98,.08); }
    .dialin.ok { border-color: rgba(95,211,154,.4); background: rgba(95,211,154,.07); }
    .dialin small { display: block; margin-top: 3px; font-size: 12px; color: var(--text-2); }
    .link-btn { border: 0; background: none; padding: 8px 0; color: var(--text-2); font-size: 13px; cursor: pointer; text-decoration: underline; text-underline-offset: 3px; }

    /* Focus mode while a shot runs: nothing to wander off to. */
    .app.shooting .tabbar, .app.shooting .profiles-block, .app.shooting .last-shot-card, .app.shooting .mode-switch { display: none !important; }
    .app.shooting .shot-time { font-size: 96px; }
    /* Phone during a shot: timer + Stop first (thumb reach), smaller dial. */
    @media (max-width: 899px) {
      .app.shooting .brew-col.right { order: -1; }
      .app.shooting .dial { max-width: 280px; }
    }

    /* ---- Forms ---- */
    .field { display: flex; flex-direction: column; gap: 6px; margin-top: var(--sp-3); }
    .field label, .check-row { font-size: 13px; color: var(--text-2); }
    .field-row, .field-row-3 { display: grid; grid-gap: var(--sp-3); gap: var(--sp-3); }
    .field-row { grid-template-columns: 1fr 1fr; }
    .field-row-3 { grid-template-columns: 1fr 1fr 1fr; }
    input[type=number], input[type=text], input[type=password], input[type=time], select {
      width: 100%; height: 48px; padding: 0 14px; border-radius: 12px;
      background: rgba(0,0,0,.25); border: 1px solid var(--border-strong); color: var(--text); font-size: 16px;
      -webkit-appearance: none; appearance: none;
    }
    select { background-image: linear-gradient(45deg, transparent 50%, var(--text-dim) 50%), linear-gradient(135deg, var(--text-dim) 50%, transparent 50%); background-position: calc(100% - 20px) 21px, calc(100% - 15px) 21px; background-size: 5px 5px; background-repeat: no-repeat; }
    input:focus, select:focus { border-color: var(--cyan); outline: none; }
    .check-row { display: flex; align-items: center; gap: 12px; min-height: 44px; margin-top: var(--sp-2); color: var(--text); font-size: 15px; cursor: pointer; }
    .check-row input { width: 22px; height: 22px; accent-color: var(--cyan); }
    .submit, .btn-secondary, .btn-danger, .btn-autotune {
      display: inline-flex; align-items: center; justify-content: center; min-height: 46px; padding: 0 22px;
      margin-top: var(--sp-4); margin-right: var(--sp-2); border-radius: 23px; cursor: pointer;
      font-size: 15px; font-weight: 600; text-decoration: none;
    }
    .submit { border: 0; background: var(--text); color: var(--bg); }
    .submit:disabled, .btn-autotune:disabled, .btn-secondary:disabled { opacity: .45; cursor: default; }
    .btn-secondary { border: 1px solid var(--border-strong); background: transparent; color: var(--text); }
    .btn-danger { border: 1px solid rgba(240,113,106,.5); background: transparent; color: var(--danger); }
    .btn-autotune { border: 1px solid rgba(227,154,98,.5); background: rgba(227,154,98,.1); color: #ffd2a6; }
    .btn-autotune.running { background: var(--copper); color: var(--copper-ink); }
    .autotune-status { display: block; margin-top: var(--sp-2); font-size: 13px; color: var(--text-2); }
    .save-confirm { display: inline-block; font-size: 13px; color: var(--ok); opacity: 0; transition: opacity .2s; vertical-align: middle; }
    .save-confirm.show { opacity: 1; }
    .save-confirm.error { color: var(--danger); }

    /* ---- Settings ---- */
    .settings-layout { display: flex; flex-direction: column; gap: var(--sp-4); }
    .settings-nav { display: flex; gap: var(--sp-2); overflow-x: auto; padding-bottom: 2px; -webkit-overflow-scrolling: touch; }
    .settings-nav-title { display: none; }
    .sec-btn {
      flex: none; display: flex; align-items: center; justify-content: space-between; gap: var(--sp-3);
      min-height: 44px; padding: 0 16px; border-radius: 22px; border: 1px solid var(--border);
      background: var(--surface); color: var(--text-2); font-size: 14px; cursor: pointer; text-align: left;
    }
    .sec-btn small { display: none; }
    .sec-btn svg { display: none; }
    .sec-btn.active { color: var(--bg); background: var(--text); border-color: var(--text); font-weight: 600; }
    .set-panel h2 { margin: 0 0 var(--sp-3); font-size: 22px; font-weight: 500; letter-spacing: -.02em; }
    .set-grid { display: grid; grid-template-columns: 1fr; grid-gap: var(--sp-3); gap: var(--sp-3); }
    .set-grid > .card + .card { margin-top: 0; }
    .adv { border-top: 1px solid var(--border-soft); }
    .adv:first-of-type { border-top: 0; }
    .adv > summary {
      list-style: none; display: flex; justify-content: space-between; align-items: center; gap: var(--sp-3);
      min-height: 64px; padding: 10px 0; cursor: pointer;
    }
    .adv > summary::-webkit-details-marker { display: none; }
    .adv > summary span { display: flex; flex-direction: column; gap: 2px; }
    .adv > summary small { font-size: 12px; color: var(--text-dim); }
    .adv > summary::after { content: ""; width: 8px; height: 8px; border-right: 2px solid var(--text-dim); border-bottom: 2px solid var(--text-dim); transform: rotate(-45deg); transition: transform .2s; flex: none; margin-right: 6px; }
    .adv[open] > summary::after { transform: rotate(45deg); }
    .adv-body { padding: 0 0 var(--sp-5); }
    .adv-body + .adv-body { border-top: 1px dashed var(--border-soft); padding-top: var(--sp-4); }
    .sub-title { font-size: 14px; font-weight: 600; margin: var(--sp-2) 0 0; }
    .disclosure { margin-top: var(--sp-3); border-top: 1px solid var(--border-soft); padding-top: var(--sp-2); }
    .disclosure > summary { cursor: pointer; min-height: 40px; display: flex; align-items: center; color: var(--text-2); font-size: 14px; }
    .sched-slot { padding: var(--sp-3) 0; border-bottom: 1px solid var(--border-soft); }
    .water-live { display: flex; justify-content: space-between; align-items: center; gap: var(--sp-3); margin-top: var(--sp-3); padding-top: var(--sp-3); border-top: 1px solid var(--border-soft); font-size: 14px; color: var(--text-2); }
    .water-live b.ok { color: var(--cyan); }
    .water-live b.low { color: var(--danger); }
    .profile-row { display: flex; flex-wrap: wrap; align-items: center; gap: var(--sp-3); padding: 14px 0; border-top: 1px solid var(--border-soft); }
    .profile-row:first-child { border-top: 0; }
    .profile-row-main { flex: 1 1 200px; display: flex; flex-direction: column; gap: 2px; cursor: pointer; min-width: 0; }
    .profile-row-main b { font-weight: 500; font-size: 15px; }
    .profile-row-main span { font-size: 12px; color: var(--text-dim); }
    .profile-row .profile-mini { width: 90px; height: 26px; flex: none; }
    .profile-row.active .profile-row-main b::after { content: "Active"; margin-left: 10px; padding: 2px 9px; border-radius: 999px; font-size: 11px; font-weight: 600; color: var(--cyan); background: rgba(70,227,240,.1); }
    .profile-row-actions { display: flex; flex-wrap: wrap; gap: 6px; }
    .footer { margin-top: var(--sp-8); text-align: center; font-size: 12px; color: var(--text-dim); }
    .footer a { color: var(--text-2); }

    .skeleton { color: transparent !important; background: rgba(255,255,255,.06); border-radius: 8px; }

    /* ---- Tablet / landscape: left rail, two columns ---- */
    @media (min-width: 900px) {
      .app { padding: 0 40px 32px 136px; }
      .tabbar {
        top: 0; right: auto; width: 96px; flex-direction: column; justify-content: flex-start; align-items: center;
        gap: 8px; padding: 28px 0; border-top: 0; border-right: 1px solid var(--border-soft); background: var(--bg);
      }
      .tab { width: 72px; min-height: 64px; }
      .tab .rail-logo {
        display: flex; align-items: center; justify-content: center; width: 40px; height: 40px; margin-bottom: 20px;
        border-radius: 20px; border: 1px solid rgba(227,154,98,.5); color: var(--copper); font-size: 16px; font-weight: 600;
      }
      .topbar { padding-top: 22px; padding-bottom: 12px; }
      .brew-col.right { gap: 12px; }
      .brew-col.right .card { padding: 16px 20px; }
      .taste-row { margin-top: 10px; }
      .taste-tip { margin-top: 8px; }
      .top-controls { width: auto; }
      .top-controls .mode-switch { flex: none; }
      .top-controls .mode-btn { flex: none; min-width: 82px; }
      .chart-wrap canvas { height: 120px; }
      .dial { max-width: 380px; }
      .app.shooting .dial { max-width: 360px; }
      .app.shooting .live-read b { font-size: 44px; }
      .card-shot { gap: 10px; }
      .brew-grid { flex-direction: row; align-items: flex-start; gap: 40px; }
      .brew-col { flex: 1 1 0; }
      .brew-col.right { padding-top: 4px; }
      #temp { font-size: 108px; }
      .deg { font-size: 32px; margin-top: 10px; }
      .shot-time { font-size: 64px; }
      .app.shooting { padding-left: 40px; }
      .app.shooting .shot-time { font-size: 140px; }
      .settings-layout { flex-direction: row; align-items: flex-start; gap: 40px; }
      .settings-nav { flex-direction: column; width: 280px; flex: none; overflow: visible; position: sticky; top: 24px; }
      .settings-nav-title { display: block; margin: 0 12px 18px; font-size: 30px; font-weight: 400; letter-spacing: -.03em; }
      .sec-btn { min-height: 64px; padding: 10px 14px; border: 0; border-radius: 16px; background: transparent; }
      .sec-btn span { display: flex; flex-direction: column; gap: 2px; }
      .sec-btn small { display: block; font-size: 12px; color: var(--text-dim); font-weight: 400; }
      .sec-btn svg { display: block; width: 14px; height: 14px; }
      .sec-btn.active { color: var(--text); background: var(--surface-2); }
      .set-panels { flex: 1 1 0; min-width: 0; padding-top: 6px; }
      .set-grid { grid-template-columns: 1fr 1fr; align-items: start; }
      .set-grid > .wide { grid-column: 1 / -1; }
    }
    /* No flexbox `gap` (Chrome < 84, e.g. the kitchen tablet): the script adds
       .no-flexgap to <html> and these margins stand in for it. Keep in sync with
       the gap values above. */
    .no-flexgap .tabbar > * + * { margin-left: 48px; }
    .no-flexgap .btn-shot::before { margin-right: 12px; }
    .no-flexgap .tab > * + * { margin-top: 4px; }
    .no-flexgap .brand-status > * + * { margin-top: 4px; }
    .no-flexgap .status-line > * + * { margin-left: 10px; }
    .no-flexgap .top-controls > * + * { margin-left: 8px; }
    .no-flexgap .tank-pill > * + * { margin-left: 8px; }
    .no-flexgap .alerts > * + * { margin-top: 8px; }
    .no-flexgap .banner > * + * { margin-left: 12px; }
    .no-flexgap .metric-row > * + * { margin-left: 12px; }
    .no-flexgap .brew-grid > * + * { margin-top: 16px; }
    .no-flexgap .brew-col > * + * { margin-top: 16px; }
    .no-flexgap .dial-press-line > * + * { margin-left: 10px; }
    .no-flexgap .btn-preset > * + * { margin-top: 8px; }
    .no-flexgap .card-shot > * + * { margin-top: 12px; }
    .no-flexgap .shot-head > * + * { margin-left: 16px; }
    .no-flexgap .btn-shot > * + * { margin-left: 12px; }
    .no-flexgap .legend > * + * { margin-left: 14px; }
    .no-flexgap .shot-live > * + * { margin-top: 16px; }
    .no-flexgap .live-read > * + * { margin-top: 4px; }
    .no-flexgap .stepper > * + * { margin-left: 12px; }
    .no-flexgap .taste-row > * + * { margin-left: 8px; }
    .no-flexgap .field > * + * { margin-top: 6px; }
    .no-flexgap .check-row > * + * { margin-left: 12px; }
    .no-flexgap .settings-layout > * + * { margin-top: 16px; }
    .no-flexgap .settings-nav > * + * { margin-left: 8px; }
    .no-flexgap .sec-btn > * + * { margin-left: 12px; }
    .no-flexgap .adv > summary > * + * { margin-left: 12px; }
    .no-flexgap .adv > summary span > * + * { margin-top: 2px; }
    .no-flexgap .water-live > * + * { margin-left: 12px; }
    .no-flexgap .profile-row-main > * + * { margin-top: 2px; }
    .no-flexgap .topbar > * { margin: 0 12px 12px 0; }
    .no-flexgap .preset-row > * { margin: 0 8px 8px 0; }
    .no-flexgap .phase-steps > * { margin: 0 8px 6px 0; }
    .no-flexgap .last-shot-sum > * { margin: 0 14px 6px 0; }
    .no-flexgap .profile-row > * { margin: 0 12px 12px 0; }
    .no-flexgap .profile-row-actions > * { margin: 0 6px 6px 0; }
    @media (min-width: 900px) {
      .no-flexgap .tabbar > * + * { margin-left: 0; margin-top: 8px; }
      .no-flexgap .brew-grid > * + *, .no-flexgap .settings-layout > * + * { margin-top: 0; margin-left: 40px; }
      .no-flexgap .settings-nav > * + * { margin-left: 0; margin-top: 8px; }
      .no-flexgap .brew-col.right > * + * { margin-top: 12px; }
      .no-flexgap .card-shot > * + * { margin-top: 10px; }
      .no-flexgap .sec-btn span > * + * { margin-top: 2px; }
    }
    @media (min-width: 900px) and (max-height: 700px) {
      .dial, .app.shooting .dial { max-width: 320px; }
      #temp { font-size: 92px; }
    }
  </style>
</head>
<body>
  <div class="app" id="app_root">
    <nav class="tabbar" role="tablist" aria-label="Sections">
      <button type="button" class="tab active" role="tab" aria-selected="true" aria-controls="view-now" id="tab-now" data-tab="now">
        <span class="rail-logo" aria-hidden="true">G</span>
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" aria-hidden="true"><path d="M5 9h12v5a5 5 0 0 1-5 5h-2a5 5 0 0 1-5-5z"/><path d="M17 11h1.5a2 2 0 0 1 0 4H17"/><path d="M9 3v3M13 3v3"/></svg>
        <span>Brew</span>
      </button>
      <button type="button" class="tab" role="tab" aria-selected="false" tabindex="-1" aria-controls="view-settings" id="tab-settings" data-tab="settings">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" aria-hidden="true"><path d="M4 7h10M18 7h2M4 17h4M12 17h8"/><circle cx="16" cy="7" r="2"/><circle cx="10" cy="17" r="2"/></svg>
        <span>Settings</span>
      </button>
    </nav>

    <header class="topbar">
      <div class="brand-status">
        <span class="label">Gaggia Classic</span>
        <div id="status_pill" class="pill off status-line" aria-live="polite"><span class="status-dot" aria-hidden="true"></span><span id="mode_status">--</span></div>
        <div id="last_updated" class="last-updated">--</div>
      </div>
      <div class="top-controls">
        <div id="tank_pill" class="tank-pill" aria-live="polite">
          <svg viewBox="0 0 12 16" aria-hidden="true"><path d="M6 1.5C4 4.4 1.5 7.3 1.5 10a4.5 4.5 0 0 0 9 0C10.5 7.3 8 4.4 6 1.5z" fill="none" stroke="#46e3f0" stroke-width="1.4"/></svg>
          <span id="tank_pill_text">Tank OK</span>
        </div>
        <div class="mode-switch" role="group" aria-label="Boiler mode">
          <button type="button" onclick="setMode('off')" id="btn_off" class="mode-btn mode-off" aria-pressed="false">Off</button>
          <button type="button" onclick="setMode('brew')" id="btn_brew" class="mode-btn mode-brew" aria-pressed="false">Brew</button>
          <button type="button" onclick="setMode('steam')" id="btn_steam" class="mode-btn mode-steam" aria-pressed="false">Steam</button>
        </div>
      </div>
    </header>

    <div class="alerts" role="status">
      <div id="offline_banner" class="banner banner-error">Controller unreachable &mdash; showing last known values. Check Wi-Fi.</div>
      <div id="fault_banner" class="banner banner-error">Sensor fault &mdash; heater held off. Check the PT100 wiring.</div>
      <div id="pressure_banner" class="banner banner-error"></div>
      <div id="pump_banner" class="banner banner-warn"></div>
      <div id="water_banner" class="banner banner-warn">Refill the water tank. New shots are paused until it's back.</div>
      <div id="cal_banner" class="banner banner-info"><span>Pump not calibrated &mdash; shots use a default pump model.</span><a href="#settings" onclick="openSettings('advanced','adv_pump'); return false;">Calibrate</a></div>
      <div id="sleep_banner" class="banner banner-info">
        <span id="sleep_banner_text">Asleep (eco timeout) &mdash; heater off</span>
        <span>
          <button type="button" onclick="wake()" class="btn-chip">Wake up</button>
          <button type="button" onclick="dismissSleepBanner()" class="btn-chip-sm" aria-label="Dismiss sleep notice">&times;</button>
        </span>
      </div>
      <div id="descale_banner_top" class="banner banner-warn"><span>Descale recommended.</span><a href="#settings" onclick="openSettings('maint'); return false;">Details</a></div>
    </div>

    <main>
    <div class="view" data-view="now" id="view-now" role="tabpanel" aria-labelledby="tab-now">
      <div class="brew-grid">
        <div class="brew-col left">
          <section class="dial" id="dial" aria-label="Boiler temperature and pump pressure">
            <div class="dial-glow" aria-hidden="true"></div>
            <svg viewBox="0 0 320 320" aria-hidden="true">
              <defs>
                <linearGradient id="dialCopper" x1="0" y1="1" x2="1" y2="0">
                  <stop offset="0" stop-color="#6e3a1a"/><stop offset=".55" stop-color="#d88c55"/><stop offset="1" stop-color="#ffd2a6"/>
                </linearGradient>
                <filter id="dialBlur" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="6"/></filter>
              </defs>
              <g id="dial_ticks"></g>
              <circle class="dial-track" cx="160" cy="160" r="136" stroke-dasharray="640.9 854.5" transform="rotate(135 160 160)"/>
              <circle id="temp_ready_halo" class="dial-ready-halo" cx="160" cy="160" r="136" stroke-dasharray="0 854.5" transform="rotate(135 160 160)"/>
              <circle id="temp_ring_glow" class="dial-temp-glow" cx="160" cy="160" r="136" stroke-dasharray="0 854.5" transform="rotate(135 160 160)"/>
              <circle id="temp_ring_fill" class="dial-temp" cx="160" cy="160" r="136" stroke-dasharray="0 854.5" transform="rotate(135 160 160)"/>
              <g id="dial_target" class="dial-marker" transform="rotate(135 160 160)" style="display:none"><polygon points="300,160 310,155 310,165" fill="#f5f5f3" transform="rotate(180 305 160)"/></g>
              <g id="dial_dot" class="dial-marker" transform="rotate(135 160 160)" style="display:none"><circle class="dial-pulse" cx="296" cy="160" r="6"/><circle cx="296" cy="160" r="9" fill="#e39a62" fill-opacity=".35" filter="url(#dialBlur)"/><circle cx="296" cy="160" r="5" fill="#fff4ea"/></g>
              <circle class="dial-press-track" cx="160" cy="160" r="118" stroke-dasharray="556.1 741.4" transform="rotate(135 160 160)"/>
              <circle id="pressure_ring_fill" class="dial-press" cx="160" cy="160" r="118" stroke-dasharray="0 741.4" transform="rotate(135 160 160)"/>
              <g id="press_peak" class="dial-marker" transform="rotate(135 160 160)" style="display:none"><circle cx="278" cy="160" r="3" fill="#46e3f0"/></g>
            </svg>
            <div class="dial-center">
              <span class="label">Boiler</span>
              <div class="dial-temp-value"><span id="temp" class="skeleton">--</span><span class="deg">&deg;</span></div>
              <div class="gauge-target" aria-live="polite"><span id="target_on"><span class="ready-mark">&#10003; At temp &middot; </span><span class="target-word">Target </span><b><span id="target" class="skeleton">--</span>&deg;</b></span><span id="target_off" style="display:none">Heater off</span></div>
              <div class="dial-press-line">
                <span class="dot" aria-hidden="true"></span><span><b id="pressure_gauge_val">--</b> bar</span>
                <span class="sep" aria-hidden="true"></span>
                <span id="pressure_target_caption">pressure</span>
              </div>
            </div>
          </section>

          <section class="shot-live" aria-label="Live shot">
            <div class="phase-steps" id="phase_steps"></div>
            <div class="live-reads">
              <div class="live-read press"><span class="label">Pressure</span><b><span id="live_press">--</span><small>bar</small></b></div>
              <div class="live-read temp"><span class="label">Boiler</span><b><span id="live_temp">--</span><small>&deg;</small></b></div>
              <div class="live-read"><span class="label">Pump</span><b><span id="live_pump">--</span><small>%</small></b></div>
            </div>
          </section>

          <section class="profiles-block" aria-label="Shot profile">
            <div class="section-head"><span class="label">Profile</span><a href="#settings" onclick="openSettings('brewing'); return false;">Edit profiles</a></div>
            <div class="preset-row" id="profile_chip_row"></div>
          </section>
        </div>

        <div class="brew-col right">
          <section class="card card-shot" aria-label="Shot">
            <div class="shot-head">
              <div>
                <div id="shot_time" class="shot-time" aria-live="polite">0:00</div>
                <div id="shot_auto_stop_label" class="shot-sub">--</div>
              </div>
            </div>
            <div class="shot-progress"><span id="shot_progress_bar"></span></div>
            <div id="shot_block_msg" class="shot-block-msg" role="alert">Tank is low. Refill, then tap again.</div>
            <button type="button" onclick="toggleShot()" id="btn_shot" class="btn-shot">Start shot</button>
          </section>

          <section class="card chart-card" aria-label="Live charts">
            <div class="chart-head">
              <span class="label">Last 2 min</span>
              <div class="legend"><span><i style="background:#e39a62"></i>Temp</span><span><i style="background:#46e3f0"></i>Pressure</span><span><i style="background:none;border-top:1.5px dashed rgba(255,255,255,.6);height:0"></i>Target</span></div>
            </div>
            <div class="chart-wrap"><canvas id="live_chart" aria-hidden="true"></canvas></div>
            <div class="chart-foot"><span id="temp_range_label">&deg;C</span><span id="pressure_label">-- bar</span></div>
          </section>

          <section class="card last-shot-card" id="last_shot_card" aria-label="Last shot" hidden>
            <div class="section-head"><span class="label" id="last_shot_title">Last shot</span></div>
            <div class="last-shot-sum" id="last_shot_sum"></div>
            <form class="yield-row" onsubmit="saveYield(event)">
              <label for="input_yield_quick">Cup weight (g)</label>
              <input type="number" inputmode="decimal" step="0.1" min="1" id="input_yield_quick" placeholder="e.g. 36">
              <button type="submit">Save</button>
            </form>
            <div class="dialin" id="dialin_tip" role="status" hidden></div>
            <div class="taste-row" role="group" aria-label="How did it taste?">
              <button type="button" class="taste-btn" data-taste="Sour" onclick="quickTaste('Sour')">Sour</button>
              <button type="button" class="taste-btn" data-taste="Balanced" onclick="quickTaste('Balanced')">Balanced</button>
              <button type="button" class="taste-btn" data-taste="Bitter" onclick="quickTaste('Bitter')">Bitter</button>
            </div>
            <p class="taste-tip" id="taste_tip">How did it taste?</p>
            <div id="shot_notes_card" hidden>
              <span id="shot_notes_title" hidden>Shot notes</span>
              <form onsubmit="submitShotNotes(event)">
                <input type="hidden" id="input_shot_note_index" value="-1">
                <div class="field-row">
                  <div class="field"><label for="input_shot_note_bean">Bean</label><input type="text" id="input_shot_note_bean" maxlength="24" placeholder="e.g. Ethiopia Yirgacheffe"></div>
                  <div class="field"><label for="input_shot_note_dose">Dose in (g)</label><input type="number" step="0.1" id="input_shot_note_dose" value="18"></div>
                  <div class="field"><label for="input_shot_note_yield">Yield out (g)</label><input type="number" step="0.1" min="0" id="input_shot_note_yield" placeholder="cup weight"></div>
                </div>
                <div class="field-row">
                  <div class="field"><label for="input_shot_note_grind">Grind setting</label><input type="text" id="input_shot_note_grind" maxlength="16" placeholder="e.g. 3.2"></div>
                  <div class="field">
                    <label for="input_shot_note_rating">Rating</label>
                    <select id="input_shot_note_rating">
                      <option value="0">Unrated</option><option value="1">1 - Poor</option><option value="2">2</option>
                      <option value="3">3 - OK</option><option value="4">4</option><option value="5">5 - Great</option>
                    </select>
                  </div>
                </div>
                <div class="field"><label for="input_shot_note_text">Notes</label><input type="text" id="input_shot_note_text" maxlength="60" placeholder="e.g. sour, tighten grind next time"></div>
                <button type="submit" class="submit">Save notes</button>
              </form>
            </div>
          </section>
        </div>
      </div>
    </div>

    <div class="view" data-view="settings" id="view-settings" role="tabpanel" aria-labelledby="tab-settings" hidden>
      <div class="settings-layout">
        <nav class="settings-nav" aria-label="Settings sections">
          <h1 class="settings-nav-title">Settings</h1>
          <button type="button" class="sec-btn active" data-sec="brewing" onclick="showSection('brewing')"><span>Brewing<small>Temperatures, profiles</small></span><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><path d="M9 6l6 6-6 6"/></svg></button>
          <button type="button" class="sec-btn" data-sec="power" onclick="showSection('power')"><span>Power &amp; schedule<small>Auto sleep, warm-up</small></span><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><path d="M9 6l6 6-6 6"/></svg></button>
          <button type="button" class="sec-btn" data-sec="maint" onclick="showSection('maint')"><span>Maintenance<small>Descale reminder</small></span><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><path d="M9 6l6 6-6 6"/></svg></button>
          <button type="button" class="sec-btn" data-sec="advanced" onclick="showSection('advanced')"><span>Advanced<small>PID, calibration, network</small></span><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><path d="M9 6l6 6-6 6"/></svg></button>
        </nav>

        <div class="set-panels">
          <section class="set-panel" data-sec="brewing">
            <h2>Brewing</h2>
            <div class="set-grid">
              <div class="card">
                <div class="tab-section-title">Brew temperature</div>
                <form action="/update" method="GET">
                  <label class="hint" for="input_brew_target" style="display:block">Boiler target for shots (&deg;C)</label>
                  <div class="stepper">
                    <button type="button" class="step-btn" aria-label="Lower brew temperature" onclick="stepField('input_brew_target','brew_target',-0.5,80,100)">&minus;</button>
                    <input type="number" step="0.1" min="80" max="100" name="brew_target" id="input_brew_target" value="">
                    <button type="button" class="step-btn" aria-label="Raise brew temperature" onclick="stepField('input_brew_target','brew_target',0.5,80,100)">+</button>
                  </div>
                  <div class="stepper-note"><span>Saves automatically</span><span class="save-confirm" id="brew_target_saved"></span></div>
                </form>
              </div>
              <div class="card">
                <div class="tab-section-title">Steam temperature</div>
                <form action="/update" method="GET">
                  <label class="hint" for="input_steam_target" style="display:block">Boiler target for milk (&deg;C)</label>
                  <div class="stepper">
                    <button type="button" class="step-btn" aria-label="Lower steam temperature" onclick="stepField('input_steam_target','steam_target',-1,110,150)">&minus;</button>
                    <input type="number" step="0.1" min="110" max="150" name="steam_target" id="input_steam_target" value="">
                    <button type="button" class="step-btn" aria-label="Raise steam temperature" onclick="stepField('input_steam_target','steam_target',1,110,150)">+</button>
                  </div>
                  <div class="stepper-note"><span>Saves automatically</span><span class="save-confirm" id="steam_target_saved"></span></div>
                </form>
              </div>
              <div class="card wide">
                <div class="tab-section-title">Shot profiles</div>
                <p class="hint">Tap a profile to use it. Tweak changes its temperature and stop time; Copy makes your own version to rename or delete.</p>
                <div id="profile_list_body"></div>
              </div>
              <div class="card wide" id="profile_tweak_card" hidden>
                <div class="tab-section-title" id="profile_tweak_title">Tweak</div>
                <form onsubmit="submitProfileTweak(event)">
                  <input type="hidden" id="input_tweak_name" value="">
                  <div class="field-row">
                    <div class="field" id="tweak_temp_field"><label for="input_tweak_temp">Temperature (&deg;C)</label><input type="number" step="0.5" min="80" max="100" id="input_tweak_temp"></div>
                    <div class="field"><label for="input_tweak_max_sec">Max shot time (s)</label><input type="number" step="1" min="5" max="120" id="input_tweak_max_sec"></div>
                  </div>
                  <details class="disclosure">
                    <summary>Pressure scale and pump trim</summary>
                    <div class="field-row">
                      <div class="field"><label for="input_tweak_scale">Pressure scale</label><input type="number" step="0.05" min="0.7" max="1.1" id="input_tweak_scale"></div>
                      <div class="field"><label for="input_tweak_trim">Pump trim (%)</label><input type="number" step="1" min="-20" max="20" id="input_tweak_trim"></div>
                    </div>
                    <p class="hint">Trim + pushes more water (faster shot), &minus; less. Adjust the grind first.</p>
                  </details>
                  <button type="submit" id="profile_tweak_submit" class="submit">Save</button>
                  <button type="button" onclick="closeProfileTweak()" class="btn-secondary">Cancel</button>
                </form>
              </div>
            </div>
          </section>

          <section class="set-panel" data-sec="power" hidden>
            <h2>Power &amp; schedule</h2>
            <div class="set-grid">
              <div class="card">
                <div class="tab-section-title">Auto sleep</div>
                <form action="/update" method="GET">
                  <div class="field"><label for="input_eco_min">Brew: heater off after (min, 0 = never)</label><input type="number" step="1" min="0" name="eco_timeout_min" id="input_eco_min" value=""></div>
                  <div class="field"><label for="input_steam_off_min">Steam: off after (min, 0 = never)</label><input type="number" step="1" min="0" name="steam_auto_off_min" id="input_steam_off_min" value=""></div>
                  <button type="submit" class="submit">Save</button>
                </form>
                <p class="hint">Counts from the last thing you did in this app. Status polling doesn't count.</p>
              </div>
              <div class="card">
                <div class="tab-section-title">Morning warm-up</div>
                <div id="ntp_banner" class="banner banner-warn" style="margin-top:var(--sp-2)">Clock not synced yet &mdash; warm-up won't fire until it is.</div>
                <form action="/update" method="GET">
                  <div class="sched-slot">
                    <label class="check-row"><input type="checkbox" id="input_sched0_en_cb" onchange="setSchedEnabled(0,this.checked)"> On</label>
                    <div class="field-row">
                      <div class="field"><label for="input_sched0_time">Time</label><input type="time" name="sched0_time" id="input_sched0_time" value="07:00"></div>
                      <div class="field"><label for="input_sched0_steam">Mode</label><select name="sched0_steam" id="input_sched0_steam"><option value="0">Brew</option><option value="1">Steam</option></select></div>
                    </div>
                  </div>
                  <details class="disclosure">
                    <summary>More times</summary>
                    <div class="sched-slot">
                      <label class="check-row"><input type="checkbox" id="input_sched1_en_cb" onchange="setSchedEnabled(1,this.checked)"> Time 2</label>
                      <div class="field-row">
                        <div class="field"><label for="input_sched1_time">Time</label><input type="time" name="sched1_time" id="input_sched1_time" value="07:00"></div>
                        <div class="field"><label for="input_sched1_steam">Mode</label><select name="sched1_steam" id="input_sched1_steam"><option value="0">Brew</option><option value="1">Steam</option></select></div>
                      </div>
                    </div>
                    <div class="sched-slot">
                      <label class="check-row"><input type="checkbox" id="input_sched2_en_cb" onchange="setSchedEnabled(2,this.checked)"> Time 3</label>
                      <div class="field-row">
                        <div class="field"><label for="input_sched2_time">Time</label><input type="time" name="sched2_time" id="input_sched2_time" value="07:00"></div>
                        <div class="field"><label for="input_sched2_steam">Mode</label><select name="sched2_steam" id="input_sched2_steam"><option value="0">Brew</option><option value="1">Steam</option></select></div>
                      </div>
                    </div>
                  </details>
                  <input type="hidden" name="sched_tz_min" id="input_sched_tz_min" value="0">
                  <button type="submit" class="submit">Save times</button>
                </form>
                <p class="hint">Fires once a day. Time zone (<span id="sched_tz_display">--</span>) follows this device.</p>
              </div>
            </div>
          </section>

          <section class="set-panel" data-sec="maint" hidden>
            <h2>Maintenance</h2>
            <div class="card">
              <div class="tab-section-title">Descale</div>
              <div class="metric-row"><span>Shots since last descale</span><b><span id="descale_shots">--</span></b></div>
              <div class="metric-row"><span>Days since last descale</span><b><span id="descale_days">--</span></b></div>
              <form action="/update" method="GET">
                <div class="field-row">
                  <div class="field"><label for="input_descale_shots">Remind after (shots)</label><input type="number" step="1" min="1" name="descale_shot_threshold" id="input_descale_shots" value=""></div>
                  <div class="field"><label for="input_descale_days">or after (days)</label><input type="number" step="1" min="1" name="descale_day_threshold" id="input_descale_days" value=""></div>
                </div>
                <button type="submit" class="submit">Save</button>
              </form>
              <button type="button" onclick="markDescaled()" class="btn-secondary">Mark descaled today</button>
            </div>
          </section>

          <section class="set-panel" data-sec="advanced" hidden>
            <h2>Advanced</h2>
            <p class="hint" style="margin:-6px 0 var(--sp-4)">Set once and forget &mdash; nothing here is needed day to day.</p>
            <div class="card">
              <details class="adv" id="adv_temp">
                <summary><span>Temperature control<small>PID gains, auto-tune, steam safety limit</small></span></summary>
                <div class="adv-body">
                  <div class="sub-title">Brew &middot; idle gains</div>
                  <form action="/update" method="GET">
                    <div class="field-row-3">
                      <div class="field"><label for="input_brew_kp">Kp</label><input type="number" step="any" name="brew_kp" id="input_brew_kp" value=""></div>
                      <div class="field"><label for="input_brew_ki">Ki</label><input type="number" step="any" name="brew_ki" id="input_brew_ki" value=""></div>
                      <div class="field"><label for="input_brew_kd">Kd</label><input type="number" step="any" name="brew_kd" id="input_brew_kd" value=""></div>
                    </div>
                    <button type="submit" class="submit">Save</button>
                  </form>
                </div>
                <div class="adv-body">
                  <div class="sub-title">Brew &middot; gains during a shot</div>
                  <p class="hint">Used while a shot runs, so flow doesn't pull the boiler temperature down.</p>
                  <form action="/update" method="GET">
                    <div class="field-row-3">
                      <div class="field"><label for="input_brew_akp">Kp</label><input type="number" step="any" name="brew_akp" id="input_brew_akp" value=""></div>
                      <div class="field"><label for="input_brew_aki">Ki</label><input type="number" step="any" name="brew_aki" id="input_brew_aki" value=""></div>
                      <div class="field"><label for="input_brew_akd">Kd</label><input type="number" step="any" name="brew_akd" id="input_brew_akd" value=""></div>
                    </div>
                    <button type="submit" class="submit">Save</button>
                  </form>
                </div>
                <div class="adv-body">
                  <div class="sub-title">Steam &middot; gains and safety ceiling</div>
                  <form action="/update" method="GET">
                    <div class="field-row-3">
                      <div class="field"><label for="input_steam_kp">Kp</label><input type="number" step="any" name="steam_kp" id="input_steam_kp" value=""></div>
                      <div class="field"><label for="input_steam_ki">Ki</label><input type="number" step="any" name="steam_ki" id="input_steam_ki" value=""></div>
                      <div class="field"><label for="input_steam_kd">Kd</label><input type="number" step="any" name="steam_kd" id="input_steam_kd" value=""></div>
                    </div>
                    <div class="field"><label for="input_steam_max_safety">Max safety ceiling (&deg;C)</label><input type="number" step="1" min="100" max="150" name="steam_max_safety" id="input_steam_max_safety" value=""></div>
                    <button type="submit" class="submit">Save</button>
                  </form>
                </div>
                <div class="adv-body">
                  <div class="sub-title">Auto-tune</div>
                  <p class="hint">Cycles the heater around the current Brew or Steam target for several minutes. Stay by the machine.</p>
                  <button type="button" onclick="startAutotune()" id="btn_autotune" class="btn-autotune">Start auto-tune</button>
                  <span id="autotune_status" class="autotune-status"></span>
                </div>
              </details>

              <details class="adv" id="adv_pressure">
                <summary><span>Pump pressure control<small>Pressure PID for the 0&ndash;16 bar sensor</small></span></summary>
                <div class="adv-body">
                  <p class="hint">With a 0&ndash;16 bar sensor fitted, corrects the pump model from the real reading during pressure phases.</p>
                  <form action="/update" method="GET">
                    <div class="field-row-3">
                      <div class="field"><label for="input_press_kp">Kp</label><input type="number" step="any" name="press_kp" id="input_press_kp" value=""></div>
                      <div class="field"><label for="input_press_ki">Ki</label><input type="number" step="0.01" name="press_ki" id="input_press_ki" value=""></div>
                      <div class="field"><label for="input_press_kd">Kd</label><input type="number" step="any" name="press_kd" id="input_press_kd" value=""></div>
                    </div>
                    <button type="submit" class="submit">Save</button>
                  </form>
                </div>
              </details>

              <details class="adv" id="adv_pump">
                <summary><span>Pump calibration<small>How much water the pump moves</small></span></summary>
                <div class="adv-body" id="pump_cal_card">
                  <p class="hint">Remove the portafilter, put a cup on a scale under the group and tare it, and turn the Brew switch on. Each run pumps at full power for 10 s. Three runs are averaged; a run more than 10% off the others has to be redone.</p>
                  <div class="metric-row"><span>Current value</span><b><span id="pump_cal_current">--</span></b></div>
                  <div id="pump_cal_runs"></div>
                  <p id="pump_cal_status" class="hint" aria-live="polite"></p>
                  <p id="pump_cal_error" class="hint" style="color:var(--danger)" aria-live="assertive"></p>
                  <div id="pump_cal_grams_row" hidden>
                    <div class="field"><label for="input_pump_cal_grams">Grams in the cup</label><input type="number" step="0.1" min="1" max="500" id="input_pump_cal_grams"></div>
                    <button type="button" onclick="submitCalGrams()" class="submit">Record</button>
                  </div>
                  <button type="button" onclick="calAction('run')" id="btn_pump_cal_run" class="submit">Start run</button>
                  <button type="button" onclick="calAction('stop')" id="btn_pump_cal_stop" class="btn-secondary" hidden>Stop</button>
                  <button type="button" onclick="calAction('save')" id="btn_pump_cal_save" class="submit" hidden>Save calibration</button>
                  <button type="button" onclick="calAction('cancel')" class="btn-secondary">Clear runs</button>
                </div>
              </details>

              <details class="adv" id="adv_water">
                <summary><span>Water tank sensor<small>Installed, polarity, live reading</small></span></summary>
                <div class="adv-body">
                  <label class="check-row"><input type="checkbox" id="input_water_enabled" onchange="setWater('water_enabled', this.checked ? 1 : 0)"> Sensor installed</label>
                  <label class="check-row"><input type="checkbox" id="input_water_interlock" onchange="setWater('water_interlock', this.checked ? 1 : 0)"> Pause new shots on low water</label>
                  <div class="field">
                    <label for="input_water_active_low">Water reads as</label>
                    <select id="input_water_active_low" onchange="setWater('water_active_low', this.value)"><option value="1">LOW (default)</option><option value="0">HIGH</option></select>
                  </div>
                  <div class="water-live"><span>Live reading &middot; GPIO7</span><b id="water_live">--</b></div>
                  <p class="hint">Flat against the outside of the tank at the "low" height (red 3V3, black GND, yellow GPIO7). A removed tank reads as no water. A running shot always finishes; the heater is never affected.</p>
                </div>
              </details>

              <details class="adv" id="adv_wifi">
                <summary><span>Wi-Fi &amp; firmware<small>Reset network, OTA update</small></span></summary>
                <div class="adv-body">
                  <button type="button" onclick="wifiReset()" class="btn-danger">Reset Wi-Fi settings</button>
                  <p class="hint">Reboots into the <b>GaggiaBrewMasterESP_Setup</b> network so you can join a different Wi-Fi without reflashing.</p>
                  <a href="/firmware" class="hint-link">Firmware update (OTA)</a>
                </div>
              </details>

              <details class="adv" id="adv_mqtt">
                <summary><span>MQTT / Home Assistant<small>Broker and auto-discovery</small></span></summary>
                <div class="adv-body">
                  <form action="/update" method="GET" id="mqtt_form">
                    <div class="field-row">
                      <div class="field"><label for="input_mqtt_server">Server</label><input type="text" name="mqtt_server" id="input_mqtt_server" placeholder="192.168.1.100"></div>
                      <div class="field"><label for="input_mqtt_port">Port</label><input type="number" name="mqtt_port" id="input_mqtt_port" value="1883"></div>
                    </div>
                    <div class="field-row">
                      <div class="field"><label for="input_mqtt_user">User</label><input type="text" name="mqtt_user" id="input_mqtt_user"></div>
                      <div class="field"><label for="input_mqtt_pass">Password</label><input type="password" name="mqtt_pass" id="input_mqtt_pass" placeholder="Unchanged if blank" autocomplete="off"></div>
                    </div>
                    <p class="hint" id="mqtt_pass_hint" hidden>A password is already stored. Leave blank to keep it.</p>
                    <button type="submit" class="submit">Save and restart</button>
                  </form>
                  <p class="hint">Saving reboots the controller. Home Assistant auto-discovers mode, targets, sensors and shot/fault/descale binaries.</p>
                </div>
              </details>

              <details class="adv" id="adv_backup">
                <summary><span>Backup &amp; restore<small>Settings file</small></span></summary>
                <div class="adv-body">
                  <a href="/settings_export" class="btn-secondary">Download backup</a>
                  <label class="btn-secondary">Restore from file<input type="file" accept=".txt" onchange="restoreSettings(this)" style="display:none"></label>
                  <p class="hint">Brew/Steam/schedule/MQTT settings, the active profile and pump calibration. Your own shot profiles live in a separate file and are not included.</p>
                </div>
              </details>
            </div>
          </section>
        </div>
      </div>
    </div>
    </main>

    <footer class="footer">
      <a href="/firmware">Firmware update</a> &middot; <span id="host">gaggia.local</span> &middot; build <span id="fw_build">--</span>
    </footer>
  </div>

<script>
// Old WebViews (the kitchen tablet) lack flexbox `gap`; detect it and let the
// .no-flexgap margin fallbacks in the stylesheet take over.
(function () {
  var d = document.createElement("div");
  d.style.display = "flex";
  d.style.flexDirection = "column";
  d.style.rowGap = "1px";
  d.appendChild(document.createElement("div"));
  d.appendChild(document.createElement("div"));
  document.body.appendChild(d);
  var ok = d.scrollHeight === 1;
  document.body.removeChild(d);
  if (!ok) document.documentElement.className += " no-flexgap";
})();
function setVal(id, v) {
  var el = document.getElementById(id);
  if (!el) return;
  if (document.activeElement.id !== id && (el.value === "" || el.dataset.synced !== "1")) {
    el.value = v; el.dataset.synced = "1";
  }
}
function clamp(x) { return Math.max(0, Math.min(100, x)); }
function setText(id, v) {
  var el = document.getElementById(id);
  if (el) el.textContent = v;
}
function showBanner(id, on, text) {
  var el = document.getElementById(id);
  if (!el) return;
  if (text !== undefined && text !== null) el.textContent = text;
  el.style.display = on ? "flex" : "none";
}
function xhr(url, done) {
  var req = new XMLHttpRequest();
  req.timeout = 8000;
  req.onreadystatechange = function () {
    if (req.readyState !== 4) return;
    if (done) done(req.status, req.responseText);
  };
  req.ontimeout = function () { if (done) done(0, ""); };
  req.onerror = function () { if (done) done(0, ""); };
  req.open("GET", url, true);
  req.send();
  return req;
}

// Every plain settings form (action="/update", method="GET") is
// intercepted here and sent via XHR instead, so saving no longer does a
// full page reload - the fields keep whatever the user just typed, and an
// inline confirmation appears next to the button instead. Forms with their
// own onsubmit handler (profile editor, shot notes) already do this and
// are untouched. MQTT save reboots the controller - shown a distinct
// "Restarting..." message immediately, since the HTTP response may never
// complete before the device restarts.
document.addEventListener("submit", function (e) {
  var form = e.target;
  if (!form || form.tagName !== "FORM" || form.getAttribute("action") !== "/update") return;
  e.preventDefault();

  var parts = [];
  for (var i = 0; i < form.elements.length; i++) {
    var el = form.elements[i];
    if (!el.name) continue;
    if (el.name === "mqtt_pass" && !el.value) continue;
    parts.push(encodeURIComponent(el.name) + "=" + encodeURIComponent(el.value));
  }
  var btn = form.querySelector("button[type=submit]");
  var isMqtt = !!form.querySelector("#input_mqtt_server");
  if (isMqtt) showSaveConfirm(btn, "Restarting...", false);
  xhr("/update?" + parts.join("&"), function (status) {
    if (isMqtt) return;
    showSaveConfirm(btn, status === 200 ? "Saved" : "Save failed", status !== 200);
  });
});

function showSaveConfirm(btn, text, isError) {
  if (!btn) return;
  var el = btn.nextElementSibling;
  if (!el || !el.classList.contains("save-confirm")) {
    el = document.createElement("span");
    el.className = "save-confirm";
    el.setAttribute("aria-live", "polite");
    btn.insertAdjacentElement("afterend", el);
  }
  el.textContent = text;
  el.classList.toggle("error", !!isError);
  el.classList.add("show");
  clearTimeout(el._hideTimer);
  el._hideTimer = setTimeout(function () { el.classList.remove("show"); }, 2200);
}

// Tabs - hash-addressable (#now/#tune/#history/#settings) so a reload keeps
// whichever view was open; all four share the single /status poll below.
function showTab(name) {
  // Old deep links (#tune, #history) from the four-tab layout still land
  // somewhere sensible: Tune -> Settings > Brewing, History -> Brew.
  if (name === "tune") { name = "settings"; showSection("brewing"); }
  var valid = ["now", "settings"];
  if (valid.indexOf(name) === -1) name = "now";
  var views = document.querySelectorAll(".view");
  for (var i = 0; i < views.length; i++) views[i].hidden = views[i].dataset.view !== name;
  var tabs = document.querySelectorAll(".tab");
  for (var j = 0; j < tabs.length; j++) {
    var active = tabs[j].dataset.tab === name;
    tabs[j].classList.toggle("active", active);
    tabs[j].setAttribute("aria-selected", active ? "true" : "false");
    tabs[j].tabIndex = active ? 0 : -1;
  }
  history.replaceState(null, "", "#" + name);
}
(function () {
  var tabs = document.querySelectorAll(".tab");
  for (var i = 0; i < tabs.length; i++) {
    tabs[i].addEventListener("click", (function (t) { return function () { showTab(t.dataset.tab); }; })(tabs[i]));
  }
  var tablist = document.querySelector(".tabbar");
  if (tablist) {
    tablist.addEventListener("keydown", function (e) {
      var list = Array.prototype.slice.call(document.querySelectorAll(".tab"));
      var idx = list.indexOf(document.activeElement);
      if (idx < 0) return;
      var next = idx;
      if (e.key === "ArrowRight" || e.key === "ArrowDown") next = (idx + 1) % list.length;
      else if (e.key === "ArrowLeft" || e.key === "ArrowUp") next = (idx - 1 + list.length) % list.length;
      else if (e.key === "Home") next = 0;
      else if (e.key === "End") next = list.length - 1;
      else return;
      e.preventDefault();
      showTab(list[next].dataset.tab);
      list[next].focus();
    });
  }
})();
// Settings sections (Brewing / Power & schedule / Maintenance / Advanced):
// a pill row on phones, a master list beside the panel on wide screens.
function showSection(sec) {
  var panels = document.querySelectorAll(".set-panel");
  for (var i = 0; i < panels.length; i++) panels[i].hidden = panels[i].dataset.sec !== sec;
  var btns = document.querySelectorAll(".sec-btn");
  for (var j = 0; j < btns.length; j++) {
    var on = btns[j].dataset.sec === sec;
    btns[j].classList.toggle("active", on);
    btns[j].setAttribute("aria-current", on ? "true" : "false");
  }
}
// Jump straight to a setting from a banner/link (optionally opening one
// collapsed Advanced group).
function openSettings(sec, detailsId) {
  showTab("settings");
  showSection(sec);
  if (detailsId) {
    var d = document.getElementById(detailsId);
    if (d) { d.open = true; d.scrollIntoView({ behavior: "smooth", block: "start" }); }
  } else {
    window.scrollTo(0, 0);
  }
}
showTab(location.hash.slice(1));

// Phase-transition markers on the temp sparkline (2026-08-23) - adapted from
// GaggiMate's live shot chart, which draws a vertical annotation line each
// time the brew phase changes. history[] has no per-sample timestamp/phase
// of its own (it's a plain rolling temperature buffer, 2s/sample,
// independent of any shot), so markers are tracked client-side purely by
// "how many samples ago did shot_phase last change" - aged by one every
// poll (each poll adds exactly one new history sample) and dropped once
// they scroll off the visible window.
var phaseMarkers = [];
var lastSeenShotPhase = null;
var firstStatusReceived = false;

function trackPhaseMarkers(shotPhase, shotInProgress) {
  phaseMarkers.forEach(function (m) { m.age++; });
  if (shotInProgress && lastSeenShotPhase !== null && shotPhase !== lastSeenShotPhase) {
    phaseMarkers.push({ age: 0 });
  }
  lastSeenShotPhase = shotInProgress ? shotPhase : null;
}

// Canvases are sized in CSS (fluid width); match the backing store to the
// displayed size x devicePixelRatio on every draw so lines stay crisp on
// the tablet and phone alike. Drawing then happens in CSS pixels.
function fitCanvas(canvas) {
  var dpr = window.devicePixelRatio || 1;
  var cw = canvas.clientWidth || 300, ch = canvas.clientHeight || 140;
  if (canvas.width !== Math.round(cw * dpr) || canvas.height !== Math.round(ch * dpr)) {
    canvas.width = Math.round(cw * dpr);
    canvas.height = Math.round(ch * dpr);
  }
  var ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cw, ch);
  return { ctx: ctx, w: cw, h: ch };
}

// One combined live chart (2026-10-15 redesign): boiler temp (copper, own
// auto-scaled axis incl. the target) and pressure (cyan, 0-10 bar) on the
// same 2-min window, each target dashed, phase changes as cyan ticks.
function drawLiveChart(temps, tempTarget, est, sensor, pTarget, sensorLive) {
  var canvas = document.getElementById("live_chart");
  if (!canvas) return;
  var fit = fitCanvas(canvas);
  var ctx = fit.ctx, w = fit.w, h = fit.h;
  // Axis numbers (2026-10-16): pressure in the left gutter (cyan, bar),
  // boiler temp in the right gutter (copper, whole degrees) - both on the
  // same 3 gridline intervals - plus the latest value of each line tagged
  // at its end.
  var x0 = 24, x1 = w - 30;
  var top = 8, bottom = h - 8, span = bottom - top;
  function X(i, n) { return x0 + (i / (n - 1)) * (x1 - x0); }
  var AXIS_FONT = "500 10px 'Geist Mono', ui-monospace, Menlo, monospace";

  ctx.strokeStyle = "rgba(255,255,255,.05)";
  ctx.lineWidth = 1;
  for (var g = 0; g <= 3; g++) {
    var gy = Math.round(top + (span * g) / 3) + 0.5;
    ctx.beginPath(); ctx.moveTo(x0, gy); ctx.lineTo(x1, gy); ctx.stroke();
  }

  function path(data, yOf, gapsBelowZero) {
    ctx.beginPath();
    var pen = false;
    for (var i = 0; i < data.length; i++) {
      var v = data[i];
      if (gapsBelowZero && v < 0) { pen = false; continue; }
      var x = X(i, data.length), yy = yOf(v);
      if (!pen) { ctx.moveTo(x, yy); pen = true; } else ctx.lineTo(x, yy);
    }
  }
  function axisLabel(text, x, y, color, align) {
    ctx.font = AXIS_FONT;
    ctx.textAlign = align;
    ctx.textBaseline = "middle";
    ctx.fillStyle = color;
    ctx.fillText(text, x, Math.max(top + 4, Math.min(bottom - 4, y)));
  }
  // Small pill with the latest value, right-aligned just inside the plot,
  // above (or below, near the top) the line's end point.
  var tagBoxes = [];
  function valueTag(text, y, color, border) {
    ctx.font = "600 11px 'Geist', system-ui, sans-serif";
    var tw = ctx.measureText(text).width + 14, th = 17;
    var tx = x1 - tw - 4;
    var ty2 = y - th - 5 < top ? y + 5 : y - th - 5;
    tagBoxes.forEach(function (b) {
      if (ty2 < b.y + b.h + 2 && ty2 + th > b.y - 2) ty2 = (ty2 < b.y) ? b.y - th - 3 : b.y + b.h + 3;
    });
    ty2 = Math.max(top, Math.min(bottom - th, ty2));
    tagBoxes.push({ y: ty2, h: th });
    ctx.fillStyle = "rgba(14,15,17,.88)";
    ctx.strokeStyle = border;
    ctx.lineWidth = 1;
    ctx.beginPath();
    var r = th / 2;
    ctx.moveTo(tx + r, ty2); ctx.lineTo(tx + tw - r, ty2);
    ctx.arc(tx + tw - r, ty2 + r, r, -Math.PI / 2, Math.PI / 2);
    ctx.lineTo(tx + r, ty2 + th);
    ctx.arc(tx + r, ty2 + r, r, Math.PI / 2, Math.PI * 1.5);
    ctx.closePath(); ctx.fill(); ctx.stroke();
    ctx.fillStyle = color;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(text, tx + tw / 2, ty2 + th / 2 + 0.5);
  }

  // Main (cyan) line = the real reading whenever a usable sensor is fitted
  // (2026-10-21: independent of the closed loop being on); the other series
  // is drawn thin underneath for reference (model estimate, or the raw
  // sensor while it can't read brew pressure).
  var main = sensorLive ? sensor : est;
  var other = sensorLive ? est : sensor;
  // 0-12 bar (same scale as the dial's pressure ring) so the 3 gridline
  // intervals land on whole numbers: 0 / 4 / 8 / 12.
  var PMAX = 12;
  function py(v) { return bottom - (Math.min(PMAX, Math.max(0, v)) / PMAX) * span; }
  for (var pg = 0; pg <= 3; pg++) axisLabel(String(pg * 4), x0 - 6, py(pg * 4), "rgba(70,227,240,.75)", "right");
  if (main && main.length > 1) {
    path(main, py, false);
    ctx.lineTo(x1, bottom); ctx.lineTo(x0, bottom); ctx.closePath();
    var grad = ctx.createLinearGradient(0, top, 0, bottom);
    grad.addColorStop(0, "rgba(70,227,240,.18)");
    grad.addColorStop(1, "rgba(70,227,240,0)");
    ctx.fillStyle = grad;
    ctx.fill();
  }
  ctx.lineJoin = "round";
  ctx.lineCap = "round";
  if (pTarget && pTarget.length > 1) {
    ctx.setLineDash([4, 5]);
    path(pTarget, py, true);
    ctx.strokeStyle = "rgba(255,255,255,.45)"; ctx.lineWidth = 1.2; ctx.stroke();
    ctx.setLineDash([]);
  }
  if (other && other.length > 1) {
    path(other, py, false);
    ctx.strokeStyle = "rgba(255,255,255,.16)"; ctx.lineWidth = 1; ctx.stroke();
  }
  if (main && main.length > 1) {
    path(main, py, false);
    ctx.strokeStyle = "#46e3f0"; ctx.lineWidth = 2; ctx.stroke();
  }

  if (temps && temps.length > 1) {
    var scale = (tempTarget === null || tempTarget === undefined) ? temps : temps.concat([tempTarget]);
    var dmin = Math.min.apply(null, scale), dmax = Math.max.apply(null, scale);
    var pad = Math.max(0.5, (dmax - dmin) * 0.12);
    // Whole-degree axis whose 3 intervals are whole numbers too.
    var tmin = Math.floor(dmin - pad);
    var step = Math.max(1, Math.ceil((dmax + pad - tmin) / 3));
    var tmax = tmin + step * 3;
    var ty = function (v) { return bottom - ((v - tmin) / (tmax - tmin)) * span; };
    for (var tg = 0; tg <= 3; tg++) axisLabel((tmin + tg * step) + "°", x1 + 6, ty(tmin + tg * step), "rgba(227,154,98,.8)", "left");
    if (tempTarget !== null && tempTarget !== undefined) {
      ctx.setLineDash([4, 5]);
      ctx.beginPath(); ctx.moveTo(x0, ty(tempTarget)); ctx.lineTo(x1, ty(tempTarget));
      ctx.strokeStyle = "rgba(227,154,98,.45)"; ctx.lineWidth = 1.2; ctx.stroke();
      ctx.setLineDash([]);
    }
    phaseMarkers = phaseMarkers.filter(function (m) { return m.age < temps.length; });
    ctx.strokeStyle = "rgba(70,227,240,.45)";
    ctx.lineWidth = 1;
    phaseMarkers.forEach(function (m) {
      var mx = X(temps.length - 1 - m.age, temps.length);
      ctx.beginPath(); ctx.moveTo(mx, top); ctx.lineTo(mx, bottom); ctx.stroke();
    });
    path(temps, ty, false);
    ctx.strokeStyle = "#e39a62"; ctx.lineWidth = 2; ctx.stroke();
    var tLast = temps[temps.length - 1];
    valueTag(tLast.toFixed(1) + "°", ty(tLast), "#e39a62", "rgba(227,154,98,.5)");
    setText("temp_range_label", tmin + "–" + tmax + " °C");
  }
  if (main && main.length > 1) {
    var pLast = main[main.length - 1];
    if (sensorLive || pLast > 0.05) valueTag(pLast.toFixed(1) + " bar", py(pLast), "#46e3f0", "rgba(70,227,240,.5)");
  }
}

function formatElapsed(ms) {
  var s = Math.floor(ms / 1000);
  var m = Math.floor(s / 60);
  s = s % 60;
  return m + ":" + (s < 10 ? "0" : "") + s;
}

var lastUpdateTime = null;
var lastAutotuneState = null; // tracks transitions, so the force-refresh below fires once
var pageLoadedAt = Date.now();

// Shot timer: shotRunning/shotElapsedBaseMs are refreshed from each /status
// poll; the 1s ticker below interpolates smoothly between polls rather than
// only updating every 2s.
var shotRunning = false;
var shotElapsedBaseMs = 0;
var shotElapsedCapturedAt = null;
var prevShotRunning = false; // detects the stop transition, to refresh history once
// Populated from /status (active_max_sec) - the progress bar and
// in-window/over coloring track THIS configured value, not a hardcoded
// number, so they stay correct whenever the auto-stop duration changes
// (directly, or via loading a profile with a different auto-stop time).
// 0 means auto-stop is disabled (manual-only); fall back to the original
// fixed 25-30s SCA-referenced reference window in that case, since there's
// no other meaningful target to size the bar against.
var shotAutoStopSec = 0;

setInterval(function () {
  var el = document.getElementById("shot_time");
  if (!el) return;
  var ms = shotElapsedBaseMs;
  if (shotRunning && shotElapsedCapturedAt !== null) {
    ms += Date.now() - shotElapsedCapturedAt;
  }
  el.textContent = formatElapsed(ms);
  var secs = ms / 1000;
  var hasAutoStop = shotAutoStopSec > 0;
  var target = hasAutoStop ? shotAutoStopSec : 30;
  var inWindow = shotRunning && (hasAutoStop ? secs >= target - 5 && secs <= target : secs >= 25 && secs <= 30);
  var over = shotRunning && secs > target;
  el.classList.toggle("in-window", inWindow);
  el.classList.toggle("over", over);
  var pbar = document.getElementById("shot_progress_bar");
  if (pbar) {
    pbar.style.width = clamp((secs / target) * 100) + "%";
    pbar.classList.toggle("in-window", inWindow);
    pbar.classList.toggle("over", over);
  }
}, 1000);

// Cached oldest-first, exactly as /shots returns it - shotLogUpdateNotes()
// on the device indexes by this same oldest-first order, NOT the table's
// reversed (newest-first) display order, so fillShotNotes()/submitShotNotes()
// need the original index preserved alongside each displayed row.
var shotsCache = [];

// The old History tab is gone (2026-10-15 redesign): only the latest shot
// matters day to day, so it gets a card on the Brew view with a one-tap
// taste rating. Notes still go through the same shot_note_* API.
var TASTE_TIPS = {
  Sour: "Under-extracted. Try a finer grind or +1 \u00b0C.",
  Balanced: "Nice. Keep this grind and dose for this bean.",
  Bitter: "Over-extracted. Try a coarser grind or \u22121 \u00b0C."
};
var TASTE_RATING = { Sour: 2, Balanced: 5, Bitter: 2 };

// Dial-in advice (2026-10-21, revised same day): the dose is always 18 g;
// the user only weighs the cup. The verdict reads the yield against the
// time, the way the shot ended, and the profile's style:
//  - ran to the end by itself: the time was fixed by the profile, so the
//    yield says how fast the water went (too much = too fast = finer).
//  - stopped by hand: the time it took to get that yield (scaled to the
//    target yield) vs the profile's time window (too quick = finer).
// The sensor's pressure vs the profile's own target backs it up.
var lastShotPress = null;
var SHOT_DOSE_G = 18;
var PROFILE_STYLE = {
  "Classic 9 bar": { ratio: 2.0, win: [25, 33] },
  "Lever": { ratio: 2.0, win: [30, 42] },
  "Blooming": { ratio: 2.5, win: [45, 60] },
  "Turbo": { ratio: 2.75, win: [12, 20] }
};
var DEFAULT_STYLE = { ratio: 2.0, win: [25, 33] };
var RATIO_BAND = 0.35;      // +- around the target ratio counts as on target
var PRESS_BAND_BAR = 1.5;   // +- around the profile target counts as on target

function profileRunSeconds(name) {
  for (var i = 0; i < profilesCache.length; i++) {
    var p = profilesCache[i];
    if (p.name !== name) continue;
    var t = 0;
    (p.phases || []).forEach(function (ph) { t += ph.duration_s || 0; });
    return p.max_sec > 0 ? Math.min(t, p.max_sec) : t;
  }
  return 0;
}

function renderDialIn(s, lp) {
  var el = document.getElementById("dialin_tip");
  if (!el) return;
  var prof = (lp && lp.profile) || activeProfileName;
  var st = PROFILE_STYLE[prof] || DEFAULT_STYLE;
  var t = s.duration_ms / 1000, ts = t.toFixed(0);
  var end = lp && lp.end;
  if (end !== "user" && end !== "auto") {        // after a reboot: infer it
    var run = profileRunSeconds(prof);
    end = (run > 0 && t >= run - 1) ? "auto" : "user";
  }
  var y = s.weight, wantY = st.ratio * SHOT_DOSE_G;
  var votes = [], reasons = [], extra = "";
  if (y > 0) {
    var r = y / SHOT_DOSE_G, rs = "1:" + r.toFixed(1);
    if (end === "auto") {
      if (r > st.ratio + RATIO_BAND) { votes.push(-1); reasons.push("It ran its full " + ts + " s and gave <b>" + y.toFixed(0) + " g</b> (" + rs + "), more than the ~" + wantY.toFixed(0) + " g target: the water went through too fast."); }
      else if (r < st.ratio - RATIO_BAND) { votes.push(1); reasons.push("It ran its full " + ts + " s and gave only <b>" + y.toFixed(0) + " g</b> (" + rs + ") of ~" + wantY.toFixed(0) + " g: the water went through too slowly."); }
      else { votes.push(0); reasons.push("<b>" + y.toFixed(0) + " g</b> (" + rs + ") in the full " + ts + " s."); }
    } else {
      var tAt = t * wantY / y;                     // time to reach the target yield at this pace
      if (r < st.ratio - 0.5 && t >= st.win[1]) { votes.push(1); reasons.push("You stopped at <b>" + y.toFixed(0) + " g</b> after " + ts + " s, well short of ~" + wantY.toFixed(0) + " g: it was running too slowly."); }
      else if (r < st.ratio - 0.5) { votes.push(0); reasons.push("You stopped at <b>" + y.toFixed(0) + " g</b> after " + ts + " s, short of the ~" + wantY.toFixed(0) + " g target."); extra = " Let the next one run to about " + wantY.toFixed(0) + " g before judging the grind."; }
      else if (tAt < st.win[0]) { votes.push(-1); reasons.push("<b>" + y.toFixed(0) + " g</b> (" + rs + ") in " + ts + " s is faster than the " + st.win[0] + "–" + st.win[1] + " s this profile aims for."); }
      else if (tAt > st.win[1]) { votes.push(1); reasons.push("<b>" + y.toFixed(0) + " g</b> (" + rs + ") in " + ts + " s is slower than the " + st.win[0] + "–" + st.win[1] + " s this profile aims for."); }
      else { votes.push(0); reasons.push("<b>" + y.toFixed(0) + " g</b> (" + rs + ") in " + ts + " s, inside the " + st.win[0] + "–" + st.win[1] + " s window."); }
    }
  }
  if (lp && lp.avg >= 0 && lp.target > 0) {
    var d = lp.avg - lp.target;
    if (d <= -PRESS_BAND_BAR) { votes.push(-1); reasons.push("Pressure averaged <b>" + lp.avg.toFixed(1) + " bar</b> against " + lp.target.toFixed(1) + ": the puck is too loose."); }
    else if (d >= PRESS_BAND_BAR || lp.peak >= 11.5) { votes.push(1); reasons.push("Pressure averaged <b>" + lp.avg.toFixed(1) + " bar</b> (peak " + lp.peak.toFixed(1) + ") against " + lp.target.toFixed(1) + ": the puck is too tight."); }
    else { votes.push(0); reasons.push("Pressure on target: " + lp.avg.toFixed(1) + " / " + lp.target.toFixed(1) + " bar."); }
  }
  el.hidden = false;
  if (!(y > 0)) {
    el.className = "dialin";
    el.innerHTML = "Weigh the cup and enter it above for a grind tip." + (reasons.length ? "<small>" + reasons.join(" ") + "</small>" : "");
    return;
  }
  var finer = votes.indexOf(-1) >= 0, coarser = votes.indexOf(1) >= 0;
  var head;
  if (finer && coarser) head = "<b>Mixed signals</b> - the cup and the pressure disagree, which usually means channeling. Level the grounds and tamp evenly before touching the grind.";
  else if (finer) head = "<b>Grind finer</b> next shot.";
  else if (coarser) head = "<b>Grind coarser</b> next shot.";
  else head = extra ? "<b>Keep the grind</b> for now." : "<b>Dialled in</b> - keep this grind.";
  el.className = "dialin " + ((finer || coarser) ? "act" : "ok");
  el.innerHTML = head + "<small>" + reasons.join(" ") + extra + "</small>";
}

function saveYield(ev) {
  ev.preventDefault();
  var idx = shotsCache.length - 1;
  var v = parseFloat(document.getElementById("input_yield_quick").value);
  if (idx < 0 || !(v > 0)) return;
  fillShotNotes(idx);
  document.getElementById("input_shot_note_yield").value = v.toFixed(1);
  document.getElementById("input_shot_note_dose").value = SHOT_DOSE_G;
  shotsCache[idx].weight = v;
  shotsCache[idx].dose_in = SHOT_DOSE_G;
  document.getElementById("input_yield_quick").blur();
  renderLastShot();
  xhr("/update?" + shotNotesQuery(), function () { fetchShotHistory(); });
}

function shotAgo(ts) {
  if (!ts || ts < 1600000000) return "";
  var mins = Math.round((Date.now() / 1000 - ts) / 60);
  if (mins < 1) return "just now";
  if (mins < 60) return mins + " min ago";
  var hrs = Math.round(mins / 60);
  if (hrs < 24) return hrs + " h ago";
  return new Date(ts * 1000).toLocaleDateString([], { month: "short", day: "numeric" });
}

function renderLastShot() {
  var card = document.getElementById("last_shot_card");
  if (!card) return;
  if (!shotsCache.length) { card.hidden = true; return; }
  card.hidden = false;
  var idx = shotsCache.length - 1;
  var s = shotsCache[idx];
  var ago = shotAgo(s.ts);
  setText("last_shot_title", "Last shot" + (ago ? " \u00b7 " + ago : ""));
  var sum = document.getElementById("last_shot_sum");
  sum.textContent = "";
  function part(big, small) {
    var span = document.createElement("span");
    var b = document.createElement("b");
    b.textContent = big;
    span.appendChild(b);
    span.appendChild(document.createTextNode(" " + small));
    sum.appendChild(span);
  }
  part((s.duration_ms / 1000).toFixed(1), "s");
  part(s.peak_temp.toFixed(1), "\u00b0 peak");
  if (s.weight > 0) part(s.weight.toFixed(1), "g");
  if (s.weight > 0) part("1:" + (s.weight / SHOT_DOSE_G).toFixed(1), "ratio");
  var lp = (lastShotPress && lastShotPress.ts === s.ts) ? lastShotPress : null;
  if (lp && lp.avg >= 0) part(lp.avg.toFixed(1), "bar avg");
  var yq = document.getElementById("input_yield_quick");
  if (yq && document.activeElement !== yq) yq.value = s.weight > 0 ? s.weight : "";
  renderDialIn(s, lp);
  var taste = null;
  for (var k in TASTE_TIPS) { if (s.notes === k) taste = k; }
  var btns = document.querySelectorAll(".taste-btn");
  for (var i = 0; i < btns.length; i++) {
    var on = btns[i].dataset.taste === taste;
    btns[i].classList.toggle("active", on);
    btns[i].setAttribute("aria-pressed", on ? "true" : "false");
  }
  setText("taste_tip", taste ? TASTE_TIPS[taste] : "How did it taste?");
  var form = document.getElementById("shot_notes_card");
  if (form && form.hidden) fillShotNotes(idx);
}

function fetchShotHistory() {
  xhr("/shots", function (status, text) {
    if (status !== 200) return;
    try { shotsCache = JSON.parse(text); } catch (err) { return; }
    renderLastShot();
  });
}
fetchShotHistory();

function fillShotNotes(idx) {
  var s = shotsCache[idx];
  if (!s) return;
  document.getElementById("input_shot_note_index").value = idx;
  document.getElementById("input_shot_note_bean").value = s.bean || "";
  document.getElementById("input_shot_note_dose").value = SHOT_DOSE_G;
  document.getElementById("input_shot_note_yield").value = s.weight > 0 ? s.weight : "";
  document.getElementById("input_shot_note_grind").value = s.grind || "";
  document.getElementById("input_shot_note_rating").value = s.rating || 0;
  document.getElementById("input_shot_note_text").value = s.notes || "";
}

function toggleShotDetails() {
  var card = document.getElementById("shot_notes_card");
  if (card.hidden) fillShotNotes(shotsCache.length - 1);
  card.hidden = !card.hidden;
}

function shotNotesQuery() {
  var q = "shot_note_index=" + document.getElementById("input_shot_note_index").value;
  q += "&shot_note_bean=" + encodeURIComponent(document.getElementById("input_shot_note_bean").value);
  q += "&shot_note_dose=" + document.getElementById("input_shot_note_dose").value;
  q += "&shot_note_grind=" + encodeURIComponent(document.getElementById("input_shot_note_grind").value);
  q += "&shot_note_rating=" + document.getElementById("input_shot_note_rating").value;
  q += "&shot_note_text=" + encodeURIComponent(document.getElementById("input_shot_note_text").value);
  var y = document.getElementById("input_shot_note_yield").value;
  if (y !== "") q += "&shot_note_yield=" + y;
  return q;
}

// One tap: stores the taste word as the shot's note (and a rating), keeping
// any bean/dose/grind already saved for that shot.
function quickTaste(taste) {
  var idx = shotsCache.length - 1;
  if (idx < 0) return;
  fillShotNotes(idx);
  document.getElementById("input_shot_note_text").value = taste;
  document.getElementById("input_shot_note_rating").value = TASTE_RATING[taste];
  shotsCache[idx].notes = taste;
  shotsCache[idx].rating = TASTE_RATING[taste];
  renderLastShot();
  xhr("/update?" + shotNotesQuery(), function () { fetchShotHistory(); });
}

function submitShotNotes(ev) {
  ev.preventDefault();
  xhr("/update?" + shotNotesQuery(), function () {
    document.getElementById("shot_notes_card").hidden = true;
    fetchShotHistory();
  });
}

// Last known Brew target, refreshed by the main /status poll below - drawn
// as a reference line on the shot-trend chart so a peak-temp overshoot (like
// the integral-windup bug this chart exists to catch) is visible against
// what the shot was actually targeting, not just as a bare number.
var lastBrewTarget = null;

// Scheduled warm-up timezone (2026-08-16 fix): getTimezoneOffset() returns
// minutes BEHIND UTC (positive west of UTC), the opposite sign of what the
// firmware wants (UTC+3 -> +180) - hence the negation. Computed from the
// browser, not entered by hand, specifically because a wrong manual UTC
// offset was the root cause of the schedule silently firing at the wrong
// hour (looked like it "didn't work" - it was just firing hours off from
// the intended local time). Automatically follows DST since it's
// recomputed fresh on every page load.
var browserTzOffsetMin = -(new Date().getTimezoneOffset());
var schedTzSyncedOnce = false;
function syncSchedTz(serverValue) {
  var disp = document.getElementById("sched_tz_display");
  if (disp) {
    var h = browserTzOffsetMin / 60;
    disp.textContent = "UTC" + (h >= 0 ? "+" : "") + h;
  }
  if (schedTzSyncedOnce || serverValue === browserTzOffsetMin) return;
  schedTzSyncedOnce = true;
  if (serverValue !== browserTzOffsetMin) {
    xhr("/update?sched_tz_min=" + browserTzOffsetMin);
  }
}

setInterval(function () {
  var el = document.getElementById("last_updated");
  if (!el) return;
  if (lastUpdateTime === null) {
    el.textContent = "--";
    var waited = Math.round((Date.now() - pageLoadedAt) / 1000);
    var neverConnected = waited > 6;
    showBanner("offline_banner", neverConnected);
    var appBoot = document.getElementById("app_root");
    if (appBoot) appBoot.classList.toggle("degraded", neverConnected);
    return;
  }
  var secs = Math.round((Date.now() - lastUpdateTime) / 1000);
  el.textContent = secs + "s ago";
  el.style.color = secs > 6 ? "var(--danger)" : "var(--text-dim)";
  var stale = lastUpdateTime !== null && secs > 6;
  showBanner("offline_banner", stale);
  var app = document.getElementById("app_root");
  if (app) app.classList.toggle("degraded", stale);
}, 1000);

var statusInflight = false;

function applyModeButtons(mode) {
  document.getElementById("btn_off").classList.toggle("active", mode === "off");
  document.getElementById("btn_brew").classList.toggle("active", mode === "brew");
  document.getElementById("btn_steam").classList.toggle("active", mode === "steam");
  document.getElementById("btn_off").setAttribute("aria-pressed", mode === "off" ? "true" : "false");
  document.getElementById("btn_brew").setAttribute("aria-pressed", mode === "brew" ? "true" : "false");
  document.getElementById("btn_steam").setAttribute("aria-pressed", mode === "steam" ? "true" : "false");
}

function applyShotButton(running) {
  var btnShot = document.getElementById("btn_shot");
  var blocked = !running && waterLow && waterInterlockOn;
  btnShot.textContent = running ? "Stop shot" : blocked ? "Refill to brew" : "Start shot";
  btnShot.classList.toggle("running", running);
  btnShot.classList.toggle("blocked", blocked);
}

// Phase strip for the running shot, from the active profile's phase names;
// rebuilt only when the profile changes, then just re-classed per poll.
var phaseStepsFor = "";
function renderPhaseSteps(json) {
  var box = document.getElementById("phase_steps");
  if (!box) return;
  var prof = null;
  for (var i = 0; i < profilesCache.length; i++) if (profilesCache[i].name === json.active_profile_name) prof = profilesCache[i];
  var phases = prof ? prof.phases : [];
  var key = (prof ? prof.name : "") + "|" + phases.length;
  if (key !== phaseStepsFor) {
    phaseStepsFor = key;
    box.textContent = "";
    phases.forEach(function (ph, k) {
      if (k > 0) { var sep = document.createElement("span"); sep.className = "phase-sep"; box.appendChild(sep); }
      var el = document.createElement("span");
      el.className = "phase-step";
      el.textContent = ph.name || ("Phase " + (k + 1));
      box.appendChild(el);
    });
  }
  var steps = box.querySelectorAll(".phase-step");
  for (var j = 0; j < steps.length; j++) {
    steps[j].classList.toggle("done", json.shot_in_progress && j < json.phase_index);
    steps[j].classList.toggle("current", json.shot_in_progress && j === json.phase_index);
  }
}

// -/+ stepper: change the field, then save after a short pause so a few
// quick taps send one request.
var stepTimers = {};
function stepField(inputId, param, delta, min, max) {
  var el = document.getElementById(inputId);
  var v = parseFloat(el.value);
  if (isNaN(v)) return;
  v = Math.round(Math.max(min, Math.min(max, v + delta)) * 10) / 10;
  el.value = v;
  el.dataset.synced = "1";
  clearTimeout(stepTimers[param]);
  stepTimers[param] = setTimeout(function () {
    xhr("/update?" + param + "=" + v, function (status) {
      var note = document.getElementById(param + "_saved");
      if (!note) return;
      note.textContent = status === 200 ? "Saved " + v + "\u00b0" : "Save failed";
      note.classList.toggle("error", status !== 200);
      note.classList.add("show");
      clearTimeout(note._t);
      note._t = setTimeout(function () { note.classList.remove("show"); }, 2200);
    });
  }, 700);
}

var DIAL_TICKS = 33, shotPeakBar = 0;
(function buildDialTicks() {
  var g = document.getElementById("dial_ticks");
  if (!g) return;
  for (var i = 0; i < DIAL_TICKS; i++) {
    var a = (135 + (i / (DIAL_TICKS - 1)) * 270) * Math.PI / 180;
    var major = i % 4 === 0;
    var r1 = major ? 146 : 149, r2 = 156;
    var ln = document.createElementNS("http://www.w3.org/2000/svg", "line");
    ln.setAttribute("x1", (160 + r1 * Math.cos(a)).toFixed(1));
    ln.setAttribute("y1", (160 + r1 * Math.sin(a)).toFixed(1));
    ln.setAttribute("x2", (160 + r2 * Math.cos(a)).toFixed(1));
    ln.setAttribute("y2", (160 + r2 * Math.sin(a)).toFixed(1));
    ln.setAttribute("class", major ? "tick major" : "tick");
    g.appendChild(ln);
  }
})();

function updateDial(temp, target, scaleHi, state, mode) {
  var LO = 20;
  function frac(v) { return Math.max(0, Math.min(1, (v - LO) / (scaleHi - LO))); }
  var f = temp > 0 ? frac(temp) : 0;
  var dash = (f * 640.9).toFixed(1) + " 854.5";
  var ring = document.getElementById("temp_ring_fill");
  var glow = document.getElementById("temp_ring_glow");
  ring.setAttribute("stroke-dasharray", dash);
  glow.setAttribute("stroke-dasharray", dash);
  ring.classList.toggle("over", state === "over");
  ring.classList.toggle("zero", f <= 0.005);
  glow.classList.toggle("zero", f <= 0.005);
  glow.classList.toggle("over", state === "over");
  var ticks = document.querySelectorAll("#dial_ticks line");
  for (var i = 0; i < ticks.length; i++) ticks[i].classList.toggle("lit", f > 0 && i / (ticks.length - 1) <= f + 0.001);
  var dot = document.getElementById("dial_dot");
  dot.style.display = f > 0.005 ? "" : "none";
  dot.setAttribute("transform", "rotate(" + (135 + f * 270).toFixed(1) + " 160 160)");
  var tg = document.getElementById("dial_target");
  tg.style.display = target !== null ? "" : "none";
  if (target !== null) tg.setAttribute("transform", "rotate(" + (135 + frac(target) * 270).toFixed(1) + " 160 160)");
  var atTemp = state === "ready" && target !== null && mode !== "off";
  var halo = document.getElementById("temp_ready_halo");
  if (halo) halo.setAttribute("stroke-dasharray", (atTemp ? frac(target) * 640.9 : 0).toFixed(1) + " 854.5");
  var dialEl = document.getElementById("dial");
  dialEl.classList.toggle("off", mode === "off");
  dialEl.classList.toggle("at-temp", atTemp);
}

function applyStatus(json) {
  lastUpdateTime = Date.now();
  showBanner("offline_banner", false);
  var app = document.getElementById("app_root");
  if (app) app.classList.remove("degraded");

  var temp = json.temp, target = json.target, output = json.output;
  var outputPct = output / 10;
  var mode = json.opmode;
  var hasTarget = mode !== "off" && target > 0;
  var pumpPct = typeof json.pump_power === "number" ? json.pump_power : 0;

  if (!firstStatusReceived) {
    firstStatusReceived = true;
    var skeletons = document.querySelectorAll(".skeleton");
    for (var s = 0; s < skeletons.length; s++) skeletons[s].classList.remove("skeleton");
  }

  showBanner("fault_banner", !!json.fault);
  setText("temp", json.fault ? "--" : temp.toFixed(1));
  setText("target", hasTarget ? target.toFixed(1) : "--");
  document.getElementById("target_on").style.display = mode === "off" ? "none" : "";
  document.getElementById("target_off").style.display = mode === "off" ? "" : "none";

  trackPhaseMarkers(json.phase_index, json.shot_in_progress);
  var closedLoop = json.press_available === true && !json.pressure_fault;
  // sensorLive (2026-10-21): a sensor that reads brew pressure is fitted and
  // healthy - show ITS reading on the dial, the live readout and the chart
  // even while the closed loop is off (it only decides whether the pump
  // follows the reading, not whether we show it).
  var sensorLive = json.press_sensor === true && !json.pressure_fault;
  // Last shot's pressure stats for the dial-in tip; re-render the card when
  // a new shot lands (its log row arrives through fetchShotHistory()).
  if (typeof json.last_shot_ts === "number") {
    var lpNew = { ts: json.last_shot_ts, avg: json.last_avg_bar, target: json.last_target_bar, peak: json.last_peak_bar, end: json.last_end, profile: json.last_profile };
    var changed = !lastShotPress || lastShotPress.ts !== lpNew.ts || lastShotPress.avg !== lpNew.avg;
    lastShotPress = lpNew;
    if (changed && lpNew.ts > 0) {
      if (!shotsCache.length || shotsCache[shotsCache.length - 1].ts !== lpNew.ts) fetchShotHistory();
      else renderLastShot();
    }
  }
  drawLiveChart(json.history, hasTarget ? target : null, json.est_history, json.pressure_history, json.target_history, sensorLive);
  var pLabel = document.getElementById("pressure_label");
  if (pLabel) {
    pLabel.textContent = json.pressure_fault ? "sensor fault"
      : "sensor " + (json.pressure_max_bar && json.pressure >= json.pressure_max_bar
        ? json.pressure_max_bar.toFixed(0) + "+" : json.pressure.toFixed(1)) + " bar";
  }
  applyWaterStatus(json, mode);

  // Dial (2026-10-15): outer copper arc = boiler temp on a 20-100 C scale
  // (20-160 in Steam), lit ticks up to the current temp, white triangle at
  // the target; inner cyan arc = pressure 0-12 bar with a peak-hold dot.
  var readyMargin = 1.0;
  var tempState = (!json.fault && temp > 0 && hasTarget)
    ? (temp < target - readyMargin ? "heating" : temp > target + readyMargin ? "over" : "ready") : "";
  var scaleHi = mode === "steam" ? 160 : 100;
  updateDial(json.fault ? 0 : temp, hasTarget ? target : null, scaleHi, tempState, mode);

  var PRESSURE_GAUGE_MAX_BAR = 12;
  var showPressure = sensorLive || json.shot_in_progress;
  var pressureVal = sensorLive ? json.pressure : (json.shot_in_progress ? json.pressure_est : 0);
  var pressureGoal = (json.shot_in_progress && json.phase_type === "pressure") ? json.phase_target : null;
  var pRing = document.getElementById("pressure_ring_fill");
  if (pRing) {
    var pFrac = Math.max(0, Math.min(1, pressureVal / PRESSURE_GAUGE_MAX_BAR));
    pRing.setAttribute("stroke-dasharray", (pFrac * 556.1).toFixed(1) + " 741.4");
    pRing.classList.remove("heating", "ready", "over");
    pRing.classList.toggle("zero", pFrac <= 0.005);
    if (pressureGoal !== null) {
      var PRESSURE_READY_MARGIN_BAR = 1.0;
      if (pressureVal < pressureGoal - PRESSURE_READY_MARGIN_BAR) pRing.classList.add("heating");
      else if (pressureVal > pressureGoal + PRESSURE_READY_MARGIN_BAR) pRing.classList.add("over");
      else pRing.classList.add("ready");
    }
  }
  setText("live_press", pressureVal.toFixed(1));
  setText("live_temp", json.fault ? "--" : temp.toFixed(1));
  setText("live_pump", pumpPct.toFixed(0));
  renderPhaseSteps(json);
  if (json.shot_in_progress) {
    if (!prevShotRunning) shotPeakBar = 0;
    shotPeakBar = Math.max(shotPeakBar, pressureVal);
  }
  var peakG = document.getElementById("press_peak");
  if (peakG) {
    peakG.style.display = shotPeakBar > 0.5 ? "" : "none";
    peakG.setAttribute("transform", "rotate(" + (135 + Math.min(1, shotPeakBar / PRESSURE_GAUGE_MAX_BAR) * 270).toFixed(1) + " 160 160)");
  }
  setText("pressure_gauge_val", showPressure ? pressureVal.toFixed(1) : "0.0");
  var pCap = document.getElementById("pressure_target_caption");
  if (pCap) {
    pCap.textContent = pressureGoal !== null ? (sensorLive ? "" : "est. \u00b7 ") + "target " + pressureGoal.toFixed(1)
      : shotPeakBar > 0.5 ? "Last peak " + shotPeakBar.toFixed(1)
      : "pump idle";
  }

  // Header status: one plain-language state instead of the raw mode.
  var pill = document.getElementById("status_pill");
  var label, cls;
  if (json.fault) { label = "Sensor fault"; cls = "fault"; }
  else if (json.shot_in_progress) { label = json.phase_name ? "Extracting \u00b7 " + json.phase_name : "Extracting"; cls = "shot"; }
  else if (json.auto_sleeping) { label = "Asleep"; cls = "off"; }
  else if (mode === "off") { label = "Off"; cls = "off"; }
  else if (mode === "steam") { label = tempState === "ready" ? "Steam ready" : "Heating for steam"; cls = tempState === "ready" ? "steam ready" : "steam"; }
  else { label = tempState === "ready" ? "Ready to brew" : tempState === "over" ? "Cooling down" : "Heating"; cls = tempState === "ready" ? "brew ready" : "brew"; }
  setText("mode_status", label);
  pill.className = "pill status-line " + cls;
  applyModeButtons(mode);

  lastBrewTarget = json.brew_target;

  setVal("input_brew_target", json.brew_target);
  setVal("input_brew_kp", json.brew_kp);
  setVal("input_brew_ki", json.brew_ki);
  setVal("input_brew_kd", json.brew_kd);
  setVal("input_brew_akp", json.brew_akp);
  setVal("input_brew_aki", json.brew_aki);
  setVal("input_brew_akd", json.brew_akd);
  setVal("input_steam_target", json.steam_target);
  setVal("input_steam_kp", json.steam_kp);
  setVal("input_steam_ki", json.steam_ki);
  setVal("input_steam_kd", json.steam_kd);
  setVal("input_steam_max_safety", json.steam_max_safety);
  setVal("input_press_kp", json.press_kp);
  setVal("input_press_ki", json.press_ki);
  setVal("input_press_kd", json.press_kd);
  setVal("input_mqtt_server", json.mqtt_server || "");
  setVal("input_mqtt_port", json.mqtt_port);
  setVal("input_mqtt_user", json.mqtt_user || "");
  setVal("input_eco_min", json.eco_timeout_min);
  setVal("input_steam_off_min", json.steam_auto_off_min);
  var passHint = document.getElementById("mqtt_pass_hint");
  if (passHint) passHint.hidden = !json.mqtt_pass_set;

  var ntpBanner = document.getElementById("ntp_banner");
  if (ntpBanner) ntpBanner.style.display = json.ntp_synced ? "none" : "flex";

  var pumpMsg = "";
  if (json.pressure_ceiling_tripped) {
    pumpMsg = json.pressure_fault
      ? "Pump cut — pressure sensor fault or reading over the safety ceiling."
      : "Pump cut — pressure over the safety ceiling.";
    showBanner("pressure_banner", true, pumpMsg);
  } else if (json.pressure_fault && json.shot_in_progress) {
    showBanner("pressure_banner", true, "Pressure sensor fault — closed-loop pressure control is disabled.");
  } else {
    showBanner("pressure_banner", false);
  }

  var pumpWanted = json.shot_in_progress || json.pump_cal_state === "running";
  if (pumpWanted && json.pump_mains === "absent") {
    showBanner("pump_banner", true, "No mains to the pump — turn the machine's Brew switch on.");
  } else if (pumpWanted && json.pump_mains === "unstable") {
    showBanner("pump_banner", true, "Mains zero-cross signal looks unstable (" + json.mains_hz.toFixed(1) + " Hz).");
  } else {
    showBanner("pump_banner", false);
  }
  showBanner("cal_banner", json.pump_calibrated === false);
  applyCalStatus(json);

  var shotLabel = document.getElementById("shot_auto_stop_label");
  if (json.shot_in_progress && json.phase_index >= 0) {
    var unit = json.phase_type === "pressure" ? " bar" : json.phase_type === "flow" ? " ml/s" : json.phase_type === "power" ? "%" : "";
    var tgt = json.phase_type === "soak" ? "pump off" : json.phase_target.toFixed(1) + unit;
    shotLabel.textContent = json.phase_name + " · " + (json.phase_index + 1) + "/" + json.phase_count + " · " + tgt;
  } else {
    shotLabel.textContent = (json.active_profile_name || "--") + " · stops by " + json.active_max_sec + "s";
  }
  shotAutoStopSec = json.active_max_sec;
  
  if (typeof json.active_profile_name === "string" && json.active_profile_name !== activeProfileName) {
    activeProfileName = json.active_profile_name;
    markActiveProfile();
  }

  if (json.sched) {
    json.sched.forEach(function (s, i) {
      var cb = document.getElementById("input_sched" + i + "_en_cb");
      if (cb && document.activeElement !== cb) cb.checked = s.en;
      var timeStr = (s.hr < 10 ? "0" : "") + s.hr + ":" + (s.mn < 10 ? "0" : "") + s.mn;
      setVal("input_sched" + i + "_time", timeStr);
      var steamSel = document.getElementById("input_sched" + i + "_steam");
      if (steamSel && document.activeElement !== steamSel) steamSel.value = s.st ? "1" : "0";
    });
  }
  document.getElementById("input_sched_tz_min").value = browserTzOffsetMin;
  syncSchedTz(json.sched_tz_min);

  if (!json.auto_sleeping) sleepBannerDismissed = false;
  document.getElementById("sleep_banner").style.display =
    (json.auto_sleeping && !sleepBannerDismissed) ? "flex" : "none";
  if (json.auto_sleeping) {
    setText("sleep_banner_text", json.asleep_from === "steam"
      ? "Steam off · Heater off — idle timeout"
      : "Asleep (eco timeout) — heater off");
  }

  var atBtn = document.getElementById("btn_autotune");
  var atStatus = document.getElementById("autotune_status");
  if (json.autotune_state === "running") {
    atBtn.disabled = false;
    atBtn.textContent = "Stop Auto-Tune";
    atBtn.onclick = stopAutotune;
    atBtn.classList.add("running");
    atStatus.textContent = "Autotuning… " + (json.autotune_message || "");
  } else {
    atBtn.disabled = (mode === "off");
    atBtn.textContent = "Start Auto-Tune";
    atBtn.onclick = startAutotune;
    atBtn.classList.remove("running");
    if (json.autotune_state === "done_ok") {
      atStatus.textContent = json.autotune_message || "Autotune complete";
    } else if (json.autotune_state === "done_fail") {
      atStatus.textContent = json.autotune_message || "Autotune failed";
    } else {
      atStatus.textContent = "";
    }
  }

  if (json.autotune_state === "done_ok" && lastAutotuneState !== "done_ok") {
    var p = (mode === "steam") ? "steam" : "brew";
    document.getElementById("input_" + p + "_kp").value = json[p + "_kp"];
    document.getElementById("input_" + p + "_ki").value = json[p + "_ki"];
    document.getElementById("input_" + p + "_kd").value = json[p + "_kd"];
  }
  lastAutotuneState = json.autotune_state;

  shotRunning = json.shot_in_progress;
  if (app) app.classList.toggle("shooting", !!shotRunning);
  shotElapsedBaseMs = json.shot_elapsed_ms;
  shotElapsedCapturedAt = Date.now();
  applyShotButton(shotRunning);
  if (prevShotRunning && !shotRunning) fetchShotHistory();
  prevShotRunning = shotRunning;

  setVal("input_descale_shots", json.descale_shot_threshold);
  setVal("input_descale_days", json.descale_day_threshold);
  setText("descale_shots", json.shot_count);
  setText("descale_days", json.days_since_descale >= 0 ? json.days_since_descale : "--");
  showBanner("descale_banner_top", !!json.descale_due);
  setText("fw_build", json.fw_build);
}

function pollStatus() {
  if (statusInflight) return;
  statusInflight = true;
  xhr("/status", function (status, text) {
    statusInflight = false;
    if (status !== 200) return;
    var json;
    try { json = JSON.parse(text); } catch (err) { return; }
    applyStatus(json);
  });
}
pollStatus();
setInterval(pollStatus, 2000);

function setMode(mode) {
  applyModeButtons(mode);
  document.getElementById("sleep_banner").style.display = "none"; // a manual mode change ends any auto-sleep
  xhr("/update?mode=" + mode);
}

function wake() {
  xhr("/update?wake=1");
}

var sleepBannerDismissed = false;
function dismissSleepBanner() {
  sleepBannerDismissed = true;
  document.getElementById("sleep_banner").style.display = "none";
}

function startAutotune() {
  if (!confirm("Start PID auto-tune for the current mode? The heater will cycle on/off repeatedly for several minutes. Stay nearby.")) return;
  xhr("/update?autotune=start");
}

function stopAutotune() {
  xhr("/update?autotune=stop");
}

// ============================================================================
// Water tank sensor (design spec 2026-10-15). waterLow is only true while
// the sensor is enabled AND reads low; with the interlock on, Start is
// replaced by "Refill to brew" (still tappable, explains itself).
// ============================================================================
var waterLow = false, waterInterlockOn = true, shotBlockMsgTimer = null;

function applyWaterStatus(json, mode) {
  if (typeof json.water_state !== "string") return;
  var en = json.water_state !== "disabled";
  waterLow = json.water_state === "low";
  waterInterlockOn = json.water_interlock !== false;

  var pill = document.getElementById("tank_pill");
  pill.className = "tank-pill" + (en ? (waterLow ? " low" : " ok") : "");
  pill.hidden = !en;
  setText("tank_pill_text", waterLow ? "Low water" : "Tank OK");
  showBanner("water_banner", waterLow, waterLow && !waterInterlockOn
    ? "Water tank is low. Refill it soon."
    : "Refill the water tank. New shots are paused until it's back.");
  if (!waterLow) hideShotBlockMsg();
  applyShotButton(shotRunning);

  var cbEn = document.getElementById("input_water_enabled");
  if (cbEn && document.activeElement !== cbEn) cbEn.checked = !!json.water_enabled;
  var sel = document.getElementById("input_water_active_low");
  if (sel && document.activeElement !== sel) sel.value = json.water_active_low ? "1" : "0";
  var cbLock = document.getElementById("input_water_interlock");
  if (cbLock && document.activeElement !== cbLock) cbLock.checked = !!json.water_interlock;
  var live = document.getElementById("water_live");
  if (live) {
    var raw = json.water_raw_high ? "HIGH" : "LOW";
    var wet = json.water_active_low ? !json.water_raw_high : json.water_raw_high;
    live.textContent = raw + " \u00b7 " + (wet ? "water detected" : "no water");
    live.className = wet ? "ok" : "low";
  }
}

function setWater(key, value) {
  xhr("/update?" + key + "=" + value);
}

function showShotBlockMsg(text) {
  var el = document.getElementById("shot_block_msg");
  if (text) el.textContent = text;
  el.classList.add("show");
  clearTimeout(shotBlockMsgTimer);
  shotBlockMsgTimer = setTimeout(hideShotBlockMsg, 6000);
}
function hideShotBlockMsg() {
  var el = document.getElementById("shot_block_msg");
  if (el) el.classList.remove("show");
}

function toggleShot() {
  var next = !shotRunning;
  if (next && waterLow && waterInterlockOn) {
    showShotBlockMsg("Tank is low. Refill, then tap again.");
    return;
  }
  shotRunning = next;
  if (next) {
    shotElapsedBaseMs = 0;
    shotElapsedCapturedAt = Date.now();
  }
  applyShotButton(next);
  xhr("/update?shot=" + (next ? "start" : "stop"), function (status, text) {
    if (!next || status !== 409) return;
    // Refused by the controller (e.g. the tank went low since the last
    // poll) - roll the optimistic timer back and say why.
    shotRunning = false;
    shotElapsedBaseMs = 0;
    shotElapsedCapturedAt = null;
    applyShotButton(false);
    showShotBlockMsg(text || "Shot refused");
  });
}

// ============================================================================
// Shot profiles - by NAME (design spec 2026-10-02 §7). All profile data goes
// into the DOM via textContent/createElement(NS) only - names are user input.
// ============================================================================
var profilesCache = [];
var activeProfileName = "";
var lastProfilesJson = "";

function profileTotalSec(p) {
  var t = 0;
  p.phases.forEach(function (ph) { t += ph.duration_s; });
  return Math.min(t, p.max_sec);
}

function profileSummary(p) {
  if (p.utility) return "Utility · " + profileTotalSec(p).toFixed(0) + "s";
  var peak = 0;
  p.phases.forEach(function (ph) { if (ph.type === "pressure") peak = Math.max(peak, ph.target * p.pressure_scale); });
  var s = p.temp.toFixed(1) + "°C · " + p.phases.length + " phases · " + profileTotalSec(p).toFixed(0) + "s";
  if (peak > 0) s += " · peak " + peak.toFixed(1) + " bar";
  if (p.trim_pct) s += " · trim " + (p.trim_pct > 0 ? "+" : "") + p.trim_pct + "%";
  if (p.tweaked) s += " · tweaked";
  return s;
}

function easeJs(curve, x) {
  if (curve === "linear") return x;
  if (curve === "ease-in") return x * x * x;
  if (curve === "ease-out") return 1 - Math.pow(1 - x, 3);
  if (curve === "ease-in-out") return x < 0.5 ? 4 * x * x * x : 1 - Math.pow(-2 * x + 2, 3) / 2;
  return 1;
}

// Mini phase chart: each phase's target over time, normalised per unit
// (bar/10, ml/s/10, %/100) so mixed phases share one axis; colour = type.
var SVG_NS = "http://www.w3.org/2000/svg";
var PHASE_COLORS = { pressure: "#46e3f0", flow: "#e39a62", power: "#a1a4a8", soak: "#5f6268" };
function phaseNorm(ph, v, scale) {
  if (ph.type === "pressure") return (v * scale) / 10;
  if (ph.type === "flow") return v / 10;
  if (ph.type === "power") return v / 100;
  return 0;
}
function buildProfileMiniChart(p) {
  var W = 120, H = 32;
  var svg = document.createElementNS(SVG_NS, "svg");
  svg.setAttribute("viewBox", "0 0 " + W + " " + H);
  svg.setAttribute("class", "profile-mini");
  svg.setAttribute("aria-hidden", "true");
  var total = 0;
  p.phases.forEach(function (ph) { total += ph.duration_s; });
  if (total <= 0) return svg;
  var t = 0, prevEnd = 0;
  p.phases.forEach(function (ph) {
    var end = phaseNorm(ph, ph.target, p.pressure_scale);
    var start = ph.start !== undefined ? phaseNorm(ph, ph.start, p.pressure_scale) : prevEnd;
    if (ph.type === "soak") { start = 0; end = 0; }
    var ramp = (ph.curve && ph.curve !== "instant") ? Math.min(ph.ramp_s || 0, ph.duration_s) : 0;
    var pts = [];
    if (ramp > 0) {
      for (var i = 0; i <= 8; i++) pts.push([t + (i / 8) * ramp, start + (end - start) * easeJs(ph.curve, i / 8)]);
    } else {
      pts.push([t, end]);
    }
    pts.push([t + ph.duration_s, end]);
    var line = document.createElementNS(SVG_NS, "polyline");
    line.setAttribute("points", pts.map(function (q) {
      return ((q[0] / total) * W).toFixed(1) + "," + (H - 2 - Math.min(1, q[1]) * (H - 4)).toFixed(1);
    }).join(" "));
    line.setAttribute("fill", "none");
    line.setAttribute("stroke", PHASE_COLORS[ph.type] || "#888");
    line.setAttribute("stroke-width", "2");
    line.setAttribute("stroke-linecap", "round");
    line.setAttribute("stroke-linejoin", "round");
    svg.appendChild(line);
    t += ph.duration_s;
    prevEnd = end;
  });
  return svg;
}

function markActiveProfile() {
  var chips = document.querySelectorAll("#profile_chip_row .btn-preset");
  for (var i = 0; i < chips.length; i++) chips[i].classList.toggle("active", chips[i].dataset.name === activeProfileName);
  var rows = document.querySelectorAll("#profile_list_body .profile-row");
  for (var j = 0; j < rows.length; j++) rows[j].classList.toggle("active", rows[j].dataset.name === activeProfileName);
}

function profileActionDone(status, text) {
  if (status !== 200) alert(text || "Request failed");
  lastProfilesJson = "";
  fetchProfiles();
}

function selectProfileByName(name) {
  activeProfileName = name;
  markActiveProfile();
  xhr("/update?profile_select=" + encodeURIComponent(name), profileActionDone);
}

function renderProfileChips() {
  var row = document.getElementById("profile_chip_row");
  if (!row) return;
  row.textContent = "";
  profilesCache.forEach(function (p) {
    var btn = document.createElement("button");
    btn.type = "button";
    btn.className = "btn-preset";
    btn.dataset.name = p.name;
    btn.appendChild(buildProfileMiniChart(p));
    var lbl = document.createElement("span");
    lbl.textContent = p.name;
    btn.appendChild(lbl);
    btn.addEventListener("click", function () { selectProfileByName(p.name); });
    row.appendChild(btn);
  });
  markActiveProfile();
}

function addRowButton(parent, label, cls, onClick) {
  var b = document.createElement("button");
  b.type = "button";
  b.className = "btn-chip-sm" + (cls ? " " + cls : "");
  b.textContent = label;
  b.addEventListener("click", function (e) { e.stopPropagation(); onClick(b); });
  parent.appendChild(b);
  return b;
}

function renderProfileList() {
  var body = document.getElementById("profile_list_body");
  if (!body) return;
  body.textContent = "";
  profilesCache.forEach(function (p) {
    var row = document.createElement("div");
    row.className = "profile-row";
    row.dataset.name = p.name;
    var main = document.createElement("div");
    main.className = "profile-row-main";
    var nameEl = document.createElement("b");
    nameEl.textContent = p.name + (p.builtin ? "" : " ★");
    var meta = document.createElement("span");
    meta.textContent = profileSummary(p);
    main.appendChild(nameEl);
    main.appendChild(meta);
    main.addEventListener("click", function () { selectProfileByName(p.name); });
    var actions = document.createElement("div");
    actions.className = "profile-row-actions";
    addRowButton(actions, "Tweak", "", function () { openProfileTweak(p); });
    addRowButton(actions, "Copy", "", function () {
      var n = prompt("Name for the copy (max 20 characters):", (p.name + " copy").slice(0, 20));
      if (!n) return;
      xhr("/update?profile_duplicate=" + encodeURIComponent(p.name) + "&new_name=" + encodeURIComponent(n), profileActionDone);
    });
    if (p.builtin && p.tweaked) {
      addRowButton(actions, "Reset", "", function () { xhr("/update?profile_reset=" + encodeURIComponent(p.name), profileActionDone); });
    }
    if (!p.builtin) {
      addRowButton(actions, "Rename", "", function () {
        var n = prompt("New name (max 20 characters):", p.name);
        if (!n || n === p.name) return;
        xhr("/update?profile_rename=" + encodeURIComponent(p.name) + "&new_name=" + encodeURIComponent(n), profileActionDone);
      });
      addRowButton(actions, "Delete", "danger", function (btn) { armDeleteProfile(btn, p.name); });
    }
    row.appendChild(main);
    row.appendChild(buildProfileMiniChart(p));
    row.appendChild(actions);
    body.appendChild(row);
  });
  markActiveProfile();
}

function fetchProfiles() {
  xhr("/profiles", function (status, text) {
    if (status !== 200 || text === lastProfilesJson) return;
    lastProfilesJson = text;
    try { profilesCache = JSON.parse(text); } catch (err) { return; }
    renderProfileChips();
    renderProfileList();
  });
}
fetchProfiles();

function openProfileTweak(p) {
  document.getElementById("input_tweak_name").value = p.name;
  document.getElementById("profile_tweak_title").textContent = "Tweak \"" + p.name + "\"";
  document.getElementById("tweak_temp_field").hidden = !!p.utility;
  document.getElementById("input_tweak_temp").value = p.temp;
  document.getElementById("input_tweak_max_sec").value = p.max_sec;
  document.getElementById("input_tweak_scale").value = p.pressure_scale;
  document.getElementById("input_tweak_trim").value = p.trim_pct;
  var card = document.getElementById("profile_tweak_card");
  card.hidden = false;
  card.scrollIntoView({ behavior: "smooth", block: "center" });
}

function closeProfileTweak() { document.getElementById("profile_tweak_card").hidden = true; }

function submitProfileTweak(ev) {
  ev.preventDefault();
  var q = "profile_tweak=" + encodeURIComponent(document.getElementById("input_tweak_name").value) +
    "&temp=" + encodeURIComponent(document.getElementById("input_tweak_temp").value) +
    "&max_sec=" + encodeURIComponent(document.getElementById("input_tweak_max_sec").value) +
    "&pressure_scale=" + encodeURIComponent(document.getElementById("input_tweak_scale").value) +
    "&trim_pct=" + encodeURIComponent(document.getElementById("input_tweak_trim").value);
  var btn = document.getElementById("profile_tweak_submit");
  btn.disabled = true;
  btn.textContent = "Saving…";
  xhr("/update?" + q, function (status, text) {
    profileActionDone(status, text);
    btn.textContent = status === 200 ? "Saved" : "Save failed";
    setTimeout(function () {
      btn.disabled = false;
      btn.textContent = "Save";
      if (status === 200) closeProfileTweak();
    }, 900);
  });
}

// Arm-then-confirm delete (no native confirm()): first click arms for 4 s,
// second click deletes; arming another disarms the first.
var armedDeleteBtn = null, armedDeleteTimer = null;
function disarmDeleteProfile() {
  if (armedDeleteTimer) { clearTimeout(armedDeleteTimer); armedDeleteTimer = null; }
  if (armedDeleteBtn) { armedDeleteBtn.textContent = "Delete"; armedDeleteBtn.classList.remove("armed"); armedDeleteBtn = null; }
}
function armDeleteProfile(btn, name) {
  if (btn === armedDeleteBtn) {
    disarmDeleteProfile();
    xhr("/update?profile_delete=" + encodeURIComponent(name), profileActionDone);
    return;
  }
  disarmDeleteProfile();
  armedDeleteBtn = btn;
  btn.textContent = "Confirm?";
  btn.classList.add("armed");
  armedDeleteTimer = setTimeout(disarmDeleteProfile, 4000);
}

function setSchedEnabled(i, checked) {
  xhr("/update?sched" + i + "_en=" + (checked ? "1" : "0"));
}

// Pump calibration card (design spec §6). Errors come back as 409 + text.
function calDone(status, text) { setText("pump_cal_error", status === 200 ? "" : (text || "Request failed")); }
function calAction(a) { xhr("/update?pump_cal=" + a, calDone); }
function calRedo(n) { xhr("/update?pump_cal_redo=" + n, calDone); }
function submitCalGrams() {
  var input = document.getElementById("input_pump_cal_grams");
  xhr("/update?pump_cal_grams=" + encodeURIComponent(input.value), function (s, t) {
    calDone(s, t);
    if (s === 200) input.value = "";
  });
}
function applyCalStatus(json) {
  if (typeof json.pump_cal_state !== "string") return;
  setText("pump_cal_current", json.pump_ml_per_click.toFixed(3) + " ml/stroke" + (json.pump_calibrated ? "" : " (default)"));
  var vals = json.pump_cal_runs || [];
  var n = 0, mean = 0;
  vals.forEach(function (v) { if (v !== null) { n++; mean += v; } });
  mean = n ? mean / n : 0;
  var st = json.pump_cal_state;
  var runsEl = document.getElementById("pump_cal_runs");
  runsEl.textContent = "";
  for (var i = 0; i < 3; i++) {
    var v = vals[i] === undefined ? null : vals[i];
    var row = document.createElement("div");
    row.className = "metric-row";
    var label = document.createElement("span");
    label.textContent = "Run " + (i + 1);
    var val = document.createElement("b");
    var off = v !== null && n === 3 && mean > 0 && Math.abs(v - mean) / mean > 0.10;
    if (v !== null) val.textContent = v.toFixed(3) + (off ? " ⚠ redo" : "");
    else if (json.pump_cal_run === i) val.textContent = st === "running" ? "pumping…" : "enter grams";
    else val.textContent = "—";
    row.appendChild(label);
    row.appendChild(val);
    if (v !== null && (st === "idle" || st === "aborted")) {
      var b = document.createElement("button");
      b.type = "button";
      b.className = "btn-chip-sm";
      b.textContent = "Redo";
      b.addEventListener("click", (function (k) { return function () { calRedo(k); }; })(i + 1));
      row.appendChild(b);
    }
    runsEl.appendChild(row);
  }
  var msg = st === "running" ? "Run " + (json.pump_cal_run + 1) + ": pumping " + Math.floor(json.pump_cal_elapsed_ms / 1000) + " / 10 s"
    : st === "await_grams" ? "Weigh the cup and enter the grams for run " + (json.pump_cal_run + 1) + "."
    : st === "aborted" ? "Run aborted: " + json.pump_cal_reason + ". Start it again."
    : n === 3 ? "All 3 runs done — Save, or Redo one."
    : json.pump_mains !== "ok" ? "Turn the Brew switch on to start a run."
    : "";
  setText("pump_cal_status", msg);
  document.getElementById("pump_cal_grams_row").hidden = st !== "await_grams";
  var runBtn = document.getElementById("btn_pump_cal_run");
  runBtn.hidden = st === "running" || st === "await_grams" || n === 3;
  runBtn.disabled = json.pump_mains !== "ok";
  var stopBtn = document.getElementById("btn_pump_cal_stop");
  stopBtn.hidden = !(st === "running" || st === "await_grams");
  stopBtn.textContent = st === "await_grams" ? "Discard run" : "Stop";
  document.getElementById("btn_pump_cal_save").hidden = !(n === 3 && (st === "idle" || st === "aborted"));
}

function restoreSettings(input) {
  if (!input.files || !input.files.length) return;
  if (!confirm("Restore settings from this backup file? This overwrites your current Brew/Steam/schedule/MQTT settings and reboots if MQTT config is included.")) {
    input.value = "";
    return;
  }
  var reader = new FileReader();
  reader.onload = function () {
    xhr("/update?" + reader.result.trim());
  };
  reader.readAsText(input.files[0]);
  input.value = "";
}

function markDescaled() {
  if (!confirm("Reset the descale counters? Confirm you've just descaled the machine.")) return;
  xhr("/update?mark_descaled=1");
}

function wifiReset() {
  if (!confirm("Reset WiFi settings and reboot? You'll need to rejoin the GaggiaBrewMasterESP_Setup network to reconfigure.")) return;
  xhr("/wifi_reset");
}

document.getElementById("host").textContent = location.host;
</script>
</body>
</html>)rawliteral";

// Minimal vector coffee-cup icon (no external assets/fonts) and a matching
// PWA manifest, so the page can be added to a phone home screen.
const char *icon_svg = R"rawliteral(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect width="100" height="100" rx="22" fill="#d98c3f"/>
<path d="M25 45h40v20a20 20 0 0 1-20 20 20 20 0 0 1-20-20z" fill="#1a1206"/>
<path d="M65 50h6a10 10 0 0 1 0 20h-6" fill="none" stroke="#1a1206" stroke-width="6"/>
<path d="M35 40c-2-6 4-8 2-14M48 40c-2-6 4-8 2-14M61 40c-2-6 4-8 2-14" fill="none" stroke="#1a1206" stroke-width="4" stroke-linecap="round"/>
</svg>)rawliteral";

const char *manifest_json = R"rawliteral({
"name":"GaggiaBrewMasterESP",
"short_name":"BrewMaster",
"start_url":"/",
"display":"standalone",
"background_color":"#0e0b09",
"theme_color":"#0e0b09",
"icons":[{"src":"/icon.svg","sizes":"any","type":"image/svg+xml"}]
})rawliteral";

const char *firmware_html = R"fwhtml(
<!DOCTYPE HTML><html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Firmware update</title>
<style>
  :root { --bg:#0e0b09; --surface:#1a1512; --border:#392e26; --text:#f6efe6; --muted:#b3a294; --primary:#d98c3f; --danger:#e5544b; --success:#48b583; }
  * { box-sizing: border-box; }
  body { margin:0; font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif; background:var(--bg); color:var(--text); min-height:100vh; padding:24px 16px; }
  .wrap { max-width:440px; margin:0 auto; }
  a { color:var(--primary); text-decoration:none; font-weight:600; }
  h1 { font-size:17px; margin:0 0 8px; }
  p { color:var(--muted); font-size:14px; line-height:1.5; }
  .card { background:var(--surface); border:1px solid var(--border); border-radius:16px; padding:24px; margin-top:16px; }
  input[type=file] { width:100%; margin:12px 0 16px; color:var(--text); }
  button, .submit { width:100%; border:0; cursor:pointer; padding:14px; border-radius:12px; font-size:15px; font-weight:700; color:#1a1206; background:var(--primary); }
  button:disabled { opacity:.45; cursor:default; }
  .bar { height:8px; border-radius:99px; background:#100d0b; border:1px solid var(--border); overflow:hidden; margin-top:16px; display:none; }
  .bar > span { display:block; height:100%; width:0; background:var(--primary); }
  .status { margin-top:12px; font-size:14px; font-weight:700; }
  .status.err { color:var(--danger); }
  .status.ok { color:var(--success); }
  :focus { outline:2px solid #f2b46f; outline-offset:2px; }
</style>
</head><body>
<div class="wrap">
  <a href="/">Back to BrewMaster</a>
  <div class="card">
    <h1>Firmware update</h1>
    <p>Upload a <code>firmware.bin</code> from a PlatformIO build. The heater is forced off for the transfer. Stay on this page until it reboots.</p>
    <form id="fwform">
      <label for="fwfile">Firmware file</label>
      <input id="fwfile" type="file" name="update" accept=".bin" required>
      <button type="submit" id="fwbtn" class="submit">Update firmware</button>
    </form>
    <div class="bar" id="fwbar"><span id="fwfill"></span></div>
    <div class="status" id="fwstatus"></div>
  </div>
</div>
<script>
document.getElementById("fwform").addEventListener("submit", function (e) {
  e.preventDefault();
  var file = document.getElementById("fwfile").files[0];
  if (!file) return;
  var btn = document.getElementById("fwbtn");
  var bar = document.getElementById("fwbar");
  var fill = document.getElementById("fwfill");
  var status = document.getElementById("fwstatus");
  btn.disabled = true;
  bar.style.display = "block";
  status.textContent = "Uploading…";
  status.className = "status";
  var fd = new FormData();
  fd.append("update", file);
  var req = new XMLHttpRequest();
  req.timeout = 180000;
  req.upload.onprogress = function (ev) {
    if (!ev.lengthComputable) return;
    fill.style.width = Math.round((ev.loaded / ev.total) * 100) + "%";
  };
  req.onreadystatechange = function () {
    if (req.readyState !== 4) return;
    if (req.status === 200) {
      status.textContent = req.responseText || "Update success. Restarting…";
      status.className = "status ok";
    } else {
      status.textContent = "Update failed. Stay here and retry, or flash over USB.";
      status.className = "status err";
      btn.disabled = false;
    }
  };
  req.onerror = req.ontimeout = function () {
    status.textContent = "Connection lost during upload. If the board rebooted, wait and return to the dashboard. Otherwise retry.";
    status.className = "status err";
    btn.disabled = false;
  };
  req.open("POST", "/update_fw");
  req.send(fd);
});
</script>
</body></html>
)fwhtml";

// Profile and phase names are user-supplied - escape before splicing into
// the hand-built /status JSON.
static String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 2);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) { char b[7]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

// Snapshots every control-task-owned field under the lock first (see
// config.h "Shared-state lock"), then builds JSON from the local copies
// unlocked - keeps the lock held for microseconds instead of for the whole
// (much slower) String-concatenation pass below.
static void handleStatus(AsyncWebServerRequest *request) {
  lockState();
  float snapTemp = currentTemperature;
  double snapSetpoint = Setpoint, snapOutput = Output;
  OpMode snapMode = currentMode;
  double snapBrewTarget = brewSetpoint, snapBrewKp = brewKp, snapBrewKi = brewKi, snapBrewKd = brewKd;
  double snapBrewAkp = brewActiveKp, snapBrewAki = brewActiveKi, snapBrewAkd = brewActiveKd;
  double snapSteamTarget = steamSetpoint, snapSteamKp = steamKp, snapSteamKi = steamKi, snapSteamKd = steamKd;
  double snapSteamMaxSafety = steamMaxSafety;
  bool snapFault = sensorFault;
  unsigned long snapEcoTimeoutMin = ecoTimeoutMin;
  unsigned long snapSteamAutoOffMin = steamAutoOffMin;
  bool snapAutoSleeping = autoSleeping;
  OpMode snapModeBeforeSleep = modeBeforeSleep;
  AutotuneState snapAutotuneState = autotuneState;
  String snapAutotuneMessage = autotuneMessage;
  bool snapShotInProgress = shotInProgress;
  unsigned long snapShotStartMillis = shotStartMillis;
  unsigned long snapShotCount = shotCount;
  time_t snapLastDescaleTime = lastDescaleTime;
  unsigned long snapDescaleShotThreshold = descaleShotThreshold;
  unsigned long snapDescaleDayThreshold = descaleDayThreshold;
  ShotPhase snapShotPhase = currentShotPhase;
  bool snapSchedEnabled[SCHED_MAX_COUNT];
  int snapSchedHour[SCHED_MAX_COUNT], snapSchedMin[SCHED_MAX_COUNT];
  bool snapSchedModeSteam[SCHED_MAX_COUNT];
  for (int i = 0; i < SCHED_MAX_COUNT; i++) {
    snapSchedEnabled[i] = schedEnabled[i];
    snapSchedHour[i] = schedHour[i];
    snapSchedMin[i] = schedMin[i];
    snapSchedModeSteam[i] = schedModeSteam[i];
  }
  int snapHistoryCount = tempHistoryCount, snapHistoryHead = tempHistoryHead;
  float snapHistory[TEMP_HISTORY_LEN];
  for (int i = 0; i < snapHistoryCount; i++) {
    int idx = (snapHistoryHead - snapHistoryCount + i + TEMP_HISTORY_LEN * 2) % TEMP_HISTORY_LEN;
    snapHistory[i] = tempHistory[idx];
  }
  bool snapPressureFault = pressureFault;
  float snapPressure = currentPressure;
  float snapLastAvgBar = lastShotAvgBar, snapLastTargetBar = lastShotTargetBar, snapLastPeakBar = lastShotPeakBar;
  long long snapLastShotTs = lastShotTs;
  bool snapLastByUser = lastShotStoppedByUser;
  String snapLastProfile = lastShotProfile;
  double snapPressKp = pressureKp, snapPressKi = pressureKi, snapPressKd = pressureKd;
  // Pump telemetry - lets a bench operator (Milestones A-C) distinguish
  // "pump idle" / "pump at 100% duty" / "PID commanding some percent" /
  // "safety ceiling just tripped it to 0%", most importantly the last one
  // now that Fix 1 makes an unwired-sensor fault the likeliest Milestone A
  // confusion point.
  float snapPumpPower = dimmerGetPowerPercent();
  // Zero-cross count (dimmer health), exposed as zc_count.
  uint32_t snapDimmerZcCount = dimmerGetZcCount();
  uint32_t snapPressureMv = pressureSensorLastMv();
  bool snapPressureCeilingTripped = (pressureFault || currentPressure > PUMP_MAX_SAFETY_BAR);
  int snapPressHistoryCount = pressureHistoryCount, snapPressHistoryHead = pressureHistoryHead;
  float snapPressHistory[TEMP_HISTORY_LEN];
  for (int i = 0; i < snapPressHistoryCount; i++) {
    int idx = (snapPressHistoryHead - snapPressHistoryCount + i + TEMP_HISTORY_LEN * 2) % TEMP_HISTORY_LEN;
    snapPressHistory[i] = pressureHistory[idx];
  }
  bool snapPumpCalibrated = pumpCalibrated;
  float snapPumpMlpc = pumpMlPerClick;
  String snapCalState = pumpCalStateName();
  String snapCalReason = pumpCalAbortReason();
  int snapCalRun = pumpCalActiveRun();
  unsigned long snapCalElapsed = pumpCalRunElapsedMs(millis());
  float snapCalRuns[3] = {pumpCalRunValue(0), pumpCalRunValue(1), pumpCalRunValue(2)};
  String snapMains = mainsStateName(dimmerGetMainsState());
  float snapMainsHz = dimmerGetMainsHz();
  uint32_t snapFired = dimmerGetFiredCycles();
  String snapProfileName = activeProfileName;
  const Profile *snapProf = profileFind(activeProfileName.c_str());
  int snapMaxSec = snapProf ? snapProf->maxSec : 0;
  PlannerOutput snapPlan = plannerOut;
  String snapPhaseName = snapPlan.phaseName ? snapPlan.phaseName : "";
  int snapEstCount = estHistoryCount, snapEstHead = estHistoryHead;
  float snapEst[TEMP_HISTORY_LEN], snapTgt[TEMP_HISTORY_LEN];
  for (int i = 0; i < snapEstCount; i++) {
    int idx = (snapEstHead - snapEstCount + i + TEMP_HISTORY_LEN * 2) % TEMP_HISTORY_LEN;
    snapEst[i] = estPressureHistory[idx];
    snapTgt[i] = targetPressureHistory[idx];
  }
  bool snapWaterEnabled = waterSensorEnabled, snapWaterActiveLow = waterActiveLow;
  bool snapWaterInterlock = waterInterlock;
  WaterState snapWaterState = waterLevelState();
  bool snapWaterRawHigh = waterLevelRawHigh();
  String snapShotBlocked =
      (shotBlockedReason[0] && (long)(millis() - shotBlockedAtMillis) < SHOT_BLOCKED_REPORT_MS)
          ? shotBlockedReason : "";
  unlockState();

  String json = "{";
  json += "\"temp\":" + String(snapTemp);
  json += ",\"target\":" + String(snapSetpoint);
  json += ",\"output\":" + String(snapOutput);

  json += ",\"opmode\":\"";
  json += (snapMode == OpMode::BREW)    ? "brew"
           : (snapMode == OpMode::STEAM) ? "steam"
                                          : "off";
  json += "\"";

  // Kp/Ki/Kd get 4 decimal places, not String()'s default 2 - a value like
  // autotune's Ki=1.1782 would otherwise silently truncate to "1.18" here,
  // and re-saving without noticing would overwrite the real value with
  // the rounded one. Target/safety fields stay at the default (2 decimals
  // is already more precision than a human ever types for a temperature).
  json += ",\"brew_target\":" + String(snapBrewTarget);
  json += ",\"brew_kp\":" + String(snapBrewKp, 4);
  json += ",\"brew_ki\":" + String(snapBrewKi, 4);
  json += ",\"brew_kd\":" + String(snapBrewKd, 4);
  json += ",\"brew_akp\":" + String(snapBrewAkp, 4);
  json += ",\"brew_aki\":" + String(snapBrewAki, 4);
  json += ",\"brew_akd\":" + String(snapBrewAkd, 4);
  json += ",\"steam_target\":" + String(snapSteamTarget);
  json += ",\"steam_kp\":" + String(snapSteamKp, 4);
  json += ",\"steam_ki\":" + String(snapSteamKi, 4);
  json += ",\"steam_kd\":" + String(snapSteamKd, 4);
  json += ",\"steam_max_safety\":" + String(snapSteamMaxSafety);

  Preferences preferences;
  preferences.begin("gaggia", true);
  json +=
      ",\"mqtt_server\":\"" + preferences.getString("mqtt_server", "") + "\"";
  json += ",\"mqtt_port\":" + String(preferences.getInt("mqtt_port", 1883));
  json += ",\"mqtt_user\":\"" + preferences.getString("mqtt_user", "") + "\"";
  json += ",\"mqtt_pass_set\":" + String(preferences.getString("mqtt_pass", "").length() > 0 ? "true" : "false");
  preferences.end();

  json += ",\"fw_build\":\"" + String(FIRMWARE_BUILD_TIMESTAMP) + "\"";

  json += ",\"fault\":" + String(snapFault ? "true" : "false");

  // NTP sync status - an unsynced clock reads as ~1970 and silently blocks
  // the scheduled-warmup check (main.cpp) with no other visible symptom.
  time_t nowEpoch = time(nullptr);
  json += ",\"ntp_synced\":" + String(nowEpoch > 1600000000L ? "true" : "false");
  json += ",\"server_time\":" + String((long long)nowEpoch);

  json += ",\"eco_timeout_min\":" + String(snapEcoTimeoutMin);
  json += ",\"steam_auto_off_min\":" + String(snapSteamAutoOffMin);
  json += ",\"auto_sleeping\":" + String(snapAutoSleeping ? "true" : "false");
  // Which mode's timeout put it to sleep - lets the Web UI banner say "Steam
  // auto-off" vs "Eco timeout" correctly, instead of a generic label that
  // would be misleading now that the two have different (and very
  // different-sized) configured minutes.
  json += ",\"asleep_from\":\"";
  json += (snapModeBeforeSleep == OpMode::STEAM) ? "steam" : "brew";
  json += "\"";
  json += ",\"pressure_max_bar\":" + String(PRESSURE_SENSOR_MAX_BAR, 1);
  json += ",\"press_available\":" + String(PRESSURE_CLOSED_LOOP_AVAILABLE ? "true" : "false");
  // A sensor that reads brew pressure is fitted (shown in the UI even while
  // the closed loop is off) - the 0-5 bar part clipped below brew pressure.
  json += ",\"press_sensor\":" + String(PRESSURE_SENSOR_IS_16BAR ? "true" : "false");

  json += ",\"autotune_state\":\"";
  switch (snapAutotuneState) {
    case AutotuneState::RUNNING: json += "running"; break;
    case AutotuneState::DONE_OK: json += "done_ok"; break;
    case AutotuneState::DONE_FAIL: json += "done_fail"; break;
    default: json += "idle"; break;
  }
  json += "\"";
  json += ",\"autotune_message\":\"" + snapAutotuneMessage + "\"";

  json += ",\"shot_in_progress\":" + String(snapShotInProgress ? "true" : "false");
  json += ",\"shot_elapsed_ms\":" +
          String(snapShotInProgress ? (millis() - snapShotStartMillis) : 0);

  json += ",\"shot_count\":" + String(snapShotCount);
  json += ",\"descale_shot_threshold\":" + String(snapDescaleShotThreshold);
  json += ",\"descale_day_threshold\":" + String(snapDescaleDayThreshold);
  // -1 means "unknown" (never descaled/reset since this feature was added) -
  // avoids flashing a false "descale overdue" banner from an epoch-0 default.
  long daysSinceDescale =
      (snapLastDescaleTime > 0) ? (long)((time(nullptr) - snapLastDescaleTime) / 86400L) : -1;
  json += ",\"days_since_descale\":" + String(daysSinceDescale);
  bool descaleDue =
      (snapShotCount >= snapDescaleShotThreshold) ||
      (daysSinceDescale >= 0 && (unsigned long)daysSinceDescale >= snapDescaleDayThreshold);
  json += ",\"descale_due\":" + String(descaleDue ? "true" : "false");

  json += ",\"pressure\":" + String(snapPressure, 2);
  json += ",\"pressure_fault\":" + String(snapPressureFault ? "true" : "false");
  json += ",\"pump_power\":" + String(snapPumpPower, 1);
  json += ",\"zc_count\":" + String(snapDimmerZcCount);
  json += ",\"fired_cycles\":" + String(snapFired);
  json += ",\"pump_mains\":\"" + snapMains + "\"";
  json += ",\"mains_hz\":" + String(snapMainsHz, 1);
  json += ",\"pump_calibrated\":" + String(snapPumpCalibrated ? "true" : "false");
  json += ",\"pump_ml_per_click\":" + String(snapPumpMlpc, 4);
  json += ",\"pump_cal_state\":\"" + snapCalState + "\"";
  json += ",\"gate_hold_us\":" + String((long)dimmerGetGateHoldUs());
  json += ",\"pump_cal_reason\":\"" + snapCalReason + "\"";
  json += ",\"pump_cal_run\":" + String(snapCalRun);
  json += ",\"pump_cal_elapsed_ms\":" + String(snapCalElapsed);
  json += ",\"pump_cal_runs\":[";
  for (int i = 0; i < 3; i++) {
    if (i > 0) json += ",";
    json += snapCalRuns[i] < 0.0f ? String("null") : String(snapCalRuns[i], 4);
  }
  json += "]";
  json += ",\"pressure_mv\":" + String(snapPressureMv);
  // Last shot's pressure vs its profile targets (dial-in advice, -1 = unknown).
  json += ",\"last_shot_ts\":" + String((double)snapLastShotTs, 0);
  json += ",\"last_avg_bar\":" + String(snapLastAvgBar, 2);
  json += ",\"last_target_bar\":" + String(snapLastTargetBar, 2);
  json += ",\"last_peak_bar\":" + String(snapLastPeakBar, 2);
  json += ",\"last_end\":\"" + String(snapLastByUser ? "user" : "auto") + "\"";
  json += ",\"last_profile\":\"" + jsonEscape(snapLastProfile) + "\"";
  json += ",\"pressure_ceiling_tripped\":" + String(snapPressureCeilingTripped ? "true" : "false");
  json += ",\"press_kp\":" + String(snapPressKp, 4);
  json += ",\"press_ki\":" + String(snapPressKi, 4);
  json += ",\"press_kd\":" + String(snapPressKd, 4);
  json += ",\"shot_phase\":\"";
  switch (snapShotPhase) {
    case ShotPhase::PREINFUSION_ON:
    case ShotPhase::PREINFUSION_OFF: json += "preinfusion"; break;
    case ShotPhase::PRESSURE: json += "pressure"; break;
    case ShotPhase::EXTRACTION: json += "extraction"; break;
    default: json += "none"; break;
  }
  json += "\"";

  json += ",\"active_profile_name\":\"" + jsonEscape(snapProfileName) + "\"";
  json += ",\"active_max_sec\":" + String(snapMaxSec);
  json += ",\"phase_name\":\"" + jsonEscape(snapPhaseName) + "\"";
  json += ",\"phase_index\":" + String(snapPlan.phaseIndex);
  json += ",\"phase_count\":" + String(snapPlan.phaseCount);
  json += ",\"phase_type\":\"" + String(snapPlan.phaseIndex >= 0 ? phaseTypeName(snapPlan.phaseType) : "") + "\"";
  json += ",\"phase_target\":" + String(snapPlan.phaseTarget, 2);
  json += ",\"pressure_est\":" + String(snapPlan.pressureEst, 2);

  // Water tank sensor (water_level.cpp, design spec 2026-10-15)
  json += ",\"water_enabled\":" + String(snapWaterEnabled ? "true" : "false");
  json += ",\"water_state\":\"" + String(waterStateName(snapWaterState)) + "\"";
  json += ",\"water_raw_high\":" + String(snapWaterRawHigh ? "true" : "false");
  json += ",\"water_active_low\":" + String(snapWaterActiveLow ? "true" : "false");
  json += ",\"water_interlock\":" + String(snapWaterInterlock ? "true" : "false");
  json += ",\"shot_blocked\":\"" + snapShotBlocked + "\"";

  json += ",\"sched\":[";
  for (int i = 0; i < SCHED_MAX_COUNT; i++) {
    if (i > 0) json += ",";
    json += "{\"en\":" + String(snapSchedEnabled[i] ? "true" : "false") +
            ",\"hr\":" + String(snapSchedHour[i]) + ",\"mn\":" + String(snapSchedMin[i]) +
            ",\"st\":" + String(snapSchedModeSteam[i] ? "true" : "false") + "}";
  }
  json += "]";
  json += ",\"sched_tz_min\":" + String(schedTzOffsetMin);

  json += ",\"history\":[";
  for (int i = 0; i < snapHistoryCount; i++) {
    if (i > 0) json += ",";
    json += String(snapHistory[i], 1);
  }
  json += "]";
  json += ",\"pressure_history\":[";
  for (int i = 0; i < snapPressHistoryCount; i++) {
    if (i > 0) json += ",";
    json += String(snapPressHistory[i], 2);
  }
  json += "]";
  json += ",\"est_history\":[";
  for (int i = 0; i < snapEstCount; i++) { if (i > 0) json += ","; json += String(snapEst[i], 2); }
  json += "],\"target_history\":[";
  for (int i = 0; i < snapEstCount; i++) { if (i > 0) json += ","; json += String(snapTgt[i], 2); }
  json += "]";

  json += "}";
  request->send(200, "application/json", json);
}

// Settings/action endpoint - every mode/tuning/shot/profile/schedule/MQTT
// change funnels through this one handler (see the comment above the
// server.on("/update", ...) registration in setupWeb() for why).
static void handleUpdate(AsyncWebServerRequest *request) {
  // Thin wrappers so the body below reads exactly like the old
  // WebServer-based handler did, instead of request->getParam(x)->value()
  // at every call site.
  auto hasArg = [request](const char *name) { return request->hasParam(name); };
  auto arg = [request](const char *name) { return request->getParam(name)->value(); };

  // Holds the whole handler body under stateMutex - see config.h
  // "Shared-state lock". This runs on the AsyncTCP task, a different task
  // than controlTick(), so every field this handler touches (mode,
  // setpoint, gains, shot state) needs the same lock controlTick() holds
  // during its own tick, for exactly the same reason.
  lockState();
  noteActivity(); // after the lock: any /update call is explicit user action - resets eco-sleep timer

  // Actions that can be refused (calibration, profiles) set this; the
  // response is then 409 + the reason instead of the usual redirect, so
  // the Web UI can show WHY (xhr callback gets status + text).
  const char *actionErr = nullptr;

  Preferences preferences;
  preferences.begin("gaggia", false); // false = read/write

  if (hasArg("brew_target")) {
    brewSetpoint = arg("brew_target").toDouble();
    preferences.putDouble("brew_target", brewSetpoint);
  }
  if (hasArg("brew_kp")) {
    brewKp = arg("brew_kp").toDouble();
    preferences.putDouble("brew_kp", brewKp);
  }
  if (hasArg("brew_ki")) {
    brewKi = arg("brew_ki").toDouble();
    preferences.putDouble("brew_ki", brewKi);
  }
  if (hasArg("brew_kd")) {
    brewKd = arg("brew_kd").toDouble();
    preferences.putDouble("brew_kd", brewKd);
  }
  if (hasArg("brew_akp")) {
    brewActiveKp = arg("brew_akp").toDouble();
    preferences.putDouble("brew_akp", brewActiveKp);
  }
  if (hasArg("brew_aki")) {
    brewActiveKi = arg("brew_aki").toDouble();
    preferences.putDouble("brew_aki", brewActiveKi);
  }
  if (hasArg("brew_akd")) {
    brewActiveKd = arg("brew_akd").toDouble();
    preferences.putDouble("brew_akd", brewActiveKd);
  }
  if (hasArg("steam_target")) {
    steamSetpoint = arg("steam_target").toDouble();
    preferences.putDouble("steam_target", steamSetpoint);
  }
  if (hasArg("steam_kp")) {
    steamKp = arg("steam_kp").toDouble();
    preferences.putDouble("steam_kp", steamKp);
  }
  if (hasArg("steam_ki")) {
    steamKi = arg("steam_ki").toDouble();
    preferences.putDouble("steam_ki", steamKi);
  }
  if (hasArg("steam_kd")) {
    steamKd = arg("steam_kd").toDouble();
    preferences.putDouble("steam_kd", steamKd);
  }
  if (hasArg("press_kp")) {
    pressureKp = arg("press_kp").toDouble();
    preferences.putDouble("press_kp", pressureKp);
  }
  if (hasArg("press_ki")) {
    pressureKi = arg("press_ki").toDouble();
    preferences.putDouble("press_ki", pressureKi);
  }
  if (hasArg("press_kd")) {
    pressureKd = arg("press_kd").toDouble();
    preferences.putDouble("press_kd", pressureKd);
  }
  if (hasArg("press_kp") || hasArg("press_ki") || hasArg("press_kd")) {
    pressurePID.SetTunings(pressureKp, pressureKi, pressureKd);
  }
  if (hasArg("steam_max_safety")) {
    // Clamped server-side - a typo in this field shouldn't be able to set
    // a dangerously high (or uselessly low) steam safety ceiling.
    double v = arg("steam_max_safety").toDouble();
    steamMaxSafety = constrain(v, STEAM_MAX_SAFETY_MIN, STEAM_MAX_SAFETY_MAX);
    preferences.putDouble("steam_max_safety", steamMaxSafety);
  }
  // If the currently-active profile's own values just changed, apply them
  // live (no mode change, so no PID reset - matches how tuning edits have
  // always behaved here).
  if (hasArg("brew_target") || hasArg("brew_kp") ||
      hasArg("brew_ki") || hasArg("brew_kd") ||
      hasArg("brew_akp") || hasArg("brew_aki") ||
      hasArg("brew_akd") || hasArg("steam_target") ||
      hasArg("steam_kp") || hasArg("steam_ki") ||
      hasArg("steam_kd") || hasArg("steam_max_safety")) {
    refreshActiveProfileIfChanged();
  }

  if (hasArg("mode")) {
    // A mode-button click always safely interrupts an in-progress
    // autotune first - autotune's relay-driven Output must never keep
    // running once the user has asked for a different mode.
    stopAutotune();
    String mode = arg("mode");
    if (mode == "off") {
      setOpMode(OpMode::OFF);
    } else if (mode == "brew") {
      setOpMode(OpMode::BREW);
    } else if (mode == "steam") {
      setOpMode(OpMode::STEAM);
    }
  }

  // Eco / auto-sleep
  if (hasArg("eco_timeout_min")) {
    ecoTimeoutMin = arg("eco_timeout_min").toInt();
    preferences.putULong("eco_min", ecoTimeoutMin);
  }
  if (hasArg("steam_auto_off_min")) {
    steamAutoOffMin = arg("steam_auto_off_min").toInt();
    preferences.putULong("steam_off_min", steamAutoOffMin);
  }
  if (hasArg("wake") && arg("wake") == "1") {
    wakeFromSleep();
  }

  // Water tank sensor settings
  bool waterSettingsChanged = false;
  if (hasArg("water_enabled")) {
    waterSensorEnabled = arg("water_enabled") == "1";
    preferences.putBool("water_en", waterSensorEnabled);
    waterSettingsChanged = true;
  }
  if (hasArg("water_active_low")) {
    waterActiveLow = arg("water_active_low") == "1";
    preferences.putBool("water_low_act", waterActiveLow);
    waterSettingsChanged = true;
  }
  if (hasArg("water_interlock")) {
    waterInterlock = arg("water_interlock") == "1";
    preferences.putBool("water_lock", waterInterlock);
  }
  if (waterSettingsChanged) {
    applyWaterSettings();
    mqttWaterDiscoveryDirty = true; // add/remove the HA "Low water" entity
  }

  // PID Autotune - runs the currently-active profile (must be Brew or
  // Steam already, not Off) through a relay-feedback tuning cycle.
  if (hasArg("autotune")) {
    String at = arg("autotune");
    if (at == "start") {
      startAutotune(currentMode == OpMode::STEAM ? OpMode::STEAM : OpMode::BREW);
    } else if (at == "stop") {
      stopAutotune();
    }
  }

  // Shot timer (manual Start/Stop trigger - see config.h / AGENTS.md
  // roadmap item 7 for why this isn't automatic yet).
  if (hasArg("shot")) {
    String s = arg("shot");
    if (s == "start") {
      startShot();
      if (!shotInProgress && strcmp(shotBlockedReason, "low_water") == 0) {
        actionErr = "Water tank is low - refill it first";
      }
    } else if (s == "stop") {
      stopShot(true);
    }
  }

  // Pump calibration (pump_calibration.cpp, design spec §6)
  if (hasArg("pump_cal")) {
    String a = arg("pump_cal");
    if (a == "run") pumpCalStartRun(-1, &actionErr);
    else if (a == "stop") pumpCalStop();
    else if (a == "save") pumpCalSave(&actionErr);
    else if (a == "cancel") pumpCalClear();
  }
  if (hasArg("pump_cal_redo")) {
    int n = arg("pump_cal_redo").toInt();
    if (n < 1 || n > 3) actionErr = "Bad run number";
    else pumpCalStartRun(n - 1, &actionErr);
  }
  if (hasArg("pump_cal_grams")) pumpCalRecordGrams(arg("pump_cal_grams").toFloat(), &actionErr);
  if (hasArg("dimmer_gate_hold_us")) dimmerSetGateHoldUs(arg("dimmer_gate_hold_us").toInt());
  if (hasArg("pump_mlpc") && !pumpCalSetValue(arg("pump_mlpc").toFloat())) actionErr = "pump_mlpc out of range (0.08-0.40)";

  // Descale / maintenance reminder
  if (hasArg("mark_descaled") && arg("mark_descaled") == "1") {
    markDescaled();
  }
  if (hasArg("descale_shot_threshold")) {
    descaleShotThreshold = arg("descale_shot_threshold").toInt();
    preferences.putULong("descale_shots", descaleShotThreshold);
  }
  if (hasArg("descale_day_threshold")) {
    descaleDayThreshold = arg("descale_day_threshold").toInt();
    preferences.putULong("descale_days", descaleDayThreshold);
  }

  // Shot tasting/dial-in notes (shot_log.cpp) - edited after the fact
  // (you don't know the rating/notes until you've tasted the coffee), so
  // this is a separate action from shotLogAppend() at shot-stop time, not
  // a field collected while starting/stopping a shot. shot_note_index is
  // the JSON array's own 0-based (oldest-first) index, NOT the Web UI's
  // reversed (newest-first) display order - the frontend translates.
  if (hasArg("shot_note_index")) {
    int idx = arg("shot_note_index").toInt();
    String bean = hasArg("shot_note_bean") ? arg("shot_note_bean") : "";
    float doseIn = hasArg("shot_note_dose") ? arg("shot_note_dose").toFloat() : 0.0f;
    String grind = hasArg("shot_note_grind") ? arg("shot_note_grind") : "";
    int rating = hasArg("shot_note_rating") ? arg("shot_note_rating").toInt() : 0;
    String notes = hasArg("shot_note_text") ? arg("shot_note_text") : "";
    // Cup weight after the shot (no scale) - only sent by the notes form;
    // the one-tap taste buttons leave it as stored.
    float yieldG = hasArg("shot_note_yield") ? arg("shot_note_yield").toFloat() : -1.0f;
    shotLogUpdateNotes(idx, bean, doseIn, grind, rating, notes, yieldG);
  }

  // Shot profiles (profiles.cpp) - addressed by NAME (design spec §7).
  if (hasArg("profile_select") && !selectProfile(arg("profile_select").c_str())) actionErr = "No such profile";
  if (hasArg("profile_tweak")) {
    String name = arg("profile_tweak");
    const Profile *p = profileFind(name.c_str());
    if (!p) {
      actionErr = "No such profile";
    } else {
      ProfileTweak t = profileGetTweak(*p);
      if (hasArg("temp")) t.temp = arg("temp").toFloat();
      if (hasArg("max_sec")) t.maxSec = (uint16_t)constrain(arg("max_sec").toInt(), 0, 600);
      if (hasArg("pressure_scale")) t.pressureScale = arg("pressure_scale").toFloat();
      if (hasArg("trim_pct")) t.trimPct = (int8_t)constrain(arg("trim_pct").toInt(), -100, 100);
      if (profileTweak(name.c_str(), t, &actionErr) && activeProfileName.equalsIgnoreCase(name)) selectProfile(name.c_str());
    }
  }
  if (hasArg("profile_duplicate")) {
    String newName = hasArg("new_name") ? arg("new_name") : "";
    profileDuplicate(arg("profile_duplicate").c_str(), newName.c_str(), &actionErr);
  }
  if (hasArg("profile_rename")) {
    String oldName = arg("profile_rename");
    String newName = hasArg("new_name") ? arg("new_name") : "";
    if (profileRename(oldName.c_str(), newName.c_str(), &actionErr) && activeProfileName.equalsIgnoreCase(oldName))
      selectProfile(newName.c_str());
  }
  if (hasArg("profile_delete")) {
    String name = arg("profile_delete");
    if (profileDeleteUser(name.c_str(), &actionErr) && activeProfileName.equalsIgnoreCase(name))
      selectProfile(PROFILE_DEFAULT_NAME);
  }
  if (hasArg("profile_reset")) {
    String name = arg("profile_reset");
    if (profileReset(name.c_str(), &actionErr) && activeProfileName.equalsIgnoreCase(name)) selectProfile(name.c_str());
  }

  // Scheduled warm-up - SCHED_MAX_COUNT independent slots. Editing a slot's
  // enabled state or time always re-arms it for today (resetSchedFired) -
  // otherwise a slot that already fired once today would silently refuse
  // to fire again after a later edit the same day, until the next
  // calendar day (confirmed 2026-08-16 as the actual cause of a schedule
  // that looked like it "stopped working" mid-testing).
  for (int i = 0; i < SCHED_MAX_COUNT; i++) {
    String prefix = "sched" + String(i) + "_";
    String enArg = prefix + "en", timeArg = prefix + "time", steamArg = prefix + "steam";
    String enKey = "sched" + String(i) + "_en", hrKey = "sched" + String(i) + "_hr",
           mnKey = "sched" + String(i) + "_mn", stKey = "sched" + String(i) + "_st";
    if (hasArg(enArg.c_str())) {
      schedEnabled[i] = arg(enArg.c_str()) == "1";
      preferences.putBool(enKey.c_str(), schedEnabled[i]);
      resetSchedFired(i);
    }
    if (hasArg(timeArg.c_str())) {
      // Native <input type="time"> submits "HH:MM" as one field.
      String t = arg(timeArg.c_str());
      int colon = t.indexOf(':');
      if (colon > 0) {
        schedHour[i] = constrain(t.substring(0, colon).toInt(), 0, 23);
        schedMin[i] = constrain(t.substring(colon + 1).toInt(), 0, 59);
        preferences.putInt(hrKey.c_str(), schedHour[i]);
        preferences.putInt(mnKey.c_str(), schedMin[i]);
        resetSchedFired(i);
      }
    }
    if (hasArg(steamArg.c_str())) {
      schedModeSteam[i] = arg(steamArg.c_str()) == "1";
      preferences.putBool(stKey.c_str(), schedModeSteam[i]);
    }
  }
  if (hasArg("sched_tz_min")) {
    schedTzOffsetMin = arg("sched_tz_min").toInt();
    preferences.putInt("sched_tz_min", schedTzOffsetMin);
  }

  // MQTT Settings
  if (hasArg("mqtt_server")) {
    preferences.putString("mqtt_server", arg("mqtt_server"));
  }
  if (hasArg("mqtt_port")) {
    preferences.putInt("mqtt_port", arg("mqtt_port").toInt());
  }
  if (hasArg("mqtt_user")) {
    preferences.putString("mqtt_user", arg("mqtt_user"));
  }
  if (hasArg("mqtt_pass") && arg("mqtt_pass").length() > 0) {
    preferences.putString("mqtt_pass", arg("mqtt_pass"));
  }

  preferences.end();
  bool restartForMqtt = hasArg("mqtt_server");

  unlockState();

  if (actionErr) {
    request->send(409, "text/plain", actionErr);
  } else {
    request->redirect("/");
  }

  // Reboot to apply MQTT settings cleanly (simplest way)
  if (restartForMqtt) {
    delay(500);
    ESP.restart();
  }
}

static void handleOtaComplete(AsyncWebServerRequest *request) {
  request->send(200, "text/plain",
                (Update.hasError()) ? "Update Failed"
                                    : "Update Success! Restarting...");
  ESP.restart();
}

static void handleOtaUpload(AsyncWebServerRequest *request, String filename, size_t index,
                             uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    // Force the heater off up front as a defensive "safe state on a risky
    // operation" measure, same rule already used for boot in setup() - not
    // a workaround for a blocked control loop anymore (controlTick() keeps
    // running throughout the upload), just belt and suspenders before
    // Update.begin() (which itself can block a while erasing flash, on the
    // AsyncTCP task only).
    digitalWrite(PIN_SSR, LOW);
    Serial.printf("Update: %s\n", filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  }
  if (len) {
    if (Update.write(data, len) != len) {
      Update.printError(Serial);
    }
  }
  if (final) {
    if (Update.end(true)) {
      Serial.printf("Update Success: %u\n", (unsigned)(index + len));
    } else {
      Update.printError(Serial);
    }
  }
}

void setupWeb() {
  WiFi.mode(WIFI_STA);

  // WiFiManager
  WiFiManager wm;

  // wm.resetSettings(); // Unlock to reset if needed

  bool res;
  res = wm.autoConnect("GaggiaBrewMasterESP_Setup");

  if (!res) {
    Serial.println("Failed to connect");
  } else {
    Serial.println("connected...yeey :)");
    Serial.println(WiFi.localIP());
    if (MDNS.begin("gaggia")) {
      Serial.println("MDNS responder started");
      MDNS.addService("http", "tcp", 80);
    }
  }

  // Main Page Handler
  server.on("/", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    // No-store: this page has been changing rapidly during active
    // development, and with no versioned URL/ETag, a browser's default
    // caching heuristics can silently keep serving a stale copy after a
    // firmware update - confirmed 2026-09-15 (see AGENTS.md) when a
    // browser showed no visible change immediately after a fresh OTA
    // flash. Always revalidate instead of guessing.
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", index_html);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  // PWA manifest + icon (add-to-home-screen support)
  server.on("/manifest.json", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/manifest+json", manifest_json);
  });
  server.on("/icon.svg", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "image/svg+xml", icon_svg);
  });

  // Status Handler. Snapshots every control-task-owned field under the lock
  // first (see config.h "Shared-state lock"), then builds JSON from the
  // local copies unlocked - keeps the lock held for microseconds instead of
  // for the whole (much slower) String-concatenation pass below.
  server.on("/status", AsyncWebRequestMethod::HTTP_GET, handleStatus);

  // Shot history log
  server.on("/shots", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", shotLogReadJson());
  });

  // Named shot profiles (temp + phase list + max_sec) - see
  // profiles.cpp. List only; add/edit/delete/apply all go through /update
  // like every other setting, for the same reason /settings_export reuses
  // it for restore - one code path, not two.
  server.on("/profiles", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    lockState();
    String body = profilesListJson();
    unlockState();
    request->send(200, "application/json", body);
  });

  // Settings backup/restore (2026-08-16). Export builds a query-string in
  // EXACTLY the same field names /update already accepts and applies; the
  // browser downloads it as a plain text file. Restore is just the reverse:
  // the Web UI reads that file back client-side and re-POSTs its contents
  // straight to /update - no JSON parser needed on the device at all, and
  // zero new field-handling code to keep in sync with /update itself.
  server.on("/settings_export", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    auto enc = [](const String &s) {
      String out;
      char buf[4];
      for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
          out += c;
        } else {
          snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
          out += buf;
        }
      }
      return out;
    };

    Preferences preferences;
    preferences.begin("gaggia", true);
    String q;
    q += "brew_target=" + String(brewSetpoint, 2);
    q += "&brew_kp=" + String(brewKp, 4);
    q += "&brew_ki=" + String(brewKi, 4);
    q += "&brew_kd=" + String(brewKd, 4);
    q += "&brew_akp=" + String(brewActiveKp, 4);
    q += "&brew_aki=" + String(brewActiveKi, 4);
    q += "&brew_akd=" + String(brewActiveKd, 4);
    q += "&steam_target=" + String(steamSetpoint, 2);
    q += "&steam_kp=" + String(steamKp, 4);
    q += "&steam_ki=" + String(steamKi, 4);
    q += "&steam_kd=" + String(steamKd, 4);
    q += "&steam_max_safety=" + String(steamMaxSafety, 1);
    q += "&profile_select=" + enc(activeProfileName);
    q += "&eco_timeout_min=" + String(ecoTimeoutMin);
    q += "&steam_auto_off_min=" + String(steamAutoOffMin);
    q += "&descale_shot_threshold=" + String(descaleShotThreshold);
    q += "&descale_day_threshold=" + String(descaleDayThreshold);
    if (pumpCalibrated) q += "&pump_mlpc=" + String(pumpMlPerClick, 4);
    // Note: named shot profiles live in their own LittleFS file (profiles.cpp,
    // GET /profiles), not NVS - not included in this key=value backup format.
    for (int i = 0; i < SCHED_MAX_COUNT; i++) {
      char timeBuf[6];
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", schedHour[i], schedMin[i]);
      q += "&sched" + String(i) + "_en=" + String(schedEnabled[i] ? "1" : "0");
      q += "&sched" + String(i) + "_time=" + String(timeBuf);
      q += "&sched" + String(i) + "_steam=" + String(schedModeSteam[i] ? "1" : "0");
    }
    q += "&sched_tz_min=" + String(schedTzOffsetMin);
    q += "&mqtt_server=" + enc(preferences.getString("mqtt_server", ""));
    q += "&mqtt_port=" + String(preferences.getInt("mqtt_port", 1883));
    q += "&mqtt_user=" + enc(preferences.getString("mqtt_user", ""));
    q += "&mqtt_pass=" + enc(preferences.getString("mqtt_pass", ""));
    preferences.end();

    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", q);
    response->addHeader("Content-Disposition",
                         "attachment; filename=\"gaggia-settings-backup.txt\"");
    request->send(response);
  });

  // Settings Update Handler
  // Settings/action endpoint (mode, tuning, shot control, profiles,
  // schedule, MQTT) - see handleUpdate() above setupWeb().
  server.on("/update", AsyncWebRequestMethod::HTTP_GET, handleUpdate);

  // WiFi reconfiguration - clears stored credentials and reboots. On next
  // boot, WiFiManager's autoConnect() (in this same setupWeb()) will fail to
  // join and fall back to the "GaggiaBrewMasterESP_Setup" captive portal
  // automatically - the same well-tested path used on first boot, reused
  // here instead of building a second, custom WiFi-config UI from scratch.
  server.on("/wifi_reset", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html",
                  "<html><body><h2>Resetting WiFi...</h2>"
                  "<p>Rejoin the <b>GaggiaBrewMasterESP_Setup</b> WiFi network "
                  "from your phone/laptop in a few seconds to enter new "
                  "credentials.</p>"
                  "</body></html>");
    delay(500);
    WiFiManager wm;
    wm.resetSettings();
    delay(200);
    ESP.restart();
  });

  // OTA Update Form
  server.on("/firmware", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", firmware_html);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  // OTA Update Handler. Async by construction - unlike the old synchronous
  // WebServer, this upload callback runs on the AsyncTCP task and never
  // blocks controlTick() (its own dedicated task), so PID/safety-cutoff
  // timing and the watchdog keep running normally for the whole 60-70+
  // second upload instead of stalling (see config.h "Hardware watchdog").
  server.on("/update_fw", AsyncWebRequestMethod::HTTP_POST, handleOtaComplete, handleOtaUpload);

  server.begin();
}
