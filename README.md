# VESC 4.3" Touch Display (800x480) - Waveshare ESP32-S3
## Flagship Hyper-VESC 35kW+ Cluster & Autarkic Touch Dashboard

Custom high-performance dashboard for VESC-driven electric scooters, e-bikes, and hyper-scooters, optimized for the **Waveshare ESP32-S3-Touch-LCD-4.3** (800×480 RGB LCD, Goodix GT911 capacitive touch, 16MB Flash, 8MB Octal PSRAM).

<p align="center">
  <img src="tools/simulator/screenshots/00_hyper_35kw_boost_race.png" alt="Hyper-VESC 35kW+ Cockpit in Action" width="90%">
</p>

---

## 📸 Feature Highlights & Screenshots

### 1. Flagship Hyper-35kW+ Cockpit
| Cockpit in Action (92 km/h, GPS Clock & Satellite) | Minimalist Mode (GPS & IMU ausgeblendet) |
|:---:|:---:|
| <img src="tools/simulator/screenshots/00_hyper_35kw_boost_race.png" width="100%"> | <img src="tools/simulator/screenshots/25_dash_minimal_no_gps_no_imu.png" width="100%"> |
| **Cockpit Leerlauf (Live-Uhrzeit & Rekuperation)** | **HUD Mode Banner Overlay** |
| <img src="tools/simulator/screenshots/00_hyper_35kw_idle.png" width="100%"> | <img src="tools/simulator/screenshots/00_hyper_35kw_mode_banner.png" width="100%"> |

### 2. Ersteinrichtungs-Assistent (Setup Wizard)
*Erscheint automatisch beim ersten Start nach dem Flashen oder Reset (`setup_completed == false`). Führt den Fahrer durch die Bedienung, vergibt einen eigenen PIN und konfiguriert die Hardware.*

| Schritt 1: Schnellanleitung & Tuning-Geste | Schritt 2: Sicherheits-PIN wählen | Schritt 3: Hardware-Optionen & Start |
|:---:|:---:|:---:|
| <img src="tools/simulator/screenshots/22_setup_wizard_step1_welcome.png" width="100%"> | <img src="tools/simulator/screenshots/23_setup_wizard_step2_pin.png" width="100%"> | <img src="tools/simulator/screenshots/24_setup_wizard_step3_features.png" width="100%"> |

### 3. Dashboard-Einstellungen & Touch-Konfiguration
| Neuer Tab `[ DASH ]` (GPS-, IMU- & WiFi-Optionen) | Touch Mode Limits (Speed & Watt Sliders) |
|:---:|:---:|
| <img src="tools/simulator/screenshots/12_settings_dash_options.png" width="100%"> | <img src="tools/simulator/screenshots/09_settings_power_limits.png" width="100%"> |
| **Wi-Fi Bridge (Kabelloser VESC Tool TCP Fallback)** | **Dragy Split-Geschwindigkeiten Konfiguration** |
| <img src="tools/simulator/screenshots/13_wifi_bridge.png" width="100%"> | <img src="tools/simulator/screenshots/10_settings_edit_mode.png" width="100%"> |

### 4. RACE Dragy / Launch Control & Dyno
| Dragy Live-Messung (0–22, 0–50, 0–70 km/h) | Beschleunigungs-Dyno (Power & Torque Kurve) |
|:---:|:---:|
| <img src="tools/simulator/screenshots/19_race_timer_measuring.png" width="100%"> | <img src="tools/simulator/screenshots/21_race_dyno_modal.png" width="100%"> |
| **Persönliche Bestzeiten (NVS Rekorde Modal)** | **Legal / Drossel-Modus (Diskreter roter Statuspunkt)** |
| <img src="tools/simulator/screenshots/20_race_records_modal.png" width="100%"> | <img src="tools/simulator/screenshots/14_stealth_mode_idle.png" width="100%"> |

### 5. Sicherheit & System-Diagnose
| Smartphone-Style 4-Digit PIN Lockscreen | VESC Fault Blackbox Alarm Banner |
|:---:|:---:|
| <img src="tools/simulator/screenshots/17_pin_lockscreen.png" width="100%"> | <img src="tools/simulator/screenshots/16_vesc_fault_banner.png" width="100%"> |
| **Erweiterte Fahrt-Statistiken & Helligkeits-Slider** | **PIN Fehleranzeige (Error Shake)** |
| <img src="tools/simulator/screenshots/15_stats_rich_overview.png" width="100%"> | <img src="tools/simulator/screenshots/18_pin_lockscreen_error.png" width="100%"> |

