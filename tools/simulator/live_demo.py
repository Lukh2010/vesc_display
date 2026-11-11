#!/usr/bin/env python3
"""
VESC Wide Display (800x480) - Pure Display Simulator
Waveshare ESP32-S3-Touch-LCD-4.3

Minimalist, clean display-only emulator:
- 100% focused on the 800x480 screen without external buttons, panels, or clutter
- Direct GT911 capacitive touch emulation (mouse drag/swipe and taps on the screen)
- Native 30 FPS LovyanGFX C++ engine
- Web Stream on http://localhost:8080 (clean centered display)
- Desktop GUI on DISPLAY=:0 (clean 800x480 window)
"""

import sys
import os
import time
import io
import ctypes
import threading
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import urllib.parse
import json

from PIL import Image, ImageTk
import tkinter as tk

# -------------------------------------------------------------
# C++ Simulator Backend (ctypes)
# -------------------------------------------------------------
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LIB_PATH = os.path.join(SCRIPT_DIR, "libdash_sim.so")

if not os.path.exists(LIB_PATH):
    print(f"[ERROR] Shared library {LIB_PATH} not found!")
    sys.exit(1)

sim_lib = ctypes.CDLL(LIB_PATH)

class SimTelemetry(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("speed_kmh", ctypes.c_float),
        ("duty_cycle_pct", ctypes.c_float),
        ("throttle_pct", ctypes.c_float),
        ("voltage", ctypes.c_float),
        ("current_amps", ctypes.c_float),
        ("phase_amps", ctypes.c_float),
        ("power_watts", ctypes.c_float),
        ("battery_pct", ctypes.c_float),
        ("remaining_wh", ctypes.c_float),
        ("est_range_km", ctypes.c_float),
        ("temp_motor", ctypes.c_float),
        ("temp_esc", ctypes.c_float),
        ("trip_km", ctypes.c_float),
        ("odo_km", ctypes.c_float),
        ("screen", ctypes.c_int32),
        ("dash_style", ctypes.c_int32),
        ("auto_mode", ctypes.c_int32),
        ("ride_mode", ctypes.c_int32),
        ("boost_active", ctypes.c_int32),
        ("hazard_active", ctypes.c_int32),
        ("light_mode", ctypes.c_int32),
        ("light_hue", ctypes.c_int32),
        ("temp_fet_rear", ctypes.c_float),
        ("temp_mot_rear", ctypes.c_float),
        ("temp_fet_front", ctypes.c_float),
        ("temp_mot_front", ctypes.c_float),
        ("max_speed_kmh", ctypes.c_float),
        ("avg_speed_kmh", ctypes.c_float),
        ("ride_time_sec", ctypes.c_uint32),
        ("tuning_mode", ctypes.c_int32),
        ("is_locked", ctypes.c_int32),
        ("race_state", ctypes.c_int32),
        ("race_elapsed_sec", ctypes.c_float),
        ("race_split_0_22", ctypes.c_float),
        ("race_split_0_50", ctypes.c_float),
        ("race_split_0_70", ctypes.c_float),
        ("g_force", ctypes.c_float),
        ("incline_deg", ctypes.c_float),
    ]

sim_lib.sim_init.restype = None
sim_lib.sim_step.argtypes = [ctypes.c_uint32]
sim_lib.sim_step.restype = None

sim_lib.sim_touch_event.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
sim_lib.sim_touch_event.restype = None

sim_lib.sim_trigger_nav.argtypes = [ctypes.c_int]
sim_lib.sim_trigger_nav.restype = None

sim_lib.sim_toggle_boost.restype = None
sim_lib.sim_cycle_mode.restype = None
sim_lib.sim_toggle_hazard.restype = None
sim_lib.sim_toggle_lights.restype = None

sim_lib.sim_set_tuning_mode.argtypes = [ctypes.c_int]
sim_lib.sim_set_tuning_mode.restype = None
sim_lib.sim_toggle_tuning_mode.restype = None

sim_lib.sim_set_auto_mode.argtypes = [ctypes.c_int]
sim_lib.sim_set_auto_mode.restype = None

sim_lib.sim_toggle_auto_mode.restype = None

sim_lib.sim_set_manual_throttle.argtypes = [ctypes.c_float]
sim_lib.sim_set_manual_throttle.restype = None

