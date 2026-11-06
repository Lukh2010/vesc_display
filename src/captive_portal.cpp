#include "captive_portal.h"

#if !defined(NATIVE_SIMULATOR)
#include "mode_manager.h"
#include "vesc_uart.h"
#include "settings_manager.h"
#include "fault_manager.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_task_wdt.h>

static const byte DNS_PORT = 53;
static IPAddress apIP(192, 168, 4, 1);
static IPAddress netMsk(255, 255, 255, 0);
static DNSServer dnsServer;
static WebServer server(80);
static TaskHandle_t s_portalTaskHandle = NULL;

// Raw HTML for the Captive Portal & Web Configuration Interface
static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,maximum-scale=1.0,user-scalable=no">
<title>VESC HyperDash Captive Portal</title>
<style>
:root {
  --bg: #070b14;
  --surface: #0f172a;
  --surface-card: #141f38;
  --surface-hover: #1e2d4f;
  --border: #1e293b;
  --text: #f8fafc;
  --muted: #94a3b8;
  --cyan: #00e5ff;
  --green: #00e676;
  --pink: #ff007f;
  --amber: #ffb300;
  --red: #ef4444;
}
* { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
body { background: var(--bg); color: var(--text); padding-bottom: 90px; }
.header { background: linear-gradient(180deg, #0f172a 0%, #070b14 100%); padding: 18px 16px 12px; border-bottom: 1px solid var(--border); text-align: center; }
.logo { font-size: 22px; font-weight: 900; letter-spacing: 1.5px; color: var(--cyan); text-shadow: 0 0 16px rgba(0,229,255,0.4); }
.subtitle { font-size: 11px; color: var(--muted); letter-spacing: 0.8px; margin-top: 3px; text-transform: uppercase; }

.status-box { margin: 14px 16px 10px; padding: 14px; border-radius: 12px; background: var(--surface); border: 1px solid var(--border); text-align: center; }
.badge { display: inline-flex; align-items: center; gap: 8px; padding: 6px 14px; border-radius: 20px; font-size: 13px; font-weight: 700; letter-spacing: 0.5px; }
.badge.locked { background: rgba(239, 68, 68, 0.15); color: var(--red); border: 1px solid rgba(239, 68, 68, 0.4); }
.badge.unlocked { background: rgba(0, 229, 255, 0.15); color: var(--cyan); border: 1px solid rgba(0, 229, 255, 0.4); }
.dot { width: 8px; height: 8px; border-radius: 50%; }
.badge.locked .dot { background: var(--red); box-shadow: 0 0 8px var(--red); }
.badge.unlocked .dot { background: var(--cyan); box-shadow: 0 0 8px var(--cyan); }

.quick-actions { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 12px; }
.btn { border: none; padding: 10px 14px; border-radius: 8px; font-size: 13px; font-weight: 700; cursor: pointer; transition: all 0.2s; display: flex; align-items: center; justify-content: center; gap: 6px; }
.btn-red { background: rgba(239, 68, 68, 0.2); color: #fca5a5; border: 1px solid #ef4444; }
.btn-red:active { background: #ef4444; color: #fff; }
.btn-cyan { background: rgba(0, 229, 255, 0.2); color: #67e8f9; border: 1px solid #00e5ff; }
.btn-cyan:active { background: #00e5ff; color: #000; }

.telem-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; margin: 0 16px 14px; }
.telem-card { background: var(--surface); border: 1px solid var(--border); border-radius: 10px; padding: 8px 6px; text-align: center; }
.telem-val { font-size: 16px; font-weight: 800; color: #fff; }
.telem-lbl { font-size: 9px; color: var(--muted); text-transform: uppercase; margin-top: 2px; }

.tabs { display: flex; overflow-x: auto; gap: 6px; padding: 4px 16px 10px; scrollbar-width: none; }
.tabs::-webkit-scrollbar { display: none; }
.tab-btn { background: var(--surface); border: 1px solid var(--border); color: var(--muted); padding: 8px 14px; border-radius: 8px; font-size: 12px; font-weight: 700; white-space: nowrap; cursor: pointer; }
.tab-btn.active { background: var(--surface-hover); color: #fff; border-color: var(--cyan); }
.tab-btn.tab-stealth.active { border-color: var(--red); color: #fca5a5; }
.tab-btn.tab-eco.active { border-color: var(--green); color: #86efac; }
.tab-btn.tab-sport.active { border-color: var(--cyan); color: #67e8f9; }
.tab-btn.tab-race.active { border-color: var(--pink); color: #f472b6; }

.tab-content { display: none; padding: 0 16px; }
.tab-content.active { display: block; }

.card { background: var(--surface); border: 1px solid var(--border); border-radius: 12px; padding: 14px; margin-bottom: 12px; }
.card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; font-size: 14px; font-weight: 700; }
.stealth-title { color: #f87171; }
.eco-title { color: var(--green); }
.sport-title { color: var(--cyan); }
.race-title { color: var(--pink); }

.param-group { margin-bottom: 14px; }
.param-group:last-child { margin-bottom: 0; }
.param-lbl { display: flex; justify-content: space-between; font-size: 12px; color: var(--muted); margin-bottom: 6px; }
.param-controls { display: flex; align-items: center; gap: 10px; }
input[type=range] { flex: 1; height: 6px; border-radius: 3px; background: #1e293b; outline: none; -webkit-appearance: none; }
input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 18px; height: 18px; border-radius: 50%; background: var(--cyan); cursor: pointer; border: 2px solid #fff; box-shadow: 0 0 6px rgba(0,229,255,0.6); }
input[type=number] { width: 75px; background: var(--surface-card); border: 1px solid var(--border); color: #fff; padding: 6px 8px; border-radius: 6px; font-size: 13px; font-weight: 700; text-align: right; }

.footer-bar { position: fixed; bottom: 0; left: 0; right: 0; background: rgba(15, 23, 42, 0.95); backdrop-filter: blur(10px); border-top: 1px solid var(--border); padding: 12px 16px; display: flex; gap: 10px; z-index: 99; }
.btn-save { flex: 1; background: linear-gradient(90deg, #00e5ff 0%, #00b4d8 100%); color: #000; font-size: 14px; font-weight: 800; padding: 12px; border-radius: 10px; border: none; cursor: pointer; box-shadow: 0 0 14px rgba(0,229,255,0.4); }
.btn-save:active { transform: scale(0.98); }

.toast { position: fixed; top: 18px; left: 50%; transform: translateX(-50%); background: #10b981; color: #fff; padding: 10px 20px; border-radius: 8px; font-weight: 700; font-size: 13px; box-shadow: 0 4px 16px rgba(0,0,0,0.5); z-index: 1000; opacity: 0; transition: opacity 0.3s; pointer-events: none; }
.toast.show { opacity: 1; }

.ota-progress { width: 100%; height: 8px; background: #1e293b; border-radius: 4px; overflow: hidden; margin: 10px 0; display: none; }
.ota-bar { width: 0%; height: 100%; background: var(--green); transition: width 0.2s; }
</style>
</head>
<body>

<div class="header">
  <div class="logo">VESC HYPERDASH</div>
  <div class="subtitle">Live Performance & Mode Controller</div>
</div>

<div class="status-box">
  <div id="statusBadge" class="badge locked">
    <span class="dot"></span>
    <span id="statusText">LEGAL STREET LIMITS (ECO: 15 / SPORT: 20 / RACE: 22 km/h)</span>
  </div>
  <div class="quick-actions">
    <button class="btn btn-red" onclick="setTuning(false)">🔒 Lock Legal Limits</button>
    <button class="btn btn-cyan" onclick="setTuning(true)">🚀 Unlock Tuning Profiles</button>
  </div>
</div>

<div class="telem-grid">
  <div class="telem-card">
    <div class="telem-val" id="t_spd">0.0</div>
    <div class="telem-lbl">km/h</div>
  </div>
  <div class="telem-card">
    <div class="telem-val" id="t_pwr">0</div>
    <div class="telem-lbl">Watts</div>
  </div>
  <div class="telem-card">
    <div class="telem-val" id="t_bat">0.0V</div>
    <div class="telem-lbl" id="t_pct">0%</div>
  </div>
  <div class="telem-card">
    <div class="telem-val" id="t_pha">0.0</div>
    <div class="telem-lbl">Phase A</div>
  </div>
</div>

<div class="tabs">
  <button class="tab-btn tab-stealth active" onclick="switchTab('tab_legal', this)">🛡️ Legal Modes (15/20/22)</button>
  <button class="tab-btn tab-race" onclick="switchTab('tab_tuned', this)">🚀 Tuned Profiles (ECO / SPORT / RACE)</button>
  <button class="tab-btn" onclick="switchTab('tab_settings', this)">🔒 Security, Faults & OTA</button>
</div>

<!-- Tab 0: Legal Modes (Street-Legal 15, 20, 22 km/h) -->
<div id="tab_legal" class="tab-content active">
  <div class="card">
    <div class="card-header stealth-title">
      <span>🌿 LEGAL ECO MODE</span>
      <span style="font-size:11px;color:var(--muted)">15 km/h Safe Cruise</span>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Speed Limit</span><span id="lbl_le_spd">15 km/h</span></div>
      <div class="param-controls">
        <input type="range" id="rng_le_spd" min="10" max="25" step="1" value="15" oninput="syncVal('le_spd', this.value, ' km/h')">
        <input type="number" id="num_le_spd" value="15" oninput="syncVal('le_spd', this.value, ' km/h')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Continuous Cruising Power</span><span id="lbl_le_pwr">500 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_le_pwr" min="200" max="1000" step="25" value="500" oninput="syncVal('le_pwr', this.value, ' W')">
        <input type="number" id="num_le_pwr" value="500" oninput="syncVal('le_pwr', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Peak Acceleration Pull Power</span><span id="lbl_le_pul">1000 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_le_pul" min="500" max="1500" step="50" value="1000" oninput="syncVal('le_pul', this.value, ' W')">
        <input type="number" id="num_le_pul" value="1000" oninput="syncVal('le_pul', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Battery Current</span><span id="lbl_le_bat">20 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_le_bat" min="5" max="40" step="1" value="20" oninput="syncVal('le_bat', this.value, ' A')">
        <input type="number" id="num_le_bat" value="20" oninput="syncVal('le_bat', this.value, ' A')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Phase Current</span><span id="lbl_le_pha">40 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_le_pha" min="10" max="60" step="1" value="40" oninput="syncVal('le_pha', this.value, ' A')">
        <input type="number" id="num_le_pha" value="40" oninput="syncVal('le_pha', this.value, ' A')">
      </div>
    </div>
  </div>

  <div class="card">
    <div class="card-header stealth-title">
      <span>⚡ LEGAL SPORT MODE</span>
      <span style="font-size:11px;color:var(--muted)">20 km/h Nominal Standard</span>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Speed Limit</span><span id="lbl_ls_spd">20 km/h</span></div>
      <div class="param-controls">
        <input type="range" id="rng_ls_spd" min="10" max="30" step="1" value="20" oninput="syncVal('ls_spd', this.value, ' km/h')">
        <input type="number" id="num_ls_spd" value="20" oninput="syncVal('ls_spd', this.value, ' km/h')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Continuous Cruising Power</span><span id="lbl_ls_pwr">500 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_ls_pwr" min="200" max="1000" step="25" value="500" oninput="syncVal('ls_pwr', this.value, ' W')">
        <input type="number" id="num_ls_pwr" value="500" oninput="syncVal('ls_pwr', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Peak Acceleration Pull Power</span><span id="lbl_ls_pul">1000 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_ls_pul" min="500" max="1500" step="50" value="1000" oninput="syncVal('ls_pul', this.value, ' W')">
        <input type="number" id="num_ls_pul" value="1000" oninput="syncVal('ls_pul', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Battery Current</span><span id="lbl_ls_bat">20 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_ls_bat" min="5" max="40" step="1" value="20" oninput="syncVal('ls_bat', this.value, ' A')">
        <input type="number" id="num_ls_bat" value="20" oninput="syncVal('ls_bat', this.value, ' A')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Phase Current</span><span id="lbl_ls_pha">45 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_ls_pha" min="10" max="70" step="1" value="45" oninput="syncVal('ls_pha', this.value, ' A')">
        <input type="number" id="num_ls_pha" value="45" oninput="syncVal('ls_pha', this.value, ' A')">
      </div>
    </div>
  </div>

  <div class="card">
    <div class="card-header stealth-title">
      <span>🔥 LEGAL RACE MODE</span>
      <span style="font-size:11px;color:var(--muted)">22 km/h Legal Maximum (eKFV +10%)</span>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Speed Limit</span><span id="lbl_lr_spd">22 km/h</span></div>
      <div class="param-controls">
        <input type="range" id="rng_lr_spd" min="10" max="35" step="1" value="22" oninput="syncVal('lr_spd', this.value, ' km/h')">
        <input type="number" id="num_lr_spd" value="22" oninput="syncVal('lr_spd', this.value, ' km/h')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Continuous Cruising Power (at 22 km/h)</span><span id="lbl_lr_pwr">500 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_lr_pwr" min="200" max="1000" step="25" value="500" oninput="syncVal('lr_pwr', this.value, ' W')">
        <input type="number" id="num_lr_pwr" value="500" oninput="syncVal('lr_pwr', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Peak Acceleration Pull Power</span><span id="lbl_lr_pul">1000 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_lr_pul" min="500" max="1500" step="50" value="1000" oninput="syncVal('lr_pul', this.value, ' W')">
        <input type="number" id="num_lr_pul" value="1000" oninput="syncVal('lr_pul', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Battery Current</span><span id="lbl_lr_bat">22 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_lr_bat" min="5" max="45" step="1" value="22" oninput="syncVal('lr_bat', this.value, ' A')">
        <input type="number" id="num_lr_bat" value="22" oninput="syncVal('lr_bat', this.value, ' A')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Phase Current</span><span id="lbl_lr_pha">50 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_lr_pha" min="10" max="80" step="1" value="50" oninput="syncVal('lr_pha', this.value, ' A')">
        <input type="number" id="num_lr_pha" value="50" oninput="syncVal('lr_pha', this.value, ' A')">
      </div>
    </div>
  </div>
</div>

<!-- Tab 1: Tuned Profiles (ECO, SPORT, RACE in One Unified View) -->
<div id="tab_tuned" class="tab-content">
  <div class="card">
    <div class="card-header eco-title">
      <span>🌿 TUNED ECO MODE</span>
      <span style="font-size:11px;color:var(--muted)">Unlocked Efficient Cruise</span>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Speed Limit</span><span id="lbl_te_spd">35 km/h</span></div>
      <div class="param-controls">
        <input type="range" id="rng_te_spd" min="15" max="60" step="1" value="35" oninput="syncVal('te_spd', this.value, ' km/h')">
        <input type="number" id="num_te_spd" value="35" oninput="syncVal('te_spd', this.value, ' km/h')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Continuous Power</span><span id="lbl_te_pwr">1500 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_te_pwr" min="300" max="3000" step="50" value="1500" oninput="syncVal('te_pwr', this.value, ' W')">
        <input type="number" id="num_te_pwr" value="1500" oninput="syncVal('te_pwr', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Peak Pull Power</span><span id="lbl_te_pul">2000 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_te_pul" min="500" max="4000" step="100" value="2000" oninput="syncVal('te_pul', this.value, ' W')">
        <input type="number" id="num_te_pul" value="2000" oninput="syncVal('te_pul', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Battery Current</span><span id="lbl_te_bat">30 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_te_bat" min="10" max="60" step="1" value="30" oninput="syncVal('te_bat', this.value, ' A')">
        <input type="number" id="num_te_bat" value="30" oninput="syncVal('te_bat', this.value, ' A')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Motor Phase Current</span><span id="lbl_te_pha">60 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_te_pha" min="15" max="100" step="1" value="60" oninput="syncVal('te_pha', this.value, ' A')">
        <input type="number" id="num_te_pha" value="60" oninput="syncVal('te_pha', this.value, ' A')">
      </div>
    </div>
  </div>

  <div class="card">
    <div class="card-header sport-title">
      <span>⚡ TUNED SPORT MODE</span>
      <span style="font-size:11px;color:var(--muted)">Dynamic High Power</span>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Speed Limit</span><span id="lbl_ts_spd">55 km/h</span></div>
      <div class="param-controls">
        <input type="range" id="rng_ts_spd" min="25" max="90" step="1" value="55" oninput="syncVal('ts_spd', this.value, ' km/h')">
        <input type="number" id="num_ts_spd" value="55" oninput="syncVal('ts_spd', this.value, ' km/h')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Continuous Power</span><span id="lbl_ts_pwr">3500 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_ts_pwr" min="500" max="8000" step="100" value="3500" oninput="syncVal('ts_pwr', this.value, ' W')">
        <input type="number" id="num_ts_pwr" value="3500" oninput="syncVal('ts_pwr', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Peak Pull Power</span><span id="lbl_ts_pul">5000 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_ts_pul" min="1000" max="10000" step="250" value="5000" oninput="syncVal('ts_pul', this.value, ' W')">
        <input type="number" id="num_ts_pul" value="5000" oninput="syncVal('ts_pul', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Battery Current</span><span id="lbl_ts_bat">55 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_ts_bat" min="20" max="100" step="1" value="55" oninput="syncVal('ts_bat', this.value, ' A')">
        <input type="number" id="num_ts_bat" value="55" oninput="syncVal('ts_bat', this.value, ' A')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Motor Phase Current</span><span id="lbl_ts_pha">110 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_ts_pha" min="25" max="180" step="1" value="110" oninput="syncVal('ts_pha', this.value, ' A')">
        <input type="number" id="num_ts_pha" value="110" oninput="syncVal('ts_pha', this.value, ' A')">
      </div>
    </div>
  </div>

  <div class="card">
    <div class="card-header race-title">
      <span>🔥 TUNED RACE MODE</span>
      <span style="font-size:11px;color:var(--muted)">Full 35kW+ Output</span>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Speed Limit (0 = Unlimited)</span><span id="lbl_tr_spd">85 km/h</span></div>
      <div class="param-controls">
        <input type="range" id="rng_tr_spd" min="0" max="120" step="1" value="85" oninput="syncVal('tr_spd', this.value, ' km/h', true)">
        <input type="number" id="num_tr_spd" min="0" max="150" value="85" oninput="syncVal('tr_spd', this.value, ' km/h', true)">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Continuous Power</span><span id="lbl_tr_pwr">15000 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_tr_pwr" min="1000" max="35000" step="250" value="15000" oninput="syncVal('tr_pwr', this.value, ' W')">
        <input type="number" id="num_tr_pwr" value="15000" oninput="syncVal('tr_pwr', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Peak Pull Power</span><span id="lbl_tr_pul">25000 W</span></div>
      <div class="param-controls">
        <input type="range" id="rng_tr_pul" min="2000" max="35000" step="500" value="25000" oninput="syncVal('tr_pul', this.value, ' W')">
        <input type="number" id="num_tr_pul" value="25000" oninput="syncVal('tr_pul', this.value, ' W')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Battery Current</span><span id="lbl_tr_bat">95 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_tr_bat" min="30" max="150" step="1" value="95" oninput="syncVal('tr_bat', this.value, ' A')">
        <input type="number" id="num_tr_bat" value="95" oninput="syncVal('tr_bat', this.value, ' A')">
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Max Motor Phase Current</span><span id="lbl_tr_pha">220 A</span></div>
      <div class="param-controls">
        <input type="range" id="rng_tr_pha" min="40" max="300" step="5" value="220" oninput="syncVal('tr_pha', this.value, ' A')">
        <input type="number" id="num_tr_pha" value="220" oninput="syncVal('tr_pha', this.value, ' A')">
      </div>
    </div>
  </div>
</div>

<!-- Tab 4: Security, Faults & OTA -->
<div id="tab_settings" class="tab-content">
  <div class="card">
    <div class="card-header" style="color:var(--cyan)">
      <span>🔒 Security & Boot PIN Lockscreen</span>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>Require PIN on Startup</span><span id="lbl_pin_en">Enabled</span></div>
      <div class="param-controls" style="justify-content:flex-start;gap:15px;">
        <input type="checkbox" id="chk_pin_en" style="width:22px;height:22px;accent-color:var(--cyan);" onchange="document.getElementById('lbl_pin_en').textContent = this.checked ? 'Enabled' : 'Disabled'">
        <span style="font-size:12px;color:var(--muted)">Locks display & keeps VESC at 0A until unlocked</span>
      </div>
    </div>
    <div class="param-group">
      <div class="param-lbl"><span>4-Digit PIN Code</span><span id="lbl_pin_code">1234</span></div>
      <div class="param-controls">
        <input type="password" maxlength="4" pattern="[0-9]{4}" id="txt_pin_code" value="1234" style="width:120px;background:var(--surface-card);border:1px solid var(--border);color:#fff;padding:8px 12px;border-radius:6px;font-size:16px;font-weight:700;letter-spacing:4px;text-align:center;" oninput="document.getElementById('lbl_pin_code').textContent = this.value">
        <button type="button" class="btn btn-cyan" style="padding:6px 12px;font-size:11px;" onclick="var p=document.getElementById('txt_pin_code');p.type=(p.type==='password'?'text':'password');">Show/Hide</button>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="card-header" style="color:var(--pink)">
      <span>⚠️ VESC Fault Blackbox Log</span>
      <button type="button" class="btn btn-red" style="padding:4px 10px;font-size:11px;" onclick="clearFaults()">Clear Log</button>
    </div>
    <div id="fault_list" style="font-size:12px;color:var(--muted)">Loading faults...</div>
  </div>

  <div class="card">
    <div class="card-header" style="color:var(--green)">
      <span>📡 Wireless OTA Firmware Update</span>
    </div>
    <p style="font-size:12px;color:var(--muted);margin-bottom:12px;">Flash new firmware (.bin) directly over Wi-Fi without USB cables.</p>
    <form id="otaForm" onsubmit="uploadOta(event)">
      <input type="file" id="otaFile" accept=".bin" style="margin-bottom:10px;font-size:12px;">
      <div class="ota-progress" id="otaProg"><div class="ota-bar" id="otaBar"></div></div>
      <button type="submit" class="btn btn-cyan" style="width:100%" id="btnOta">⚡ Flash Firmware</button>
    </form>
  </div>
</div>

<div class="footer-bar">
  <button class="btn-save" onclick="saveConfiguration()">💾 SAVE & APPLY LIVE TO VESC</button>
</div>

<div id="toast" class="toast">✓ Configuration saved and applied!</div>

<script>
function switchTab(tabId, btn) {
  document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(el => el.classList.remove('active'));
  document.getElementById(tabId).classList.add('active');
  btn.classList.add('active');
}

function syncVal(key, val, unit, allowZeroUnlimited) {
  const rng = document.getElementById('rng_' + key);
  const num = document.getElementById('num_' + key);
  const lbl = document.getElementById('lbl_' + key);
  if (rng) rng.value = val;
  if (num) num.value = val;
  if (lbl) {
    if (allowZeroUnlimited && parseFloat(val) === 0) {
      lbl.textContent = 'Unlimited (0 km/h)';
    } else {
      lbl.textContent = val + unit;
    }
  }
}

function showToast(msg) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.add('show');
  setTimeout(() => t.classList.remove('show'), 2500);
}

function setTuning(enable) {
  fetch('/api/tuning', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ enabled: enable })
  })
  .then(r => r.json())
  .then(data => {
    updateBadge(data.tuning_mode);
    showToast(data.tuning_mode ? '🚀 Tuning Mode Unlocked!' : '🔒 Legal Limits Active!');
  });
}

function updateBadge(unlocked) {
  const b = document.getElementById('statusBadge');
  const txt = document.getElementById('statusText');
  if (unlocked) {
    b.className = 'badge unlocked';
    txt.textContent = 'TUNING UNLOCKED (Custom Profiles Active)';
  } else {
    b.className = 'badge locked';
    txt.textContent = 'LEGAL STREET LIMITS (ECO: 15 / SPORT: 20 / RACE: 22 km/h)';
  }
}

function loadConfig() {
  fetch('/api/config')
  .then(r => r.json())
  .then(cfg => {
    updateBadge(cfg.tuning_mode);

    const le = cfg.legal_eco || {};
    syncVal('le_spd', le.speed !== undefined ? le.speed : 15, ' km/h');
    syncVal('le_pwr', le.power !== undefined ? le.power : 500, ' W');
    syncVal('le_pul', le.pull !== undefined ? le.pull : 1000, ' W');
    syncVal('le_bat', le.bat_a !== undefined ? le.bat_a : 20, ' A');
    syncVal('le_pha', le.pha_a !== undefined ? le.pha_a : 40, ' A');

    const ls = cfg.legal_sport || cfg.stealth || {};
    syncVal('ls_spd', ls.speed !== undefined ? ls.speed : 20, ' km/h');
    syncVal('ls_pwr', ls.power !== undefined ? ls.power : 500, ' W');
    syncVal('ls_pul', ls.pull !== undefined ? ls.pull : 1000, ' W');
    syncVal('ls_bat', ls.bat_a !== undefined ? ls.bat_a : 20, ' A');
    syncVal('ls_pha', ls.pha_a !== undefined ? ls.pha_a : 45, ' A');

    const lr = cfg.legal_race || {};
    syncVal('lr_spd', lr.speed !== undefined ? lr.speed : 22, ' km/h');
    syncVal('lr_pwr', lr.power !== undefined ? lr.power : 500, ' W');
    syncVal('lr_pul', lr.pull !== undefined ? lr.pull : 1000, ' W');
    syncVal('lr_bat', lr.bat_a !== undefined ? lr.bat_a : 22, ' A');
    syncVal('lr_pha', lr.pha_a !== undefined ? lr.pha_a : 50, ' A');

    const te = cfg.tuned_eco || cfg.eco || {};
    syncVal('te_spd', te.speed !== undefined ? te.speed : 35, ' km/h');
    syncVal('te_pwr', te.power !== undefined ? te.power : 1500, ' W');
    syncVal('te_pul', te.pull !== undefined ? te.pull : 2000, ' W');
    syncVal('te_bat', te.bat_a !== undefined ? te.bat_a : 30, ' A');
    syncVal('te_pha', te.pha_a !== undefined ? te.pha_a : 60, ' A');

    const ts = cfg.tuned_sport || cfg.sport || {};
    syncVal('ts_spd', ts.speed !== undefined ? ts.speed : 55, ' km/h');
    syncVal('ts_pwr', ts.power !== undefined ? ts.power : 3500, ' W');
    syncVal('ts_pul', ts.pull !== undefined ? ts.pull : 5000, ' W');
    syncVal('ts_bat', ts.bat_a !== undefined ? ts.bat_a : 55, ' A');
    syncVal('ts_pha', ts.pha_a !== undefined ? ts.pha_a : 110, ' A');

    const tr = cfg.tuned_race || cfg.race || {};
    syncVal('tr_spd', tr.speed !== undefined ? tr.speed : 85, ' km/h', true);
    syncVal('tr_pwr', tr.power !== undefined ? tr.power : 15000, ' W');
    syncVal('tr_pul', tr.pull !== undefined ? tr.pull : 25000, ' W');
    syncVal('tr_bat', tr.bat_a !== undefined ? tr.bat_a : 95, ' A');
    syncVal('tr_pha', tr.pha_a !== undefined ? tr.pha_a : 220, ' A');

    if (cfg.pin_lock_enabled !== undefined) {
      document.getElementById('chk_pin_en').checked = cfg.pin_lock_enabled;
      document.getElementById('lbl_pin_en').textContent = cfg.pin_lock_enabled ? 'Enabled' : 'Disabled';
    }
    if (cfg.pin_code) {
      document.getElementById('txt_pin_code').value = cfg.pin_code;
      document.getElementById('lbl_pin_code').textContent = cfg.pin_code;
    }
    loadFaults();
  })
  .catch(e => console.error('Error loading config:', e));
}

function loadFaults() {
  fetch('/api/faults')
  .then(r => r.json())
  .then(data => {
    const el = document.getElementById('fault_list');
    if (!data.faults || data.faults.length === 0) {
      el.innerHTML = '<span style="color:var(--green)">✓ Keine VESC Fehler aufgezeichnet (Blackbox sauber)</span>';
      return;
    }
    let h = '<div style="display:flex;flex-direction:column;gap:8px;">';
    data.faults.forEach((f, idx) => {
      h += '<div style="background:var(--surface-card);border:1px solid var(--border);border-radius:8px;padding:8px 10px;">';
      h += '<div style="display:flex;justify-content:space-between;font-weight:700;color:var(--pink);margin-bottom:4px;">';
      h += '<span>#' + (idx+1) + ' ' + f.name + '</span><span style="font-size:10px;color:var(--muted)">T+' + f.timestamp_s + 's</span></div>';
      h += '<div style="font-size:11px;color:#fff;margin-bottom:3px;">' + f.desc + '</div>';
      h += '<div style="font-size:10px;color:var(--muted)">' + f.voltage + 'V | ' + f.current + 'A | ' + f.phase_current + 'A Phase | FET: ' + f.temp_fet + '°C | Motor: ' + f.temp_mot + '°C</div>';
      h += '</div>';
    });
    h += '</div>';
    el.innerHTML = h;
  })
  .catch(() => {
    const el = document.getElementById('fault_list');
    if (el) el.textContent = 'Keine Fehlerdaten verfügbar';
  });
}

function clearFaults() {
  if (!confirm('VESC Fehler-Blackbox wirklich leeren?')) return;
  fetch('/api/faults/clear', { method: 'POST' })
  .then(r => r.json())
  .then(() => {
    showToast('✓ VESC Fehler-Speicher gelöscht!');
    loadFaults();
  });
}

function getNumVal(id, fallback) {
  const el = document.getElementById(id);
  if (!el) return fallback;
  const v = parseFloat(el.value);
  return isNaN(v) ? fallback : v;
}

function saveConfiguration() {
  const payload = {
    legal_eco: {
      speed: getNumVal('num_le_spd', 15),
      power: getNumVal('num_le_pwr', 500),
      pull:  getNumVal('num_le_pul', 1000),
      bat_a: getNumVal('num_le_bat', 20),
      pha_a: getNumVal('num_le_pha', 40)
    },
    legal_sport: {
      speed: getNumVal('num_ls_spd', 20),
      power: getNumVal('num_ls_pwr', 500),
      pull:  getNumVal('num_ls_pul', 1000),
      bat_a: getNumVal('num_ls_bat', 20),
      pha_a: getNumVal('num_ls_pha', 45)
    },
    legal_race: {
      speed: getNumVal('num_lr_spd', 22),
      power: getNumVal('num_lr_pwr', 500),
      pull:  getNumVal('num_lr_pul', 1000),
      bat_a: getNumVal('num_lr_bat', 22),
      pha_a: getNumVal('num_lr_pha', 50)
    },
    tuned_eco: {
      speed: getNumVal('num_te_spd', 35),
      power: getNumVal('num_te_pwr', 1500),
      pull:  getNumVal('num_te_pul', 2000),
      bat_a: getNumVal('num_te_bat', 30),
      pha_a: getNumVal('num_te_pha', 60)
    },
    tuned_sport: {
      speed: getNumVal('num_ts_spd', 55),
      power: getNumVal('num_ts_pwr', 3500),
      pull:  getNumVal('num_ts_pul', 5000),
      bat_a: getNumVal('num_ts_bat', 55),
      pha_a: getNumVal('num_ts_pha', 110)
    },
    tuned_race: {
      speed: getNumVal('num_tr_spd', 85),
      power: getNumVal('num_tr_pwr', 15000),
      pull:  getNumVal('num_tr_pul', 25000),
      bat_a: getNumVal('num_tr_bat', 95),
      pha_a: getNumVal('num_tr_pha', 220)
    },
    pin_lock_enabled: document.getElementById('chk_pin_en').checked,
    pin_code: document.getElementById('txt_pin_code').value
  };

  fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  })
  .then(r => r.json())
  .then(res => {
    showToast('✓ Saved to NVS & Applied to VESC!');
  })
  .catch(err => {
    showToast('❌ Error saving settings');
  });
}

function pollTelemetry() {
  fetch('/api/telemetry')
  .then(r => r.json())
  .then(t => {
    document.getElementById('t_spd').textContent = t.speed.toFixed(1);
    document.getElementById('t_pwr').textContent = Math.round(t.power);
    document.getElementById('t_bat').textContent = t.voltage.toFixed(1) + 'V';
    document.getElementById('t_pct').textContent = Math.round(t.battery_pct) + '%';
    document.getElementById('t_pha').textContent = t.phase_current.toFixed(1);
    updateBadge(t.tuning_mode);
  })
  .catch(() => {});
}

function uploadOta(e) {
  e.preventDefault();
  const file = document.getElementById('otaFile').files[0];
  if (!file) { alert('Please select a .bin file first'); return; }
  const btn = document.getElementById('btnOta');
  const prog = document.getElementById('otaProg');
  const bar = document.getElementById('otaBar');
  prog.style.display = 'block';
  btn.disabled = true;
  btn.textContent = 'Flashing...';

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update', true);
  xhr.upload.onprogress = ev => {
    if (ev.lengthComputable) {
      const pct = Math.round((ev.loaded / ev.total) * 100);
      bar.style.width = pct + '%';
      btn.textContent = 'Flashing: ' + pct + '%';
    }
  };
  xhr.onload = () => {
    if (xhr.status === 200 && xhr.responseText.trim() === 'OK') {
      showToast('✓ Flash successful! Rebooting...');
      setTimeout(() => location.reload(), 5000);
    } else {
      alert('Flash failed: ' + xhr.responseText);
      btn.disabled = false;
      btn.textContent = '⚡ Flash Firmware';
      prog.style.display = 'none';
    }
  };
  const formData = new FormData();
  formData.append('update', file);
  xhr.send(formData);
}

window.onload = () => {
  loadConfig();
  setInterval(pollTelemetry, 1000);
};
</script>
</body>
</html>
)rawliteral";

CaptivePortal& CaptivePortal::instance() {
    static CaptivePortal s_instance;
    return s_instance;
}

CaptivePortal::CaptivePortal()
    : _initialized(false) {
    memset(&_telemetry_snapshot, 0, sizeof(_telemetry_snapshot));
}

void CaptivePortal::updateTelemetry(const DashTelemetry &telemetry) {
    _telemetry_snapshot.speed_kmh     = telemetry.speed_kmh;
    _telemetry_snapshot.power_watts   = telemetry.power_watts;
    _telemetry_snapshot.voltage       = telemetry.voltage;
    _telemetry_snapshot.current_amps  = telemetry.current_amps;
    _telemetry_snapshot.phase_amps    = telemetry.phase_amps;
    _telemetry_snapshot.battery_pct   = telemetry.battery_pct;
}

static void handleCaptiveRedirect() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "Redirecting to VESC HyperDash Captive Portal...");
}

static void handleRoot() {
    if (server.hasHeader("Host")) {
        String host = server.header("Host");
        if (host.length() > 0 && !host.startsWith("192.168.4.1") && !host.startsWith("vesc-hyperdash")) {
            handleCaptiveRedirect();
            return;
        }
    }
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send_P(200, "text/html", INDEX_HTML);
}

static void handleGetConfig() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    const VehicleModeConfig &cfg = ModeManager::instance().getConfig();
    char buf[1536];
    snprintf(buf, sizeof(buf),
        "{\"tuning_mode\":%s,\"active_mode\":%d,"
        "\"legal_eco\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"legal_sport\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"legal_race\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"tuned_eco\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"tuned_sport\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"tuned_race\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"throttle_curve\":%d,\"battery_cells\":%d,\"cutoff_start\":%.1f,\"cutoff_end\":%.1f,"
        "\"pin_lock_enabled\":%s,\"pin_code\":\"%s\"}",
        ModeManager::instance().isTuningMode() ? "true" : "false",
        (int)ModeManager::instance().getRideMode(),
        cfg.legal_modes[PROFILE_MODE_ECO].max_speed_kmh, cfg.legal_modes[PROFILE_MODE_ECO].max_power_watts,
        cfg.legal_modes[PROFILE_MODE_ECO].max_pull_watts,
        cfg.legal_modes[PROFILE_MODE_ECO].max_battery_amps, cfg.legal_modes[PROFILE_MODE_ECO].max_phase_amps,
        cfg.legal_modes[PROFILE_MODE_SPORT].max_speed_kmh, cfg.legal_modes[PROFILE_MODE_SPORT].max_power_watts,
        cfg.legal_modes[PROFILE_MODE_SPORT].max_pull_watts,
        cfg.legal_modes[PROFILE_MODE_SPORT].max_battery_amps, cfg.legal_modes[PROFILE_MODE_SPORT].max_phase_amps,
        cfg.legal_modes[PROFILE_MODE_RACE].max_speed_kmh, cfg.legal_modes[PROFILE_MODE_RACE].max_power_watts,
        cfg.legal_modes[PROFILE_MODE_RACE].max_pull_watts,
        cfg.legal_modes[PROFILE_MODE_RACE].max_battery_amps, cfg.legal_modes[PROFILE_MODE_RACE].max_phase_amps,
        cfg.tuned_modes[PROFILE_MODE_ECO].max_speed_kmh, cfg.tuned_modes[PROFILE_MODE_ECO].max_power_watts,
        cfg.tuned_modes[PROFILE_MODE_ECO].max_pull_watts,
        cfg.tuned_modes[PROFILE_MODE_ECO].max_battery_amps, cfg.tuned_modes[PROFILE_MODE_ECO].max_phase_amps,
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_speed_kmh, cfg.tuned_modes[PROFILE_MODE_SPORT].max_power_watts,
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_pull_watts,
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_battery_amps, cfg.tuned_modes[PROFILE_MODE_SPORT].max_phase_amps,
        cfg.tuned_modes[PROFILE_MODE_RACE].max_speed_kmh, cfg.tuned_modes[PROFILE_MODE_RACE].max_power_watts,
        cfg.tuned_modes[PROFILE_MODE_RACE].max_pull_watts,
        cfg.tuned_modes[PROFILE_MODE_RACE].max_battery_amps, cfg.tuned_modes[PROFILE_MODE_RACE].max_phase_amps,
        (int)cfg.throttle_curve, (int)cfg.battery_cells,
        cfg.battery_cutoff_start, cfg.battery_cutoff_end,
        Settings.get().pin_lock_enabled ? "true" : "false",
        Settings.get().pin_code
    );
    server.send(200, "application/json", buf);
}

// Lightweight JSON field extraction
static float parseJsonFloat(const String &body, const char *key, float fallback) {
    int idx = body.indexOf(key);
    if (idx < 0) return fallback;
    idx = body.indexOf(':', idx);
    if (idx < 0) return fallback;
    idx++;
    while (idx < (int)body.length() && (body[idx] == ' ' || body[idx] == '\"')) idx++;
    if (idx >= (int)body.length()) return fallback;
    return body.substring(idx).toFloat();
}

static int parseJsonInt(const String &body, const char *key, int fallback) {
    int idx = body.indexOf(key);
    if (idx < 0) return fallback;
    idx = body.indexOf(':', idx);
    if (idx < 0) return fallback;
    idx++;
    while (idx < (int)body.length() && (body[idx] == ' ' || body[idx] == '\"')) idx++;
    if (idx >= (int)body.length()) return fallback;
    return body.substring(idx).toInt();
}

static String extractJsonObject(const String &src, const char *key) {
    int idx = src.indexOf(key);
    if (idx < 0) return "";
    int b_start = src.indexOf('{', idx);
    if (b_start < 0) return "";
    int b_end = src.indexOf('}', b_start);
    if (b_end < 0) return "";
    return src.substring(b_start, b_end + 1);
}

static void handlePostConfig() {
    String body = server.arg("plain");
    if (body.length() < 10) {
        server.send(400, "application/json", "{\"error\":\"Empty or invalid payload\"}");
        return;
    }
    VehicleModeConfig &cfg = ModeManager::instance().getConfig();

    // Legal ECO
    String le_part = extractJsonObject(body, "\"legal_eco\"");
    if (le_part.length() > 0) {
        cfg.legal_modes[PROFILE_MODE_ECO].max_speed_kmh    = parseJsonFloat(le_part, "\"speed\"", cfg.legal_modes[PROFILE_MODE_ECO].max_speed_kmh);
        cfg.legal_modes[PROFILE_MODE_ECO].max_power_watts  = parseJsonFloat(le_part, "\"power\"", cfg.legal_modes[PROFILE_MODE_ECO].max_power_watts);
        cfg.legal_modes[PROFILE_MODE_ECO].max_pull_watts   = parseJsonFloat(le_part, "\"pull\"", cfg.legal_modes[PROFILE_MODE_ECO].max_pull_watts);
        cfg.legal_modes[PROFILE_MODE_ECO].max_battery_amps = parseJsonFloat(le_part, "\"bat_a\"", cfg.legal_modes[PROFILE_MODE_ECO].max_battery_amps);
        cfg.legal_modes[PROFILE_MODE_ECO].max_phase_amps   = parseJsonFloat(le_part, "\"pha_a\"", cfg.legal_modes[PROFILE_MODE_ECO].max_phase_amps);
    }

    // Legal SPORT
    String ls_part = extractJsonObject(body, "\"legal_sport\"");
    if (ls_part.length() == 0) ls_part = extractJsonObject(body, "\"stealth\"");
    if (ls_part.length() > 0) {
        cfg.legal_modes[PROFILE_MODE_SPORT].max_speed_kmh    = parseJsonFloat(ls_part, "\"speed\"", cfg.legal_modes[PROFILE_MODE_SPORT].max_speed_kmh);
        cfg.legal_modes[PROFILE_MODE_SPORT].max_power_watts  = parseJsonFloat(ls_part, "\"power\"", cfg.legal_modes[PROFILE_MODE_SPORT].max_power_watts);
        cfg.legal_modes[PROFILE_MODE_SPORT].max_pull_watts   = parseJsonFloat(ls_part, "\"pull\"", cfg.legal_modes[PROFILE_MODE_SPORT].max_pull_watts);
        cfg.legal_modes[PROFILE_MODE_SPORT].max_battery_amps = parseJsonFloat(ls_part, "\"bat_a\"", cfg.legal_modes[PROFILE_MODE_SPORT].max_battery_amps);
        cfg.legal_modes[PROFILE_MODE_SPORT].max_phase_amps   = parseJsonFloat(ls_part, "\"pha_a\"", cfg.legal_modes[PROFILE_MODE_SPORT].max_phase_amps);
    }

    // Legal RACE
    String lr_part = extractJsonObject(body, "\"legal_race\"");
    if (lr_part.length() > 0) {
        cfg.legal_modes[PROFILE_MODE_RACE].max_speed_kmh    = parseJsonFloat(lr_part, "\"speed\"", cfg.legal_modes[PROFILE_MODE_RACE].max_speed_kmh);
        cfg.legal_modes[PROFILE_MODE_RACE].max_power_watts  = parseJsonFloat(lr_part, "\"power\"", cfg.legal_modes[PROFILE_MODE_RACE].max_power_watts);
        cfg.legal_modes[PROFILE_MODE_RACE].max_pull_watts   = parseJsonFloat(lr_part, "\"pull\"", cfg.legal_modes[PROFILE_MODE_RACE].max_pull_watts);
        cfg.legal_modes[PROFILE_MODE_RACE].max_battery_amps = parseJsonFloat(lr_part, "\"bat_a\"", cfg.legal_modes[PROFILE_MODE_RACE].max_battery_amps);
        cfg.legal_modes[PROFILE_MODE_RACE].max_phase_amps   = parseJsonFloat(lr_part, "\"pha_a\"", cfg.legal_modes[PROFILE_MODE_RACE].max_phase_amps);
    }

    // Tuned ECO
    String te_part = extractJsonObject(body, "\"tuned_eco\"");
    if (te_part.length() == 0) te_part = extractJsonObject(body, "\"eco\"");
    if (te_part.length() > 0) {
        cfg.tuned_modes[PROFILE_MODE_ECO].max_speed_kmh    = parseJsonFloat(te_part, "\"speed\"", cfg.tuned_modes[PROFILE_MODE_ECO].max_speed_kmh);
        cfg.tuned_modes[PROFILE_MODE_ECO].max_power_watts  = parseJsonFloat(te_part, "\"power\"", cfg.tuned_modes[PROFILE_MODE_ECO].max_power_watts);
        cfg.tuned_modes[PROFILE_MODE_ECO].max_pull_watts   = parseJsonFloat(te_part, "\"pull\"", cfg.tuned_modes[PROFILE_MODE_ECO].max_pull_watts);
        cfg.tuned_modes[PROFILE_MODE_ECO].max_battery_amps = parseJsonFloat(te_part, "\"bat_a\"", cfg.tuned_modes[PROFILE_MODE_ECO].max_battery_amps);
        cfg.tuned_modes[PROFILE_MODE_ECO].max_phase_amps   = parseJsonFloat(te_part, "\"pha_a\"", cfg.tuned_modes[PROFILE_MODE_ECO].max_phase_amps);
    }

    // Tuned SPORT
    String ts_part = extractJsonObject(body, "\"tuned_sport\"");
    if (ts_part.length() == 0) ts_part = extractJsonObject(body, "\"sport\"");
    if (ts_part.length() > 0) {
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_speed_kmh    = parseJsonFloat(ts_part, "\"speed\"", cfg.tuned_modes[PROFILE_MODE_SPORT].max_speed_kmh);
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_power_watts  = parseJsonFloat(ts_part, "\"power\"", cfg.tuned_modes[PROFILE_MODE_SPORT].max_power_watts);
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_pull_watts   = parseJsonFloat(ts_part, "\"pull\"", cfg.tuned_modes[PROFILE_MODE_SPORT].max_pull_watts);
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_battery_amps = parseJsonFloat(ts_part, "\"bat_a\"", cfg.tuned_modes[PROFILE_MODE_SPORT].max_battery_amps);
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_phase_amps   = parseJsonFloat(ts_part, "\"pha_a\"", cfg.tuned_modes[PROFILE_MODE_SPORT].max_phase_amps);
    }

    // Tuned RACE
    String tr_part = extractJsonObject(body, "\"tuned_race\"");
    if (tr_part.length() == 0) tr_part = extractJsonObject(body, "\"race\"");
    if (tr_part.length() > 0) {
        cfg.tuned_modes[PROFILE_MODE_RACE].max_speed_kmh    = parseJsonFloat(tr_part, "\"speed\"", cfg.tuned_modes[PROFILE_MODE_RACE].max_speed_kmh);
        cfg.tuned_modes[PROFILE_MODE_RACE].max_power_watts  = parseJsonFloat(tr_part, "\"power\"", cfg.tuned_modes[PROFILE_MODE_RACE].max_power_watts);
        cfg.tuned_modes[PROFILE_MODE_RACE].max_pull_watts   = parseJsonFloat(tr_part, "\"pull\"", cfg.tuned_modes[PROFILE_MODE_RACE].max_pull_watts);
        cfg.tuned_modes[PROFILE_MODE_RACE].max_battery_amps = parseJsonFloat(tr_part, "\"bat_a\"", cfg.tuned_modes[PROFILE_MODE_RACE].max_battery_amps);
        cfg.tuned_modes[PROFILE_MODE_RACE].max_phase_amps   = parseJsonFloat(tr_part, "\"pha_a\"", cfg.tuned_modes[PROFILE_MODE_RACE].max_phase_amps);
    }

    // Global settings (optional fallbacks)
    if (body.indexOf("\"throttle_curve\"") >= 0) {
        cfg.throttle_curve       = (int8_t)parseJsonInt(body, "\"throttle_curve\"", cfg.throttle_curve);
    }
    if (body.indexOf("\"battery_cells\"") >= 0) {
        cfg.battery_cells        = (uint8_t)parseJsonInt(body, "\"battery_cells\"", cfg.battery_cells);
    }
    if (body.indexOf("\"cutoff_start\"") >= 0) {
        cfg.battery_cutoff_start = parseJsonFloat(body, "\"cutoff_start\"", cfg.battery_cutoff_start);
    }
    if (body.indexOf("\"cutoff_end\"") >= 0) {
        cfg.battery_cutoff_end   = parseJsonFloat(body, "\"cutoff_end\"", cfg.battery_cutoff_end);
    }

    // PIN lock settings
    int pin_en_idx = body.indexOf("\"pin_lock_enabled\"");
    if (pin_en_idx >= 0) {
        int c_idx = body.indexOf(':', pin_en_idx);
        if (c_idx >= 0) {
            while (c_idx < (int)body.length() && (body[c_idx] == ':' || body[c_idx] == ' ')) c_idx++;
            Settings.get().pin_lock_enabled = (body[c_idx] == 't' || body[c_idx] == 'T' || body[c_idx] == '1');
        }
    }
    int pin_code_idx = body.indexOf("\"pin_code\"");
    if (pin_code_idx >= 0) {
        int q1 = body.indexOf('\"', pin_code_idx + 10);
        if (q1 >= 0) {
            int q2 = body.indexOf('\"', q1 + 1);
            if (q2 > q1) {
                String code = body.substring(q1 + 1, q2);
                if (code.length() == 4) {
                    bool all_digits = true;
                    for (int i = 0; i < 4; i++) {
                        if (code[i] < '0' || code[i] > '9') {
                            all_digits = false;
                            break;
                        }
                    }
                    if (all_digits) {
                        strncpy(Settings.get().pin_code, code.c_str(), sizeof(Settings.get().pin_code) - 1);
                        Settings.get().pin_code[sizeof(Settings.get().pin_code) - 1] = '\0';
                    }
                }
            }
        }
    }
    Settings.save();

    ModeManager::instance().saveToNvs();
    ModeManager::instance().applyLimitsToVesc();

    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void handleGetFaults() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    uint8_t count = FaultManager::instance().getFaultCount();
    String json = "{\"fault_count\":" + String(count) + ",\"faults\":[";
    for (uint8_t i = 0; i < count; i++) {
        FaultRecord rec = FaultManager::instance().getFault(i);
        if (i > 0) json += ",";
        json += "{\"code\":" + String(rec.code) +
                ",\"name\":\"" + String(FaultManager::getFaultTitleDe(rec.code)) + "\"" +
                ",\"desc\":\"" + String(FaultManager::getFaultDescriptionDe(rec.code)) + "\"" +
                ",\"voltage\":" + String(rec.voltage, 1) +
                ",\"current\":" + String(rec.current, 1) +
                ",\"phase_current\":" + String(rec.phase_current, 1) +
                ",\"temp_fet\":" + String(rec.temp_fet, 1) +
                ",\"temp_mot\":" + String(rec.temp_mot, 1) +
                ",\"timestamp_s\":" + String(rec.timestamp_s) + "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

static void handleClearFaults() {
    FaultManager::instance().clearFaults();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void handleGetTelemetry() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    char buf[256];
    PortalTelemetrySnapshot t = CaptivePortal::instance().getTelemetrySnapshot();
    ModeLimits lim = ModeManager::instance().getActiveLimits();
    snprintf(buf, sizeof(buf),
        "{\"speed\":%.1f,\"power\":%.0f,\"voltage\":%.1f,\"current\":%.1f,\"phase_current\":%.1f,\"battery_pct\":%.0f,\"tuning_mode\":%s,\"ride_mode\":%d,\"mode_name\":\"%s\",\"max_speed\":%.1f}",
        t.speed_kmh, t.power_watts, t.voltage, t.current_amps, t.phase_amps, t.battery_pct,
        ModeManager::instance().isTuningMode() ? "true" : "false",
        (int)ModeManager::instance().getRideMode(),
        ModeManager::instance().getActiveModeName(),
        lim.max_speed_kmh
    );
    server.send(200, "application/json", buf);
}

static void handlePostTuning() {
    String body = server.arg("plain");
    bool enable = false;
    int idx = body.indexOf("\"enabled\"");
    if (idx >= 0) {
        idx = body.indexOf(':', idx);
        if (idx >= 0) {
            while (idx < (int)body.length() && (body[idx] == ':' || body[idx] == ' ' || body[idx] == '\"')) idx++;
            if (idx < (int)body.length()) {
                enable = (body[idx] == 't' || body[idx] == 'T' || body[idx] == '1');
            }
        }
    } else {
        enable = (body.indexOf("true") >= 0);
    }
    ModeManager::instance().setTuningMode(enable);
    ModeManager::instance().applyLimitsToVesc();
    char buf[96];
    ModeLimits lim = ModeManager::instance().getActiveLimits();
    snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"tuning_mode\":%s,\"mode_name\":\"%s\",\"max_speed\":%.1f}",
             enable ? "true" : "false", ModeManager::instance().getActiveModeName(), lim.max_speed_kmh);
    server.send(200, "application/json", buf);
}

void CaptivePortal::core0TaskEntry(void *param) {
    CaptivePortal *portal = (CaptivePortal*)param;
    portal->core0Loop();
}

void CaptivePortal::core0Loop() {
    Serial.println("[CORE 0] Captive Portal task running on Core 0.");

    // Initial limit push on Core 0 boot
    ModeManager::instance().applyLimitsToVesc();

    while (true) {
        dnsServer.processNextRequest();
        server.handleClient();

        // Check for deferred limit pushes to VESC
        if (ModeManager::instance().checkAndClearLimitsDirty()) {
            ModeManager::instance().applyLimitsToVesc();
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void CaptivePortal::begin() {
    if (_initialized) return;

    // 1. SoftAP Setup
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, netMsk);
    bool ap_ok = WiFi.softAP("VESC-HyperDash");
    Serial.printf("[WIFI_AP] SoftAP %s: SSID='VESC-HyperDash', IP=192.168.4.1\n",
                  ap_ok ? "Started" : "Failed");

    // 2. DNS Server (Captive Portal: Redirect all domains to 192.168.4.1)
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", apIP);
    Serial.println("[DNS] DNS Server listening on port 53 (Wildcard Captive Portal).");

    const char *headerkeys[] = { "Host" };
    server.collectHeaders(headerkeys, 1);

    // 3. Web Server Routes
    server.on("/", HTTP_GET, handleRoot);

    // Captive Portal Probe endpoints
    server.on("/generate_204", HTTP_GET, handleCaptiveRedirect); // Android
    server.on("/gen_204", HTTP_GET, handleCaptiveRedirect);      // Android
    server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirect); // Apple iOS / macOS
    server.on("/canonical.html", HTTP_GET, handleCaptiveRedirect);
    server.on("/ncsi.txt", HTTP_GET, handleCaptiveRedirect);    // Windows Captive Portal Trigger
    server.on("/connecttest.txt", HTTP_GET, handleCaptiveRedirect);
    server.on("/redirect", HTTP_GET, handleCaptiveRedirect);

    // REST APIs
    server.on("/api/config", HTTP_GET, handleGetConfig);
    server.on("/api/config", HTTP_POST, handlePostConfig);
    server.on("/api/telemetry", HTTP_GET, handleGetTelemetry);
    server.on("/api/tuning", HTTP_POST, handlePostTuning);
    server.on("/api/faults", HTTP_GET, handleGetFaults);
    server.on("/api/faults/clear", HTTP_POST, handleClearFaults);

    // OTA Update Handler
    server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        if (Update.hasError()) {
            server.send(500, "text/plain", "FAIL");
        } else {
            server.send(200, "text/plain", "OK");
            delay(500);
            ESP.restart();
        }
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("[OTA] Update Start: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("[OTA] Update Success: %u bytes\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
            Update.abort();
            Serial.println("[OTA] Update Aborted");
        }
    });

    server.onNotFound(handleCaptiveRedirect);
    server.begin();
    Serial.println("[HTTP] WebServer listening on port 80.");

    // 4. Pin Core 0 FreeRTOS Worker Task
    xTaskCreatePinnedToCore(
        core0TaskEntry,
        "Core0_Portal",
        8192,
        this,
        1,
        &s_portalTaskHandle,
        0 // Core 0
    );

    _initialized = true;
}

#else
// Simulator Stubs
CaptivePortal& CaptivePortal::instance() {
    static CaptivePortal s_instance;
    return s_instance;
}
CaptivePortal::CaptivePortal() : _initialized(false) {
    memset(&_telemetry_snapshot, 0, sizeof(_telemetry_snapshot));
}
void CaptivePortal::begin() {}
void CaptivePortal::updateTelemetry(const DashTelemetry &telemetry) {
    _telemetry_snapshot.speed_kmh     = telemetry.speed_kmh;
    _telemetry_snapshot.power_watts   = telemetry.power_watts;
    _telemetry_snapshot.voltage       = telemetry.voltage;
    _telemetry_snapshot.current_amps  = telemetry.current_amps;
    _telemetry_snapshot.phase_amps    = telemetry.phase_amps;
    _telemetry_snapshot.battery_pct   = telemetry.battery_pct;
}
#endif
