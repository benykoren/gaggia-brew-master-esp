#include "config.h"
#include <Arduino.h>
#include <PID_v1.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <time.h>

#include "shot_log.h"

extern float currentTemperature;
extern bool sensorFault;
extern double Setpoint, Input, Output;
extern PID myPID;
extern float tempHistory[];
extern int tempHistoryHead;
extern int tempHistoryCount;

extern OpMode currentMode;
extern double brewSetpoint, brewKp, brewKi, brewKd;
extern double brewActiveKp, brewActiveKi, brewActiveKd;
extern double steamSetpoint, steamKp, steamKi, steamKd;
extern double steamMaxSafety;
extern void setOpMode(OpMode mode);
extern void refreshActiveProfileIfChanged();

extern unsigned long ecoTimeoutMin;
extern bool autoSleeping;
extern void noteActivity();
extern void wakeFromSleep();

extern AutotuneState autotuneState;
extern String autotuneMessage;
extern void startAutotune(OpMode forMode);
extern void stopAutotune();

extern bool shotInProgress;
extern unsigned long shotStartMillis;
extern void startShot();
extern void stopShot();

extern unsigned long shotCount;
extern time_t lastDescaleTime;
extern unsigned long descaleShotThreshold;
extern unsigned long descaleDayThreshold;
extern void markDescaled();