sim_lib.sim_set_manual_brake.argtypes = [ctypes.c_float]
sim_lib.sim_set_manual_brake.restype = None

sim_lib.sim_get_frame_rgb24.argtypes = [ctypes.c_char_p]
sim_lib.sim_get_frame_rgb24.restype = None

sim_lib.sim_get_telemetry.argtypes = [ctypes.POINTER(SimTelemetry)]
sim_lib.sim_get_telemetry.restype = None

sim_lib.sim_get_config_json.argtypes = [ctypes.c_char_p, ctypes.c_int]
sim_lib.sim_get_config_json.restype = ctypes.c_int

sim_lib.sim_set_config_json.argtypes = [ctypes.c_char_p]
sim_lib.sim_set_config_json.restype = ctypes.c_int

# Navigation Constants (Matches include/dash_types.h NavAction enum)
NAV_NONE = 0
NAV_BTN1_SHORT = 1    # Prev screen (Swipe Right) / Menu Up
NAV_BTN1_LONG = 2     # Settings Menu
NAV_BTN2_SHORT = 3    # Next screen (Swipe Left) / Menu Down
NAV_BTN2_LONG = 4     # Long press (Trip Reset)
NAV_EDIT_DEC = 5
NAV_EDIT_INC = 6
NAV_BOTH_PRESSED = 7  # Select / Enter / Edit
NAV_BOOST_TOGGLE = 8
NAV_MODE_CYCLE = 9
NAV_HAZARD_TOGGLE = 10
NAV_SCREEN_STATS = 11
NAV_TUNING_ENABLE = 12
NAV_TUNING_DISABLE = 13
NAV_SCREEN_DASH = 14
NAV_MENU_BACK = 15
NAV_SCREEN_RACE = 16
NAV_RACE_TOGGLE_POPUP = 17
NAV_RACE_CLOSE_POPUP = 18
NAV_RACE_RESET_RECORDS = 19
NAV_RACE_START_LAUNCH = 20
NAV_RACE_ABORT = 21
NAV_LIGHT_TOGGLE = 22

SCREEN_RIDE_DASH = 0
SCREEN_LIGHTS = 1
SCREEN_RT_GRAPHS = 2
SCREEN_SETTINGS_MENU = 3
SCREEN_VESC_BRIDGE = 4
SCREEN_WIFI_BRIDGE = 5
SCREEN_RACE_TIMER = 6

# Initialize backend
sim_lib.sim_init()

LCD_W = 800
LCD_H = 480
rgb_buf = bytearray(LCD_W * LCD_H * 3)
c_rgb_buf = (ctypes.c_char * len(rgb_buf)).from_buffer(rgb_buf)
current_telemetry = SimTelemetry()

sim_lock = threading.Lock()
latest_jpeg_frame = None
latest_jpeg_lock = threading.Lock()
latest_pil_image = None
latest_pil_lock = threading.Lock()
sim_running = True

def step_simulation(elapsed_ms=33):
    global latest_jpeg_frame, latest_pil_image
    with sim_lock:
        sim_lib.sim_step(elapsed_ms)
        sim_lib.sim_get_frame_rgb24(c_rgb_buf)
        sim_lib.sim_get_telemetry(ctypes.byref(current_telemetry))
        img = Image.frombytes("RGB", (LCD_W, LCD_H), bytes(rgb_buf))
        bio = io.BytesIO()
        img.save(bio, format="JPEG", quality=98, subsampling=0)
        with latest_jpeg_lock:
            latest_jpeg_frame = bio.getvalue()
        with latest_pil_lock:
            latest_pil_image = img
    return img

def sim_worker_loop():
    last_time = time.time()
    while sim_running:
        now = time.time()
        elapsed_ms = int((now - last_time) * 1000)
        last_time = now
        if elapsed_ms < 10:
            elapsed_ms = 10
        if elapsed_ms > 100:
            elapsed_ms = 100
        step_simulation(elapsed_ms)
        time.sleep(0.016)

initial_image = step_simulation(16)

