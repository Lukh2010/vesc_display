#include "ota_manager.h"

OtaManager& OtaManager::instance() {
    static OtaManager s_instance;
    return s_instance;
}

OtaManager::OtaManager()
    : _running(false),
      _is_updating(false),
      _bytes_written(0),
      _total_bytes(0)
#if !defined(NATIVE_SIMULATOR)
      , _server(80)
#endif
{
    strncpy(_status_text, "Bereit", sizeof(_status_text));
}

int OtaManager::getProgressPct() const {
    if (!_is_updating || _total_bytes == 0) return 0;
    int pct = (int)((_bytes_written * 100) / _total_bytes);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

void OtaManager::setSimProgress(bool updating, int pct) {
    _is_updating = updating;
    if (updating) {
        _total_bytes = 1000000;
        _bytes_written = (_total_bytes * pct) / 100;
        snprintf(_status_text, sizeof(_status_text), "Flashe %d%%...", pct);
    } else {
        _total_bytes = 0;
        _bytes_written = 0;
        strncpy(_status_text, "Bereit", sizeof(_status_text));
    }
}

#if !defined(NATIVE_SIMULATOR)

static const char OTA_HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>VESC Hyper Display - OTA Flasher</title>
<style>
  :root {
    --bg: #0A0F1D;
    --surface: #131B2E;
    --border: #1E293B;
    --cyan: #00F0FF;
    --green: #00FF66;
    --amber: #FFB800;
    --red: #FF3366;
    --text: #F8FAFC;
    --muted: #94A3B8;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
  body { background: var(--bg); color: var(--text); display: flex; align-items: center; justify-content: center; min-height: 100vh; padding: 20px; }
  .card { background: var(--surface); border: 1px solid var(--border); border-radius: 16px; max-width: 540px; width: 100%; padding: 32px; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }
  .header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 24px; border-bottom: 1px solid var(--border); padding-bottom: 16px; }
  .title { font-size: 20px; font-weight: 700; color: var(--cyan); letter-spacing: 0.5px; }
  .badge { background: rgba(0, 240, 255, 0.1); color: var(--cyan); border: 1px solid var(--cyan); padding: 4px 10px; border-radius: 8px; font-size: 11px; font-weight: 600; }
  .info-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 24px; }
  .info-box { background: rgba(15, 23, 42, 0.6); border: 1px solid var(--border); padding: 12px; border-radius: 10px; font-size: 12px; }
  .info-box span { display: block; color: var(--muted); font-size: 10px; text-transform: uppercase; margin-bottom: 4px; }
  .drop-zone { border: 2px dashed var(--border); border-radius: 12px; padding: 36px 20px; text-align: center; cursor: pointer; transition: all 0.2s; background: rgba(15, 23, 42, 0.4); margin-bottom: 24px; }
  .drop-zone:hover, .drop-zone.dragover { border-color: var(--cyan); background: rgba(0, 240, 255, 0.05); }
  .drop-zone svg { width: 44px; height: 44px; fill: var(--cyan); margin-bottom: 12px; opacity: 0.85; }
  .drop-zone p { font-size: 14px; color: var(--muted); }
  .drop-zone strong { color: var(--text); }
  input[type="file"] { display: none; }
  .btn { width: 100%; background: linear-gradient(135deg, #00C6FF, #0072FF); color: #fff; border: none; padding: 14px; border-radius: 10px; font-size: 14px; font-weight: 700; letter-spacing: 0.5px; cursor: pointer; transition: opacity 0.2s; }
  .btn:hover { opacity: 0.9; }
  .btn:disabled { background: var(--border); color: var(--muted); cursor: not-allowed; }
  .progress-wrap { display: none; margin-top: 24px; }
  .progress-bar-bg { width: 100%; height: 12px; background: rgba(15, 23, 42, 0.8); border: 1px solid var(--border); border-radius: 6px; overflow: hidden; }
  .progress-bar-fill { width: 0%; height: 100%; background: linear-gradient(90deg, var(--cyan), var(--green)); border-radius: 6px; transition: width 0.15s ease; }
  .status-text { margin-top: 10px; font-size: 13px; color: var(--muted); display: flex; justify-content: space-between; }
  .status-text b { color: var(--cyan); }
  .footer { margin-top: 24px; text-align: center; font-size: 11px; color: var(--muted); }
</style>
</head>
<body>
<div class="card">
  <div class="header">
    <div class="title">VESC HYPER OTA</div>
    <div class="badge">ESP32-S3 (16MB)</div>
  </div>
  <div class="info-grid">
    <div class="info-box"><span>Ziel-Ger&auml;t</span>Waveshare 4.3" Touch</div>
    <div class="info-box"><span>Controller</span>Spintend UBOX 100V</div>
    <div class="info-box"><span>Partition</span>Dual-Bank A/B Safe</div>
    <div class="info-box"><span>WiFi-Modus</span>SoftAP (192.168.4.1)</div>
  </div>

  <div class="drop-zone" id="dropZone" onclick="document.getElementById('fileInput').click()">
    <svg viewBox="0 0 24 24"><path d="M19.35 10.04C18.67 6.59 15.64 4 12 4 9.11 4 6.6 5.64 5.35 8.04 2.34 8.36 0 10.91 0 14c0 3.31 2.69 6 6 6h13c2.76 0 5-2.24 5-5 0-2.64-2.05-4.78-4.65-4.96zM14 13v4h-4v-4H7l5-5 5 5h-3z"/></svg>
    <p><strong>firmware.bin ausw&auml;hlen</strong> oder hier ablegen</p>
    <p style="font-size: 12px; margin-top: 6px;" id="fileName">Keine Datei ausgew&auml;hlt</p>
  </div>
  <input type="file" id="fileInput" accept=".bin" onchange="fileSelected(this)">

  <button class="btn" id="uploadBtn" disabled onclick="startUpload()">&#9889; FIRMWARE JETZT FLASCHEN</button>

  <div class="progress-wrap" id="progressWrap">
    <div class="progress-bar-bg">
      <div class="progress-bar-fill" id="progressBar"></div>
    </div>
    <div class="status-text">
      <span id="statusMsg">&Uuml;bertrage Daten...</span>
      <b id="statusPct">0%</b>
    </div>
  </div>

  <div class="footer">
    PlatformIO CLI: <code>pio run -t upload --upload-port 192.168.4.1</code><br>
    oder <code>curl -F "update=@firmware.bin" http://192.168.4.1/update</code>
  </div>
</div>

<script>
let selectedFile = null;
const dropZone = document.getElementById('dropZone');
['dragenter', 'dragover'].forEach(name => {
  dropZone.addEventListener(name, (e) => { e.preventDefault(); dropZone.classList.add('dragover'); }, false);
});
['dragleave', 'drop'].forEach(name => {
  dropZone.addEventListener(name, (e) => { e.preventDefault(); dropZone.classList.remove('dragover'); }, false);
});
dropZone.addEventListener('drop', (e) => {
  const dt = e.dataTransfer;
  const files = dt.files;
  if (files.length) handleFile(files[0]);
});

function fileSelected(input) {
  if (input.files.length) handleFile(input.files[0]);
}

function handleFile(f) {
  if (!f.name.endsWith('.bin')) {
    alert('Bitte nur .bin Firmware-Dateien auswaehlen!');
    return;
  }
  selectedFile = f;
  document.getElementById('fileName').innerHTML = '<b>' + f.name + '</b> (' + (f.size / 1024).toFixed(1) + ' KB)';
  document.getElementById('uploadBtn').disabled = false;
}

function startUpload() {
  if (!selectedFile) return;
  document.getElementById('uploadBtn').disabled = true;
  document.getElementById('progressWrap').style.display = 'block';

  const formData = new FormData();
  formData.append('update', selectedFile, selectedFile.name);

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update?size=' + encodeURIComponent(selectedFile.size), true);

  xhr.upload.addEventListener('progress', (e) => {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      document.getElementById('progressBar').style.width = pct + '%';
      document.getElementById('statusPct').innerText = pct + '%';
      document.getElementById('statusMsg').innerText = 'Uebertrage Firmware (' + (e.loaded/1024).toFixed(0) + ' / ' + (e.total/1024).toFixed(0) + ' KB)...';
    }
  });

  xhr.onload = function() {
    if (xhr.status === 200 && xhr.responseText.indexOf('FAIL') === -1) {
      document.getElementById('progressBar').style.width = '100%';
      document.getElementById('statusPct').innerText = '100%';
      document.getElementById('statusMsg').innerText = 'Erfolgreich! Display startet in 5s neu...';
      setTimeout(() => { location.reload(); }, 6000);
    } else {
      document.getElementById('statusMsg').innerText = 'Fehler beim Flashen: ' + xhr.responseText;
      document.getElementById('uploadBtn').disabled = false;
    }
  };

  xhr.onerror = function() {
    document.getElementById('statusMsg').innerText = 'Verbindungsfehler waehrend des Uploads!';
    document.getElementById('uploadBtn').disabled = false;
  };

  xhr.send(formData);
}
</script>
</body>
</html>
)rawliteral";