---

## 🚀 Key Features im Detail

### 1. Flagship Hyper-35kW+ Cluster (Screen 0)
- **High-Contrast Digitaler Tacho:**
  - Riesige Geschwindigkeitsanzeige mit dynamischem 270°-Bogen.
  - **Startup Needle Sweep:** Flüssige Zeigertest-Animation beim Booten bzw. Entsperren (fährt dynamisch von 0 → 100 km/h → 0 km/h).
  - Dynamische Farbübergänge je nach Geschwindigkeit und Beschleunigung.
  - Zentrierte thermische Telemetrie direkt unter der Geschwindigkeitszahl: `FET 60°  MOT 75°`.
- **GPS-Priorität & Atomzeit-Uhr:**
  - Uhrzeit oben in der Mitte auf exakter Höhe des `[ STATS ]`-Buttons.
  - Sobald ein 3D-Satelliten-Fix anliegt, erscheint unter `km/h` ein diskretes Satelliten-Icon mit Funkwellen.
  - Satellitengestützte Geschwindigkeit (immun gegen Radschlupf/Reifenabrieb) hat Priorität und wird auch an den VESC übertragen; bei Signalverlust erfolgt nahtloser Fallback auf Motor-ERPM.
- **HUD Mode Banner:**
  - Elegantes horizontales Einblend-Banner beim Umschalten der Fahrmodi (`[ ECO ]`, `[ SPORT ]`, `[ RACE ]`), ohne die Geschwindigkeitsanzeige zu verdecken.
- **Echte Rekuperations- & Leistungsanzeige:**
  - Leistungsmessung bis über 35 kW Peak.
  - Echte bi-direktionale Rekuperations-Erkennung: wechselt beim Bremsen sofort in smaragdgrün mit negativem Vorzeichen (z.B. `-820 W` / `-310 W`).
- **Batterie- & Phasenströme:**
  - Präzise SoC-% und Spannungsanzeige für Akkupacks von 10S bis 24S (bis 105 V).
  - Phasenstrom (`MOTOR 142 A`) und Batteriestrom (`BATTERY 83 A`) in Echtzeit.
  - Dynamische Restreichweitenberechnung (`RESTREICHWEITE 48 km`) basierend auf rollendem Wh/km-Verbrauch.
  - Trip-Tageskilometerzähler (`TRIP 14.2 km`) direkt im Cockpit über dem Fahrmodus-Button.
- **Interaktive Touch-Buttons:**
  - `[ BOOST ]`: Zündet einen 10-sekündigen 120% Leistungs-Burst.
  - `[ ECO / SPORT / RACE ]`: Schaltet Profile durch.

### 2. Ersteinrichtungs-Assistent (Setup Wizard)
- **Automatischer Erststart:** Wird das System nach dem ersten Flashen oder einem Reset gestartet (`setup_completed == false`), startet direkt der 3-stufige Assistent (der PIN-Lockscreen wird dabei automatisch umgangen).
- **Schritt 1 (Schnellanleitung & Tuning-Geheimnis):**
  - Erklärt mit klaren Anweisungen, wie das Einstellungsmenü geöffnet wird (Tipp oben rechts auf das Batterie-Badge).
  - Erklärt die geheime Tuning-Geste: 4 Sekunden langes Halten direkt auf der großen `km/h`-Zahl schaltet die Drosselung ab (roter Punkt oben rechts erlischt).
  - Sofortige Drosselung (Polizei-Modus): Ein einfacher Einzeltipp auf die km/h-Zahl sperrt das Fahrzeug sofort wieder auf 20 km/h.
- **Schritt 2 (Sicherheits-PIN wählen):**
  - Integriertes 12-Tasten Touch-Ziffernfeld (`0`–`9`, `DEL`, `OK`), um einen individuellen 4-stelligen Wegfahrsperren-PIN festzulegen.
  - Wird dauerhaft im ESP32 NVS-Flash gespeichert.
- **Schritt 3 (Hardware-Features anpassen):**
  - Direkte Schalter für GPS & Live-Uhrzeit, Neigungswinkel & G-Force (IMU) sowie WiFi-Bridge Fallback.
  - Klick auf `[ SETUP ABSCHLIESSEN & STARTEN ]` speichert `setup_completed = true`, führt den Zeigertest aus und öffnet das Fahr-Cockpit. Der Assistent erscheint bei normalen Starts nicht mehr.