# -------------------------------------------------------------
# Embedded Web Server (Port 8080) - Clean Display Only
# -------------------------------------------------------------
HTML_PAGE = """<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>VESC 4.3" Display (800x480)</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: #000000;
    color: #e0e6ed;
    display: flex;
    flex-direction: column;
    justify-content: center;
    align-items: center;
    min-height: 100vh;
    overflow: hidden;
    user-select: none;
    -webkit-user-select: none;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  }
  .screen-wrap {
    position: relative;
    width: 800px;
    height: 480px;
    background: #000000;
    border-radius: 6px;
    box-shadow: 0 0 50px rgba(0, 229, 255, 0.12);
    overflow: hidden;
    cursor: crosshair;
  }
  #stream-img {
    width: 800px;
    height: 480px;
    display: block;
    pointer-events: none;
  }
  .touch-overlay {
    position: absolute;
    top: 0; left: 0;
    width: 100%; height: 100%;
    z-index: 10;
    touch-action: none;
  }
  .control-hints {
    margin-top: 14px;
    font-size: 11px;
    color: #4b5d73;
    letter-spacing: 0.4px;
    display: flex;
    gap: 14px;
    text-transform: uppercase;
  }
  .control-hints kbd {
    background: #0f172a;
    border: 1px solid #1e293b;
    padding: 2px 6px;
    border-radius: 4px;
    color: #00e5ff;
    font-family: monospace;
    font-size: 11px;
  }
</style>
</head>
<body>

<div class="screen-wrap" id="screen-wrap">
  <img id="stream-img" src="/stream.mjpg" alt="VESC Screen">
  <div class="touch-overlay" id="touch-layer"></div>
</div>
<div class="control-hints">
  <span><kbd>▲ / W</kbd> Gasgeben</span>
  <span><kbd>Loslassen</kbd> Ausrollen</span>
  <span><kbd>▼ / S</kbd> Bremse</span>
  <span><kbd>M</kbd> Mode</span>
  <span><kbd>Space / B</kbd> Boost</span>
  <span><kbd>L</kbd> Licht (Hauptschalter)</span>
  <span><kbd>X</kbd> Tuning / Legal</span>
  <span><a href="/portal" target="_blank" style="color:#00e5ff; text-decoration:underline; font-weight:bold;">⚙️ Captive Portal</a> (<a href="http://localhost:8081" target="_blank" style="color:#00e5ff;">Port 8081</a>)</span>
</div>

<script>
  let isDown = false;
  const touchLayer = document.getElementById('touch-layer');
  let pendingTouch = null;
  let rafId = null;

  function sendTouch(down, x, y) {
    fetch(`/api/touch?down=${down}&x=${Math.round(x)}&y=${Math.round(y)}`).catch(() => {});
  }

  function queueTouch(down, x, y) {
    pendingTouch = {down, x, y};
    if (!rafId) {
      rafId = requestAnimationFrame(() => {
        rafId = null;
        if (pendingTouch) {
          sendTouch(pendingTouch.down, pendingTouch.x, pendingTouch.y);
          pendingTouch = null;
        }
      });
    }
  }

  touchLayer.addEventListener('pointerdown', (e) => {
    isDown = true;
    touchLayer.setPointerCapture(e.pointerId);
    const rect = touchLayer.getBoundingClientRect();
    const x = (e.clientX - rect.left) * (800 / rect.width);
    const y = (e.clientY - rect.top) * (480 / rect.height);
    sendTouch(1, x, y);
  });

  touchLayer.addEventListener('pointermove', (e) => {
    if (!isDown) return;
    const rect = touchLayer.getBoundingClientRect();
    const x = (e.clientX - rect.left) * (800 / rect.width);
    const y = (e.clientY - rect.top) * (480 / rect.height);
    queueTouch(1, x, y);
  });

  function releaseTouch(e) {
    if (!isDown) return;
    isDown = false;
    const rect = touchLayer.getBoundingClientRect();
    const x = (e.clientX - rect.left) * (800 / rect.width);
    const y = (e.clientY - rect.top) * (480 / rect.height);
    sendTouch(0, x, y);
  }

  touchLayer.addEventListener('pointerup', releaseTouch);
  touchLayer.addEventListener('pointercancel', releaseTouch);

  // Realistic Spring-Return Throttle & Controls
  let throttleHeld = false;
  let brakeHeld = false;

  window.addEventListener('keydown', (e) => {
    if (e.repeat) return;
    if (e.key === 'ArrowUp' || e.key === 'w' || e.key === 'W') {
      throttleHeld = true;
      fetch('/api/throttle?val=100');
    } else if (e.key === 'ArrowDown' || e.key === 's' || e.key === 'S') {
      brakeHeld = true;
      fetch('/api/brake?val=75');
    } else if (e.key === 'ArrowRight') {
      fetch('/api/nav?action=3');
    } else if (e.key === 'ArrowLeft') {
      fetch('/api/nav?action=1');
    } else if (e.key === ' ' || e.key === 'b' || e.key === 'B') {
      fetch('/api/nav?action=8');
    } else if (e.key === 'm' || e.key === 'M') {
      fetch('/api/nav?action=9');
    } else if (e.key === 'h' || e.key === 'H') {
      fetch('/api/nav?action=10');
    } else if (e.key === 't' || e.key === 'T') {
      fetch('/api/nav?action=11');
    } else if (e.key === 'r' || e.key === 'R') {
      fetch('/api/nav?action=16');
    } else if (e.key === 'x' || e.key === 'X') {
      fetch('/api/tuning?toggle=1');
    }
  });

  window.addEventListener('keyup', (e) => {
    if (e.key === 'ArrowUp' || e.key === 'w' || e.key === 'W') {
      if (throttleHeld) {
        throttleHeld = false;
        fetch('/api/throttle?val=0');
      }
    } else if (e.key === 'ArrowDown' || e.key === 's' || e.key === 'S') {
      if (brakeHeld) {
        brakeHeld = false;
        fetch('/api/brake?val=0');
      }
    }
  });
</script>
</body>
</html>
"""