void OtaManager::setupRoutes() {
    const char* headerkeys[] = {"Content-Length"};
    _server.collectHeaders(headerkeys, 1);

    // 1. Root redirect to /update
    _server.on("/", HTTP_GET, [this]() {
        _server.sendHeader("Location", "/update");
        _server.send(302, "text/plain", "");
    });

    // 2. HTML Web-Flasher UI
    _server.on("/update", HTTP_GET, [this]() {
        _server.send(200, "text/html", OTA_HTML_PAGE);
    });

    // 3. Status JSON API for terminal/scripts
    _server.on("/api/status", HTTP_GET, [this]() {
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"status\":\"online\",\"free_heap\":%u,\"ota_updating\":%s,\"progress\":%d,\"written\":%u,\"total\":%u}",
                 (unsigned int)ESP.getFreeHeap(),
                 _is_updating ? "true" : "false",
                 getProgressPct(),
                 (unsigned int)_bytes_written,
                 (unsigned int)_total_bytes);
        _server.send(200, "application/json", json);
    });

    // 4. Multipart Firmware Upload Handler (Web UI & curl compatible)
    _server.on("/update", HTTP_POST, [this]() {
        _server.sendHeader("Connection", "close");
        if (Update.hasError()) {
            _server.send(500, "text/plain", "FLASH FEHLER: Update fehlgeschlagen!");
        } else {
            _server.send(200, "text/plain", "OK - Flash erfolgreich! Display startet neu...");
            delay(500);
            ESP.restart();
        }
    }, [this]() {
        HTTPUpload& upload = _server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            _is_updating = true;
            _total_bytes = 1200000;
            if (_server.hasArg("size")) {
                uint32_t s = (uint32_t)_server.arg("size").toInt();
                if (s > 10000) _total_bytes = s;
            } else if (_server.hasHeader("Content-Length")) {
                uint32_t cl = (uint32_t)_server.header("Content-Length").toInt();
                if (cl > 10000) _total_bytes = cl;
            }
            _bytes_written = 0;
            strncpy(_status_text, "Starte OTA Flash...", sizeof(_status_text));
            Serial.printf("[OTA] Firmware Upload gestartet: %s (Erwartete Groesse: %u Bytes)\n",
                          upload.filename.c_str(), (unsigned int)_total_bytes);

            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
                snprintf(_status_text, sizeof(_status_text), "Update.begin Fehler");
                _is_updating = false;
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (!_is_updating || Update.hasError()) return;
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
                snprintf(_status_text, sizeof(_status_text), "Schreibfehler");
            }
            _bytes_written = upload.totalSize;
            if (_bytes_written > _total_bytes) _total_bytes = _bytes_written;
            int pct = getProgressPct();
            snprintf(_status_text, sizeof(_status_text), "Flashe %d%%...", pct);
        } else if (upload.status == UPLOAD_FILE_END) {
            _total_bytes = upload.totalSize;
            _bytes_written = upload.totalSize;
            if (Update.end(true)) {
                Serial.printf("[OTA] Flash erfolgreich beendet: %u Bytes geschrieben!\n", (unsigned int)upload.totalSize);
                strncpy(_status_text, "Erfolgreich! Neustart...", sizeof(_status_text));
            } else {
                Update.printError(Serial);
                strncpy(_status_text, "Verifikationsfehler", sizeof(_status_text));
            }
            _is_updating = false;
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
            Update.end();
            _is_updating = false;
            strncpy(_status_text, "Upload abgebrochen", sizeof(_status_text));
            Serial.println("[OTA] Upload abgebrochen!");
        }
    });
}