WebServer server(80);

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
      --border: #392e26;
      --border-soft: #2a221c;
      --text: #f6efe6;
      --text-dim: #b3a294;
      --text-faint: #7d7166;

      --copper: #d98c3f;
      --copper-deep: #b06a2c;
      --copper-light: #f2b46f;
      --steam: #4fa3d8;
      --green: #48b583;
      --red: #e5544b;
      --amber: #e0a13a;

      --radius-sm: 10px;
      --radius-md: 16px;
      --radius-lg: 22px;
      --radius-full: 999px;

      --shadow-sm: 0 2px 10px rgba(0,0,0,.35);
      --shadow-md: 0 10px 28px rgba(0,0,0,.45);

      --sp-1: 4px; --sp-2: 8px; --sp-3: 12px; --sp-4: 16px; --sp-5: 24px; --sp-6: 32px;
      --ease: cubic-bezier(.22,.7,.32,1);
    }
    * { box-sizing: border-box; }
    html, body { margin: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      background: var(--bg-grad), var(--bg);
      color: var(--text);
      min-height: 100vh;
      -webkit-font-smoothing: antialiased;
    }
    .app { max-width: 720px; margin: 0 auto; padding: var(--sp-5) var(--sp-4) calc(96px + env(safe-area-inset-bottom)); }

    .topbar {
      position: sticky; top: 0; z-index: 20;
      display: flex; align-items: center; justify-content: space-between; gap: var(--sp-3);
      margin: calc(var(--sp-5) * -1) calc(var(--sp-4) * -1) var(--sp-4);
      padding: var(--sp-5) var(--sp-4) var(--sp-4);
      background: linear-gradient(180deg, rgba(14,11,9,.97), rgba(14,11,9,.85) 70%, rgba(14,11,9,0));
      backdrop-filter: blur(8px);
    }
    .brand { display: flex; align-items: center; gap: var(--sp-3); }
    .logo {
      width: 42px; height: 42px; border-radius: var(--radius-md); flex: none;
      background: linear-gradient(135deg, var(--copper-light), var(--copper-deep));
      display: grid; place-items: center; font-size: 22px; box-shadow: var(--shadow-sm);
    }
    .brand-text h1 { font-size: 17px; margin: 0; letter-spacing: .2px; }
    .brand-text small { display: block; color: var(--text-dim); font-size: 11.5px; font-weight: 500; }
    .brand-status { text-align: right; }

    .pill {
      display: inline-flex; align-items: center; gap: var(--sp-2);
      padding: 7px 13px; border-radius: var(--radius-full); font-size: 12.5px; font-weight: 700;
      background: var(--surface); border: 1px solid var(--border);
    }
    .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--text-faint); }
    .pill.brew .dot { background: var(--green); box-shadow: 0 0 0 4px rgba(72,181,131,.18); animation: pulse-dot 2s var(--ease) infinite; }
    .pill.steam .dot { background: var(--steam); box-shadow: 0 0 0 4px rgba(79,163,216,.18); animation: pulse-dot 2s var(--ease) infinite; }
    .pill.fault { border-color: rgba(229,84,75,.5); }
    .pill.fault .dot { background: var(--red); box-shadow: 0 0 0 4px rgba(229,84,75,.2); animation: pulse-dot 1.1s var(--ease) infinite; }
    @keyframes pulse-dot { 0%,100% { opacity: 1 } 50% { opacity: .45 } }
    .last-updated { font-size: 10.5px; color: var(--text-dim); margin-top: 5px; }

    .alerts { display: flex; flex-direction: column; gap: var(--sp-2); margin-bottom: var(--sp-4); }
    .banner {
      display: none; align-items: center; justify-content: space-between; gap: var(--sp-3);
      padding: 11px 15px; border-radius: var(--radius-md); font-size: 13px; font-weight: 700;
    }
    .banner-error { background: rgba(229,84,75,.14); border: 1px solid rgba(229,84,75,.4); color: #f3c6c2; }
    .banner-info { background: rgba(79,163,216,.14); border: 1px solid rgba(79,163,216,.4); color: #cfe8f7; }
    .banner-warn { background: rgba(224,161,58,.14); border: 1px solid rgba(224,161,58,.4); color: #f3dcc2; }
    .btn-chip {
      border: 1px solid var(--steam); background: var(--steam); color: #071824;
      padding: 6px 13px; border-radius: var(--radius-sm); font-size: 12px; font-weight: 700; cursor: pointer; flex: none;
    }

    .card {
      background: linear-gradient(180deg, var(--surface-2), var(--surface));
      border: 1px solid var(--border-soft); border-radius: var(--radius-lg);
      box-shadow: var(--shadow-md); padding: var(--sp-5); margin-bottom: var(--sp-4);
    }

    .hero-card { text-align: center; }
    .gauge-wrap { position: relative; width: clamp(210px, 62vw, 280px); height: clamp(210px, 62vw, 280px); margin: 0 auto var(--sp-3); }
    .gauge { width: 100%; height: 100%; transform: rotate(-90deg); }
    .gauge-track { fill: none; stroke: var(--border); stroke-width: 14; }
    .gauge-fill { fill: none; stroke: var(--text-faint); stroke-width: 14; stroke-linecap: round; transition: stroke-dashoffset .6s var(--ease), stroke .4s ease; }
    /* Ring color is a functional signal, not decoration: blue while still
       climbing to target, green once at/near it (readable without parsing
       the number - also covers a shot's temperature sag, which is just
       "below target" again), red once over. */
    .gauge-fill.heating { stroke: var(--steam); filter: drop-shadow(0 0 10px rgba(79,163,216,.55)); }
    .gauge-fill.ready { stroke: var(--green); filter: drop-shadow(0 0 10px rgba(72,181,131,.55)); }
    .gauge-fill.over { stroke: var(--red); filter: drop-shadow(0 0 10px rgba(229,84,75,.55)); }
    /* The number is the ONLY thing centered against this box, which exactly
       covers the ring (inset: 0 on .gauge-wrap) - so its flex centering
       lands on the ring's true geometric midpoint regardless of viewport
       size. The "Target XX C" caption used to live inside this same box,
       stacked under the number - that made flex/grid center the *pair* as a
       group instead of the number alone, which visibly dragged the number
       upward off the ring's actual center. It now lives outside, below the
       ring, in normal document flow (see .gauge-target). */
    .gauge-center { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; }
    .gauge-value { display: flex; align-items: baseline; justify-content: center; font-size: clamp(3.4rem, 14vw, 5.2rem); font-weight: 800; line-height: 1; letter-spacing: -2px; font-variant-numeric: tabular-nums; }
    .gauge-unit { font-size: clamp(1.1rem, 4vw, 1.4rem); color: var(--text-dim); font-weight: 600; margin-left: 3px; }
    .gauge-target { color: var(--text-dim); margin: 0 0 var(--sp-4); font-size: 14px; }
    .gauge-target b { color: var(--text); }

    .mode-switch { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: var(--sp-3); margin: var(--sp-4) 0; }
    .mode-btn {
      border: 1px solid var(--border); color: var(--text); background: var(--surface);
      padding: 15px 10px; border-radius: var(--radius-md); font-size: 15px; font-weight: 700;
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
    .stat-label { display: block; font-size: 11px; color: var(--text-dim); text-transform: uppercase; letter-spacing: .6px; font-weight: 700; margin-bottom: 6px; }
    .stat-value { font-size: 21px; font-weight: 800; font-variant-numeric: tabular-nums; }
    .stat-value small { font-size: 13px; color: var(--text-dim); font-weight: 600; margin-left: 2px; }

    .bar { height: 9px; border-radius: var(--radius-full); background: #100d0b; border: 1px solid var(--border-soft); overflow: hidden; margin-top: 9px; }
    .bar > span { display: block; height: 100%; width: 0%; border-radius: var(--radius-full); transition: width .5s ease; }
    .bar.heat > span { background: linear-gradient(90deg, var(--copper-deep), var(--copper)); }

    /* Generic chart card - reused today for the temp sparkline; a future
       pressure graph (roadmap item 7) is meant to drop in as a second
       .chart-card with its own canvas id, same label/legend pattern. */
    .chart-card { margin-top: var(--sp-4); text-align: left; }
    .chart-card canvas { width: 100%; height: 60px; display: block; }
    .chart-label { display: flex; justify-content: space-between; font-size: 11px; color: var(--text-dim); margin-bottom: 6px; }

    .shot-head { display: flex; align-items: center; justify-content: space-between; gap: var(--sp-4); }
    .shot-time { font-size: clamp(2.6rem, 13vw, 4rem); font-weight: 800; font-variant-numeric: tabular-nums; color: var(--text-dim); line-height: 1; transition: color .2s ease; }
    .shot-time.in-window { color: var(--green); }
    .shot-time.over { color: var(--red); }
    .shot-sub { font-size: 12px; color: var(--text-dim); margin-top: 4px; }
    .btn-shot {
      border: 1px solid rgba(72,181,131,.4); cursor: pointer; padding: 15px 22px; border-radius: var(--radius-md);
      font-size: 14px; font-weight: 700; color: #cdeede; background: rgba(72,181,131,.14); flex: none;
      transition: transform .05s ease, background .15s ease;
    }
    .btn-shot:active { transform: translateY(1px); }
    .btn-shot.running { background: var(--red); border-color: var(--red); color: #1a0d0c; }
    .shot-progress { height: 7px; border-radius: var(--radius-full); background: #100d0b; border: 1px solid var(--border-soft); overflow: hidden; margin-top: var(--sp-4); }
    .shot-progress > span { display: block; height: 100%; width: 0%; border-radius: var(--radius-full); background: var(--text-faint); transition: width .4s linear, background .3s ease; }
    .shot-progress > span.in-window { background: var(--green); }
    .shot-progress > span.over { background: var(--red); }

    .btn-autotune {
      width: 100%; border: none; cursor: pointer; display: flex; align-items: center; justify-content: center; gap: var(--sp-2);
      padding: 15px 16px; border-radius: var(--radius-md); font-size: 14.5px; font-weight: 700; color: #1a1206;
      background: linear-gradient(135deg, var(--copper-light), var(--copper-deep));
      box-shadow: 0 6px 18px rgba(217,140,63,.28); transition: transform .05s ease, opacity .15s ease;
    }
    .btn-autotune:active:not(:disabled) { transform: translateY(1px); }
    .btn-autotune:disabled { opacity: .4; cursor: default; box-shadow: none; }
    .btn-autotune.running { background: var(--red); color: #1a0d0c; box-shadow: 0 6px 18px rgba(229,84,75,.32); }
    .autotune-status { display: block; margin-top: var(--sp-3); font-size: 12.5px; color: var(--text-dim); text-align: center; }

    .tab-section-title { font-size: 12px; font-weight: 700; color: var(--text-dim); text-transform: uppercase; letter-spacing: 1px; margin-bottom: var(--sp-3); }
    .tab-section-title.mt { margin-top: var(--sp-5); }
    .hint { font-size: 12px; color: var(--text-dim); margin: 0 0 var(--sp-3); line-height: 1.5; }
    .hint-link { display: inline-block; margin-top: var(--sp-3); font-size: 13px; color: var(--copper); text-decoration: none; font-weight: 600; }

    label { display: block; font-size: 12px; color: var(--text-dim); margin-bottom: 6px; font-weight: 600; }
    .field { margin-bottom: var(--sp-3); }
    input {
      width: 100%; padding: 12px 13px; border-radius: var(--radius-sm); font-size: 15px;
      background: #100d0b; border: 1px solid var(--border); color: var(--text);
      outline: none; transition: border-color .15s ease, box-shadow .15s ease;
    }
    input:focus { border-color: var(--copper); box-shadow: 0 0 0 3px rgba(217,140,63,.18); }
    .field-row { display: grid; grid-template-columns: 1fr 1fr; gap: var(--sp-3); }
    .field-row-3 { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: var(--sp-3); }

    .submit, .btn-secondary, .btn-danger { width: 100%; border: none; cursor: pointer; padding: 14px; border-radius: var(--radius-md); font-size: 14.5px; font-weight: 700; margin-top: var(--sp-1); }
    .submit { color: #1a1206; background: linear-gradient(135deg, var(--copper-light), var(--copper-deep)); box-shadow: 0 6px 18px rgba(217,140,63,.28); }
    .submit:active { transform: translateY(1px); }
    .btn-secondary { color: var(--text); background: var(--surface); border: 1px solid var(--border); margin-top: var(--sp-4); }
    .btn-danger { color: #1a0d0c; background: var(--red); }

    table.history { width: 100%; border-collapse: collapse; font-size: 13px; }
    table.history th, table.history td { text-align: left; padding: 9px 6px; border-bottom: 1px solid var(--border-soft); }
    table.history th { color: var(--text-dim); font-weight: 600; font-size: 10.5px; text-transform: uppercase; letter-spacing: .5px; }
    table.history td.num { text-align: right; font-variant-numeric: tabular-nums; }
    .table-scroll { overflow-x: auto; }
    .empty-hint { color: var(--text-dim); font-size: 13px; padding: 6px 0; }
    .metric-row { display: flex; align-items: center; justify-content: space-between; margin: 0 0 var(--sp-3); font-size: 14px; color: var(--text-dim); }
    .metric-row b { color: var(--text); font-size: 15px; }

    .footer { text-align: center; color: var(--text-dim); font-size: 11.5px; margin: var(--sp-5) 0 var(--sp-3); }
    .footer a { color: var(--copper); text-decoration: none; }

    .view[hidden] { display: none; }

    .tabbar {
      position: fixed; left: 0; right: 0; bottom: 0; z-index: 30;
      display: flex; justify-content: center; gap: var(--sp-2);
      padding: var(--sp-2) var(--sp-3) calc(var(--sp-2) + env(safe-area-inset-bottom));
      background: linear-gradient(0deg, rgba(14,11,9,.97), rgba(14,11,9,.9) 75%, rgba(14,11,9,0));
      backdrop-filter: blur(10px);
    }
    .tab {
      flex: 1; max-width: 200px; display: flex; flex-direction: column; align-items: center; gap: 3px;
      border: none; background: none; color: var(--text-faint); font-size: 10.5px; font-weight: 700;
      padding: 8px 4px 6px; cursor: pointer; border-radius: var(--radius-md); transition: color .15s ease;
    }
    .tab-icon { font-size: 19px; }
    .tab.active { color: var(--copper-light); }
  </style>
</head>
<body>
  <div class="app">
    <header class="topbar">
      <div class="brand">
        <div class="logo">&#9749;</div>
        <div class="brand-text">
          <h1>BrewMaster</h1>
          <small>Gaggia Espresso Color</small>
        </div>
      </div>
      <div class="brand-status">
        <div id="status_pill" class="pill off"><span class="dot"></span><span id="mode_status">--</span></div>
        <div id="last_updated" class="last-updated">--</div>
      </div>
    </header>

    <div class="alerts">
      <div id="fault_banner" class="banner banner-error">&#9888; Sensor fault &mdash; check wiring</div>
      <div id="sleep_banner" class="banner banner-info">
        <span>&#9866; Asleep (eco timeout) &mdash; heater off</span>
        <button onclick="wake()" class="btn-chip">Wake Up</button>
      </div>
      <div id="descale_banner_top" class="banner banner-warn">&#9888; Descale recommended &mdash; see History tab</div>
    </div>

    <main class="view" data-view="now">
      <div class="card hero-card">
        <div class="gauge-wrap">
          <svg class="gauge" viewBox="0 0 220 220">
            <circle class="gauge-track" cx="110" cy="110" r="96"></circle>
            <circle id="temp_ring_fill" class="gauge-fill" cx="110" cy="110" r="96"
                    stroke-dasharray="603" stroke-dashoffset="603"></circle>
          </svg>
          <div class="gauge-center">
            <div class="gauge-value"><span id="temp">--</span><span class="gauge-unit">&deg;C</span></div>
          </div>
        </div>
        <div class="gauge-target">Target <b><span id="target">--</span>&deg;C</b></div>

        <div class="mode-switch" role="group" aria-label="Mode">
          <button onclick="setMode('off')" id="btn_off" class="mode-btn mode-off">Off</button>
          <button onclick="setMode('brew')" id="btn_brew" class="mode-btn mode-brew">Brew</button>
          <button onclick="setMode('steam')" id="btn_steam" class="mode-btn mode-steam">Steam</button>
        </div>
      </div>

      <!-- Start/Stop Shot sits directly under Mode - the most-tapped control
           during an actual pull, so it stays within reach of the top of the
           page (temp -> mode -> shot) without scrolling past the secondary
           output/chart telemetry below. -->
      <div class="card">
        <div class="shot-head">
          <div>
            <div id="shot_time" class="shot-time">0:00</div>
            <div class="shot-sub">Target window 25&ndash;30s</div>
          </div>
          <button onclick="toggleShot()" id="btn_shot" class="btn-shot">Start Shot</button>
        </div>
        <div class="shot-progress"><span id="shot_progress_bar"></span></div>
      </div>

      <div class="card">
        <div class="stat-grid">
          <div class="stat-tile">
            <span class="stat-label">Heater Output</span>
            <span class="stat-value"><span id="output">--</span><small>%</small></span>
            <div class="bar heat"><span id="output_bar"></span></div>
          </div>
        </div>

        <div class="chart-card">
          <div class="chart-label"><span>Temp &middot; last 2 min</span><span>&deg;C</span></div>
          <canvas id="temp_chart" width="300" height="60"></canvas>
        </div>
      </div>

      <div class="card">
        <button onclick="startAutotune()" id="btn_autotune" class="btn-autotune">&#9889; Start Auto-Tune</button>
        <span id="autotune_status" class="autotune-status"></span>
      </div>
    </main>

    <main class="view" data-view="tune" hidden>
      <div class="card">
        <div class="tab-section-title">Brew</div>
        <form action="/update" method="GET">
          <div class="field">
            <label for="input_brew_target">Target temperature (&deg;C)</label>
            <input type="number" step="0.1" name="brew_target" id="input_brew_target" value="">
          </div>
          <div class="field-row-3">
            <div class="field"><label for="input_brew_kp">Kp</label><input type="number" step="any" name="brew_kp" id="input_brew_kp" value=""></div>
            <div class="field"><label for="input_brew_ki">Ki</label><input type="number" step="any" name="brew_ki" id="input_brew_ki" value=""></div>
            <div class="field"><label for="input_brew_kd">Kd</label><input type="number" step="any" name="brew_kd" id="input_brew_kd" value=""></div>
          </div>
          <button type="submit" class="submit">Save Brew</button>
        </form>

        <div class="tab-section-title mt">Brew &mdash; active during a shot</div>
        <p class="hint">Switches in automatically the instant a shot starts (Start Shot), reverts to the values above the instant it stops. Deliberately more aggressive &mdash; fights the temperature drop from real flow, which the gentle gains above are too slow for.</p>
        <form action="/update" method="GET">
          <div class="field-row-3">
            <div class="field"><label for="input_brew_akp">Kp</label><input type="number" step="any" name="brew_akp" id="input_brew_akp" value=""></div>
            <div class="field"><label for="input_brew_aki">Ki</label><input type="number" step="any" name="brew_aki" id="input_brew_aki" value=""></div>
            <div class="field"><label for="input_brew_akd">Kd</label><input type="number" step="any" name="brew_akd" id="input_brew_akd" value=""></div>
          </div>
          <button type="submit" class="submit">Save Active-Brew Gains</button>
        </form>
      </div>

      <div class="card">
        <div class="tab-section-title">Steam</div>
        <form action="/update" method="GET">
          <div class="field">
            <label for="input_steam_target">Target temperature (&deg;C)</label>
            <input type="number" step="0.1" name="steam_target" id="input_steam_target" value="">
          </div>
          <div class="field-row-3">
            <div class="field"><label for="input_steam_kp">Kp</label><input type="number" step="any" name="steam_kp" id="input_steam_kp" value=""></div>
            <div class="field"><label for="input_steam_ki">Ki</label><input type="number" step="any" name="steam_ki" id="input_steam_ki" value=""></div>
            <div class="field"><label for="input_steam_kd">Kd</label><input type="number" step="any" name="steam_kd" id="input_steam_kd" value=""></div>
          </div>
          <div class="field">
            <label for="input_steam_max_safety">Max safety ceiling (&deg;C)</label>
            <input type="number" step="1" min="100" max="150" name="steam_max_safety" id="input_steam_max_safety" value="">
          </div>
          <button type="submit" class="submit">Save Steam</button>
        </form>
      </div>
    </main>

    <main class="view" data-view="history" hidden>
      <div class="card">
        <div class="tab-section-title">Shot History</div>
        <div id="shot_history_empty" class="empty-hint">No shots logged yet.</div>
        <div class="table-scroll">
          <table class="history" id="shot_history_table" style="display:none">
            <thead><tr><th>When</th><th>Duration</th><th>Peak &deg;C</th><th>Weight</th></tr></thead>
            <tbody id="shot_history_body"></tbody>
          </table>
        </div>
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
        <button onclick="markDescaled()" class="btn-secondary">Mark Descaled Today</button>
      </div>
    </main>

    <main class="view" data-view="settings" hidden>
      <div class="card">
        <div class="tab-section-title">Power &amp; Eco</div>
        <form action="/update" method="GET">
          <div class="field">
            <label for="input_eco_min">Auto-sleep after (minutes, 0 = disabled)</label>
            <input type="number" step="1" min="0" name="eco_timeout_min" id="input_eco_min" value="">
          </div>
          <button type="submit" class="submit">Save</button>
        </form>
        <p class="hint" style="margin-top:var(--sp-3)">Heater force-OFF after this long with no Web UI activity (mode/tuning changes). Does not count passive status polling.</p>
      </div>

      <div class="card">
        <div class="tab-section-title">Network</div>
        <button onclick="wifiReset()" class="btn-danger">Reset WiFi Settings</button>
        <p class="hint" style="margin-top:var(--sp-3)">Reboots into the <b>GaggiaBrewMasterESP_Setup</b> setup network so you can join a different WiFi without reflashing.</p>
        <a href="/firmware" class="hint-link">Firmware update (OTA) &rarr;</a>
      </div>

      <div class="card">
        <div class="tab-section-title">MQTT / Home Assistant</div>
        <form action="/update" method="GET">
          <div class="field-row">
            <div class="field"><label for="input_mqtt_server">Server</label><input type="text" name="mqtt_server" id="input_mqtt_server" placeholder="192.168.1.100"></div>
            <div class="field"><label for="input_mqtt_port">Port</label><input type="number" name="mqtt_port" id="input_mqtt_port" value="1883"></div>
          </div>
          <div class="field-row">
            <div class="field"><label for="input_mqtt_user">User</label><input type="text" name="mqtt_user" id="input_mqtt_user"></div>
            <div class="field"><label for="input_mqtt_pass">Password</label><input type="password" name="mqtt_pass" id="input_mqtt_pass"></div>
          </div>
          <button type="submit" class="submit">Save &amp; Restart</button>
        </form>
        <p class="hint" style="margin-top:var(--sp-3)">Saving MQTT settings reboots the controller.</p>
      </div>
    </main>

    <footer class="footer">
      <a href="/firmware">Firmware update (OTA)</a> &middot; <span id="host">gaggia.local</span> &middot; build <span id="fw_build">--</span>
    </footer>
  </div>

  <nav class="tabbar">
    <button class="tab active" data-tab="now"><span class="tab-icon">&#9749;</span><span>Now</span></button>
    <button class="tab" data-tab="tune"><span class="tab-icon">&#9881;</span><span>Tune</span></button>
    <button class="tab" data-tab="history"><span class="tab-icon">&#8987;</span><span>History</span></button>
    <button class="tab" data-tab="settings"><span class="tab-icon">&#9776;</span><span>Settings</span></button>
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

// Tabs - hash-addressable (#now/#tune/#history/#settings) so a reload keeps
// whichever view was open; all four share the single /status poll below.
function showTab(name) {
  var valid = ["now", "tune", "history", "settings"];
  if (valid.indexOf(name) === -1) name = "now";
  var views = document.querySelectorAll(".view");
  for (var i = 0; i < views.length; i++) views[i].hidden = views[i].dataset.view !== name;
  var tabs = document.querySelectorAll(".tab");
  for (var j = 0; j < tabs.length; j++) tabs[j].classList.toggle("active", tabs[j].dataset.tab === name);
  history.replaceState(null, "", "#" + name);
}
(function () {
  var tabs = document.querySelectorAll(".tab");
  for (var i = 0; i < tabs.length; i++) {
    tabs[i].addEventListener("click", (function (t) { return function () { showTab(t.dataset.tab); }; })(tabs[i]));
  }
})();
showTab(location.hash.slice(1));

function drawSparkline(data) {
  var canvas = document.getElementById("temp_chart");
  if (!canvas || !data || data.length < 2) return;
  var ctx = canvas.getContext("2d");
  var w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  var min = Math.min.apply(null, data), max = Math.max.apply(null, data);
  if (max - min < 1) { max += 0.5; min -= 0.5; }
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

function formatElapsed(ms) {
  var s = Math.floor(ms / 1000);
  var m = Math.floor(s / 60);
  s = s % 60;
  return m + ":" + (s < 10 ? "0" : "") + s;
}

var lastUpdateTime = null;
var lastAutotuneState = null; // tracks transitions, so the force-refresh below fires once

// Shot timer: shotRunning/shotElapsedBaseMs are refreshed from each /status
// poll; the 1s ticker below interpolates smoothly between polls rather than
// only updating every 2s.
var shotRunning = false;
var shotElapsedBaseMs = 0;
var shotElapsedCapturedAt = null;
var prevShotRunning = false; // detects the stop transition, to refresh history once

setInterval(function () {
  var el = document.getElementById("shot_time");
  if (!el) return;
  var ms = shotElapsedBaseMs;
  if (shotRunning && shotElapsedCapturedAt !== null) {
    ms += Date.now() - shotElapsedCapturedAt;
  }
  el.textContent = formatElapsed(ms);
  var secs = ms / 1000;
  var inWindow = shotRunning && secs >= 25 && secs <= 30;
  var over = shotRunning && secs > 30;
  el.classList.toggle("in-window", inWindow);
  el.classList.toggle("over", over);
  var pbar = document.getElementById("shot_progress_bar");
  if (pbar) {
    pbar.style.width = clamp((secs / 30) * 100) + "%";
    pbar.classList.toggle("in-window", inWindow);
    pbar.classList.toggle("over", over);
  }
}, 1000);

function fetchShotHistory() {
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function () {
    if (this.readyState == 4 && this.status == 200) {
      var shots = JSON.parse(this.responseText);
      var body = document.getElementById("shot_history_body");
      var table = document.getElementById("shot_history_table");
      var empty = document.getElementById("shot_history_empty");
      if (!shots.length) {
        table.style.display = "none";
        empty.style.display = "block";
        return;
      }
      empty.style.display = "none";
      table.style.display = "table";
      body.innerHTML = "";
      shots.slice().reverse().slice(0, 15).forEach(function (s) {
        var tr = document.createElement("tr");
        var when = new Date(s.ts * 1000).toLocaleString([], { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });
        var dur = formatElapsed(s.duration_ms);
        var weight = s.weight > 0 ? s.weight.toFixed(1) + "g" : "&mdash;";
        tr.innerHTML = "<td>" + when + "</td><td class='num'>" + dur + "</td><td class='num'>" + s.peak_temp.toFixed(1) + "</td><td class='num'>" + weight + "</td>";
        body.appendChild(tr);
      });
    }
  };
  xhttp.open("GET", "/shots", true);
  xhttp.send();
}
fetchShotHistory();
setInterval(function () {
  var el = document.getElementById("last_updated");
  if (!el) return;
  if (lastUpdateTime === null) { el.textContent = "--"; return; }
  var secs = Math.round((Date.now() - lastUpdateTime) / 1000);
  el.textContent = secs + "s ago";
  el.style.color = secs > 6 ? "var(--red)" : "var(--text-dim)";
}, 1000);

setInterval(function () {
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      lastUpdateTime = Date.now();
      var json = JSON.parse(this.responseText);
      var temp = json.temp, target = json.target, output = json.output;
      // Output is on a 0-1000 scale (ms within the 1000ms SSR window) -
      // divide by 10 to get an actual 0-100% duty cycle for display.
      var outputPct = output / 10;
      // "off" means no target at all, full stop - regardless of what the
      // backend's Setpoint variable happens to still hold. setOpMode(OFF)
      // stops the heater but never resets Setpoint, so `target` in the JSON
      // is stale leftover from the last active profile, not a real target -
      // gate every target-driven display on the mode itself, not just
      // whether that stale value happens to be nonzero.
      var mode = json.opmode; // "off" | "brew" | "steam"
      var hasTarget = mode !== "off" && target > 0;

      document.getElementById("fault_banner").style.display = json.fault ? "flex" : "none";
      document.getElementById("temp").innerHTML = json.fault ? "--" : temp.toFixed(1);
      document.getElementById("target").innerHTML = hasTarget ? target.toFixed(1) : "--";
      document.getElementById("output").innerHTML = outputPct.toFixed(0);
      drawSparkline(json.history);

      // Temperature ring - fill amount reuses the same ratio the linear bar
      // used before; color is the functional "heating / ready / over" signal,
      // readable at a glance without parsing the number.
      var tempPct = (temp > 0 && hasTarget) ? clamp((temp / target) * 100) : 0;
      var ring = document.getElementById("temp_ring_fill");
      var RING_CIRCUMFERENCE = 603; // 2*pi*96, matches the SVG circle's r=96
      ring.style.strokeDashoffset = RING_CIRCUMFERENCE * (1 - tempPct / 100);
      ring.classList.remove("heating", "ready", "over");
      if (!json.fault && temp > 0 && hasTarget) {
        var READY_MARGIN_C = 1.0;
        if (temp < target - READY_MARGIN_C) ring.classList.add("heating");
        else if (temp > target + READY_MARGIN_C) ring.classList.add("over");
        else ring.classList.add("ready");
      }

      document.getElementById("output_bar").style.width = clamp(outputPct) + "%";

      // Status
      var pill = document.getElementById("status_pill");
      var label = json.fault ? "Fault" : mode === "brew" ? "Brewing" : mode === "steam" ? "Steaming" : "Off";
      document.getElementById("mode_status").innerHTML = label;
      pill.className = "pill " + mode + (json.fault ? " fault" : "");
      document.getElementById("btn_off").classList.toggle("active", mode === "off");
      document.getElementById("btn_brew").classList.toggle("active", mode === "brew");
      document.getElementById("btn_steam").classList.toggle("active", mode === "steam");

      // Sync form fields (once, unless user is editing)
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
      setVal("input_mqtt_server", json.mqtt_server || "");
      setVal("input_mqtt_port", json.mqtt_port);
      setVal("input_mqtt_user", json.mqtt_user || "");
      setVal("input_mqtt_pass", json.mqtt_pass || "");
      setVal("input_eco_min", json.eco_timeout_min);

      // Eco / auto-sleep banner
      document.getElementById("sleep_banner").style.display = json.auto_sleeping ? "flex" : "none";

      // Autotune status
      var atBtn = document.getElementById("btn_autotune");
      var atStatus = document.getElementById("autotune_status");
      if (json.autotune_state === "running") {
        atBtn.disabled = false;
        atBtn.textContent = "⏹ Stop Auto-Tune";
        atBtn.onclick = stopAutotune;
        atBtn.classList.add("running");
        atStatus.textContent = "Autotuning... " + (json.autotune_message || "");
      } else {
        atBtn.disabled = (mode === "off");
        atBtn.textContent = "⚡ Start Auto-Tune";
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

      // The moment autotune finishes successfully, force the tuned profile's
      // Kp/Ki/Kd input boxes to actually show the new numbers - setVal()'s
      // "sync once" guard (so a field being actively edited isn't clobbered)
      // would otherwise leave them showing the stale pre-autotune values,
      // even though the status message above already prints the new ones.
      if (json.autotune_state === "done_ok" && lastAutotuneState !== "done_ok") {
        var p = (mode === "steam") ? "steam" : "brew"; // autotune always runs against the currently-active mode
        document.getElementById("input_" + p + "_kp").value = json[p + "_kp"];
        document.getElementById("input_" + p + "_ki").value = json[p + "_ki"];
        document.getElementById("input_" + p + "_kd").value = json[p + "_kd"];
      }
      lastAutotuneState = json.autotune_state;

      // Shot timer
      shotRunning = json.shot_in_progress;
      shotElapsedBaseMs = json.shot_elapsed_ms;
      shotElapsedCapturedAt = Date.now();
      var btnShot = document.getElementById("btn_shot");
      btnShot.textContent = shotRunning ? "Stop Shot" : "Start Shot";
      btnShot.classList.toggle("running", shotRunning);
      if (prevShotRunning && !shotRunning) fetchShotHistory(); // shot just ended
      prevShotRunning = shotRunning;

      // Maintenance / descale
      setVal("input_descale_shots", json.descale_shot_threshold);
      setVal("input_descale_days", json.descale_day_threshold);
      document.getElementById("descale_shots").textContent = json.shot_count;
      document.getElementById("descale_days").textContent = json.days_since_descale >= 0 ? json.days_since_descale : "--";
      document.getElementById("descale_banner_top").style.display = json.descale_due ? "flex" : "none";

      document.getElementById("fw_build").textContent = json.fw_build;
    }
  };
  xhttp.open("GET", "/status", true);
  xhttp.send();
}, 2000);

function setMode(mode) {
  var xhttp = new XMLHttpRequest();
  xhttp.open("GET", "/update?mode=" + mode, true);
  xhttp.send();
}

function wake() {
  var xhttp = new XMLHttpRequest();
  xhttp.open("GET", "/update?wake=1", true);
  xhttp.send();
}

function startAutotune() {
  if (!confirm("Start PID auto-tune for the current mode? The heater will cycle on/off repeatedly for several minutes. Stay nearby.")) return;
  var xhttp = new XMLHttpRequest();
  xhttp.open("GET", "/update?autotune=start", true);
  xhttp.send();
}

function stopAutotune() {
  var xhttp = new XMLHttpRequest();
  xhttp.open("GET", "/update?autotune=stop", true);
  xhttp.send();
}

function toggleShot() {
  var xhttp = new XMLHttpRequest();
  xhttp.open("GET", "/update?shot=" + (shotRunning ? "stop" : "start"), true);
  xhttp.send();
}

function markDescaled() {
  if (!confirm("Reset the descale counters? Confirm you've just descaled the machine.")) return;
  var xhttp = new XMLHttpRequest();
  xhttp.open("GET", "/update?mark_descaled=1", true);
  xhttp.send();
}

function wifiReset() {
  if (!confirm("Reset WiFi settings and reboot? You'll need to rejoin the GaggiaBrewMasterESP_Setup network to reconfigure.")) return;
  var xhttp = new XMLHttpRequest();
  xhttp.open("GET", "/wifi_reset", true);
  xhttp.send();
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
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", index_html); });

  // PWA manifest + icon (add-to-home-screen support)
  server.on("/manifest.json", HTTP_GET, []() {
    server.send(200, "application/manifest+json", manifest_json);
  });
  server.on("/icon.svg", HTTP_GET,
            []() { server.send(200, "image/svg+xml", icon_svg); });

  // Status Handler
  server.on("/status", HTTP_GET, []() {
    String json = "{";
    json += "\"temp\":" + String(currentTemperature);
    json += ",\"target\":" + String(Setpoint);
    json += ",\"output\":" + String(Output);

    json += ",\"opmode\":\"";
    json += (currentMode == OpMode::BREW)    ? "brew"
             : (currentMode == OpMode::STEAM) ? "steam"
                                               : "off";
    json += "\"";

    // Kp/Ki/Kd get 4 decimal places, not String()'s default 2 - a value like
    // autotune's Ki=1.1782 would otherwise silently truncate to "1.18" here,
    // and re-saving without noticing would overwrite the real value with
    // the rounded one. Target/safety fields stay at the default (2 decimals
    // is already more precision than a human ever types for a temperature).
    json += ",\"brew_target\":" + String(brewSetpoint);
    json += ",\"brew_kp\":" + String(brewKp, 4);
    json += ",\"brew_ki\":" + String(brewKi, 4);
    json += ",\"brew_kd\":" + String(brewKd, 4);
    json += ",\"brew_akp\":" + String(brewActiveKp, 4);
    json += ",\"brew_aki\":" + String(brewActiveKi, 4);
    json += ",\"brew_akd\":" + String(brewActiveKd, 4);
    json += ",\"steam_target\":" + String(steamSetpoint);
    json += ",\"steam_kp\":" + String(steamKp, 4);
    json += ",\"steam_ki\":" + String(steamKi, 4);
    json += ",\"steam_kd\":" + String(steamKd, 4);
    json += ",\"steam_max_safety\":" + String(steamMaxSafety);

    Preferences preferences;
    preferences.begin("gaggia", true);
    json +=
        ",\"mqtt_server\":\"" + preferences.getString("mqtt_server", "") + "\"";
    json += ",\"mqtt_port\":" + String(preferences.getInt("mqtt_port", 1883));
    json += ",\"mqtt_user\":\"" + preferences.getString("mqtt_user", "") + "\"";
    json += ",\"mqtt_pass\":\"" + preferences.getString("mqtt_pass", "") + "\"";
    preferences.end();

    json += ",\"fw_build\":\"" + String(FIRMWARE_BUILD_TIMESTAMP) + "\"";

    json += ",\"fault\":" + String(sensorFault ? "true" : "false");

    json += ",\"eco_timeout_min\":" + String(ecoTimeoutMin);
    json += ",\"auto_sleeping\":" + String(autoSleeping ? "true" : "false");

    json += ",\"autotune_state\":\"";
    switch (autotuneState) {
      case AutotuneState::RUNNING: json += "running"; break;
      case AutotuneState::DONE_OK: json += "done_ok"; break;
      case AutotuneState::DONE_FAIL: json += "done_fail"; break;
      default: json += "idle"; break;
    }
    json += "\"";
    json += ",\"autotune_message\":\"" + autotuneMessage + "\"";

    json += ",\"shot_in_progress\":" + String(shotInProgress ? "true" : "false");
    json += ",\"shot_elapsed_ms\":" +
            String(shotInProgress ? (millis() - shotStartMillis) : 0);

    json += ",\"shot_count\":" + String(shotCount);
    json += ",\"descale_shot_threshold\":" + String(descaleShotThreshold);
    json += ",\"descale_day_threshold\":" + String(descaleDayThreshold);
    // -1 means "unknown" (never descaled/reset since this feature was added) -
    // avoids flashing a false "descale overdue" banner from an epoch-0 default.
    long daysSinceDescale =
        (lastDescaleTime > 0) ? (long)((time(nullptr) - lastDescaleTime) / 86400L) : -1;
    json += ",\"days_since_descale\":" + String(daysSinceDescale);
    bool descaleDue =
        (shotCount >= descaleShotThreshold) ||
        (daysSinceDescale >= 0 && (unsigned long)daysSinceDescale >= descaleDayThreshold);
    json += ",\"descale_due\":" + String(descaleDue ? "true" : "false");

    json += ",\"history\":[";
    for (int i = 0; i < tempHistoryCount; i++) {
      int idx = (tempHistoryHead - tempHistoryCount + i + TEMP_HISTORY_LEN * 2) %
                TEMP_HISTORY_LEN;
      if (i > 0) json += ",";
      json += String(tempHistory[idx], 1);
    }
    json += "]";

    json += "}";
    server.send(200, "application/json", json);
  });

  // Shot history log
  server.on("/shots", HTTP_GET,
            []() { server.send(200, "application/json", shotLogReadJson()); });

  // Settings Update Handler
  server.on("/update", HTTP_GET, []() {
    noteActivity(); // any /update call is explicit user action - resets eco-sleep timer

    Preferences preferences;
    preferences.begin("gaggia", false); // false = read/write

    if (server.hasArg("brew_target")) {
      brewSetpoint = server.arg("brew_target").toDouble();
      preferences.putDouble("brew_target", brewSetpoint);
    }
    if (server.hasArg("brew_kp")) {
      brewKp = server.arg("brew_kp").toDouble();
      preferences.putDouble("brew_kp", brewKp);
    }
    if (server.hasArg("brew_ki")) {
      brewKi = server.arg("brew_ki").toDouble();
      preferences.putDouble("brew_ki", brewKi);
    }
    if (server.hasArg("brew_kd")) {
      brewKd = server.arg("brew_kd").toDouble();
      preferences.putDouble("brew_kd", brewKd);
    }
    if (server.hasArg("brew_akp")) {
      brewActiveKp = server.arg("brew_akp").toDouble();
      preferences.putDouble("brew_akp", brewActiveKp);
    }
    if (server.hasArg("brew_aki")) {
      brewActiveKi = server.arg("brew_aki").toDouble();
      preferences.putDouble("brew_aki", brewActiveKi);
    }
    if (server.hasArg("brew_akd")) {
      brewActiveKd = server.arg("brew_akd").toDouble();
      preferences.putDouble("brew_akd", brewActiveKd);
    }
    if (server.hasArg("steam_target")) {
      steamSetpoint = server.arg("steam_target").toDouble();
      preferences.putDouble("steam_target", steamSetpoint);
    }
    if (server.hasArg("steam_kp")) {
      steamKp = server.arg("steam_kp").toDouble();
      preferences.putDouble("steam_kp", steamKp);
    }
    if (server.hasArg("steam_ki")) {
      steamKi = server.arg("steam_ki").toDouble();
      preferences.putDouble("steam_ki", steamKi);
    }
    if (server.hasArg("steam_kd")) {
      steamKd = server.arg("steam_kd").toDouble();
      preferences.putDouble("steam_kd", steamKd);
    }
    if (server.hasArg("steam_max_safety")) {
      // Clamped server-side - a typo in this field shouldn't be able to set
      // a dangerously high (or uselessly low) steam safety ceiling.
      double v = server.arg("steam_max_safety").toDouble();
      steamMaxSafety = constrain(v, STEAM_MAX_SAFETY_MIN, STEAM_MAX_SAFETY_MAX);
      preferences.putDouble("steam_max_safety", steamMaxSafety);
    }
    // If the currently-active profile's own values just changed, apply them
    // live (no mode change, so no PID reset - matches how tuning edits have
    // always behaved here).
    if (server.hasArg("brew_target") || server.hasArg("brew_kp") ||
        server.hasArg("brew_ki") || server.hasArg("brew_kd") ||
        server.hasArg("brew_akp") || server.hasArg("brew_aki") ||
        server.hasArg("brew_akd") || server.hasArg("steam_target") ||
        server.hasArg("steam_kp") || server.hasArg("steam_ki") ||
        server.hasArg("steam_kd") || server.hasArg("steam_max_safety")) {
      refreshActiveProfileIfChanged();
    }

    if (server.hasArg("mode")) {
      // A mode-button click always safely interrupts an in-progress
      // autotune first - autotune's relay-driven Output must never keep
      // running once the user has asked for a different mode.
      stopAutotune();
      String mode = server.arg("mode");
      if (mode == "off") {
        setOpMode(OpMode::OFF);
      } else if (mode == "brew") {
        setOpMode(OpMode::BREW);
      } else if (mode == "steam") {
        setOpMode(OpMode::STEAM);
      }
    }

    // Eco / auto-sleep
    if (server.hasArg("eco_timeout_min")) {
      ecoTimeoutMin = server.arg("eco_timeout_min").toInt();
      preferences.putULong("eco_min", ecoTimeoutMin);
    }
    if (server.hasArg("wake") && server.arg("wake") == "1") {
      wakeFromSleep();
    }

    // PID Autotune - runs the currently-active profile (must be Brew or
    // Steam already, not Off) through a relay-feedback tuning cycle.
    if (server.hasArg("autotune")) {
      String at = server.arg("autotune");
      if (at == "start") {
        startAutotune(currentMode == OpMode::STEAM ? OpMode::STEAM : OpMode::BREW);
      } else if (at == "stop") {
        stopAutotune();
      }
    }

    // Shot timer (manual Start/Stop trigger - see config.h / AGENTS.md
    // roadmap item 7 for why this isn't automatic yet).
    if (server.hasArg("shot")) {
      String s = server.arg("shot");
      if (s == "start") {
        startShot();
      } else if (s == "stop") {
        stopShot();
      }
    }

    // Descale / maintenance reminder
    if (server.hasArg("mark_descaled") && server.arg("mark_descaled") == "1") {
      markDescaled();
    }
    if (server.hasArg("descale_shot_threshold")) {
      descaleShotThreshold = server.arg("descale_shot_threshold").toInt();
      preferences.putULong("descale_shots", descaleShotThreshold);
    }
    if (server.hasArg("descale_day_threshold")) {
      descaleDayThreshold = server.arg("descale_day_threshold").toInt();
      preferences.putULong("descale_days", descaleDayThreshold);
    }

    // MQTT Settings
    if (server.hasArg("mqtt_server")) {
      preferences.putString("mqtt_server", server.arg("mqtt_server"));
    }
    if (server.hasArg("mqtt_port")) {
      preferences.putInt("mqtt_port", server.arg("mqtt_port").toInt());
    }
    if (server.hasArg("mqtt_user")) {
      preferences.putString("mqtt_user", server.arg("mqtt_user"));
    }
    if (server.hasArg("mqtt_pass")) {
      preferences.putString("mqtt_pass", server.arg("mqtt_pass"));
    }

    preferences.end();

    server.sendHeader("Location", "/");
    server.send(303);

    // Reboot to apply MQTT settings cleanly (simplest way)
    if (server.hasArg("mqtt_server")) {
      delay(500);
      ESP.restart();
    }
  });

  // WiFi reconfiguration - clears stored credentials and reboots. On next
  // boot, WiFiManager's autoConnect() (in this same setupWeb()) will fail to
  // join and fall back to the "GaggiaBrewMasterESP_Setup" captive portal
  // automatically - the same well-tested path used on first boot, reused
  // here instead of building a second, custom WiFi-config UI from scratch.
  server.on("/wifi_reset", HTTP_GET, []() {
    server.send(200, "text/html",
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
  server.on("/firmware", HTTP_GET, []() {
    String html = "<html><body><h2>OTA Update</h2>";
    html += "<form method='POST' action='/update_fw' "
            "enctype='multipart/form-data'>";
    html += "<input type='file' name='update'>";
    html += "<input type='submit' value='Update Firmware'>";
    html += "</form></body></html>";
    server.send(200, "text/html", html);
  });

  // OTA Update Handler
  server.on(
      "/update_fw", HTTP_POST,
      []() {
        server.send(200, "text/plain",
                    (Update.hasError()) ? "Update Failed"
                                        : "Update Success! Restarting...");
        ESP.restart();
      },
      []() {
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          Serial.printf("Update: %s\n", upload.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (Update.write(upload.buf, upload.currentSize) !=
              upload.currentSize) {
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (Update.end(true)) {
            Serial.printf("Update Success: %u\n", upload.totalSize);
          } else {
            Update.printError(Serial);
          }
        }
      });

  server.begin();
}

// Function to handle client requests in the loop (standard WebServer needs
// this)
void handleWebLoop() { server.handleClient(); }