PORTAL_HTML_CACHE = None

def get_captive_portal_html():
    return """<!DOCTYPE html><html><head><meta charset="utf-8"><title>HyperDash Autark</title>
    <style>body{background:#0a0c10;color:#fff;font-family:sans-serif;text-align:center;padding:50px;}</style></head>
    <body><h2>VESC Touch Display (100% Autark)</h2>
    <p style="color:#00e5ff;">Das HyperDash-Webportal wurde entfernt. Alle Einstellungen (Modi, PIN, Dragy-Splits) erfolgen jetzt direkt & autark am Touch-Display!</p>
    <p><a href="http://localhost:8080" style="color:#00ff88;text-decoration:none;font-weight:bold;">Zurueck zum Live Display Stream</a></p></body></html>"""

class WebDemoHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Silence HTTP access logs to keep console clean
        return

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        params = urllib.parse.parse_qs(parsed.query)

        if path == "/" or path == "/index.html":
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(HTML_PAGE.encode("utf-8"))

        elif path in ["/portal", "/portal.html"]:
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.end_headers()
            self.wfile.write(get_captive_portal_html().encode("utf-8"))

        elif path == "/stream.mjpg":
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-cache, private")
            self.send_header("Pragma", "no-cache")
            self.end_headers()

            try:
                while True:
                    with latest_jpeg_lock:
                        frame = latest_jpeg_frame
                    if frame:
                        self.wfile.write(b"--frame\r\n")
                        self.wfile.write(b"Content-Type: image/jpeg\r\n\r\n")
                        self.wfile.write(frame)
                        self.wfile.write(b"\r\n")
                    time.sleep(0.033)
            except (BrokenPipeError, ConnectionResetError):
                pass

        elif path == "/api/touch":
            down = int(params.get("down", [0])[0])
            x = int(params.get("x", [0])[0])
            y = int(params.get("y", [0])[0])
            with sim_lock:
                sim_lib.sim_touch_event(down, x, y)
            self.send_response(200)
            self.end_headers()

        elif path == "/api/nav":
            action = int(params.get("action", [0])[0])
            with sim_lock:
                sim_lib.sim_trigger_nav(action)
            self.send_response(200)
            self.end_headers()

        elif path == "/api/throttle":
            val = float(params.get("val", [0])[0])
            with sim_lock:
                sim_lib.sim_set_auto_mode(0)
                sim_lib.sim_set_manual_throttle(val)
            self.send_response(200)
            self.end_headers()

        elif path == "/api/brake":
            val = float(params.get("val", [0])[0])
            with sim_lock:
                sim_lib.sim_set_auto_mode(0)
                sim_lib.sim_set_manual_brake(val)
            self.send_response(200)
            self.end_headers()

        elif path == "/api/tuning":
            toggle = int(params.get("toggle", [0])[0])
            enable = int(params.get("enable", [-1])[0])
            with sim_lock:
                if toggle != 0:
                    sim_lib.sim_toggle_tuning_mode()
                elif enable >= 0:
                    sim_lib.sim_set_tuning_mode(enable)
            self.send_response(200)
            self.end_headers()

        elif path == "/api/auto_mode":
            with sim_lock:
                sim_lib.sim_toggle_auto_mode()
            self.send_response(200)
            self.end_headers()

        elif path == "/api/throttle_adjust":
            delta = float(params.get("delta", [0])[0])
            with sim_lock:
                cur = current_telemetry.throttle_pct + delta
                if cur < 0: cur = 0
                if cur > 100: cur = 100
                sim_lib.sim_set_auto_mode(0)
                sim_lib.sim_set_manual_throttle(cur)
            self.send_response(200)
            self.end_headers()

        elif path == "/api/config":
            cfg_buf = ctypes.create_string_buffer(2048)
            with sim_lock:
                sim_lib.sim_get_config_json(cfg_buf, 2048)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.end_headers()
            self.wfile.write(cfg_buf.value)

        elif path == "/api/telemetry":
            with sim_lock:
                t = {
                    "speed": current_telemetry.speed_kmh,
                    "speed_kmh": current_telemetry.speed_kmh,
                    "voltage": current_telemetry.voltage,
                    "battery_pct": current_telemetry.battery_pct,
                    "power": current_telemetry.power_watts,
                    "power_watts": current_telemetry.power_watts,
                    "current": current_telemetry.current_amps,
                    "current_amps": current_telemetry.current_amps,
                    "phase_current": current_telemetry.phase_amps,
                    "phase_amps": current_telemetry.phase_amps,
                    "screen": current_telemetry.screen,
                    "ride_mode": current_telemetry.ride_mode,
                    "boost_active": current_telemetry.boost_active,
                    "trip_km": current_telemetry.trip_km,
                    "max_speed_kmh": current_telemetry.max_speed_kmh,
                    "avg_speed_kmh": current_telemetry.avg_speed_kmh,
                    "ride_time_sec": current_telemetry.ride_time_sec,
                    "tuning_mode": bool(current_telemetry.tuning_mode),
                    "g_force": current_telemetry.g_force,
                }
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.end_headers()
            self.wfile.write(json.dumps(t).encode("utf-8"))

        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else ""

        if path == "/api/config":
            with sim_lock:
                sim_lib.sim_set_config_json(body.encode("utf-8"))
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')

        elif path == "/api/tuning":
            try:
                data = json.loads(body) if body else {}
                enable = data.get("enabled", False)
            except Exception:
                enable = "true" in body.lower()
            with sim_lock:
                sim_lib.sim_set_tuning_mode(1 if enable else 0)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.end_headers()
            resp = {"status": "ok", "tuning_mode": bool(enable)}
            self.wfile.write(json.dumps(resp).encode("utf-8"))

        elif path == "/update":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"OK")
        else:
            self.send_response(404)
            self.end_headers()

class CaptivePortalHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        return

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        if path in ["/", "/index.html", "/portal", "/portal.html", "/generate_204", "/hotspot-detect.html", "/canonical.html"]:
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.end_headers()
            self.wfile.write(get_captive_portal_html().encode("utf-8"))
        elif path == "/api/config":
            cfg_buf = ctypes.create_string_buffer(2048)
            with sim_lock:
                sim_lib.sim_get_config_json(cfg_buf, 2048)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.end_headers()
            self.wfile.write(cfg_buf.value)
        elif path == "/api/telemetry":
            with sim_lock:
                t = {
                    "speed": current_telemetry.speed_kmh,
                    "speed_kmh": current_telemetry.speed_kmh,
                    "voltage": current_telemetry.voltage,
                    "battery_pct": current_telemetry.battery_pct,
                    "power": current_telemetry.power_watts,
                    "power_watts": current_telemetry.power_watts,
                    "current": current_telemetry.current_amps,
                    "phase_current": current_telemetry.phase_amps,
                    "tuning_mode": bool(current_telemetry.tuning_mode),
                    "ride_mode": current_telemetry.ride_mode,
                    "g_force": current_telemetry.g_force,
                }
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.end_headers()
            self.wfile.write(json.dumps(t).encode("utf-8"))
        else:
            self.send_response(302)
            self.send_header("Location", "/")
            self.end_headers()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else ""

        if path == "/api/config":
            with sim_lock:
                sim_lib.sim_set_config_json(body.encode("utf-8"))
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
        elif path == "/api/tuning":
            try:
                data = json.loads(body) if body else {}
                enable = data.get("enabled", False)
            except Exception:
                enable = "true" in body.lower()
            with sim_lock:
                sim_lib.sim_set_tuning_mode(1 if enable else 0)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.end_headers()
            resp = {"status": "ok", "tuning_mode": bool(enable)}
            self.wfile.write(json.dumps(resp).encode("utf-8"))
        elif path == "/update":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"OK")
        else:
            self.send_response(404)
            self.end_headers()