#endif // !NATIVE_SIMULATOR

void OtaManager::begin() {
    if (_running) return;

#if !defined(NATIVE_SIMULATOR)
    setupRoutes();
    _server.begin();

    // Setup ArduinoOTA on Port 3232 for direct PlatformIO CLI upload
    ArduinoOTA.setPort(3232);
    ArduinoOTA.setHostname("vesc-hyper-display");

    ArduinoOTA.onStart([this]() {
        _is_updating = true;
        _bytes_written = 0;
        _total_bytes = 1200000;
        strncpy(_status_text, "PlatformIO OTA Start...", sizeof(_status_text));
        Serial.println("[OTA_ARDUINO] PlatformIO OTA Upload gestartet!");
    });

    ArduinoOTA.onProgress([this](unsigned int progress, unsigned int total) {
        _bytes_written = progress;
        _total_bytes = total;
        int pct = getProgressPct();
        snprintf(_status_text, sizeof(_status_text), "PlatformIO %d%%", pct);
    });

    ArduinoOTA.onEnd([this]() {
        _is_updating = false;
        strncpy(_status_text, "Erfolgreich! Neustart...", sizeof(_status_text));
        Serial.println("[OTA_ARDUINO] OTA erfolgreich beendet! Starte neu...");
    });

    ArduinoOTA.onError([this](ota_error_t error) {
        _is_updating = false;
        snprintf(_status_text, sizeof(_status_text), "OTA Fehler: %u", error);
        Serial.printf("[OTA_ARDUINO] Fehler [%u]\n", error);
    });

    ArduinoOTA.begin();
    Serial.println("[OTA] Web-Flasher (Port 80) und ArduinoOTA (Port 3232) aktiv!");
#endif

    _running = true;
    _is_updating = false;
    strncpy(_status_text, "Bereit", sizeof(_status_text));
}

void OtaManager::update() {
    if (!_running) return;

#if !defined(NATIVE_SIMULATOR)
    _server.handleClient();
    ArduinoOTA.handle();
#endif
}

void OtaManager::stop() {
    if (!_running) return;

#if !defined(NATIVE_SIMULATOR)
    _server.stop();
    ArduinoOTA.end();
#endif

    _running = false;
    _is_updating = false;
    strncpy(_status_text, "Gestoppt", sizeof(_status_text));
}
