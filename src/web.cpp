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

// ... (index_html skip) ...

// ... (setupWeb start skip) ...

const char *index_html = R"rawliteral(
<!DOCTYPE HTML><html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="theme-color" content="#161311">
  <link rel="manifest" href="/manifest.json">
  <link rel="icon" href="/icon.svg" type="image/svg+xml">
  <link rel="apple-touch-icon" href="/icon.svg">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
  <meta name="apple-mobile-web-app-title" content="BrewMaster">
  <title>GaggiaBrewMasterESP</title>
  <style>
    :root {
      --bg: #161311;
      --bg-grad: radial-gradient(1200px 600px at 50% -10%, #2a211b 0%, #161311 55%);
      --card: #211c18;
      --card-2: #2a231e;
      --line: #3a312a;
      --text: #f2ece6;
      --muted: #a99f95;
      --accent: #d98c3f;
      --accent-2: #b9702c;
      --green: #4caf7d;
      --red: #e5544b;
      --steam: #4a9fd8;
      --shadow: 0 10px 30px rgba(0,0,0,.45);
      --radius: 16px;
    }
    * { box-sizing: border-box; }
    html, body { margin: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      background: var(--bg-grad), var(--bg);
      color: var(--text);
      min-height: 100vh;
      -webkit-font-smoothing: antialiased;
      padding: 24px 16px 48px;
    }
    .wrap { max-width: 720px; margin: 0 auto; }
    header {
      display: flex; align-items: center; justify-content: space-between;
      gap: 12px; margin-bottom: 22px;
    }
    .brand { display: flex; align-items: center; gap: 12px; }
    .logo {
      width: 40px; height: 40px; border-radius: 12px; flex: none;
      background: linear-gradient(135deg, var(--accent), var(--accent-2));
      display: grid; place-items: center; font-size: 22px;
      box-shadow: var(--shadow);
    }
    h1 { font-size: 20px; margin: 0; letter-spacing: .2px; }
    .brand small { display: block; color: var(--muted); font-size: 12px; font-weight: 500; }
    .pill {
      display: inline-flex; align-items: center; gap: 8px;
      padding: 8px 14px; border-radius: 999px; font-size: 13px; font-weight: 600;
      background: var(--card); border: 1px solid var(--line);
    }
    .dot { width: 9px; height: 9px; border-radius: 50%; background: var(--muted); }
    .pill.brew .dot { background: var(--green); box-shadow: 0 0 0 4px rgba(76,175,125,.18); }
    .pill.steam .dot { background: var(--steam); box-shadow: 0 0 0 4px rgba(74,159,216,.18); }
    .pill.off .dot { background: var(--red); box-shadow: 0 0 0 4px rgba(229,84,75,.18); }

    .grid { display: grid; gap: 16px; }
    @media (min-width: 640px) { .cols-2 { grid-template-columns: 1fr 1fr; } }

    .card {
      background: linear-gradient(180deg, var(--card-2), var(--card));
      border: 1px solid var(--line);
      border-radius: var(--radius);
      box-shadow: var(--shadow);
      padding: 20px;
    }
    .card h2 {
      margin: 0 0 16px; font-size: 13px; text-transform: uppercase;
      letter-spacing: 1.4px; color: var(--muted); font-weight: 700;
    }

    .temp-hero {
      display: grid; place-items: center; padding: 12px 0 8px;
      grid-template-areas: "stack";
    }
    .temp-ring { grid-area: stack; transform: rotate(-90deg); width: 200px; height: 200px; }
    .temp-ring-track { fill: none; stroke: var(--line); stroke-width: 14; }
    .temp-ring-fill {
      fill: none; stroke: var(--muted); stroke-width: 14; stroke-linecap: round;
      transition: stroke-dashoffset .6s ease, stroke .5s ease;
    }
    /* Ring color is a functional signal, not decoration: blue-grey while
       still climbing to target, green once at/near it (the "ready" cue,
       readable without parsing the number), amber/red once over - the same
       state also covers a shot's temperature sag, since that's just
       "below target" again. */
    .temp-ring-fill.heating { stroke: var(--steam); }
    .temp-ring-fill.ready { stroke: var(--green); }
    .temp-ring-fill.over { stroke: var(--red); }
    .temp-ring-center { grid-area: stack; text-align: center; }
    .temp-value { font-size: 56px; font-weight: 700; line-height: 1; letter-spacing: -1px; }
    .temp-value .unit { font-size: 22px; color: var(--muted); font-weight: 600; margin-left: 4px; }
    .temp-sub { color: var(--muted); margin-top: 8px; font-size: 14px; }
    .temp-sub b { color: var(--text); }

    .bar { height: 10px; border-radius: 999px; background: #17130f; border: 1px solid var(--line); overflow: hidden; margin-top: 6px; }
    .bar > span { display: block; height: 100%; width: 0%; border-radius: 999px; transition: width .5s ease; }
    .bar.heat > span { background: linear-gradient(90deg, var(--accent-2), var(--accent)); }
    .metric-row { display: flex; align-items: center; justify-content: space-between; margin: 18px 0 6px; font-size: 14px; color: var(--muted); }
    .metric-row b { color: var(--text); font-size: 15px; }

    .controls { display: flex; gap: 12px; margin-top: 20px; }
    .btn {
      flex: 1; border: 1px solid var(--line); color: var(--text); background: var(--card);
      padding: 14px 16px; border-radius: 12px; font-size: 15px; font-weight: 700;
      cursor: pointer; transition: transform .05s ease, background .15s ease, border-color .15s ease;
    }
    .btn:active { transform: translateY(1px); }
    .btn-on { background: rgba(76,175,125,.14); border-color: rgba(76,175,125,.4); color: #cdeede; }
    .btn-on.active { background: var(--green); border-color: var(--green); color: #0d1a13; }
    .btn-off { background: rgba(229,84,75,.14); border-color: rgba(229,84,75,.4); color: #f3c6c2; }
    .btn-off.active { background: var(--red); border-color: var(--red); color: #1a0d0c; }
    .btn-steam { background: rgba(74,159,216,.14); border-color: rgba(74,159,216,.4); color: #cfe8f7; }
    .btn-steam.active { background: var(--steam); border-color: var(--steam); color: #071824; }

    .section-label { font-size: 12px; font-weight: 700; color: var(--muted); text-transform: uppercase; letter-spacing: 1px; margin-bottom: 12px; }
    .divider { border: none; border-top: 1px solid var(--line); margin: 22px 0; }

    .sleep-banner {
      display: none; align-items: center; justify-content: space-between; gap: 10px;
      background: rgba(74,159,216,.14); border: 1px solid rgba(74,159,216,.4);
      color: #cfe8f7; padding: 10px 14px; border-radius: 12px;
      font-size: 13px; font-weight: 700; margin-bottom: 14px;
    }
    .btn-wake {
      border: 1px solid var(--steam); background: var(--steam); color: #071824;
      padding: 6px 12px; border-radius: 8px; font-size: 12px; font-weight: 700; cursor: pointer;
    }

    .shot-row {
      margin-top: 20px; padding-top: 18px; border-top: 1px solid var(--line);
      display: flex; align-items: center; justify-content: space-between; gap: 14px;
    }
    .shot-time { font-size: 32px; font-weight: 700; font-variant-numeric: tabular-nums; color: var(--muted); transition: color .2s ease; }
    .shot-time.in-window { color: var(--green); }
    .shot-time.over { color: var(--red); }
    .shot-sub { font-size: 11px; color: var(--muted); margin-top: 2px; }
    .btn-shot {
      border: none; cursor: pointer; padding: 13px 20px; border-radius: 12px;
      font-size: 14px; font-weight: 700; color: #cdeede;
      background: rgba(76,175,125,.14); border: 1px solid rgba(76,175,125,.4);
      transition: transform .05s ease, background .15s ease;
    }
    .btn-shot:active { transform: translateY(1px); }
    .btn-shot.running { background: var(--red); border-color: var(--red); color: #1a0d0c; }

    table.history { width: 100%; border-collapse: collapse; font-size: 13px; }
    table.history th, table.history td { text-align: left; padding: 8px 6px; border-bottom: 1px solid var(--line); }
    table.history th { color: var(--muted); font-weight: 600; font-size: 11px; text-transform: uppercase; letter-spacing: .5px; }
    table.history td.num { text-align: right; font-variant-numeric: tabular-nums; }
    .empty-hint { color: var(--muted); font-size: 13px; padding: 6px 0; }

    .descale-banner {
      display: none; align-items: center; gap: 8px;
      background: rgba(217,140,63,.14); border: 1px solid rgba(217,140,63,.4);
      color: #f3dcc2; padding: 10px 14px; border-radius: 12px;
      font-size: 13px; font-weight: 700; margin-bottom: 14px;
    }
    .btn-descaled {
      width: 100%; border: none; cursor: pointer; margin-top: 12px;
      padding: 12px; border-radius: 12px; font-size: 14px; font-weight: 700; color: #1a1206;
      background: linear-gradient(135deg, var(--accent), var(--accent-2));
    }

    .autotune-row { margin-top: 20px; padding-top: 18px; border-top: 1px solid var(--line); }
    .btn-autotune {
      width: 100%; border: none; cursor: pointer;
      display: flex; align-items: center; justify-content: center; gap: 8px;
      padding: 13px 16px; border-radius: 12px; font-size: 14px; font-weight: 700; color: #1a1206;
      background: linear-gradient(135deg, var(--accent), var(--accent-2));
      box-shadow: 0 6px 18px rgba(217,140,63,.25);
      transition: transform .05s ease, opacity .15s ease;
    }
    .btn-autotune:active:not(:disabled) { transform: translateY(1px); }
    .btn-autotune:disabled { opacity: 0.45; cursor: default; box-shadow: none; }
    .btn-autotune.running { background: var(--red); color: #1a0d0c; box-shadow: 0 6px 18px rgba(229,84,75,.3); }
    .autotune-status { display: block; margin-top: 10px; font-size: 12px; color: var(--muted); text-align: center; }

    label { display: block; font-size: 12px; color: var(--muted); margin-bottom: 6px; font-weight: 600; }
    .field { margin-bottom: 14px; }
    input {
      width: 100%; padding: 11px 12px; border-radius: 10px; font-size: 15px;
      background: #17130f; border: 1px solid var(--line); color: var(--text);
      outline: none; transition: border-color .15s ease, box-shadow .15s ease;
    }
    input:focus { border-color: var(--accent); box-shadow: 0 0 0 3px rgba(217,140,63,.18); }
    .field-row { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
    .field-row-3 { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 12px; }

    .submit {
      width: 100%; margin-top: 6px; border: none; cursor: pointer;
      padding: 14px; border-radius: 12px; font-size: 15px; font-weight: 700; color: #1a1206;
      background: linear-gradient(135deg, var(--accent), var(--accent-2));
      box-shadow: 0 6px 18px rgba(217,140,63,.28);
    }
    .submit:active { transform: translateY(1px); }
    .hint { font-size: 12px; color: var(--muted); margin-top: 10px; text-align: center; }

    footer { text-align: center; color: var(--muted); font-size: 12px; margin-top: 26px; }
    footer a { color: var(--accent); text-decoration: none; }

    .fault-banner {
      display: none; align-items: center; gap: 8px;
      background: rgba(229,84,75,.14); border: 1px solid rgba(229,84,75,.4);
      color: #f3c6c2; padding: 10px 14px; border-radius: 12px;
      font-size: 13px; font-weight: 700; margin-bottom: 14px;
    }
    .chart-wrap { margin-top: 18px; }
    .chart-wrap canvas { width: 100%; height: 56px; display: block; }
    .chart-label { display: flex; justify-content: space-between; font-size: 11px; color: var(--muted); margin-bottom: 4px; }
    .last-updated { font-size: 11px; color: var(--muted); margin-top: 4px; text-align: right; }
  </style>
</head>
<body>
  <div class="wrap">
    <header>
      <div class="brand">
        <div class="logo">&#9749;</div>
        <div>
          <h1>GaggiaBrewMaster</h1>
          <small>Smart espresso controller</small>
        </div>
      </div>
      <div>
        <div id="status_pill" class="pill off"><span class="dot"></span><span id="mode_status">--</span></div>
        <div id="last_updated" class="last-updated">--</div>
      </div>
    </header>

    <div class="grid cols-2">
      <div class="card">
        <h2>Boiler</h2>
        <div id="fault_banner" class="fault-banner">&#9888; Sensor fault &mdash; check wiring</div>
        <div id="sleep_banner" class="sleep-banner">
          <span>&#9866; Asleep (eco timeout) &mdash; heater off</span>
          <button onclick="wake()" class="btn-wake">Wake Up</button>
        </div>
        <div class="temp-hero">
          <svg class="temp-ring" viewBox="0 0 200 200">
            <circle class="temp-ring-track" cx="100" cy="100" r="85"></circle>
            <circle id="temp_ring_fill" class="temp-ring-fill" cx="100" cy="100" r="85"
                    stroke-dasharray="534" stroke-dashoffset="534"></circle>
          </svg>
          <div class="temp-ring-center">
            <div class="temp-value"><span id="temp">--</span><span class="unit">&deg;C</span></div>
            <div class="temp-sub">Target <b><span id="target">--</span> &deg;C</b></div>
          </div>
        </div>

        <div class="metric-row"><span>Heater output</span><b><span id="output">--</span>%</b></div>
        <div class="bar heat"><span id="output_bar"></span></div>

        <div class="chart-wrap">
          <div class="chart-label"><span>Last 2 min</span><span>&deg;C</span></div>
          <canvas id="temp_chart" width="300" height="56"></canvas>
        </div>

        <div class="controls">
          <button onclick="setMode('off')" id="btn_off" class="btn btn-off">Off</button>
          <button onclick="setMode('brew')" id="btn_brew" class="btn btn-on">Brew</button>
          <button onclick="setMode('steam')" id="btn_steam" class="btn btn-steam">Steam</button>
        </div>

        <div class="autotune-row">
          <button onclick="startAutotune()" id="btn_autotune" class="btn-autotune">&#9889; Start Auto-Tune</button>
          <span id="autotune_status" class="autotune-status"></span>
        </div>

        <div class="shot-row">
          <div>
            <div id="shot_time" class="shot-time">0:00</div>
            <div class="shot-sub">Target window 25&ndash;30s</div>
          </div>
          <button onclick="toggleShot()" id="btn_shot" class="btn-shot">Start Shot</button>
        </div>
      </div>

      <div class="card">
        <h2>PID Tuning</h2>

        <div class="section-label">Brew</div>
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
          <input type="submit" value="Save Brew" class="submit">
        </form>

        <div class="section-label" style="margin-top:16px">Brew - active during a shot</div>
        <div class="hint">Switches in automatically the instant a shot starts (Start Shot), reverts to the values above the instant it stops. Deliberately more aggressive - fights the temperature drop from real flow, which the gentle gains above are too slow for.</div>
        <form action="/update" method="GET">
          <div class="field-row-3">
            <div class="field"><label for="input_brew_akp">Kp</label><input type="number" step="any" name="brew_akp" id="input_brew_akp" value=""></div>
            <div class="field"><label for="input_brew_aki">Ki</label><input type="number" step="any" name="brew_aki" id="input_brew_aki" value=""></div>
            <div class="field"><label for="input_brew_akd">Kd</label><input type="number" step="any" name="brew_akd" id="input_brew_akd" value=""></div>
          </div>
          <input type="submit" value="Save Active-Brew Gains" class="submit">
        </form>

        <hr class="divider">

        <div class="section-label">Steam</div>
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
          <input type="submit" value="Save Steam" class="submit">
        </form>
      </div>
    </div>

    <div class="grid" style="margin-top:16px">
      <div class="card">
        <h2>MQTT / Home Assistant</h2>
        <form action="/update" method="GET">
          <div class="field-row">
            <div class="field"><label for="input_mqtt_server">Server</label><input type="text" name="mqtt_server" id="input_mqtt_server" placeholder="192.168.1.100"></div>
            <div class="field"><label for="input_mqtt_port">Port</label><input type="number" name="mqtt_port" id="input_mqtt_port" value="1883"></div>
          </div>
          <div class="field-row">
            <div class="field"><label for="input_mqtt_user">User</label><input type="text" name="mqtt_user" id="input_mqtt_user"></div>
            <div class="field"><label for="input_mqtt_pass">Password</label><input type="password" name="mqtt_pass" id="input_mqtt_pass"></div>
          </div>
          <input type="submit" value="Save &amp; Restart" class="submit">
          <div class="hint">Saving MQTT settings reboots the controller.</div>
        </form>
      </div>
    </div>

    <div class="grid cols-2" style="margin-top:16px">
      <div class="card">
        <h2>Power &amp; Eco</h2>
        <form action="/update" method="GET">
          <div class="field">
            <label for="input_eco_min">Auto-sleep after (minutes, 0 = disabled)</label>
            <input type="number" step="1" min="0" name="eco_timeout_min" id="input_eco_min" value="">
          </div>
          <input type="submit" value="Save" class="submit">
          <div class="hint">Heater force-OFF after this long with no Web UI activity (mode/tuning changes). Does not count passive status polling.</div>
        </form>
      </div>

      <div class="card">
        <h2>Network</h2>
        <button onclick="wifiReset()" class="submit" style="background:var(--red); color:#1a0d0c;">Reset WiFi Settings</button>
        <div class="hint">Reboots into the <b>GaggiaBrewMasterESP_Setup</b> setup network so you can join a different WiFi without reflashing.</div>
        <div class="hint"><a href="/firmware">Firmware update (OTA)</a></div>
      </div>
    </div>

    <div class="grid cols-2" style="margin-top:16px">
      <div class="card">
        <h2>Shot History</h2>
        <div id="shot_history_empty" class="empty-hint">No shots logged yet.</div>
        <div style="overflow-x:auto">
          <table class="history" id="shot_history_table" style="display:none">
            <thead><tr><th>When</th><th>Duration</th><th>Peak &deg;C</th><th>Weight</th></tr></thead>
            <tbody id="shot_history_body"></tbody>
          </table>
        </div>
      </div>

      <div class="card">
        <h2>Maintenance</h2>
        <div id="descale_banner" class="descale-banner">&#9888; Descale recommended</div>
        <div class="metric-row"><span>Shots since last descale</span><b><span id="descale_shots">--</span></b></div>
        <div class="metric-row"><span>Days since last descale</span><b><span id="descale_days">--</span></b></div>
        <form action="/update" method="GET">
          <div class="field-row">
            <div class="field"><label for="input_descale_shots">Shot threshold</label><input type="number" step="1" min="1" name="descale_shot_threshold" id="input_descale_shots" value=""></div>
            <div class="field"><label for="input_descale_days">Day threshold</label><input type="number" step="1" min="1" name="descale_day_threshold" id="input_descale_days" value=""></div>
          </div>
          <input type="submit" value="Save Thresholds" class="submit">
        </form>
        <button onclick="markDescaled()" class="btn-descaled">Mark Descaled Today</button>
      </div>
    </div>

    <footer>
      <a href="/firmware">Firmware update (OTA)</a> &middot; <span id="host">gaggia.local</span> &middot; build <span id="fw_build">--</span>
    </footer>
  </div>

<script>
function setVal(id, v) {
  var el = document.getElementById(id);
  if (document.activeElement.id !== id && (el.value === "" || el.dataset.synced !== "1")) {
    el.value = v; el.dataset.synced = "1";
  }
}
function clamp(x) { return Math.max(0, Math.min(100, x)); }

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
    var y = h - ((v - min) / (max - min)) * (h - 4) - 2;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.strokeStyle = "#d98c3f";
  ctx.lineWidth = 2;
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
  el.classList.toggle("in-window", shotRunning && secs >= 25 && secs <= 30);
  el.classList.toggle("over", shotRunning && secs > 30);
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
  el.style.color = secs > 6 ? "var(--red)" : "var(--muted)";
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

      document.getElementById("fault_banner").style.display = json.fault ? "flex" : "none";
      document.getElementById("temp").innerHTML = json.fault ? "--" : temp.toFixed(1);
      document.getElementById("target").innerHTML = target.toFixed(1);
      document.getElementById("output").innerHTML = outputPct.toFixed(0);
      drawSparkline(json.history);

      // Temperature ring - fill amount reuses the same ratio the old linear
      // bar used; color is the functional "heating / ready / over" signal,
      // readable at a glance without parsing the number (also naturally
      // covers a shot's temperature sag, which is just "below target" again).
      var tempPct = (temp > 0 && target > 0) ? clamp((temp / target) * 100) : 0;
      var ring = document.getElementById("temp_ring_fill");
      var RING_CIRCUMFERENCE = 534; // 2*pi*85, matches the SVG circle's r=85
      ring.style.strokeDashoffset = RING_CIRCUMFERENCE * (1 - tempPct / 100);
      ring.classList.remove("heating", "ready", "over");
      if (!json.fault && temp > 0 && target > 0) {
        var READY_MARGIN_C = 1.0;
        if (temp < target - READY_MARGIN_C) ring.classList.add("heating");
        else if (temp > target + READY_MARGIN_C) ring.classList.add("over");
        else ring.classList.add("ready");
      }

      document.getElementById("output_bar").style.width = clamp(outputPct) + "%";

      // Status
      var mode = json.opmode; // "off" | "brew" | "steam"
      var pill = document.getElementById("status_pill");
      var label = mode === "brew" ? "Brewing" : mode === "steam" ? "Steaming" : "Off";
      document.getElementById("mode_status").innerHTML = label;
      pill.className = "pill " + mode;
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
      document.getElementById("descale_banner").style.display = json.descale_due ? "flex" : "none";

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
"background_color":"#161311",
"theme_color":"#161311",
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