def start_web_server():
    server = ThreadingHTTPServer(("0.0.0.0", 8080), WebDemoHandler)
    server.serve_forever()

def start_portal_server():
    try:
        portal_server = ThreadingHTTPServer(("0.0.0.0", 8081), CaptivePortalHandler)
        portal_server.serve_forever()
    except Exception as e:
        print(f"[PORTAL] Server on port 8081 error: {e}")

# -------------------------------------------------------------
# Desktop Tkinter GUI - Pure Screen Only
# -------------------------------------------------------------
class DesktopDemoApp:
    def __init__(self, root):
        self.root = root
        self.root.title("VESC Display - 4.3\" (800x480)")
        self.root.configure(bg="#000000")
        self.root.resizable(False, False)

        self.touch_active = False
        self._up_release_timer = None
        self._down_release_timer = None

        # 800x480 Canvas (Display Screen Only)
        self.canvas = tk.Canvas(root, width=LCD_W, height=LCD_H, bg="#000000", highlightthickness=0)
        self.canvas.pack()

        # Canvas Mouse Touch Binding (Direct GT911 touch emulation)
        self.canvas.bind("<ButtonPress-1>", self.on_touch_down)
        self.canvas.bind("<B1-Motion>", self.on_touch_move)
        self.canvas.bind("<ButtonRelease-1>", self.on_touch_up)

        # Keyboard shortcuts: Spring-Return Throttle & Braking or Settings Navigation
        self.root.bind("<KeyPress-Up>", lambda e: self.on_up_press())
        self.root.bind("<KeyRelease-Up>", lambda e: self.on_up_release())
        self.root.bind("<KeyPress-w>", lambda e: self.on_up_press())
        self.root.bind("<KeyRelease-w>", lambda e: self.on_up_release())
        self.root.bind("<KeyPress-W>", lambda e: self.on_up_press())
        self.root.bind("<KeyRelease-W>", lambda e: self.on_up_release())

        # Number keys 1-9 for partial throttle (10% .. 90%), 0 for 0%
        for digit in range(1, 10):
            self.root.bind(f"<{digit}>", lambda e, p=digit * 10.0: self.on_digit_press(p))
        self.root.bind("<0>", lambda e: self.on_digit_press(0.0))

        # Mouse Wheel for fine throttle adjustment (+/- 5%) or Menu Scroll
        self.canvas.bind("<Button-4>", lambda e: self.on_wheel_scroll(1))
        self.canvas.bind("<Button-5>", lambda e: self.on_wheel_scroll(-1))
        self.root.bind("<MouseWheel>", lambda e: self.on_wheel_scroll(1 if e.delta > 0 else -1))

        self.root.bind("<KeyPress-Down>", lambda e: self.on_down_press())
        self.root.bind("<KeyRelease-Down>", lambda e: self.on_down_release())
        self.root.bind("<KeyPress-s>", lambda e: self.on_down_press())
        self.root.bind("<KeyRelease-s>", lambda e: self.on_down_release())
        self.root.bind("<KeyPress-S>", lambda e: self.on_down_press())
        self.root.bind("<KeyRelease-S>", lambda e: self.on_down_release())

        self.root.bind("<Return>", lambda e: self.on_enter_press())
        self.root.bind("<KP_Enter>", lambda e: self.on_enter_press())
        self.root.bind("<BackSpace>", lambda e: self.on_back_press())
        self.root.bind("<x>", lambda e: self.toggle_tuning())
        self.root.bind("<X>", lambda e: self.toggle_tuning())
        self.root.bind("<Left>", lambda e: self.on_left_press())
        self.root.bind("<Right>", lambda e: self.on_right_press())
        self.root.bind("<space>", lambda e: self.on_space_press())
        self.root.bind("<b>", lambda e: self.toggle_boost())
        self.root.bind("<B>", lambda e: self.toggle_boost())
        self.root.bind("<m>", lambda e: self.cycle_mode())
        self.root.bind("<M>", lambda e: self.cycle_mode())
        self.root.bind("<h>", lambda e: self.toggle_hazard())
        self.root.bind("<H>", lambda e: self.toggle_hazard())
        self.root.bind("<l>", lambda e: self.toggle_lights())
        self.root.bind("<L>", lambda e: self.toggle_lights())
        self.root.bind("<t>", lambda e: self.trigger_nav(NAV_SCREEN_STATS))
        self.root.bind("<T>", lambda e: self.trigger_nav(NAV_SCREEN_STATS))
        self.root.bind("<r>", lambda e: self.trigger_nav(NAV_SCREEN_RACE))
        self.root.bind("<R>", lambda e: self.trigger_nav(NAV_SCREEN_RACE))
        self.root.bind("<Escape>", lambda e: self.on_esc_press())

        self.tk_img = ImageTk.PhotoImage(initial_image)
        self.image_item = self.canvas.create_image(0, 0, image=self.tk_img, anchor=tk.NW)
        self.update_loop()

    def on_up_press(self):
        if self._up_release_timer:
            self.root.after_cancel(self._up_release_timer)
            self._up_release_timer = None
        if current_telemetry.screen == SCREEN_SETTINGS_MENU:
            self.trigger_nav(NAV_BTN1_SHORT)
        else:
            self.set_throttle(100.0)

    def on_up_release(self):
        if self._up_release_timer:
            self.root.after_cancel(self._up_release_timer)
        # 60ms buffer absorbs X11 auto-repeat release/press flutter while holding key
        self._up_release_timer = self.root.after(60, self._do_up_release)

    def _do_up_release(self):
        self._up_release_timer = None
        if current_telemetry.screen != SCREEN_SETTINGS_MENU:
            self.set_throttle(0.0)

    def on_down_press(self):
        if self._down_release_timer:
            self.root.after_cancel(self._down_release_timer)
            self._down_release_timer = None
        if current_telemetry.screen == SCREEN_SETTINGS_MENU:
            self.trigger_nav(NAV_BTN2_SHORT)
        else:
            self.set_throttle(0.0)
            self.set_brake(75.0)

    def on_down_release(self):
        if self._down_release_timer:
            self.root.after_cancel(self._down_release_timer)
        self._down_release_timer = self.root.after(60, self._do_down_release)

    def _do_down_release(self):
        self._down_release_timer = None
        if current_telemetry.screen != SCREEN_SETTINGS_MENU:
            self.set_brake(0.0)

    def on_digit_press(self, pct):
        # Ignore number keys when locked, entering PIN, or in Settings
        if current_telemetry.is_locked or current_telemetry.screen == SCREEN_SETTINGS_MENU:
            return
        self.set_throttle(pct)

    def on_wheel_scroll(self, direction):
        if current_telemetry.screen == SCREEN_SETTINGS_MENU:
            if direction > 0:
                self.trigger_nav(NAV_BTN1_SHORT)
            else:
                self.trigger_nav(NAV_BTN2_SHORT)
        elif not current_telemetry.is_locked:
            self.adjust_throttle(5.0 if direction > 0 else -5.0)

    def on_enter_press(self):
        if current_telemetry.screen == SCREEN_SETTINGS_MENU:
            self.trigger_nav(NAV_BOTH_PRESSED)

    def on_back_press(self):
        if current_telemetry.screen == SCREEN_SETTINGS_MENU:
            self.trigger_nav(NAV_MENU_BACK)

    def on_left_press(self):
        if current_telemetry.screen == SCREEN_SETTINGS_MENU:
            self.trigger_nav(NAV_MENU_BACK)
        else:
            self.trigger_nav(NAV_BTN1_SHORT)

    def on_right_press(self):
        if current_telemetry.screen == SCREEN_SETTINGS_MENU:
            self.trigger_nav(NAV_BOTH_PRESSED)
        else:
            self.trigger_nav(NAV_BTN2_SHORT)

    def on_space_press(self):
        if current_telemetry.screen == SCREEN_SETTINGS_MENU:
            self.trigger_nav(NAV_BOTH_PRESSED)
        else:
            self.toggle_boost()

    def on_esc_press(self):
        if current_telemetry.screen == SCREEN_SETTINGS_MENU:
            self.trigger_nav(NAV_SCREEN_DASH)
        else:
            self.root.destroy()

    def set_throttle(self, val):
        with sim_lock:
            sim_lib.sim_set_auto_mode(0)
            sim_lib.sim_set_manual_throttle(float(val))

    def set_brake(self, val):
        with sim_lock:
            sim_lib.sim_set_auto_mode(0)
            sim_lib.sim_set_manual_brake(float(val))

    def toggle_tuning(self):
        with sim_lock:
            sim_lib.sim_toggle_tuning_mode()

    def on_touch_down(self, event):
        self.touch_active = True
        with sim_lock:
            sim_lib.sim_touch_event(1, event.x, event.y)

    def on_touch_move(self, event):
        if self.touch_active:
            with sim_lock:
                sim_lib.sim_touch_event(1, event.x, event.y)

    def on_touch_up(self, event):
        if self.touch_active:
            self.touch_active = False
            with sim_lock:
                sim_lib.sim_touch_event(0, event.x, event.y)

    def trigger_nav(self, action):
        with sim_lock:
            sim_lib.sim_trigger_nav(action)

    def toggle_boost(self):
        with sim_lock:
            sim_lib.sim_toggle_boost()

    def cycle_mode(self):
        with sim_lock:
            sim_lib.sim_cycle_mode()

    def toggle_hazard(self):
        with sim_lock:
            sim_lib.sim_toggle_hazard()

    def toggle_lights(self):
        with sim_lock:
            sim_lib.sim_toggle_lights()

    def adjust_throttle(self, delta):
        with sim_lock:
            cur = current_telemetry.throttle_pct + delta
            if cur < 0: cur = 0
            if cur > 100: cur = 100
            sim_lib.sim_set_auto_mode(0)
            sim_lib.sim_set_manual_throttle(cur)

    def update_loop(self):
        with latest_pil_lock:
            img = latest_pil_image
        if img is not None:
            self.tk_img = ImageTk.PhotoImage(img)
            self.canvas.itemconfig(self.image_item, image=self.tk_img)
        self.root.after(33, self.update_loop)

# -------------------------------------------------------------
# Main Execution
# -------------------------------------------------------------
def main():
    print("=" * 70)
    print(" VESC 4.3\" Touch Display Simulator (800x480)")
    print(" Pure Display Mode (No external buttons)")
    print("=" * 70)

    sim_thread = threading.Thread(target=sim_worker_loop, daemon=True)
    sim_thread.start()

    web_thread = threading.Thread(target=start_web_server, daemon=True)
    web_thread.start()
    print("[SERVER] Web Display Stream active at http://localhost:8080")

    portal_thread = threading.Thread(target=start_portal_server, daemon=True)
    portal_thread.start()
    print("[SERVER] Captive Portal Web UI active at http://localhost:8081 and http://localhost:8080/portal")

    display_env = os.environ.get("DISPLAY")
    if display_env and "--no-gui" not in sys.argv:
        try:
            print(f"[GUI] Display Window active on {display_env}")
            root = tk.Tk()
            app = DesktopDemoApp(root)
            root.mainloop()
        except Exception as e:
            print(f"[GUI] Desktop window ended: {e}")

    print("[SERVER] Running background web stream on http://localhost:8080...")
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\n[STOP] Shutting down simulator.")

if __name__ == "__main__":
    main()