### 3. Neuer Settings-Tab `[ DASH ]` & Dashboard-Optionen
- Das On-Screen Einstellungsmenü bietet 6 Reiter: `[ ECO ] | [ SPORT ] | [ RACE ] | [ DRAGY ] | [ DASH ] | [ PIN ]`.
- **GPS & Live-Uhrzeit Schalter:** Schaltet GPS-Verarbeitung ein oder aus. Bei `AUS` werden die Uhr oben mittig sowie das Satelliten-Icon unter `km/h` komplett ausgeblendet und das NMEA-Polling deaktiviert.
- **Neigung & G-Force (IMU) Schalter:** Blendet die IMU-Badges unten links für ein aufgeräumtes, minimalistisches Cockpit aus.
- **Kein Bluetooth-Modul? (WiFi-Bridge Action):** Startet sofort den SoftAP `VESC-DASH-AP` auf TCP-Port 6510/6512 als kabellosen Fallback für die VESC Tool App und leitet auf die WiFi-Bridge-Seite weiter.
- **Setup-Assistent wiederholen:** Ermöglicht es, den Ersteinrichtungs-Assistenten jederzeit manuell zu wiederholen.

### 4. Touch Mode Configurator & Dragy Splits
- Parameter (Speed in km/h, Leistung in Watt) für jedes Profil (`ECO`, `SPORT`, `RACE`) direkt auf dem Display anpassen:
  - Über Touch-Slider oder Präzisions-Taster `[-]` und `[+]`.
  - Auto-Save in den NVS Flash beim Verlassen zurück ins Dashboard (`< DASH`).
- Dragy-Splits unter Tab `[ DRAGY ]` (z.B. 0–22 km/h, 0–50 km/h, 0–70 km/h) stufenlos einstellbar.

### 5. RACE Launch Control / Dragy Acceleration Timer & Dyno (Screen 6)
- **Präzise Dragy-Messung:**
  - 5-Sekunden Launch Countdown (`5 ... 1 ... GO!`) mit optischer Ampel.
  - Erfasst exakte Zwischenzeiten: **0–22 km/h**, **0–50 km/h** und **0–70 km/h**.
  - Saubere Split-Anzeige ohne vorzeitige PB-Tags; neue Bestzeiten werden nach dem Run als `*PB*` markiert und im NVS gespeichert.
  - Automatischer Messungsabschluss bei Erreichen des Split 3 oder nach 15 Sekunden Timeout.
- **Automatische Profil- & Boost-Aktivierung:**
  - Beim Wechsel auf den Race-Screen wird automatisch **RACE Mode** aktiviert und **Hyper Boost** permanent gezündet.
  - Beim Verlassen wird automatisch der vorherige Fahrmodus wiederhergestellt.
- **Thermische Schutzschaltung:**
  - Bei Motor- oder ESC-Temperaturen über **65 °C** wird der Launch mit Warnhinweis `TEMP > 65C` blockiert bzw. eine laufende Messung abgebrochen.
- **G-Force Telemetrie-Badge:**
  - Echte Längsbeschleunigung ($\pm G$) in HUD-Rundbadge mit Farbkodierung.
- **Live-Dyno Modal (`[ DIAGRAMM ]`):**
  - Zeichnet die Drehmoment- und Leistungskurve über die Zeit im Beschleunigungslauf auf.
- **Persönliche Bestzeiten (`[ BESTZEITEN ]`):**
  - Interaktives Popup mit allen Rekorden und Ein-Tipp `[ RESET ]`-Funktion.

### 6. Smartphone-Style 4-Digit PIN Lockscreen
- Numerisches Tastenfeld ($0 \dots 9$, `<`, `#`) sperrt das Fahrzeug beim Einschalten.
- VESC Wegfahrsperre: VESC-Ausgang wird auf 0A geklemmt, bis der korrekte PIN eingegeben wurde.
- PIN kann im Einstellungsmenü (Tab `[ PIN ]`) oder im Ersteinrichtungs-Assistenten geändert werden.

### 7. VESC Fault Monitor & NVS Blackbox
- Live-Dekodierung von VESC `mc_fault_code` (Übertemperatur, Überspannung, Unterspannung, DRV8302/Gate-Fehler, Überstrom).
- Blinkendes HUD-Alarm-Banner mit Handlungsanweisung.
- NVS Blackbox Ringspeicher: speichert die letzten 10 Fehler mit Zeitstempel, Spannung, Strömen und Temperaturen.

