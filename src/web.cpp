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
#include "profiles.h"
#include "shot_log.h"

extern float currentTemperature;
extern bool sensorFault;
extern double Setpoint, Input, Output;
extern PID myPID;
extern PID pressurePID;
extern float tempHistory[];
extern int tempHistoryHead;
extern int tempHistoryCount;

extern float currentPressure;
extern bool pressureFault;
extern bool activePressureEnabled;
extern double activePressureRampBar;
extern unsigned long activePressureRampMs;
extern bool activePressureDeclineEnabled;
extern double activePressureDeclineBar;
extern unsigned long activePressureDeclineMs;
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
extern unsigned long shotAutoStopSec;
extern void startShot();
extern void stopShot();
extern ShotPhase currentShotPhase;

extern unsigned long shotCount;
extern time_t lastDescaleTime;
extern unsigned long descaleShotThreshold;
extern unsigned long descaleDayThreshold;
extern void markDescaled();

extern int activeProfileIndex;
extern bool activePreinfusionEnabled;
extern int activePreinfusionPulses, activePreinfusionOnMs, activePreinfusionOffMs;
extern void applyProfile(int idx);

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
  <meta name="theme-color" content="#0e0b09">
  <link rel="manifest" href="/manifest.json">
  <link rel="icon" href="/icon.svg" type="image/svg+xml">
  <link rel="apple-touch-icon" href="/icon.svg">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
  <meta name="apple-mobile-web-app-title" content="BrewMaster">
  <title>GaggiaBrewMasterESP</title>
  <style>
    :root {
      --bg: #0e0b09;
      --bg-grad: radial-gradient(1400px 700px at 50% -14%, #241a13 0%, #0e0b09 55%);
      --surface: #1a1512;
      --surface-2: #221b16;
      --elevated: #2a221c;
      --border: #392e26;
      --border-soft: #2a221c;
      --text: #f6efe6;
      --text-muted: #b3a294;
      --text-faint: #7d7166;

      --primary: #d98c3f;
      --primary-deep: #b06a2c;
      --primary-light: #f2b46f;
      --accent: #4fa3d8;
      --success: #48b583;
      --danger: #e5544b;
      --warning: #e0a13a;
      --info: #4fa3d8;

      /* Aliases so existing rules keep working while new rules use roles. */
      --text-dim: var(--text-muted);
      --copper: var(--primary);
      --copper-deep: var(--primary-deep);
      --copper-light: var(--primary-light);
      --steam: var(--accent);
      --green: var(--success);
      --red: var(--danger);
      --amber: var(--warning);

      --radius-sm: 8px;
      --radius-md: 12px;
      --radius-lg: 16px;
      --radius-full: 999px;

      --shadow-sm: none;
      --shadow-md: none;

      --sp-1: 4px; --sp-2: 8px; --sp-3: 12px; --sp-4: 16px; --sp-5: 24px; --sp-6: 32px;
      --ease: cubic-bezier(.22,.7,.32,1);

      --fs-caption: 11px;
      --fs-label: 12.5px;
      --fs-body: 15px;
      --fs-title: 17px;
      --fs-1: var(--fs-caption); --fs-2: var(--fs-caption); --fs-3: 11.5px;
      --fs-4: 12px; --fs-5: var(--fs-label); --fs-6: 13px;
      --fs-7: 14px; --fs-8: 14.5px; --fs-9: var(--fs-body);
      --fs-10: var(--fs-title); --fs-11: 19px; --fs-12: 21px; --fs-13: 22px;
    }
    * { box-sizing: border-box; }
    html, body { margin: 0; }
    /* Visible, on-brand focus ring for keyboard/switch navigation. Plain
       :focus first (works everywhere, including the Android 4.4 tablet's
       browser), then :focus-visible narrows it to keyboard focus only on
       browsers that support that distinction (~2020+) - an unsupported
       browser just never matches the second rule and keeps the safe
       plain-:focus outline on every focus, including touch/click. */
    :focus { outline: 2px solid var(--copper-light); outline-offset: 2px; }
    :focus:not(:focus-visible) { outline: none; }
    :focus-visible { outline: 2px solid var(--copper-light); outline-offset: 2px; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      background: var(--bg-grad), var(--bg);
      color: var(--text);
      min-height: 100vh;
      -webkit-font-smoothing: antialiased;
    }
    .app { max-width: 720px; margin: 0 auto; padding: var(--sp-5) var(--sp-4) calc(96px + env(safe-area-inset-bottom)); }

    @media (prefers-reduced-motion: reduce) {
      *, *::before, *::after {
        animation-duration: 0.01ms !important;
        animation-iteration-count: 1 !important;
        transition-duration: 0.01ms !important;
        scroll-behavior: auto !important;
      }
    }
    .topbar {
      position: sticky; top: 0; z-index: 20;
      display: flex; align-items: center; justify-content: space-between; gap: var(--sp-3);
      margin: calc(var(--sp-5) * -1) calc(var(--sp-4) * -1) var(--sp-4);
      padding: var(--sp-5) var(--sp-4) var(--sp-4);
      background: var(--bg);
    }
    .brand { display: flex; align-items: center; gap: var(--sp-3); }
    .logo {
      width: 40px; height: 40px; border-radius: var(--radius-md); flex: none;
      background: var(--primary); color: #1a1206;
      display: grid; place-items: center; font-size: var(--fs-13);
    }
    .brand-text h1 { font-size: var(--fs-10); margin: 0; letter-spacing: .2px; }
    .brand-text small { display: block; color: var(--text-dim); font-size: var(--fs-3); font-weight: 500; }
    .brand-status { text-align: right; }

    .pill {
      display: inline-flex; align-items: center; gap: var(--sp-2);
      padding: 7px 13px; border-radius: var(--radius-full); font-size: var(--fs-5); font-weight: 700;
      background: var(--surface); border: 1px solid var(--border);
    }
    .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--text-faint); }
    .pill.brew .dot { background: var(--green); box-shadow: 0 0 0 4px rgba(72,181,131,.18); animation: pulse-dot 2s var(--ease) infinite; }
    .pill.steam .dot { background: var(--steam); box-shadow: 0 0 0 4px rgba(79,163,216,.18); animation: pulse-dot 2s var(--ease) infinite; }
    .pill.fault { border-color: rgba(229,84,75,.5); }
    .pill.fault .dot { background: var(--red); box-shadow: 0 0 0 4px rgba(229,84,75,.2); animation: pulse-dot 1.1s var(--ease) infinite; }
    @keyframes pulse-dot { 0%,100% { opacity: 1 } 50% { opacity: .45 } }
    .last-updated { font-size: var(--fs-1); color: var(--text-dim); margin-top: 5px; }

    .alerts { display: flex; flex-direction: column; gap: var(--sp-2); margin-bottom: var(--sp-4); }
    .banner {
      display: none; align-items: center; justify-content: space-between; gap: var(--sp-3);
      padding: 11px 15px; border-radius: var(--radius-md); font-size: var(--fs-6); font-weight: 700;
    }
    .banner-error { background: rgba(229,84,75,.14); border: 1px solid rgba(229,84,75,.4); color: #f3c6c2; }
    .banner-info { background: rgba(79,163,216,.14); border: 1px solid rgba(79,163,216,.4); color: #cfe8f7; }
    .banner-warn { background: rgba(224,161,58,.14); border: 1px solid rgba(224,161,58,.4); color: #f3dcc2; }
    .app.degraded .gauge-value, .app.degraded .shot-time, .app.degraded .stat-value { opacity: .55; }
    .sr-only {
      position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px;
      overflow: hidden; clip: rect(0,0,0,0); white-space: nowrap; border: 0;
    }
    .btn-chip {
      border: 1px solid var(--steam); background: var(--steam); color: #071824;
      padding: 6px 13px; border-radius: var(--radius-sm); font-size: var(--fs-4); font-weight: 700; cursor: pointer; flex: none;
    }

    .card {
      background: var(--surface);
      border: 1px solid var(--border-soft); border-radius: var(--radius-lg);
      padding: var(--sp-5); margin-bottom: var(--sp-4);
    }

    .hero-card { text-align: center; }
    /* Two gauges side by side (temp, pressure) - sized smaller than a
       single full-width gauge would be, so both fit on a phone screen. */
    .gauge-row { display: flex; justify-content: center; align-items: flex-start; gap: var(--sp-4); flex-wrap: wrap; margin-bottom: var(--sp-3); }
    .gauge-col { display: flex; flex-direction: column; align-items: center; }
    .gauge-wrap { position: relative; width: clamp(130px, 38vw, 170px); height: clamp(130px, 38vw, 170px); margin: 0 auto var(--sp-2); }
    .gauge { width: 100%; height: 100%; transform: rotate(-90deg); }
    .gauge-track { fill: none; stroke: var(--border); stroke-width: 14; }
    .gauge-fill { fill: none; stroke: var(--text-faint); stroke-width: 14; stroke-linecap: round; transition: stroke-dashoffset .6s var(--ease), stroke .4s ease; }
    /* Ring color is a functional signal, not decoration: blue while still
       climbing to target, green once at/near it (readable without parsing
       the number - also covers a shot's temperature sag, which is just
       "below target" again), red once over. */
    .gauge-fill.heating { stroke: var(--accent); }
    .gauge-fill.ready { stroke: var(--success); }
    .gauge-fill.over { stroke: var(--danger); }
    /* Pressure ring reuses the same heating/ready/over classes as the temp
       ring above - fill amount is a fixed 0-16 bar scale (this machine's
       safety-valve rating, a real manometer reading), but color answers a
       different question: not "% of scale" (9 bar brew target would only
       ever show ~56% full and never look "done"), but "how close to the
       9 bar brew target, while a shot is actually running." Grey/idle the
       rest of the time, same as the temp ring with no active target. */
    /* The number is the ONLY thing centered against this box, which exactly
       covers the ring (inset: 0 on .gauge-wrap) - so its flex centering
       lands on the ring's true geometric midpoint regardless of viewport
       size. The "Target XX C" caption used to live inside this same box,
       stacked under the number - that made flex/grid center the *pair* as a
       group instead of the number alone, which visibly dragged the number
       upward off the ring's actual center. It now lives outside, below the
       ring, in normal document flow (see .gauge-target). */
    .gauge-center { position: absolute; top: 0; right: 0; bottom: 0; left: 0; display: flex; align-items: center; justify-content: center; }
    .gauge-value { display: flex; align-items: baseline; justify-content: center; font-size: clamp(2rem, 8vw, 2.8rem); font-weight: 800; line-height: 1; letter-spacing: -1px; font-variant-numeric: tabular-nums; }
    .gauge-unit { font-size: clamp(0.8rem, 2.6vw, 1rem); color: var(--text-dim); font-weight: 600; margin-left: 3px; }
    .gauge-target { color: var(--text-dim); margin: 0; font-size: var(--fs-7); }
    .gauge-target b { color: var(--text); }

    .mode-switch { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: var(--sp-3); margin: var(--sp-4) 0; }
    .mode-btn {
      border: 1px solid var(--border); color: var(--text); background: var(--surface);
      padding: 15px 10px; border-radius: var(--radius-md); font-size: var(--fs-9); font-weight: 700;
      cursor: pointer; min-height: 52px;
      transition: transform .05s ease, background .15s ease, border-color .15s ease, color .15s ease;
    }
    .mode-btn:active { transform: translateY(1px); }
    .mode-off.active { background: var(--red); border-color: var(--red); color: #1a0d0c; }
    .mode-brew.active { background: var(--green); border-color: var(--green); color: #0d1a13; }
    .mode-steam.active { background: var(--steam); border-color: var(--steam); color: #071824; }

    /* Generic small-metric tile - reused today for Heater Output; the same
       component is meant for future roadmap readouts (pump pressure, water
       level, scale weight) without any layout changes - just add another
       .stat-tile and the auto-fit grid reflows. */
    .stat-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: var(--sp-3); margin-top: var(--sp-4); text-align: left; }
    .stat-tile { background: var(--surface); border: 1px solid var(--border-soft); border-radius: var(--radius-md); padding: 13px 14px; }
    .stat-label { display: block; font-size: var(--fs-2); color: var(--text-dim); text-transform: uppercase; letter-spacing: .6px; font-weight: 700; margin-bottom: 6px; }
    .stat-value { font-size: var(--fs-12); font-weight: 800; font-variant-numeric: tabular-nums; }
    .stat-value small { font-size: var(--fs-6); color: var(--text-dim); font-weight: 600; margin-left: 2px; }

    .bar { height: 9px; border-radius: var(--radius-full); background: #100d0b; border: 1px solid var(--border-soft); overflow: hidden; margin-top: 9px; }
    .bar > span { display: block; height: 100%; width: 0%; border-radius: var(--radius-full); transition: width .5s ease; }
    .bar.heat > span { background: var(--primary); }
    .bar.pump > span { background: var(--accent); }

    /* Generic chart card - reused today for the temp sparkline; a future
       pressure graph (roadmap item 7) is meant to drop in as a second
       .chart-card with its own canvas id, same label/legend pattern. */
    .chart-card { margin-top: var(--sp-4); text-align: left; }
    .chart-card canvas { width: 100%; height: 60px; display: block; }
    .chart-label { display: flex; justify-content: space-between; font-size: var(--fs-2); color: var(--text-dim); margin-bottom: 6px; }

    .shot-head { display: flex; align-items: center; justify-content: space-between; gap: var(--sp-4); }
    .shot-time { font-size: clamp(2.6rem, 13vw, 4rem); font-weight: 800; font-variant-numeric: tabular-nums; color: var(--text-dim); line-height: 1; transition: color .2s ease; }
    .shot-time.in-window { color: var(--green); }
    .shot-time.over { color: var(--red); }
    .shot-sub { font-size: var(--fs-4); color: var(--text-dim); margin-top: 4px; }
    .btn-shot {
      border: 1px solid rgba(72,181,131,.4); cursor: pointer; padding: 15px 22px; border-radius: var(--radius-md);
      font-size: var(--fs-7); font-weight: 700; color: #cdeede; background: rgba(72,181,131,.14); flex: none;
      transition: transform .05s ease, background .15s ease;
    }
    .btn-shot:active { transform: translateY(1px); }
    .btn-shot.running { background: var(--red); border-color: var(--red); color: #1a0d0c; }
    .shot-progress { height: 7px; border-radius: var(--radius-full); background: #100d0b; border: 1px solid var(--border-soft); overflow: hidden; margin-top: var(--sp-4); }
    .shot-progress > span { display: block; height: 100%; width: 0%; border-radius: var(--radius-full); background: var(--text-faint); transition: width .4s linear, background .3s ease; }
    .shot-progress > span.in-window { background: var(--green); }
    .shot-progress > span.over { background: var(--red); }

    /* Flex-wrap, not a fixed grid - profile count is dynamic (1-PROFILE_MAX_COUNT),
       unlike the old fixed 3-button preset row this replaced. */
    .preset-row { display: flex; flex-wrap: wrap; gap: var(--sp-2); margin-top: var(--sp-4); }
    .btn-preset {
      border: 1px solid var(--border); cursor: pointer; padding: 10px 14px; border-radius: var(--radius-full);
      font-size: var(--fs-5); font-weight: 700; color: var(--text-dim); background: var(--surface);
      transition: transform .05s ease, border-color .15s ease, color .15s ease;
    }
    .btn-preset:active { transform: translateY(1px); }
    .btn-preset.active { color: var(--copper-light); border-color: var(--copper); background: rgba(217,140,63,.12); }

    .btn-autotune {
      width: 100%; cursor: pointer; display: flex; align-items: center; justify-content: center; gap: var(--sp-2);
      padding: 15px 16px; border-radius: var(--radius-md); font-size: var(--fs-8); font-weight: 700;
      color: var(--primary-light); background: transparent; border: 1px solid var(--primary);
      transition: transform .05s ease, opacity .15s ease, background .15s ease, color .15s ease;
    }
    .btn-autotune:active:not(:disabled) { transform: translateY(1px); }
    .btn-autotune:disabled { opacity: .4; cursor: default; }
    .btn-autotune.running { background: var(--danger); border-color: var(--danger); color: #1a0d0c; }
    .autotune-status { display: block; margin-top: var(--sp-3); font-size: var(--fs-5); color: var(--text-dim); text-align: center; }

    .tab-section-title { font-size: var(--fs-4); font-weight: 700; color: var(--text-dim); text-transform: uppercase; letter-spacing: 1px; margin-bottom: var(--sp-3); }
    .tab-section-title.mt { margin-top: var(--sp-5); }
    .group-title {
      font-size: var(--fs-caption); font-weight: 700; color: var(--text-faint);
      text-transform: uppercase; letter-spacing: 1px; margin: var(--sp-5) 0 var(--sp-3);
    }
    .group-title:first-child { margin-top: 0; }
    .disclosure {
      border: 1px solid var(--border-soft); border-radius: var(--radius-md);
      padding: 0 var(--sp-4) var(--sp-4); margin: var(--sp-4) 0 0; background: var(--elevated);
    }
    .disclosure > summary {
      cursor: pointer; font-size: var(--fs-label); font-weight: 700; color: var(--text-muted);
      padding: var(--sp-4) 0; list-style: none;
    }
    .disclosure > summary::-webkit-details-marker { display: none; }
    .disclosure > summary::after {
      content: "Show"; float: right; color: var(--primary); font-weight: 600;
    }
    .disclosure[open] > summary::after { content: "Hide"; }
    .hint { font-size: var(--fs-4); color: var(--text-dim); margin: 0 0 var(--sp-3); line-height: 1.5; }
    .hint-link { display: inline-block; margin-top: var(--sp-3); font-size: var(--fs-6); color: var(--copper); text-decoration: none; font-weight: 600; }

    label { display: block; font-size: var(--fs-4); color: var(--text-dim); margin-bottom: 6px; font-weight: 600; }
    .field { margin-bottom: var(--sp-3); }
    input {
      width: 100%; padding: 12px 13px; border-radius: var(--radius-sm); font-size: var(--fs-9);
      background: #100d0b; border: 1px solid var(--border); color: var(--text);
      outline: none; transition: border-color .15s ease, box-shadow .15s ease;
    }
    input:focus { border-color: var(--copper); box-shadow: 0 0 0 3px rgba(217,140,63,.18); }
    .field-row { display: grid; grid-template-columns: 1fr 1fr; gap: var(--sp-3); }
    .field-row-3 { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: var(--sp-3); }
    .check-row { display: flex; align-items: center; gap: var(--sp-2); font-size: var(--fs-7); color: var(--text); cursor: pointer; }
    .sched-slot { padding: var(--sp-3) 0; border-bottom: 1px solid var(--border-soft); }
    .sched-slot:first-child { padding-top: 0; }
    .sched-slot .field-row { margin-top: var(--sp-3); }
    .check-row input { width: auto; }
    select { width: 100%; padding: 12px 13px; border-radius: var(--radius-sm); font-size: var(--fs-9); background: #100d0b; border: 1px solid var(--border); color: var(--text); outline: none; }

    .submit, .btn-secondary, .btn-danger { width: 100%; border: none; cursor: pointer; padding: 14px; border-radius: var(--radius-md); font-size: var(--fs-8); font-weight: 700; margin-top: var(--sp-1); }
    .submit { color: #1a1206; background: var(--primary); }
    .submit:active { transform: translateY(1px); }
    .btn-secondary { color: var(--text); background: var(--surface); border: 1px solid var(--border); margin-top: var(--sp-4); }
    .btn-danger { color: #1a0d0c; background: var(--red); }

    table.history { width: 100%; border-collapse: collapse; font-size: var(--fs-6); }
    table.history th, table.history td { text-align: left; padding: 9px 6px; border-bottom: 1px solid var(--border-soft); }
    table.history th { color: var(--text-dim); font-weight: 600; font-size: var(--fs-1); text-transform: uppercase; letter-spacing: .5px; }
    table.history td.num { text-align: right; font-variant-numeric: tabular-nums; }
    .table-scroll { overflow-x: auto; }
    .empty-hint { color: var(--text-dim); font-size: var(--fs-6); padding: 6px 0; }
    .metric-row { display: flex; align-items: center; justify-content: space-between; margin: 0 0 var(--sp-3); font-size: var(--fs-7); color: var(--text-dim); }
    .metric-row b { color: var(--text); font-size: var(--fs-9); }

    .profile-row {
      display: flex; align-items: center; justify-content: space-between; gap: var(--sp-3);
      padding: var(--sp-3) 0; border-bottom: 1px solid var(--border-soft);
    }
    .profile-row:last-child { border-bottom: none; }
    .profile-row.active { border-left: 2px solid var(--copper); padding-left: var(--sp-2); margin-left: calc(var(--sp-2) * -1); }
    .profile-row-main { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
    .profile-row-main b { font-size: var(--fs-8); }
    .profile-row-main span { font-size: var(--fs-4); color: var(--text-dim); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .profile-row-actions { display: flex; gap: var(--sp-2); flex: none; }
    .btn-chip-sm {
      border: 1px solid var(--border); background: var(--surface); color: var(--text-dim);
      padding: 6px 11px; border-radius: var(--radius-sm); font-size: var(--fs-3); font-weight: 700; cursor: pointer;
    }
    .btn-chip-sm.danger { border-color: rgba(229,84,75,.4); color: #f3c6c2; }
    .btn-chip-sm.danger.armed { background: var(--red); border-color: var(--red); color: #fff; }

    .footer { text-align: center; color: var(--text-dim); font-size: var(--fs-3); margin: var(--sp-5) 0 var(--sp-3); }
    .footer a { color: var(--copper); text-decoration: none; }

    .view[hidden] { display: none; }

    /* Inline "Saved"/"Restarting..."/"Save failed" confirmation, created
       on demand next to whichever submit button was just used. */
    .save-confirm {
      display: block; text-align: center; margin-top: var(--sp-2);
      font-size: var(--fs-6); font-weight: 700; color: var(--green);
      opacity: 0; transition: opacity .3s var(--ease);
    }
    .save-confirm.show { opacity: 1; }
    .save-confirm.error { color: var(--red); }

    /* Loading-skeleton placeholder for the gauge/stat values before the
       first /status response lands - swapped for the real value the
       instant it does (see the poll handler's firstStatusReceived flag). */
    .skeleton {
      color: transparent; background: linear-gradient(90deg, var(--surface-2), var(--border), var(--surface-2));
      background-size: 200% 100%; border-radius: var(--radius-sm);
    }
    @media (prefers-reduced-motion: no-preference) {
      .skeleton { animation: skeleton-pulse 1.5s ease-in-out infinite; }
    }
    @keyframes skeleton-pulse {
      0% { background-position: 200% 0; }
      100% { background-position: -200% 0; }
    }

    .tabbar {
      position: fixed; left: 0; right: 0; bottom: 0; z-index: 30;
      display: flex; justify-content: center; gap: var(--sp-2);
      padding: var(--sp-2) var(--sp-3) calc(var(--sp-2) + env(safe-area-inset-bottom));
      background: var(--bg);
      border-top: 1px solid var(--border-soft);
    }
    .tab {
      flex: 1; max-width: 200px; display: flex; flex-direction: column; align-items: center; gap: 3px;
      border: none; background: none; color: var(--text-faint); font-size: var(--fs-caption); font-weight: 700;
      padding: 8px 4px 6px; min-height: 48px; cursor: pointer; border-radius: var(--radius-md); transition: color .15s ease;
    }
    .tab-icon { font-size: var(--fs-11); filter: grayscale(1); opacity: .65; }
    .tab.active,
    .tab[aria-selected="true"] { color: var(--copper-light); }
    .tab.active .tab-icon,
    .tab[aria-selected="true"] .tab-icon { filter: none; opacity: 1; }

    /* Compact landscape layout for the "Now" tab only (live status at a
       glance on a tablet mounted sideways) - gated on max-height, not just
       orientation, so a tall landscape screen (e.g. a laptop) doesn't get
       squeezed into this. Tune/History/Settings stay single-column scrolling
       forms; they aren't something you need to see all at once. */
    @media (orientation: landscape) and (max-height: 600px) {
      .app { padding-top: var(--sp-3); }
      /* :not([hidden]) matters here: this selector (class + attribute) is
         otherwise more specific than the UA default `[hidden] { display:
         none }`, so without it, switching tabs away from "now" would leave
         this view forced visible via `display: grid` regardless of the
         `hidden` attribute showTab() sets on it. */
      .view[data-view="now"]:not([hidden]) {
        display: grid;
        grid-template-columns: minmax(190px, 260px) 1fr;
        grid-template-areas: "hero shot" "hero stats";
        gap: var(--sp-3);
        align-items: start;
      }
      .view[data-view="now"] > .hero-card { grid-area: hero; margin: 0; }
      .view[data-view="now"] > .card-shot { grid-area: shot; margin: 0; }
      .view[data-view="now"] > .card-stats { grid-area: stats; margin: 0; }

      /* The narrow hero column (190-260px) can't fit two side-by-side
         circles at their portrait size - .gauge-row's existing flex-wrap
         already stacks them here automatically; this just shrinks each one
         so the stacked pair still fits the same vertical budget a single
         circle used before. */
      .view[data-view="now"] .gauge-row { gap: var(--sp-1); margin-bottom: var(--sp-2); }
      .view[data-view="now"] .gauge-wrap {
        width: clamp(70px, 11vh, 100px); height: clamp(70px, 11vh, 100px);
        margin-bottom: var(--sp-1);
      }
      .view[data-view="now"] .gauge-value { font-size: clamp(1.1rem, 4vh, 1.5rem); letter-spacing: 0; }
      .view[data-view="now"] .gauge-unit { font-size: clamp(0.55rem, 2vh, 0.7rem); }
      .view[data-view="now"] .gauge-target { font-size: var(--fs-1); margin-bottom: var(--sp-1); }

      /* The two charts sit side-by-side instead of stacked, roughly halving
         the vertical space this card needs. */
      .view[data-view="now"] > .card-stats {
        display: grid;
        grid-template-columns: 1fr 1fr;
        grid-template-areas: "stat stat" "chart1 chart2";
        gap: var(--sp-3);
      }
      .view[data-view="now"] > .card-stats > .stat-grid { grid-area: stat; margin-top: 0; }
      .view[data-view="now"] > .card-stats > .chart-card { grid-area: chart1; margin-top: 0; }
      .view[data-view="now"] > .card-stats > .chart-card + .chart-card { grid-area: chart2; }
    }
  </style>
</head>
<body>
  <div class="app" id="app_root">
    <header class="topbar">
      <div class="brand">
        <div class="logo" aria-hidden="true">&#9749;</div>
        <div class="brand-text">
          <h1>BrewMaster</h1>
          <small>Gaggia Espresso Color</small>
        </div>
      </div>
      <div class="brand-status">
        <div id="status_pill" class="pill off" aria-live="polite"><span class="dot" aria-hidden="true"></span><span id="mode_status">--</span></div>
        <div id="last_updated" class="last-updated">--</div>
      </div>
    </header>

    <div class="alerts" role="status">
      <div id="offline_banner" class="banner banner-error">Controller unreachable &mdash; showing last known values. Check Wi-Fi.</div>
      <div id="fault_banner" class="banner banner-error">Sensor fault &mdash; heater held off. Check the PT100 wiring.</div>
      <div id="pressure_banner" class="banner banner-error"></div>
      <div id="pump_banner" class="banner banner-warn"></div>
      <div id="sleep_banner" class="banner banner-info">
        <span id="sleep_banner_text">Asleep (eco timeout) &mdash; heater off</span>
        <button type="button" onclick="wake()" class="btn-chip">Wake Up</button>
        <button type="button" onclick="dismissSleepBanner()" class="btn-chip-sm" aria-label="Dismiss sleep notice">&times;</button>
      </div>
      <div id="descale_banner_top" class="banner banner-warn">Descale recommended &mdash; see History</div>
    </div>

    <main>
    <div class="view" data-view="now" id="view-now" role="tabpanel" aria-labelledby="tab-now">
      <div class="card hero-card">
        <div class="gauge-row">
          <div class="gauge-col">
            <div class="gauge-wrap">
              <svg class="gauge" viewBox="0 0 220 220" aria-hidden="true">
                <circle class="gauge-track" cx="110" cy="110" r="96"></circle>
                <circle id="temp_ring_fill" class="gauge-fill" cx="110" cy="110" r="96"
                        stroke-dasharray="603" stroke-dashoffset="603"></circle>
              </svg>
              <div class="gauge-center">
                <div class="gauge-value"><span id="temp" class="skeleton">--</span><span class="gauge-unit">&deg;C</span></div>
              </div>
            </div>
            <div class="gauge-target">Target <b><span id="target" class="skeleton">--</span>&deg;C</b></div>
          </div>

          <div class="gauge-col">
            <div class="gauge-wrap">
              <svg class="gauge" viewBox="0 0 220 220" aria-hidden="true">
                <circle class="gauge-track" cx="110" cy="110" r="96"></circle>
                <circle id="pressure_ring_fill" class="gauge-fill" cx="110" cy="110" r="96"
                        stroke-dasharray="603" stroke-dashoffset="603"></circle>
              </svg>
              <div class="gauge-center">
                <div class="gauge-value"><span id="pressure_gauge_val" class="skeleton">--</span><span class="gauge-unit">bar</span></div>
              </div>
            </div>
            <div class="gauge-target" id="pressure_target_caption">Pressure</div>
          </div>
        </div>

        <div class="mode-switch" role="group" aria-label="Boiler mode">
          <button type="button" onclick="setMode('off')" id="btn_off" class="mode-btn mode-off" aria-pressed="false">Off</button>
          <button type="button" onclick="setMode('brew')" id="btn_brew" class="mode-btn mode-brew" aria-pressed="false">Brew</button>
          <button type="button" onclick="setMode('steam')" id="btn_steam" class="mode-btn mode-steam" aria-pressed="false">Steam</button>
        </div>
      </div>

      <div class="card card-shot">
        <div class="shot-head">
          <div>
            <div id="shot_time" class="shot-time" aria-live="polite">0:00</div>
            <div id="shot_auto_stop_label" class="shot-sub">--</div>
          </div>
          <button type="button" onclick="toggleShot()" id="btn_shot" class="btn-shot">Start Shot</button>
        </div>
        <div class="shot-progress"><span id="shot_progress_bar"></span></div>
        <div class="preset-row" id="profile_chip_row"></div>
      </div>

      <div class="card card-stats">
        <div class="stat-grid">
          <div class="stat-tile">
            <span class="stat-label">Heater</span>
            <span class="stat-value"><span id="output" class="skeleton">--</span><small>%</small></span>
            <div class="bar heat"><span id="output_bar"></span></div>
          </div>
          <div class="stat-tile">
            <span class="stat-label">Pump</span>
            <span class="stat-value"><span id="pump_power" class="skeleton">--</span><small>%</small></span>
            <div class="bar pump"><span id="pump_bar"></span></div>
          </div>
        </div>

        <div class="chart-card">
          <div class="chart-label"><span>Temp &middot; last 2 min</span><span>&deg;C</span></div>
          <canvas id="temp_chart" width="300" height="60" aria-hidden="true"></canvas>
        </div>

        <div class="chart-card">
          <div class="chart-label"><span>Pressure &middot; last 2 min</span><span id="pressure_label">-- bar</span></div>
          <canvas id="pressure_chart" width="300" height="60" aria-hidden="true"></canvas>
        </div>
      </div>
    </div>

    <div class="view" data-view="tune" id="view-tune" role="tabpanel" aria-labelledby="tab-tune" hidden>
      <div class="card">
        <div class="tab-section-title">Brew temperature</div>
        <form action="/update" method="GET">
          <div class="field">
            <label for="input_brew_target">Target (&deg;C)</label>
            <input type="number" step="0.1" name="brew_target" id="input_brew_target" value="">
          </div>
          <button type="submit" class="submit">Save target</button>
        </form>
        <details class="disclosure">
          <summary>Idle PID gains</summary>
          <form action="/update" method="GET">
            <div class="field-row-3">
              <div class="field"><label for="input_brew_kp">Kp</label><input type="number" step="any" name="brew_kp" id="input_brew_kp" value=""></div>
              <div class="field"><label for="input_brew_ki">Ki</label><input type="number" step="any" name="brew_ki" id="input_brew_ki" value=""></div>
              <div class="field"><label for="input_brew_kd">Kd</label><input type="number" step="any" name="brew_kd" id="input_brew_kd" value=""></div>
            </div>
            <button type="submit" class="submit">Save idle gains</button>
          </form>
        </details>
        <details class="disclosure">
          <summary>Gains during a shot</summary>
          <p class="hint">Switches in when a shot starts, reverts when it stops. More aggressive so flow does not pull boiler temperature down.</p>
          <form action="/update" method="GET">
            <div class="field-row-3">
              <div class="field"><label for="input_brew_akp">Kp</label><input type="number" step="any" name="brew_akp" id="input_brew_akp" value=""></div>
              <div class="field"><label for="input_brew_aki">Ki</label><input type="number" step="any" name="brew_aki" id="input_brew_aki" value=""></div>
              <div class="field"><label for="input_brew_akd">Kd</label><input type="number" step="any" name="brew_akd" id="input_brew_akd" value=""></div>
            </div>
            <button type="submit" class="submit">Save shot gains</button>
          </form>
        </details>
      </div>

      <div class="card">
        <div class="tab-section-title">Auto-tune</div>
        <p class="hint">Cycles the heater around the current Brew or Steam target for several minutes. Stay by the machine.</p>
        <button type="button" onclick="startAutotune()" id="btn_autotune" class="btn-autotune">Start Auto-Tune</button>
        <span id="autotune_status" class="autotune-status"></span>
      </div>

      <div class="card">
        <div class="tab-section-title">Steam</div>
        <form action="/update" method="GET">
          <div class="field">
            <label for="input_steam_target">Target temperature (&deg;C)</label>
            <input type="number" step="0.1" name="steam_target" id="input_steam_target" value="">
          </div>
          <details class="disclosure">
            <summary>Steam PID and safety ceiling</summary>
            <div class="field-row-3">
              <div class="field"><label for="input_steam_kp">Kp</label><input type="number" step="any" name="steam_kp" id="input_steam_kp" value=""></div>
              <div class="field"><label for="input_steam_ki">Ki</label><input type="number" step="any" name="steam_ki" id="input_steam_ki" value=""></div>
              <div class="field"><label for="input_steam_kd">Kd</label><input type="number" step="any" name="steam_kd" id="input_steam_kd" value=""></div>
            </div>
            <div class="field">
              <label for="input_steam_max_safety">Max safety ceiling (&deg;C)</label>
              <input type="number" step="1" min="100" max="150" name="steam_max_safety" id="input_steam_max_safety" value="">
            </div>
          </details>
          <button type="submit" class="submit">Save Steam</button>
        </form>
      </div>

      <div class="card">
        <div class="tab-section-title">Pump pressure PID</div>
        <p class="hint">Used only during a profile's pressure ramp/decline stages. Pre-infusion pulses stay on/off.</p>
        <details class="disclosure">
          <summary>Kp / Ki / Kd</summary>
          <form action="/update" method="GET">
            <div class="field-row-3">
              <div class="field"><label for="input_press_kp">Kp</label><input type="number" step="any" name="press_kp" id="input_press_kp" value=""></div>
              <div class="field"><label for="input_press_ki">Ki</label><input type="number" step="0.01" name="press_ki" id="input_press_ki" value=""></div>
              <div class="field"><label for="input_press_kd">Kd</label><input type="number" step="any" name="press_kd" id="input_press_kd" value=""></div>
            </div>
            <button type="submit" class="submit">Save pump pressure</button>
          </form>
        </details>
      </div>

      <div class="card">
        <div class="tab-section-title">Shot profiles</div>
        <p class="hint">Chips on Now load a saved temperature, auto-stop, pre-infusion, and pressure pattern into the live Brew settings.</p>
        <div id="profile_list_body"></div>
        <button type="button" onclick="newProfileForm()" class="btn-secondary">New profile</button>
      </div>

      <div class="card" id="profile_editor_card" hidden>
        <div class="tab-section-title" id="profile_form_title">New Profile</div>
        <form onsubmit="submitProfileForm(event)">
          <input type="hidden" id="input_profile_index" value="-1">
          <div class="field">
            <label for="input_profile_name">Name</label>
            <input type="text" id="input_profile_name" maxlength="20" placeholder="e.g. Light Roast">
          </div>
          <div class="field-row">
            <div class="field"><label for="input_profile_temp">Temperature (&deg;C)</label><input type="number" step="0.1" id="input_profile_temp" value="93"></div>
            <div class="field"><label for="input_profile_autostop">Auto-stop (sec)</label><input type="number" step="1" min="5" max="90" id="input_profile_autostop" value="27" oninput="drawProfilePreview()"></div>
          </div>
          <label class="check-row"><input type="checkbox" id="input_profile_pi_enabled" onchange="drawProfilePreview()"> Pulsed pre-infusion</label>
          <p class="hint" style="margin-top:var(--sp-2)">Pulses the pump on/off before continuous extraction to wet the puck.</p>
          <div class="field-row-3" style="margin-top:var(--sp-3)">
            <div class="field"><label for="input_profile_pi_pulses">Pulses</label><input type="number" step="1" min="0" max="10" id="input_profile_pi_pulses" value="3" oninput="drawProfilePreview()"></div>
            <div class="field"><label for="input_profile_pi_on">On (sec)</label><input type="number" step="0.1" min="0.2" max="5" id="input_profile_pi_on" value="1" oninput="drawProfilePreview()"></div>
            <div class="field"><label for="input_profile_pi_off">Off (sec)</label><input type="number" step="0.1" min="0.2" max="5" id="input_profile_pi_off" value="2" oninput="drawProfilePreview()"></div>
          </div>
          <label class="check-row"><input type="checkbox" id="input_profile_press_enabled"> Pressure profile</label>
          <p class="hint" style="margin-top:var(--sp-2)">Closed-loop ramp, hold, then optional decline. Needs the transducer plumbed in.</p>
          <div class="field-row-3" style="margin-top:var(--sp-3)">
            <div class="field"><label for="input_profile_press_ramp_bar">Ramp target (bar)</label><input type="number" step="0.1" min="0" max="11.9" id="input_profile_press_ramp_bar" value="9"></div>
            <div class="field"><label for="input_profile_press_ramp_sec">Ramp/hold (sec)</label><input type="number" step="1" min="1" id="input_profile_press_ramp_sec" value="20"></div>
          </div>
          <label class="check-row"><input type="checkbox" id="input_profile_press_decline_enabled"> Declining finish</label>
          <div class="field-row-3" style="margin-top:var(--sp-3)">
            <div class="field"><label for="input_profile_press_decline_bar">Decline target (bar)</label><input type="number" step="0.1" min="0" max="11.9" id="input_profile_press_decline_bar" value="6"></div>
            <div class="field"><label for="input_profile_press_decline_sec">Decline (sec)</label><input type="number" step="1" min="1" id="input_profile_press_decline_sec" value="8"></div>
          </div>
          <div class="chart-card">
            <div class="chart-label"><span>Pump pattern preview</span><span id="profile_preview_label">&nbsp;</span></div>
            <canvas id="profile_preview_chart" width="300" height="40" aria-hidden="true"></canvas>
          </div>
          <button type="submit" id="profile_form_submit" class="submit">Add Profile</button>
          <button type="button" onclick="cancelProfileForm()" class="btn-secondary">Cancel</button>
        </form>
      </div>
    </div>

    <div class="view" data-view="history" id="view-history" role="tabpanel" aria-labelledby="tab-history" hidden>
      <div class="card">
        <div class="tab-section-title">Shot History</div>
        <div id="shot_history_empty" class="empty-hint">No shots logged yet.</div>
        <div class="table-scroll">
          <table class="history" id="shot_history_table" style="display:none">
            <thead><tr><th>When</th><th>Duration</th><th>Peak &deg;C</th><th>End &deg;C</th><th id="shot_weight_th" hidden>Weight</th><th>Notes</th></tr></thead>
            <tbody id="shot_history_body"></tbody>
          </table>
        </div>
        <div class="chart-card">
          <div class="chart-label"><span>Peak (&#9679;) &amp; end (&mdash;) temp &middot; last 20 shots</span><span>&deg;C</span></div>
          <canvas id="shot_trend_chart" width="300" height="70"></canvas>
        </div>
      </div>

      <div class="card" id="shot_notes_card" hidden>
        <div class="tab-section-title" id="shot_notes_title">Shot Notes</div>
        <p class="hint">Filled in after tasting, not at pull time - bean/dose/grind/rating are a dial-in journal, not something you'd want to stop and type mid-shot.</p>
        <form onsubmit="submitShotNotes(event)">
          <input type="hidden" id="input_shot_note_index" value="-1">
          <div class="field-row">
            <div class="field"><label for="input_shot_note_bean">Bean</label><input type="text" id="input_shot_note_bean" maxlength="24" placeholder="e.g. Ethiopia Yirgacheffe"></div>
            <div class="field"><label for="input_shot_note_dose">Dose in (g)</label><input type="number" step="0.1" id="input_shot_note_dose" value="18"></div>
          </div>
          <div class="field-row">
            <div class="field"><label for="input_shot_note_grind">Grind setting</label><input type="text" id="input_shot_note_grind" maxlength="16" placeholder="e.g. 3.2"></div>
            <div class="field">
              <label for="input_shot_note_rating">Rating</label>
              <select id="input_shot_note_rating">
                <option value="0">Unrated</option>
                <option value="1">1 - Poor</option>
                <option value="2">2</option>
                <option value="3">3 - OK</option>
                <option value="4">4</option>
                <option value="5">5 - Great</option>
              </select>
            </div>
          </div>
          <div class="field">
            <label for="input_shot_note_text">Notes</label>
            <input type="text" id="input_shot_note_text" maxlength="60" placeholder="e.g. sour, tighten grind next time">
          </div>
          <button type="submit" class="submit">Save Shot Notes</button>
        </form>
      </div>

      <div class="card">
        <div class="tab-section-title">Maintenance</div>
        <div class="metric-row"><span>Shots since last descale</span><b><span id="descale_shots">--</span></b></div>
        <div class="metric-row"><span>Days since last descale</span><b><span id="descale_days">--</span></b></div>
        <form action="/update" method="GET">
          <div class="field-row">
            <div class="field"><label for="input_descale_shots">Shot threshold</label><input type="number" step="1" min="1" name="descale_shot_threshold" id="input_descale_shots" value=""></div>
            <div class="field"><label for="input_descale_days">Day threshold</label><input type="number" step="1" min="1" name="descale_day_threshold" id="input_descale_days" value=""></div>
          </div>
          <button type="submit" class="submit">Save Thresholds</button>
        </form>
        <button type="button" onclick="markDescaled()" class="btn-secondary">Mark Descaled Today</button>
      </div>
    </div>

    <div class="view" data-view="settings" id="view-settings" role="tabpanel" aria-labelledby="tab-settings" hidden>
      <div class="group-title">Brewing</div>
      <div class="card">
        <div class="tab-section-title">Shot auto-stop</div>
        <form action="/update" method="GET">
          <div class="field">
            <label for="input_shot_auto_stop">Stop pump after (seconds, 0 = manual only)</label>
            <input type="number" step="1" min="0" name="shot_auto_stop_sec" id="input_shot_auto_stop" value="">
          </div>
          <button type="submit" class="submit">Save</button>
        </form>
        <p class="hint" style="margin-top:var(--sp-3)">Cuts the pump via the dimmer when the timer hits this duration. Loading a profile overwrites this with that profile's own auto-stop. The physical Brew switch must still be on for the dimmer to have power.</p>
      </div>

      <div class="group-title">Power</div>
      <div class="card">
        <div class="tab-section-title">Eco / auto-off</div>
        <form action="/update" method="GET">
          <div class="field">
            <label for="input_eco_min">Brew auto-sleep after (minutes, 0 = disabled)</label>
            <input type="number" step="1" min="0" name="eco_timeout_min" id="input_eco_min" value="">
          </div>
          <div class="field">
            <label for="input_steam_off_min">Steam auto-off after (minutes, 0 = disabled)</label>
            <input type="number" step="1" min="0" name="steam_auto_off_min" id="input_steam_off_min" value="">
          </div>
          <button type="submit" class="submit">Save</button>
        </form>
        <p class="hint" style="margin-top:var(--sp-3)">Heater force-OFF after this long with no Web UI activity (mode/tuning changes). Steam uses a shorter default because it runs hotter. Status polling does not count as activity.</p>
      </div>

      <div class="card">
        <div class="tab-section-title">Scheduled warm-up</div>
        <div id="ntp_banner" class="banner banner-warn" style="margin-bottom:var(--sp-3)">Clock not synced yet &mdash; scheduled warm-up will not fire until NTP succeeds.</div>
        <form action="/update" method="GET">
          <div class="sched-slot">
            <label class="check-row"><input type="checkbox" id="input_sched0_en_cb" onchange="setSchedEnabled(0,this.checked)"> Slot 1 enabled</label>
            <div class="field-row">
              <div class="field"><label for="input_sched0_time">Local time</label><input type="time" name="sched0_time" id="input_sched0_time" value="07:00"></div>
              <div class="field">
                <label for="input_sched0_steam">Mode</label>
                <select name="sched0_steam" id="input_sched0_steam"><option value="0">Brew</option><option value="1">Steam</option></select>
              </div>
            </div>
          </div>
          <div class="sched-slot">
            <label class="check-row"><input type="checkbox" id="input_sched1_en_cb" onchange="setSchedEnabled(1,this.checked)"> Slot 2 enabled</label>
            <div class="field-row">
              <div class="field"><label for="input_sched1_time">Local time</label><input type="time" name="sched1_time" id="input_sched1_time" value="07:00"></div>
              <div class="field">
                <label for="input_sched1_steam">Mode</label>
                <select name="sched1_steam" id="input_sched1_steam"><option value="0">Brew</option><option value="1">Steam</option></select>
              </div>
            </div>
          </div>
          <div class="sched-slot">
            <label class="check-row"><input type="checkbox" id="input_sched2_en_cb" onchange="setSchedEnabled(2,this.checked)"> Slot 3 enabled</label>
            <div class="field-row">
              <div class="field"><label for="input_sched2_time">Local time</label><input type="time" name="sched2_time" id="input_sched2_time" value="07:00"></div>
              <div class="field">
                <label for="input_sched2_steam">Mode</label>
                <select name="sched2_steam" id="input_sched2_steam"><option value="0">Brew</option><option value="1">Steam</option></select>
              </div>
            </div>
          </div>
          <input type="hidden" name="sched_tz_min" id="input_sched_tz_min" value="0">
          <button type="submit" class="submit">Save schedule times</button>
        </form>
        <p class="hint" style="margin-top:var(--sp-3)">Up to 3 times. Browser timezone (<span id="sched_tz_display">--</span>) is kept in sync automatically. Each slot fires once per calendar day.</p>
      </div>

      <div class="group-title">Network</div>
      <div class="card">
        <div class="tab-section-title">Wi-Fi</div>
        <button type="button" onclick="wifiReset()" class="btn-danger">Reset WiFi settings</button>
        <p class="hint" style="margin-top:var(--sp-3)">Reboots into the <b>GaggiaBrewMasterESP_Setup</b> network so you can join a different Wi-Fi without reflashing.</p>
        <a href="/firmware" class="hint-link">Firmware update (OTA)</a>
      </div>

      <div class="card">
        <div class="tab-section-title">MQTT / Home Assistant</div>
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
        <p class="hint" style="margin-top:var(--sp-3)">Saving MQTT settings reboots the controller. Home Assistant auto-discovers mode, targets, sensors, and shot/fault/descale binaries.</p>
      </div>

      <div class="group-title">Backup</div>
      <div class="card">
        <div class="tab-section-title">Settings file</div>
        <a href="/settings_export" class="btn-secondary" style="display:block;text-align:center;text-decoration:none;margin-top:0">Download backup</a>
        <label class="btn-secondary" style="display:block;text-align:center;cursor:pointer">
          Restore from file
          <input type="file" accept=".txt" onchange="restoreSettings(this)" style="display:none">
        </label>
        <p class="hint" style="margin-top:var(--sp-3)">Brew/Steam/schedule/MQTT settings. Named shot profiles live in a separate file and are not included here.</p>
      </div>
    </div>
    </main>

    <footer class="footer">
      <a href="/firmware">Firmware update (OTA)</a> &middot; <span id="host">gaggia.local</span> &middot; build <span id="fw_build">--</span>
    </footer>
  </div>

  <nav class="tabbar" role="tablist" aria-label="Sections">
    <button type="button" class="tab active" role="tab" aria-selected="true" aria-controls="view-now" id="tab-now" data-tab="now"><span class="tab-icon" aria-hidden="true">&#9749;</span><span>Now</span></button>
    <button type="button" class="tab" role="tab" aria-selected="false" tabindex="-1" aria-controls="view-tune" id="tab-tune" data-tab="tune"><span class="tab-icon" aria-hidden="true">&#9881;</span><span>Tune</span></button>
    <button type="button" class="tab" role="tab" aria-selected="false" tabindex="-1" aria-controls="view-history" id="tab-history" data-tab="history"><span class="tab-icon" aria-hidden="true">&#8987;</span><span>History</span></button>
    <button type="button" class="tab" role="tab" aria-selected="false" tabindex="-1" aria-controls="view-settings" id="tab-settings" data-tab="settings"><span class="tab-icon" aria-hidden="true">&#9776;</span><span>Settings</span></button>
  </nav>

<script>
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
  var valid = ["now", "tune", "history", "settings"];
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

// Draws a dashed horizontal reference line at the given target value,
// using the same min/max scale as the data line - shared by both charts.
function drawTargetLine(ctx, w, h, min, max, target) {
  if (target === null || target === undefined) return;
  var y = h - ((target - min) / (max - min)) * (h - 6) - 3;
  ctx.save();
  ctx.strokeStyle = "rgba(179,162,148,.55)"; // --text-dim, translucent
  ctx.lineWidth = 1.5;
  ctx.setLineDash([4, 3]);
  ctx.beginPath();
  ctx.moveTo(0, y);
  ctx.lineTo(w, y);
  ctx.stroke();
  ctx.restore();
}

function drawSparkline(data, target) {
  var canvas = document.getElementById("temp_chart");
  if (!canvas || !data || data.length < 2) return;
  var ctx = canvas.getContext("2d");
  var w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  var scaleValues = (target === null || target === undefined) ? data : data.concat([target]);
  var min = Math.min.apply(null, scaleValues), max = Math.max.apply(null, scaleValues);
  if (max - min < 1) { max += 0.5; min -= 0.5; }

  phaseMarkers = phaseMarkers.filter(function (m) { return m.age < data.length; });
  if (phaseMarkers.length) {
    ctx.save();
    ctx.strokeStyle = "rgba(224,161,58,.55)"; // --amber, translucent
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 2]);
    phaseMarkers.forEach(function (m) {
      var idx = data.length - 1 - m.age;
      var x = (idx / (data.length - 1)) * w;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, h);
      ctx.stroke();
    });
    ctx.restore();
  }

  drawTargetLine(ctx, w, h, min, max, target);

  ctx.beginPath();
  data.forEach(function (v, i) {
    var x = (i / (data.length - 1)) * w;
    var y = h - ((v - min) / (max - min)) * (h - 6) - 3;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.strokeStyle = "#d98c3f";
  ctx.lineWidth = 2.5;
  ctx.lineJoin = "round";
  ctx.stroke();
}

function drawPressureSparkline(data, target) {
  var canvas = document.getElementById("pressure_chart");
  if (!canvas || !data || data.length < 2) return;
  var ctx = canvas.getContext("2d");
  var w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  var scaleValues = (target === null || target === undefined) ? data : data.concat([target]);
  var min = Math.min.apply(null, scaleValues), max = Math.max.apply(null, scaleValues);
  if (max - min < 0.5) { max += 0.25; min -= 0.25; }
  if (min > 0) min = 0; // pressure chart always includes zero for scale

  drawTargetLine(ctx, w, h, min, max, target);

  ctx.beginPath();
  data.forEach(function (v, i) {
    var x = (i / (data.length - 1)) * w;
    var y = h - ((v - min) / (max - min)) * (h - 6) - 3;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.strokeStyle = "#3f8cd9";
  ctx.lineWidth = 2.5;
  ctx.lineJoin = "round";
  ctx.stroke();
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
// Populated from /status (shot_auto_stop_sec) - the progress bar and
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
// reversed (newest-first) display order, so editShotNotes()/submitShotNotes()
// need the original index preserved alongside each displayed row.
var shotsCache = [];

function shotNotesSummary(s) {
  if (!s.bean && !s.rating && !s.notes) return "+ Add";
  var stars = s.rating > 0 ? "★".repeat(s.rating) : "";
  return (stars ? stars + " " : "") + (s.bean || "Notes");
}

function fetchShotHistory() {
  xhr("/shots", function (status, text) {
    if (status !== 200) return;
    try { shotsCache = JSON.parse(text); } catch (err) { return; }
    var body = document.getElementById("shot_history_body");
    var table = document.getElementById("shot_history_table");
    var empty = document.getElementById("shot_history_empty");
    var weightTh = document.getElementById("shot_weight_th");
    if (!shotsCache.length) {
      table.style.display = "none";
      empty.style.display = "block";
      return;
    }
    empty.style.display = "none";
    table.style.display = "table";
    var showWeight = shotsCache.some(function (s) { return s.weight > 0; });
    if (weightTh) weightTh.hidden = !showWeight;
    body.textContent = "";
    shotsCache.map(function (s, i) { return { s: s, idx: i }; })
      .reverse().slice(0, 15).forEach(function (entry) {
      var s = entry.s;
      var tr = document.createElement("tr");
      function td(text, cls) {
        var cell = document.createElement("td");
        if (cls) cell.className = cls;
        cell.textContent = text;
        return cell;
      }
      var when = new Date(s.ts * 1000).toLocaleString([], { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });
      tr.appendChild(td(when));
      tr.appendChild(td(formatElapsed(s.duration_ms), "num"));
      tr.appendChild(td(s.peak_temp.toFixed(1), "num"));
      tr.appendChild(td(s.end_temp > 0 ? s.end_temp.toFixed(1) : "—", "num"));
      if (showWeight) tr.appendChild(td(s.weight > 0 ? s.weight.toFixed(1) + "g" : "—", "num"));
      var notesTd = document.createElement("td");
      notesTd.style.cursor = "pointer";
      notesTd.textContent = shotNotesSummary(s);
      notesTd.addEventListener("click", function () { editShotNotes(entry.idx); });
      tr.appendChild(notesTd);
      body.appendChild(tr);
    });
    drawShotTrendChart(shotsCache.slice(-20));
  });
}
fetchShotHistory();

function editShotNotes(idx) {
  var s = shotsCache[idx];
  if (!s) return;
  document.getElementById("input_shot_note_index").value = idx;
  document.getElementById("input_shot_note_bean").value = s.bean || "";
  document.getElementById("input_shot_note_dose").value = s.dose_in || 18;
  document.getElementById("input_shot_note_grind").value = s.grind || "";
  document.getElementById("input_shot_note_rating").value = s.rating || 0;
  document.getElementById("input_shot_note_text").value = s.notes || "";
  var when = new Date(s.ts * 1000).toLocaleString([], { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });
  document.getElementById("shot_notes_title").textContent = "Shot Notes — " + when;
  var card = document.getElementById("shot_notes_card");
  card.hidden = false;
  card.scrollIntoView({ behavior: "smooth", block: "center" });
}

function submitShotNotes(ev) {
  ev.preventDefault();
  var q = "shot_note_index=" + document.getElementById("input_shot_note_index").value;
  q += "&shot_note_bean=" + encodeURIComponent(document.getElementById("input_shot_note_bean").value);
  q += "&shot_note_dose=" + document.getElementById("input_shot_note_dose").value;
  q += "&shot_note_grind=" + encodeURIComponent(document.getElementById("input_shot_note_grind").value);
  q += "&shot_note_rating=" + document.getElementById("input_shot_note_rating").value;
  q += "&shot_note_text=" + encodeURIComponent(document.getElementById("input_shot_note_text").value);
  xhr("/update?" + q, function () {
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

// Shot-quality trend (History tab) - peak temp per shot as a line, against a
// dashed reference line at the current Brew target, so a regression (like
// the overshoot this chart is meant to catch) is visible at a glance instead
// of needing a manual trace every time. End temp draws as a second, thinner
// line - a big peak/end gap shows a shot that spiked mid-pull then cooled
// back down, vs. one that simply ended hot. Duration is encoded as the peak
// marker's size (bigger dot = longer shot) rather than a third axis, to keep
// this a single simple canvas rather than a multi-series chart library.
// end_temp reads 0 ("unknown") for shots logged before that field existed -
// filtered out of both the series and the min/max range rather than drawn
// as a false dip to zero.
function drawShotTrendChart(shots) {
  var canvas = document.getElementById("shot_trend_chart");
  if (!canvas) return;
  var ctx = canvas.getContext("2d");
  var w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  if (!shots || shots.length < 2) return;

  var temps = shots.map(function (s) { return s.peak_temp; });
  var ends = shots.map(function (s) { return s.end_temp > 0 ? s.end_temp : null; });
  var durs = shots.map(function (s) { return s.duration_ms; });
  var knownEnds = ends.filter(function (v) { return v !== null; });
  var range = temps.concat(knownEnds).concat(lastBrewTarget != null ? [lastBrewTarget] : []);
  var min = Math.min.apply(null, range), max = Math.max.apply(null, range);
  if (max - min < 2) { max += 1; min -= 1; }
  var minDur = Math.min.apply(null, durs), maxDur = Math.max.apply(null, durs);

  function yFor(v) { return h - ((v - min) / (max - min)) * (h - 8) - 4; }
  function xFor(i) { return (i / (shots.length - 1)) * (w - 8) + 4; }

  if (lastBrewTarget != null) {
    var ty = yFor(lastBrewTarget);
    ctx.beginPath();
    ctx.setLineDash([4, 3]);
    ctx.moveTo(0, ty); ctx.lineTo(w, ty);
    ctx.strokeStyle = "rgba(217,140,63,.5)";
    ctx.lineWidth = 1;
    ctx.stroke();
    ctx.setLineDash([]);
  }

  // End temp - drawn first (under the peak line), breaking the line across
  // any unknown (null) gaps instead of interpolating through them.
  ctx.beginPath();
  var penDown = false;
  ends.forEach(function (v, i) {
    if (v === null) { penDown = false; return; }
    var x = xFor(i), y = yFor(v);
    if (!penDown) { ctx.moveTo(x, y); penDown = true; } else { ctx.lineTo(x, y); }
  });
  ctx.strokeStyle = "#6b93b0";
  ctx.lineWidth = 1.5;
  ctx.lineJoin = "round";
  ctx.stroke();

  ctx.beginPath();
  temps.forEach(function (v, i) {
    var x = xFor(i), y = yFor(v);
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.strokeStyle = "#d98c3f";
  ctx.lineWidth = 2;
  ctx.lineJoin = "round";
  ctx.stroke();

  temps.forEach(function (v, i) {
    var x = xFor(i), y = yFor(v);
    var durFrac = maxDur > minDur ? (durs[i] - minDur) / (maxDur - minDur) : 0.5;
    var r = 2 + durFrac * 3.5; // 2-5.5px, longer shots draw a bigger dot
    ctx.beginPath();
    ctx.arc(x, y, r, 0, 2 * Math.PI);
    ctx.fillStyle = (lastBrewTarget != null && v > lastBrewTarget + 1.5) ? "#e5544b" : "#d98c3f";
    ctx.fill();
  });
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
var lastZcCount = null;
var lastZcAt = null;
var zcHz = 0;

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
  btnShot.textContent = running ? "Stop Shot" : "Start Shot";
  btnShot.classList.toggle("running", running);
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
  setText("output", outputPct.toFixed(0));
  setText("pump_power", pumpPct.toFixed(0));
  var pumpBar = document.getElementById("pump_bar");
  if (pumpBar) pumpBar.style.width = clamp(pumpPct) + "%";

  trackPhaseMarkers(json.shot_phase, json.shot_in_progress);
  drawSparkline(json.history, hasTarget ? target : null);
  var pressureTarget = (json.press_enabled && json.shot_phase === "pressure") ? json.press_ramp_bar : null;
  drawPressureSparkline(json.pressure_history, pressureTarget);
  var pLabel = document.getElementById("pressure_label");
  if (pLabel) pLabel.textContent = (json.pressure_fault ? "fault" : json.pressure.toFixed(2) + " bar");

  var tempPct = (temp > 0 && hasTarget) ? clamp((temp / target) * 100) : 0;
  var ring = document.getElementById("temp_ring_fill");
  var RING_CIRCUMFERENCE = 603;
  ring.style.strokeDashoffset = RING_CIRCUMFERENCE * (1 - tempPct / 100);
  ring.classList.remove("heating", "ready", "over");
  if (!json.fault && temp > 0 && hasTarget) {
    var READY_MARGIN_C = 1.0;
    if (temp < target - READY_MARGIN_C) ring.classList.add("heating");
    else if (temp > target + READY_MARGIN_C) ring.classList.add("over");
    else ring.classList.add("ready");
  }

  var PRESSURE_GAUGE_MAX_BAR = 16;
  var pressureGoal = (json.press_enabled && json.press_ramp_bar > 0) ? json.press_ramp_bar : 9;
  var pressureVal = json.pressure;
  var pressurePct = json.pressure_fault ? 0 : clamp((pressureVal / PRESSURE_GAUGE_MAX_BAR) * 100);
  var pRing = document.getElementById("pressure_ring_fill");
  if (pRing) {
    pRing.style.strokeDashoffset = RING_CIRCUMFERENCE * (1 - pressurePct / 100);
    pRing.classList.remove("heating", "ready", "over");
    if (!json.pressure_fault && json.shot_in_progress) {
      var PRESSURE_READY_MARGIN_BAR = 1.5;
      if (pressureVal < pressureGoal - PRESSURE_READY_MARGIN_BAR) pRing.classList.add("heating");
      else if (pressureVal > pressureGoal + PRESSURE_READY_MARGIN_BAR) pRing.classList.add("over");
      else pRing.classList.add("ready");
    }
  }
  setText("pressure_gauge_val", json.pressure_fault ? "--" : pressureVal.toFixed(1));
  var pCap = document.getElementById("pressure_target_caption");
  if (pCap) {
    pCap.textContent = json.shot_in_progress && json.press_enabled
      ? "Target " + pressureGoal.toFixed(1) + " bar"
      : "Pressure";
  }

  document.getElementById("output_bar").style.width = clamp(outputPct) + "%";

  var pill = document.getElementById("status_pill");
  var label = json.fault ? "Fault" : mode === "brew" ? "Brewing" : mode === "steam" ? "Steaming" : "Off";
  setText("mode_status", label);
  pill.className = "pill " + mode + (json.fault ? " fault" : "");
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
  setVal("input_shot_auto_stop", json.shot_auto_stop_sec);
  var passHint = document.getElementById("mqtt_pass_hint");
  if (passHint) passHint.hidden = !json.mqtt_pass_set;

  var ntpBanner = document.getElementById("ntp_banner");
  if (ntpBanner) ntpBanner.style.display = json.ntp_synced ? "none" : "flex";

  var now = Date.now();
  if (typeof json.dimmer_zc_count === "number") {
    if (lastZcCount !== null && lastZcAt !== null) {
      var dt = (now - lastZcAt) / 1000;
      if (dt > 0) zcHz = (json.dimmer_zc_count - lastZcCount) / dt;
    }
    lastZcCount = json.dimmer_zc_count;
    lastZcAt = now;
  }

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

  var brewSwitchOff = json.shot_in_progress && zcHz < 20 && lastZcCount !== null;
  if (brewSwitchOff) {
    showBanner("pump_banner", true, "Flip the machine's Brew switch — the dimmer has no mains power.");
  } else if (json.shot_in_progress && pumpPct < 1 && zcHz >= 20 && !json.pressure_ceiling_tripped) {
    showBanner("pump_banner", true, "Pump commanded off while the Brew switch is on.");
  } else {
    showBanner("pump_banner", false);
  }

  var shotLabel = document.getElementById("shot_auto_stop_label");
  if (json.shot_in_progress && json.shot_phase === "preinfusion") {
    shotLabel.textContent = "Pre-infusing…";
  } else if (json.shot_in_progress && json.shot_phase === "pressure") {
    shotLabel.textContent = "Pressure " + (json.press_ramp_bar ? json.press_ramp_bar.toFixed(1) + " bar" : "hold");
  } else if (json.shot_in_progress && json.shot_phase === "extraction") {
    shotLabel.textContent = "Extracting…";
  } else {
    shotLabel.textContent = json.shot_auto_stop_sec > 0 ? "Auto-stops at " + json.shot_auto_stop_sec + "s" : "Auto-stop disabled";
  }

  shotAutoStopSec = json.shot_auto_stop_sec;
  if (json.active_profile !== activeProfileIndex) {
    activeProfileIndex = json.active_profile;
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

function toggleShot() {
  var next = !shotRunning;
  shotRunning = next;
  if (next) {
    shotElapsedBaseMs = 0;
    shotElapsedCapturedAt = Date.now();
  }
  applyShotButton(next);
  xhr("/update?shot=" + (next ? "start" : "stop"));
}

// ============================================================================
// Named shot profiles (Now-tab quick-select chips + Tune-tab management list)
// ============================================================================
var profilesCache = [];
var activeProfileIndex = 0;
var lastProfilesJson = "";

function piSummary(p) {
  return p.preinfusion ? p.pulses + "× " + (p.on_ms / 1000) + "s/" + (p.off_ms / 1000) + "s" : "None";
}

function markActiveProfile() {
  var chips = document.querySelectorAll("#profile_chip_row .btn-preset");
  for (var i = 0; i < chips.length; i++) chips[i].classList.toggle("active", i === activeProfileIndex);
  var rows = document.querySelectorAll("#profile_list_body .profile-row");
  for (var j = 0; j < rows.length; j++) rows[j].classList.toggle("active", j === activeProfileIndex);
}

function renderProfileChips() {
  var row = document.getElementById("profile_chip_row");
  if (!row) return;
  row.textContent = "";
  profilesCache.forEach(function (p, i) {
    var btn = document.createElement("button");
    btn.type = "button";
    btn.className = "btn-preset" + (i === activeProfileIndex ? " active" : "");
    btn.textContent = p.name;
    btn.addEventListener("click", function () { applyProfile(i); });
    row.appendChild(btn);
  });
}

function renderProfileList() {
  var body = document.getElementById("profile_list_body");
  if (!body) return;
  body.textContent = "";
  profilesCache.forEach(function (p, i) {
    var row = document.createElement("div");
    row.className = "profile-row" + (i === activeProfileIndex ? " active" : "");
    var main = document.createElement("div");
    main.className = "profile-row-main";
    var nameEl = document.createElement("b");
    nameEl.textContent = p.name;
    var meta = document.createElement("span");
    var extra = p.pressure_enabled ? " · " + p.pressure_ramp_bar + " bar" : "";
    meta.textContent = p.temp.toFixed(1) + "°C · " + p.auto_stop_sec + "s · pre-infusion: " + piSummary(p) + extra;
    main.appendChild(nameEl);
    main.appendChild(meta);
    var actions = document.createElement("div");
    actions.className = "profile-row-actions";
    var editBtn = document.createElement("button");
    editBtn.type = "button";
    editBtn.className = "btn-chip-sm";
    editBtn.textContent = "Edit";
    editBtn.addEventListener("click", function () { editProfile(i); });
    var delBtn = document.createElement("button");
    delBtn.type = "button";
    delBtn.className = "btn-chip-sm danger";
    delBtn.textContent = "Delete";
    delBtn.addEventListener("click", function () { armDeleteProfile(delBtn, i); });
    actions.appendChild(editBtn);
    actions.appendChild(delBtn);
    row.appendChild(main);
    row.appendChild(actions);
    body.appendChild(row);
  });
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

function applyProfile(idx) {
  activeProfileIndex = idx;
  markActiveProfile();
  xhr("/update?profile_apply=" + idx, function () { lastProfilesJson = ""; fetchProfiles(); });
}

// Pump pattern preview - a step chart of pulses (from the form fields, live
// as you type) followed by a solid "extraction" block for the remainder of
// the shot. This is the same kind of curve-preview Gaggiuino/GaggiMate/
// Meticulous show for their (pressure/flow) profiles, scaled to what this
// project can actually show: on/off pump state, not a measured curve, since
// there's no pressure transducer or flow sensor here.
function drawProfilePreview() {
  var canvas = document.getElementById("profile_preview_chart");
  if (!canvas) return;
  var ctx = canvas.getContext("2d");
  var w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);

  var enabled = document.getElementById("input_profile_pi_enabled").checked;
  var pulses = parseInt(document.getElementById("input_profile_pi_pulses").value, 10) || 0;
  var onSec = parseFloat(document.getElementById("input_profile_pi_on").value) || 0;
  var offSec = parseFloat(document.getElementById("input_profile_pi_off").value) || 0;
  var autoStop = parseFloat(document.getElementById("input_profile_autostop").value) || 27;

  var piTotal = (enabled && pulses > 0) ? pulses * onSec + Math.max(0, pulses - 1) * offSec : 0;
  var extractionSec = Math.max(3, autoStop - piTotal); // always show a visible extraction block
  var totalSec = piTotal + extractionSec;

  var label = document.getElementById("profile_preview_label");
  if (label) label.textContent = (enabled && pulses > 0) ? piTotal.toFixed(1) + "s pre-infusion" : "No pre-infusion";

  var x = 0;
  var barY = 6, barH = h - 12;
  function seg(durSec, on) {
    var segW = (durSec / totalSec) * w;
    ctx.fillStyle = on ? "#d98c3f" : "#2a221c";
    ctx.fillRect(x, barY, Math.max(0, segW - 1), barH);
    x += segW;
  }
  if (enabled && pulses > 0) {
    for (var i = 0; i < pulses; i++) {
      seg(onSec, true);
      if (i < pulses - 1) seg(offSec, false);
    }
  }
  seg(extractionSec, true); // continuous extraction for the rest of the shot
}

function editProfile(idx) {
  var p = profilesCache[idx];
  if (!p) return;
  document.getElementById("input_profile_index").value = idx;
  document.getElementById("input_profile_name").value = p.name;
  document.getElementById("input_profile_temp").value = p.temp;
  document.getElementById("input_profile_autostop").value = p.auto_stop_sec;
  document.getElementById("input_profile_pi_enabled").checked = p.preinfusion;
  document.getElementById("input_profile_pi_pulses").value = p.pulses;
  document.getElementById("input_profile_pi_on").value = p.on_ms / 1000;
  document.getElementById("input_profile_pi_off").value = p.off_ms / 1000;
  document.getElementById("input_profile_press_enabled").checked = p.pressure_enabled;
  document.getElementById("input_profile_press_ramp_bar").value = p.pressure_ramp_bar;
  document.getElementById("input_profile_press_ramp_sec").value = p.pressure_ramp_ms / 1000;
  document.getElementById("input_profile_press_decline_enabled").checked = p.pressure_decline_enabled;
  document.getElementById("input_profile_press_decline_bar").value = p.pressure_decline_bar;
  document.getElementById("input_profile_press_decline_sec").value = p.pressure_decline_ms / 1000;
  document.getElementById("profile_form_title").textContent = "Edit \"" + p.name + "\"";
  document.getElementById("profile_form_submit").textContent = "Save Changes";
  document.getElementById("profile_editor_card").hidden = false;
  document.getElementById("profile_editor_card").scrollIntoView({ behavior: "smooth", block: "center" });
  drawProfilePreview();
}

function newProfileForm() {
  document.getElementById("input_profile_index").value = -1;
  document.getElementById("input_profile_name").value = "";
  document.getElementById("input_profile_temp").value = 93;
  document.getElementById("input_profile_autostop").value = 27;
  document.getElementById("input_profile_pi_enabled").checked = false;
  document.getElementById("input_profile_pi_pulses").value = 3;
  document.getElementById("input_profile_pi_on").value = 1;
  document.getElementById("input_profile_pi_off").value = 2;
  document.getElementById("input_profile_press_enabled").checked = false;
  document.getElementById("input_profile_press_ramp_bar").value = 9;
  document.getElementById("input_profile_press_ramp_sec").value = 20;
  document.getElementById("input_profile_press_decline_enabled").checked = false;
  document.getElementById("input_profile_press_decline_bar").value = 6;
  document.getElementById("input_profile_press_decline_sec").value = 8;
  document.getElementById("profile_form_title").textContent = "New Profile";
  document.getElementById("profile_form_submit").textContent = "Add Profile";
  document.getElementById("profile_editor_card").hidden = false;
  document.getElementById("profile_editor_card").scrollIntoView({ behavior: "smooth", block: "center" });
  drawProfilePreview();
}

function cancelProfileForm() {
  document.getElementById("profile_editor_card").hidden = true;
}

// Arm-then-confirm instead of a native confirm() popup - first click arms
// the button (relabels it, 4s window to change your mind), second click
// within that window actually deletes. Any other Delete button clicked
// while one is armed disarms it first, so only one can ever be armed.
var armedDeleteBtn = null, armedDeleteTimer = null;

function disarmDeleteProfile() {
  if (armedDeleteTimer) { clearTimeout(armedDeleteTimer); armedDeleteTimer = null; }
  if (armedDeleteBtn) { armedDeleteBtn.textContent = "Delete"; armedDeleteBtn.classList.remove("armed"); armedDeleteBtn = null; }
}

function armDeleteProfile(btn, idx) {
  if (profilesCache.length <= 1) { alert("Can't delete the last remaining profile."); return; }
  if (btn === armedDeleteBtn) {
    disarmDeleteProfile();
    xhr("/update?profile_delete=" + idx, function () { lastProfilesJson = ""; fetchProfiles(); });
    return;
  }
  disarmDeleteProfile();
  armedDeleteBtn = btn;
  btn.textContent = "Confirm?";
  btn.classList.add("armed");
  armedDeleteTimer = setTimeout(disarmDeleteProfile, 4000);
}

function submitProfileForm(ev) {
  ev.preventDefault();
  var q = "profile_save=1";
  q += "&profile_index=" + document.getElementById("input_profile_index").value;
  q += "&profile_name=" + encodeURIComponent(document.getElementById("input_profile_name").value);
  q += "&profile_temp=" + document.getElementById("input_profile_temp").value;
  q += "&profile_autostop=" + document.getElementById("input_profile_autostop").value;
  q += "&profile_pi_enabled=" + (document.getElementById("input_profile_pi_enabled").checked ? "1" : "0");
  q += "&profile_pi_pulses=" + document.getElementById("input_profile_pi_pulses").value;
  q += "&profile_pi_on_ms=" + Math.round(document.getElementById("input_profile_pi_on").value * 1000);
  q += "&profile_pi_off_ms=" + Math.round(document.getElementById("input_profile_pi_off").value * 1000);
  q += "&profile_press_enabled=" + (document.getElementById("input_profile_press_enabled").checked ? "1" : "0");
  q += "&profile_press_ramp_bar=" + document.getElementById("input_profile_press_ramp_bar").value;
  q += "&profile_press_ramp_ms=" + Math.round(document.getElementById("input_profile_press_ramp_sec").value * 1000);
  q += "&profile_press_decline_enabled=" + (document.getElementById("input_profile_press_decline_enabled").checked ? "1" : "0");
  q += "&profile_press_decline_bar=" + document.getElementById("input_profile_press_decline_bar").value;
  q += "&profile_press_decline_ms=" + Math.round(document.getElementById("input_profile_press_decline_sec").value * 1000);

  // Inline save feedback (disable + label swap) instead of nothing happening
  // until the list silently refreshes - same "no toast library, just button
  // state" pattern GaggiMate's settings pages use.
  var btn = document.getElementById("profile_form_submit");
  var priorLabel = btn.textContent;
  btn.disabled = true;
  btn.textContent = "Saving…";
  xhr("/update?" + q, function (status) {
    lastProfilesJson = "";
    fetchProfiles();
    btn.textContent = status === 200 ? "Saved" : "Save failed";
    setTimeout(function () {
      btn.disabled = false;
      btn.textContent = priorLabel;
      if (status === 200) document.getElementById("profile_editor_card").hidden = true;
    }, 900);
  });
}

function setSchedEnabled(i, checked) {
  xhr("/update?sched" + i + "_en=" + (checked ? "1" : "0"));
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
  unsigned long snapShotAutoStopSec = shotAutoStopSec;
  AutotuneState snapAutotuneState = autotuneState;
  String snapAutotuneMessage = autotuneMessage;
  bool snapShotInProgress = shotInProgress;
  unsigned long snapShotStartMillis = shotStartMillis;
  unsigned long snapShotCount = shotCount;
  time_t snapLastDescaleTime = lastDescaleTime;
  unsigned long snapDescaleShotThreshold = descaleShotThreshold;
  unsigned long snapDescaleDayThreshold = descaleDayThreshold;
  int snapActiveProfileIndex = activeProfileIndex;
  bool snapPiEnabled = activePreinfusionEnabled;
  int snapPiPulses = activePreinfusionPulses, snapPiOnMs = activePreinfusionOnMs, snapPiOffMs = activePreinfusionOffMs;
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
  bool snapPressEnabled = activePressureEnabled;
  double snapPressRampBar = activePressureRampBar;
  unsigned long snapPressRampMs = activePressureRampMs;
  bool snapPressDeclineEnabled = activePressureDeclineEnabled;
  double snapPressDeclineBar = activePressureDeclineBar;
  unsigned long snapPressDeclineMs = activePressureDeclineMs;
  double snapPressKp = pressureKp, snapPressKi = pressureKi, snapPressKd = pressureKd;
  // Pump telemetry - lets a bench operator (Milestones A-C) distinguish
  // "pump idle" / "pump at 100% duty" / "PID commanding some percent" /
  // "safety ceiling just tripped it to 0%", most importantly the last one
  // now that Fix 1 makes an unwired-sensor fault the likeliest Milestone A
  // confusion point.
  float snapPumpPower = dimmerGetPowerPercent();
  // Temporary bring-up diagnostic (2026-09-15) - see dimmerGetZcCount()'s
  // declaration comment. Remove alongside it once bring-up is complete.
  uint32_t snapDimmerZcCount = dimmerGetZcCount();
  bool snapPressureCeilingTripped = (pressureFault || currentPressure > PUMP_MAX_SAFETY_BAR);
  int snapPressHistoryCount = pressureHistoryCount, snapPressHistoryHead = pressureHistoryHead;
  float snapPressHistory[TEMP_HISTORY_LEN];
  for (int i = 0; i < snapPressHistoryCount; i++) {
    int idx = (snapPressHistoryHead - snapPressHistoryCount + i + TEMP_HISTORY_LEN * 2) % TEMP_HISTORY_LEN;
    snapPressHistory[i] = pressureHistory[idx];
  }
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
  json += ",\"shot_auto_stop_sec\":" + String(snapShotAutoStopSec);

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

  json += ",\"active_profile\":" + String(snapActiveProfileIndex);
  json += ",\"pi_enabled\":" + String(snapPiEnabled ? "true" : "false");
  json += ",\"pi_pulses\":" + String(snapPiPulses);
  json += ",\"pi_on_ms\":" + String(snapPiOnMs);
  json += ",\"pi_off_ms\":" + String(snapPiOffMs);
  json += ",\"pressure\":" + String(snapPressure, 2);
  json += ",\"pressure_fault\":" + String(snapPressureFault ? "true" : "false");
  json += ",\"pump_power\":" + String(snapPumpPower, 1);
  json += ",\"dimmer_zc_count\":" + String(snapDimmerZcCount);
  json += ",\"pressure_ceiling_tripped\":" + String(snapPressureCeilingTripped ? "true" : "false");
  json += ",\"press_enabled\":" + String(snapPressEnabled ? "true" : "false");
  json += ",\"press_ramp_bar\":" + String(snapPressRampBar);
  json += ",\"press_ramp_ms\":" + String(snapPressRampMs);
  json += ",\"press_decline_enabled\":" + String(snapPressDeclineEnabled ? "true" : "false");
  json += ",\"press_decline_bar\":" + String(snapPressDeclineBar);
  json += ",\"press_decline_ms\":" + String(snapPressDeclineMs);
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

  noteActivity(); // any /update call is explicit user action - resets eco-sleep timer

  // Holds the whole handler body under stateMutex - see config.h
  // "Shared-state lock". This runs on the AsyncTCP task, a different task
  // than controlTick(), so every field this handler touches (mode,
  // setpoint, gains, shot state) needs the same lock controlTick() holds
  // during its own tick, for exactly the same reason.
  lockState();

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

  // Shot auto-stop - 0 (disabled) passes through untouched; any nonzero
  // value gets clamped so a typo can't set an unreasonably short/long
  // duration (same pattern as steam_max_safety's clamp above).
  if (hasArg("shot_auto_stop_sec")) {
    long v = arg("shot_auto_stop_sec").toInt();
    shotAutoStopSec = (v <= 0) ? 0 : constrain(v, SHOT_AUTO_STOP_SEC_MIN, SHOT_AUTO_STOP_SEC_MAX);
    preferences.putULong("shot_auto_stop", shotAutoStopSec);
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
    } else if (s == "stop") {
      stopShot();
    }
  }

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
    shotLogUpdateNotes(idx, bean, doseIn, grind, rating, notes);
  }

  // Named shot profiles (profiles.cpp) - three independent actions, same
  // split as everywhere else in this handler (editing vs. an explicit
  // apply action):
  //   profile_apply=<idx>       - load a saved profile into the live
  //                               settings (temp/auto-stop/pre-infusion)
  //   profile_save=1 (+ fields) - add (profile_index=-1) or overwrite an
  //                               existing saved profile
  //   profile_delete=<idx>      - remove a saved profile
  if (hasArg("profile_apply")) {
    applyProfile(arg("profile_apply").toInt());
  }
  if (hasArg("profile_save")) {
    int idx = hasArg("profile_index") ? arg("profile_index").toInt() : -1;
    String name = hasArg("profile_name") ? arg("profile_name") : "Profile";
    double temp = hasArg("profile_temp") ? arg("profile_temp").toDouble() : BREW_SETPOINT_DEFAULT;
    unsigned long autoStop = hasArg("profile_autostop")
        ? constrain((long)arg("profile_autostop").toInt(), (long)SHOT_AUTO_STOP_SEC_MIN, (long)SHOT_AUTO_STOP_SEC_MAX)
        : SHOT_AUTO_STOP_SEC_DEFAULT;
    bool piEnabled = hasArg("profile_pi_enabled") && arg("profile_pi_enabled") == "1";
    int pulses = hasArg("profile_pi_pulses")
        ? constrain(arg("profile_pi_pulses").toInt(), 0, PREINFUSION_PULSES_MAX) : 0;
    int onMs = hasArg("profile_pi_on_ms")
        ? constrain(arg("profile_pi_on_ms").toInt(), PREINFUSION_PULSE_MS_MIN, PREINFUSION_PULSE_MS_MAX) : PREINFUSION_ON_MS_DEFAULT;
    int offMs = hasArg("profile_pi_off_ms")
        ? constrain(arg("profile_pi_off_ms").toInt(), PREINFUSION_PULSE_MS_MIN, PREINFUSION_PULSE_MS_MAX) : PREINFUSION_OFF_MS_DEFAULT;
    bool pressureEnabled = hasArg("profile_press_enabled") && arg("profile_press_enabled") == "1";
    // Clamped the same way the neighboring fields above are (see
    // steam_max_safety's comment elsewhere in this file) - a typo in this
    // field shouldn't be able to command the PID to push toward a
    // dangerously high ceiling.
    double pressureRampBar = hasArg("profile_press_ramp_bar")
        ? constrain(arg("profile_press_ramp_bar").toDouble(), 0.0, PUMP_MAX_SAFETY_BAR - 1.0)
        : PRESSURE_RAMP_BAR_DEFAULT;
    unsigned long pressureRampMs = hasArg("profile_press_ramp_ms")
        ? (unsigned long)constrain((long)arg("profile_press_ramp_ms").toInt(), 0L, 120000L)
        : PRESSURE_RAMP_MS_DEFAULT;
    bool pressureDeclineEnabled = hasArg("profile_press_decline_enabled") && arg("profile_press_decline_enabled") == "1";
    double pressureDeclineBar = hasArg("profile_press_decline_bar")
        ? constrain(arg("profile_press_decline_bar").toDouble(), 0.0, PUMP_MAX_SAFETY_BAR - 1.0)
        : PRESSURE_DECLINE_BAR_DEFAULT;
    unsigned long pressureDeclineMs = hasArg("profile_press_decline_ms")
        ? (unsigned long)constrain((long)arg("profile_press_decline_ms").toInt(), 0L, 120000L)
        : PRESSURE_DECLINE_MS_DEFAULT;
    int saved = profileSave(idx, name, temp, autoStop, piEnabled, pulses, onMs, offMs,
                             pressureEnabled, pressureRampBar, pressureRampMs,
                             pressureDeclineEnabled, pressureDeclineBar, pressureDeclineMs);
    // Editing the profile that's currently active also refreshes the live
    // settings from it, so tweaking "your current setup" takes effect
    // immediately instead of silently drifting from what's now saved.
    if (saved >= 0 && saved == activeProfileIndex) applyProfile(saved);
  }
  if (hasArg("profile_delete")) {
    profileDelete(arg("profile_delete").toInt());
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

  request->redirect("/");

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

  // Named shot profiles (temp + auto-stop + pre-infusion pattern) - see
  // profiles.cpp. List only; add/edit/delete/apply all go through /update
  // like every other setting, for the same reason /settings_export reuses
  // it for restore - one code path, not two.
  server.on("/profiles", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", profilesReadJson());
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
    q += "&shot_auto_stop_sec=" + String(shotAutoStopSec);
    q += "&eco_timeout_min=" + String(ecoTimeoutMin);
    q += "&steam_auto_off_min=" + String(steamAutoOffMin);
    q += "&descale_shot_threshold=" + String(descaleShotThreshold);
    q += "&descale_day_threshold=" + String(descaleDayThreshold);
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