---

## 👆 Touch-Gesten & Navigation (GT911)

| Geste / Hitbox | Bildschirm | Aktion |
|---|---|---|
| **Swipe Links** ($\Delta X < -60\text{px}$) | Ride Dash / RT Graphs | Nächster Bildschirm (`Ride Dash` $\rightarrow$ `RT Graphs`) |
| **Swipe Rechts** ($\Delta X > +60\text{px}$) | Ride Dash / RT Graphs | Vorheriger Bildschirm (`RT Graphs` $\rightarrow$ `Ride Dash`) |
| **Tipp `[ STATS ]`** (Oben links) | Ride Dash | Öffnet Statistik- & Helligkeits-Bildschirm |
| **Tipp `[ < DASH ]`** (Oben links) | Stats / Race / Settings | Direkte Rückkehr ins Fahr-Dashboard |
| **Tipp `[ RACE ]`** (Oben links) | Stats Screen (nur bei Tuning) | Öffnet Race Dragy Launch Screen |
| **Tipp `[ BESTZEITEN ]`** (Oben rechts) | Race Screen | Öffnet Rekorde-Popup |
| **Tipp `[ DIAGRAMM ]`** (Oben mittig) | Race Screen | Öffnet Beschleunigungs-Dyno Kurve |
| **Tipp Display-Mitte** | Race Screen | Startet Launch-Countdown / Bricht Messung ab |
| **Tipp `[ BOOST ]`** | Ride Dash | Aktiviert 10-Sekunden Hyper Boost |
| **Tipp `[ ECO / SPORT / RACE ]`** | Ride Dash | Schaltet aktiven Fahrmodus um |
| **Tipp Batterie-Badge** (Oben rechts) | Ride Dash | Öffnet Einstellungsmenü (Tabs: ECO, SPORT, RACE, DRAGY, DASH, PIN) |
| **Halten auf Geschwindigkeitszahl (4s)** | Ride Dash | **Tuning freischalten / sperren** (roter Statuspunkt erlischt) |
| **Einzeltipp auf Geschwindigkeitszahl** | Ride Dash (getuned) | **Sofortige 20 km/h Drosselung (Polizei-Modus)** |
| **Langer Druck unten links** | Ride Dash / Stats | Trip-Kilometerzähler & Peak-Werte auf 0 zurücksetzen |

---

## 🔌 Hardware-Konfiguration (Waveshare ESP32-S3-Touch-LCD-4.3)

### Display (ST7262 RGB Parallel Interface)
- **Auflösung:** 800 $\times$ 480 @ 14 MHz Pixel Clock
- **Sync:** HSYNC = GPIO 46, VSYNC = GPIO 3, DE = GPIO 5, PCLK = GPIO 7
- **16-Bit RGB565 Bus:**
  - **Blau (B0–B4):** GPIO 14, 38, 18, 17, 10
  - **Grün (G0–G5):** GPIO 39, 0, 45, 48, 47, 21
  - **Rot (R0–R4):** GPIO 1, 2, 42, 41, 40
- **Touch-Controller:** Goodix GT911 (I2C SDA = GPIO 8, SCL = GPIO 9, INT = GPIO 4, Adresse `0x5D`)
- **IO-Expander:** CH422G (I2C `0x24`), EXIO2 steuert die Display-Hintergrundbeleuchtung
- **Speicher:** 16 MB Flash, 8 MB Octal PSRAM (`qio_opi`)

### VESC & Peripherie-Verkabelung

Die Kommunikation mit dem VESC (Single oder Dual Array) erfolgt wahlweise über **CAN-Bus** (empfohlen) oder **UART**. Umschaltbar in [`include/board_config.h`](include/board_config.h):
```c
#define VESC_USE_CAN_BUS 1   // 1 = High-Speed CAN Bus (500kbit/s), 0 = Legacy UART (115200)
```

#### 1. CAN-Bus Verkabelung (Empfohlen für Dual VESC & Bluetooth-Module)
Nutzt den integrierten TJA1051 CAN-Transceiver des Waveshare Displays:

| Display Pin / Klemme | VESC CAN Anschluss | Beschreibung |
|:---|:---|:---|
| **5V / VCC** | `5V` | Logikversorgung vom internen 5V BEC des VESC |
| **GND** | `GND` | Gemeinsame Masse |
| **CAN_H** | `CAN_H` | Differential CAN High Leitung (500 kbps) |
| **CAN_L** | `CAN_L` | Differential CAN Low Leitung (500 kbps) |

> [!TIP]
> Bei Nutzung von CAN-Bus bleibt der `COMM / UART`-Port des VESC zu 100% frei für dein Bluetooth-Modul (VESC Tool Mobile App)!

#### 2. VESC UART Verkabelung (Alternative bei Single-ESC)
Wenn CAN nicht genutzt wird (`#define VESC_USE_CAN_BUS 0`):

| Display Pin | VESC COMM Port | Beschreibung |
|:---|:---|:---|
| **5V / VCC** | `5V` | Logikversorgung |
| **GND** | `GND` | Gemeinsame Masse |
| **TX (GPIO 43)** | `RX` | Seriell Senden (115200 Baud) |
| **RX (GPIO 44)** | `TX` | Seriell Empfangen (115200 Baud) |

#### 3. GPS / BDS GNSS Modul Verkabelung (z.B. ATGM336H)
Standard 3.3V UART GPS/BDS-Modul verbindet direkt mit dem ESP32:
- **GPS TX** $\rightarrow$ **ESP32 RX** (GPIO 44, im CAN-Modus)
- **GPS RX** $\rightarrow$ **ESP32 TX** (GPIO 43, im CAN-Modus)
- **VCC** (3.3V / 5V) und **GND**
- Liefert Atomzeit-Uhrzeitsynchronisation, 3D-Satellitenfix-Status und schlupffreie GPS-Geschwindigkeit mit Prioritätsweiterleitung an den VESC.

---

## 💻 Desktop Simulator & Live-Demo

Das Projekt beinhaltet einen vollwertigen Desktop-Simulator mit integriertem Virtual VESC Emulator und Web-Streaming:

### Live-Demo starten
```bash
DISPLAY=:0 python3 tools/simulator/live_demo.py
```
- **Web-Stream:** Öffne [http://localhost:8080](http://localhost:8080) für einen interaktiven 60 FPS Stream mit Touch-Unterstützung im Browser.
- **Ersteinrichtungs-Assistent Reset:** 
  - Klicke im Webinterface auf `[ 🔄 Setup Wizard Reset ]`, drücke im Fenster <kbd>Ctrl</kbd>+<kbd>R</kbd> bzw. <kbd>F5</kbd>, oder starte mit `--reset-wizard`:
  ```bash
  DISPLAY=:0 python3 tools/simulator/live_demo.py --reset-wizard
  ```
- **VESC Tool TCP Bridge:** Der Simulator stellt einen Virtual VESC TCP-Server auf Port `6510`/`6512` bereit. Verbinde das offizielle VESC Tool (Connection $\rightarrow$ TCP $\rightarrow$ `127.0.0.1:6510`), um Limits einzustellen und Telemetrie live zu testen!
- **Steuerung:**
  - Maus-Klick und Drag auf das Display emulieren GT911 Kapazitiv-Touch und Wischgesten.
  - Tastatur: <kbd>W</kbd> / <kbd>Up</kbd> (Gas), <kbd>S</kbd> / <kbd>Down</kbd> (Bremse), <kbd>B</kbd> (Boost), <kbd>M</kbd> (Mode cycle), <kbd>X</kbd> (Tuning toggle).

### Tests & Screenshot-Generierung ausführen
```bash
./tools/simulator/build_and_run.sh
```
Führt 25 automatisierte Unittests und Bildschirmszenarien aus und exportiert alle Screenshots nach `tools/simulator/screenshots/`.

---

## ⚡ Flashen auf das Waveshare ESP32-S3 Board

1. Verbinde das Waveshare ESP32-S3 4.3" Board über den **USB**-Port mit deinem PC.
2. Prüfe die Konfiguration in [`include/board_config.h`](include/board_config.h):
   - Setze `#define SIMULATION_MODE 0` für echten Fahrbetrieb.
   - Wähle `#define VESC_USE_CAN_BUS 1` für CAN-Bus oder `0` für UART.
3. Baue und flashe die Firmware mit PlatformIO:
```bash
pio run -t upload
```

---

## 📄 Lizenz
MIT License. Frei nutzbar für private und kommerzielle Elektrofahrzeug-Projekte.
