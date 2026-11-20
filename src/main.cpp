#include <Arduino.h>
#include "board_config.h"
#include "dash_types.h"
#include "display_driver.h"
#include "dash_renderer.h"
#include "settings_manager.h"
#include "battery_manager.h"
#include "buttons.h"
#include "vesc_uart.h"
#include "mode_manager.h"
#include "fault_manager.h"
#include "gps_tracker.h"
#include "ble_keyless_manager.h"
#include "ota_manager.h"
#include "tcs_tpms_manager.h"

// Active Telemetry State
static DashTelemetry telemetry;

void setup() {
    // 1. Initialize USB CDC Serial Monitor with 4KB buffers for VESC configuration packets
    Serial.setRxBufferSize(4096);
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=============================================");
    Serial.println(" Waveshare ESP32-S3-Touch-LCD-4.3 Dashboard  ");
    Serial.println(" Clean OEM Instrument Cluster UI (800x480)   ");
    Serial.println(" Goodix GT911 Capacitive Touch / Swipe Nav   ");
    Serial.println(" Target: Flipsky / VESC UART (TX:43, RX:44)  ");
    Serial.println(" Target: VESC CAN Bus (TX:20, RX:19)         ");
    Serial.println("=============================================");

    // 2. Initialize Persistent Settings
    Settings.begin();
    Serial.printf("[OK] Settings loaded: MaxBat=%dA, MaxPhase=%dA, FW=%dA, Style=%d\n",
                  Settings.get().max_battery_amps,
                  Settings.get().max_phase_amps,
                  Settings.get().field_weak_amps,
                  Settings.get().dash_style);

    // 3. Initialize Battery Profile & Health Learning System
    Battery.begin();
    Serial.printf("[OK] Battery Manager initialized: Active Profile %d (%s)\n",
                  Battery.getActiveProfileIndex(),
                  Battery.getProfile(Battery.getActiveProfileIndex()).name);

    // 4. Initialize Telemetry State
    telemetry.speed_kmh           = 0.0f;
    telemetry.g_force             = 0.0f;
    telemetry.incline_deg         = 0.0f;
    telemetry.duty_cycle_pct      = 0.0f;
    telemetry.rpm                 = 0.0f;
    telemetry.throttle_pct        = 0.0f;
    telemetry.voltage             = 53.8f;
    telemetry.current_amps        = 0.0f;
    telemetry.phase_amps          = 0.0f;
    telemetry.power_watts         = 0.0f;
    telemetry.amphours_used       = 0.0f;
    telemetry.battery_profile_id  = Battery.getActiveProfileIndex();
    telemetry.battery_pct         = Battery.getFusedSocPct();
    telemetry.remaining_wh        = Battery.getRemainingWh();
    telemetry.battery_health_soh  = Battery.getProfile(Battery.getActiveProfileIndex()).health_soh_pct;
    telemetry.est_range_km        = Battery.getEstRangeKm();
    telemetry.temp_motor          = 36.0f;
    telemetry.temp_esc            = 31.0f;
    telemetry.trip_km             = 0.0f;
    telemetry.odo_km              = 142.0f;
    telemetry.stats.reset();
    telemetry.screen              = SCREEN_RIDE_DASH;
    telemetry.vesc_connected      = false;
    telemetry.uptime_sec          = 0;
    telemetry.ride_mode           = MODE_ECO;
    telemetry.saved_ride_mode     = MODE_ECO;
    telemetry.tuning_mode         = false; // Requirement 1: Boot state is ALWAYS stealth / gedrosselt!
    telemetry.is_locked           = Settings.get().pin_lock_enabled;
    telemetry.pin_entry[0]        = '\0';
    telemetry.pin_error           = false;
    telemetry.pin_error_end_ms    = 0;
    telemetry.fault_code          = 0;
    telemetry.fault_start_ms      = 0;
    telemetry.dynamic_wh_km       = Battery.getDynamicWhKm();
    telemetry.race_popup_open     = false;
    telemetry.race_dyno_open      = false;
    telemetry.race_dyno.reset();
    telemetry.race_state          = 0;
    telemetry.race_countdown_start_ms = 0;
    telemetry.race_launch_start_ms = 0;
    telemetry.race_elapsed_sec    = 0.0f;
    telemetry.race_split_0_22     = 0.0f;
    telemetry.race_split_0_50     = 0.0f;
    telemetry.race_split_0_70     = 0.0f;
    telemetry.race_records        = Settings.getRaceRecords();
    telemetry.gps_enabled         = Settings.get().gps_enabled;
    telemetry.incline_gforce_enabled = Settings.get().incline_gforce_enabled;
    telemetry.wifi_bridge_enabled = Settings.get().wifi_bridge_enabled;
    telemetry.setup_completed     = Settings.get().setup_completed;
    telemetry.wizard_step         = 0;
    telemetry.wizard_pin_buf[0]   = '\0';
    telemetry.ble_keyless_enabled = Settings.get().ble_keyless_enabled;
    telemetry.ble_autolock_enabled = Settings.get().ble_autolock_enabled;
    telemetry.ble_paired          = Settings.get().ble_paired;
    telemetry.ble_connected       = false;
    telemetry.ble_rssi            = 0;
    telemetry.ble_signal_bars     = 0;
    telemetry.ble_pairing_active  = false;
    telemetry.ble_pair_success    = false;
    telemetry.ble_device_name[0]  = '\0';
    telemetry.tcs_active          = false;
    telemetry.tcs_slip_kmh        = 0.0f;
    telemetry.tcs_throttle_scale  = 1.0f;
    telemetry.tpms_warning        = false;
    telemetry.tpms_drift_pct      = 0.0f;
    telemetry.calibrated_wheel_diam_inch = Settings.get().wheel_diameter_in;

    // 4b. Initialize Traction Control (TCS) & Virtual TPMS Engine
    TcsTpmsManager::instance().begin();

    // 5. Initialize Touch and Input Handling (GT911 I2C SDA=8, SCL=9)
    Buttons.begin();
    Buttons.setTuningMode(false);
    Serial.println("[OK] Touch & Input Handler initialized (GT911 on I2C SDA=8, SCL=9)");

    // 6. Initialize ST7262 RGB Display and Backlight (via CH422G Expander)
    Serial.println("[INFO] Initializing Waveshare ST7262 RGB Panel and Touch via LovyanGFX...");
    if (!Display.init()) {
        Serial.println("[ERROR] Display initialization failed!");
    } else {
        Serial.println("[OK] Display initialized with Octal PSRAM frame buffer (800x480 landscape).");
    }
    uint8_t br = (uint8_t)map(Settings.get().brightness_pct, 0, 100, 0, 255);
    Display.setBrightness(br);

    // 7. Initialize Renderer
    Renderer.begin(&Display);
    Renderer.triggerBootAnimation();
    if (!Settings.get().pin_lock_enabled) {
        Renderer.triggerNeedleSweep();
    }

    // 8. Initialize VESC UART or Simulation Engine
    Vesc.begin();
#if SIMULATION_MODE
    Serial.println("[MODE] Self-Test Simulation Mode Active.");
    Serial.println("       Set SIMULATION_MODE in include/board_config.h to 0 for live Flipsky / VESC UART.");
#elif VESC_USE_CAN_BUS
    Serial.println("[MODE] Live VESC CAN Bus Mode Active (TX: GPIO 20, RX: GPIO 19 @ 500kbit/s).");
#else
    Serial.println("[MODE] Live VESC UART Mode Active (TX: GPIO 43, RX: GPIO 44).");
#endif

    // 9. Initialize Mode Profiles & Send Boot Stealth Limits to VESC (Requirement 1 & 4)
    ModeManager::instance().begin();
    if (!Settings.get().setup_completed) {
        telemetry.screen = SCREEN_SETUP_WIZARD;
        telemetry.is_locked = false;
        ModeManager::instance().setLocked(false);
        Serial.println("[SETUP] First-Time Onboarding Wizard ACTIVE.");
    } else if (Settings.get().pin_lock_enabled) {
        telemetry.is_locked = true;
        ModeManager::instance().setLocked(true);
        Serial.println("[SECURITY] Boot PIN-Lockscreen ACTIVE. VESC locked at 0A.");
    } else {
        telemetry.is_locked = false;
        ModeManager::instance().setLocked(false);
    }
    Serial.println("[OK] Mode Manager initialized: Stealth limits dispatched to VESC on boot.");

    // 9b. Initialize VESC Fault Blackbox Manager
    FaultManager::instance().begin();
    Serial.printf("[OK] Fault Manager initialized (%d logged faults).\n", FaultManager::instance().getFaultCount());

    // 10. Initialize ATGM336H GPS+BDS Satellite Tracker on GPIO 44 (RX)
    GpsTracker::instance().init();

    // 10b. Initialize 12V Headlight MOSFET Trigger Switch on GPIO 43
#if defined(PIN_HEADLIGHT_MOSFET) && (PIN_HEADLIGHT_MOSFET >= 0)
    pinMode(PIN_HEADLIGHT_MOSFET, OUTPUT);
    digitalWrite(PIN_HEADLIGHT_MOSFET, LOW);
    Serial.printf("[LIGHT] Initialized 12V Headlight MOSFET Switch on GPIO %d (State: OFF).\n", PIN_HEADLIGHT_MOSFET);
#endif

    // 11. Bluetooth Low Energy Keyless-Go Manager disabled as requested
    // BleKeylessManager::instance().begin();

    Serial.println("[OK] Setup complete. Entering main render loop.");
}

void loop() {
    static uint32_t last_render_ms = 0;
    static uint32_t last_log_ms = 0;
    static uint32_t boot_ms = millis();
    uint32_t now = millis();

    // Interactive Serial Command Console via USB (115200 Baud)
    // Active both in SIMULATION_MODE and on real hardware (when USB bridge is inactive)
    static uint32_t s_test_gas_start_ms = 0;
    static uint32_t s_test_gas_duration_ms = 0;
    static bool s_test_gas_active = false;
    static float s_test_gas_current = 0.0f;
    static float s_test_gas_duty = 0.0f;
    static bool s_test_gas_is_duty = false;
    static uint32_t s_last_test_gas_stream_ms = 0;
    static uint32_t s_last_test_telem_log_ms = 0;
    static String s_cmd_line = "";

    // Continuous drive stream while test gas active (every 30ms = ~33Hz refresh for VESC watchdog)
    if (s_test_gas_active) {
        if ((uint32_t)(now - s_test_gas_start_ms) < s_test_gas_duration_ms) {
            if (now - s_last_test_gas_stream_ms >= 30) {
                s_last_test_gas_stream_ms = now;
                if (s_test_gas_is_duty) {
                    Vesc.sendDuty(s_test_gas_duty);
                } else {
                    Vesc.sendCurrent(s_test_gas_current);
                }
            }
            if (now - s_last_test_telem_log_ms >= 200) {
                s_last_test_telem_log_ms = now;
                Serial.printf("[TEST TELEM] Spd: %.1f km/h | ERPM: %.0f | MotI: %.1fA | BatI: %.1fA | Duty: %.1f%%\n",
                              telemetry.vesc_motor_speed, telemetry.rpm * 15.0f, telemetry.phase_amps, telemetry.current_amps, telemetry.duty_cycle_pct);
            }
        } else {
            // Auto-stop safety timeout expired
            s_test_gas_active = false;
            s_test_gas_start_ms = 0;
            s_test_gas_duration_ms = 0;
            s_test_gas_current = 0.0f;
            s_test_gas_duty = 0.0f;
            s_test_gas_is_duty = false;
            Vesc.sendCurrent(0.0f);
            Vesc.sendDuty(0.0f);
            Vesc.setSimThrottle(0.0f);
            Renderer.markScreenDirty();
            Serial.println("[GAS SAFETY] Test-Gas Timer abgelaufen -> Motor 0.0A gestoppt.");
            Serial.printf("[FINAL TELEM] Spd: %.1f km/h | ERPM: %.0f | MotI: %.1fA | BatI: %.1fA\n",
                          telemetry.vesc_motor_speed, telemetry.rpm * 15.0f, telemetry.phase_amps, telemetry.current_amps);
        }
    }

    if (!Vesc.isUsbBridgeActive()) {
        while (Serial.available()) {
            char c = (char)Serial.read();

            // 1. PIN Digit Direct Entry while Locked
            if (telemetry.is_locked && (c >= '0' && c <= '9')) {
                int len = strlen(telemetry.pin_entry);
                if (len < 4) {
                    telemetry.pin_entry[len] = c;
                    telemetry.pin_entry[len + 1] = '\0';
                    telemetry.pin_error = false;
                    len++;
                    Renderer.markScreenDirty();
                    Serial.printf("[SERIAL PIN] Ziffer '%c' eingetippt -> Aktuell: '%s' (%d/4)\n", c, telemetry.pin_entry, len);

                    if (len == 4) {
                        if (strcmp(telemetry.pin_entry, Settings.get().pin_code) == 0) {
                            telemetry.is_locked = false;
                            telemetry.theft_alarm_active = false;
                            telemetry.locked_moved_meters = 0.0f;
                            telemetry.pin_error = false;
                            telemetry.pin_entry[0] = '\0';
                            ModeManager::instance().setLocked(false);
                            Vesc.sendFocPlayStop();
                            Vesc.sendCurrentBrake(0.0f);
                            Renderer.markScreenDirty();
                            Renderer.invalidateCache();
                            Renderer.triggerNeedleSweep();
                            Serial.println("[PIN] System ERFOLGREICH ENTSPERRT via Serial PIN-Eingabe! Alarm deaktiviert.");
                        } else {
                            telemetry.pin_error = true;
                            telemetry.pin_error_start_ms = millis();
                            telemetry.pin_error_end_ms = telemetry.pin_error_start_ms + 800;
                            Renderer.markScreenDirty();
                            Serial.printf("[PIN] Falscher PIN-Code '%s' eingegeben! (Erwartet: '%s')\n",
                                          telemetry.pin_entry, Settings.get().pin_code);
                        }
                    }
                }
            } else if (telemetry.is_locked && (c == '<' || c == '\b' || c == 127)) {
                int len = strlen(telemetry.pin_entry);
                if (len > 0) {
                    telemetry.pin_entry[len - 1] = '\0';
                    telemetry.pin_error = false;
                    Renderer.markScreenDirty();
                    Serial.printf("[SERIAL PIN] Ziffer geloescht -> Aktuell: '%s'\n", telemetry.pin_entry);
                }
            }

            // 2. Single Key Shortcuts (when not typing a longer word)
            if (s_cmd_line.length() == 0) {
                if (c == 'w' || c == 'W' || c == '+') {
                    if (telemetry.is_locked) {
                        Serial.println("[WARNUNG] System ist GESPERRT! Erst PIN eingeben (z. B. '1234') vor Gas geben.");
                    } else {
                        float test_amps = 5.0f;
                        s_test_gas_current = test_amps;
                        s_test_gas_duty = 0.0f;
                        s_test_gas_is_duty = false;
                        s_test_gas_active = true;
                        s_test_gas_start_ms = millis();
                        s_test_gas_duration_ms = 1500; // Auto-cutoff 1.5s
                        Vesc.sendCurrent(test_amps);
                        Vesc.setSimThrottle(25.0f);
                        Renderer.markScreenDirty();
                        Serial.printf("[TEST GAS] Motor-Strom %.1fA gesendet (Auto-Cutoff in 1.5s)!\n", test_amps);
                    }
                } else if (c == ' ' || c == '-') {
                    s_test_gas_active = false;
                    s_test_gas_start_ms = 0;
                    s_test_gas_duration_ms = 0;
                    s_test_gas_current = 0.0f;
                    s_test_gas_duty = 0.0f;
                    s_test_gas_is_duty = false;
                    Vesc.sendCurrent(0.0f);
                    Vesc.sendDuty(0.0f);
                    Vesc.sendCurrentBrake(0.0f);
                    Vesc.setSimThrottle(0.0f);
                    Renderer.markScreenDirty();
                    Serial.println("[TEST STOP] Motor sofort gestoppt (0.0A Freilauf).");
                } else if (c == 'p' || c == 'P') {
                    if (!telemetry.is_locked && (telemetry.speed_kmh > 0.8f || telemetry.vesc_motor_speed > 0.8f)) {
                        Serial.println("[SAFETY] Locking rejected: vehicle is moving!");
                    } else {
                        telemetry.is_locked = !telemetry.is_locked;
                        ModeManager::instance().setLocked(telemetry.is_locked);
                        telemetry.theft_alarm_active = false;
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        if (!telemetry.is_locked) {
                            Vesc.sendFocPlayStop();
                            Vesc.sendCurrentBrake(0.0f);
                            Renderer.triggerNeedleSweep();
                        }
                        Serial.printf("[SERIAL] PIN-Lockscreen: %s\n", telemetry.is_locked ? "GESPERRT" : "ENTSPERRT");
                    }
                } else if (c == 'r' || c == 'R') {
                    strncpy(Settings.get().pin_code, "1234", sizeof(Settings.get().pin_code));
                    Settings.save();
                    telemetry.is_locked = false;
                    telemetry.pin_error = false;
                    telemetry.pin_entry[0] = '\0';
                    ModeManager::instance().setLocked(false);
                    Vesc.sendFocPlayStop();
                    Vesc.sendCurrentBrake(0.0f);
                    Renderer.markScreenDirty();
                    Renderer.invalidateCache();
                    Renderer.triggerNeedleSweep();
                    Serial.println("[SERIAL] PIN auf '1234' resetet & System entsperrt!");
                } else if (c == 'h' || c == 'H' || c == '?') {
                    Serial.println("=== DISPLAY SERIAL COMMANDS ===");
                    Serial.println("  0-9       : PIN Ziffer eintippen (1234 zum Entsperren)");
                    Serial.println("  < / bksp  : PIN Ziffer loeschen");
                    Serial.println("  w / +     : Test-Gas geben (3.0A, 1.5s Auto-Cutoff)");
                    Serial.println("  gas <A>   : Test-Gas mit Amps geben (z.B. 'gas 3.5')");
                    Serial.println("  duty <% > : Test-Duty cycle geben (z.B. 'duty 5' = 5%)");
                    Serial.println("  s / space : Motor stoppen (0.0A / 0% Duty)");
                    Serial.println("  p / lock  : Sperren / Entsperren toggeln");
                    Serial.println("  r         : PIN auf 1234 resetten & entsperren");
                    Serial.println("  status    : System Telemetrie anzeigen");
                    Serial.println("  theme     : Farb-Theme umschalten");
                    Serial.println("  unlock    : Direkt per Textbefehl entsperren");
                    Serial.println("===============================");
                }
            }

            // 3. Line accumulator for text commands (like 'unlock', 'gas 2.5', 'duty 5', 'lock', 'status')
            if (c == '\r' || c == '\n') {
                s_cmd_line.trim();
                if (s_cmd_line.length() > 0) {
                    if (s_cmd_line.equalsIgnoreCase("unlock") || s_cmd_line == Settings.get().pin_code) {
                        telemetry.is_locked = false;
                        telemetry.theft_alarm_active = false;
                        telemetry.locked_moved_meters = 0.0f;
                        telemetry.pin_error = false;
                        telemetry.pin_entry[0] = '\0';
                        ModeManager::instance().setLocked(false);
                        Vesc.sendFocPlayStop();
                        Vesc.sendCurrentBrake(0.0f);
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        Renderer.triggerNeedleSweep();
                        Serial.println("[PIN] System UNLOCKED via Serial Command! Alarm disarmed.");
                    } else if (s_cmd_line.equalsIgnoreCase("lock")) {
                        if (telemetry.speed_kmh > 0.8f || telemetry.vesc_motor_speed > 0.8f) {
                            Serial.println("[SAFETY] Locking rejected: vehicle is moving!");
                        } else {
                            telemetry.is_locked = true;
                            ModeManager::instance().setLocked(true);
                            Renderer.markScreenDirty();
                            Renderer.invalidateCache();
                            Serial.println("[PIN] System LOCKED via Serial Command.");
                        }
                    } else if (s_cmd_line.equalsIgnoreCase("tuning on") || s_cmd_line.equalsIgnoreCase("tuning")) {
                        telemetry.tuning_mode = true;
                        ModeManager::instance().setTuningMode(true);
                        Buttons.setTuningMode(true);
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        Serial.println("[MODE] TUNING-MODUS AKTIVIERT (Offener Modus, Drossel aus)!");
                    } else if (s_cmd_line.equalsIgnoreCase("tuning off") || s_cmd_line.equalsIgnoreCase("legal")) {
                        telemetry.tuning_mode = false;
                        ModeManager::instance().setTuningMode(false);
                        Buttons.setTuningMode(false);
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        Serial.println("[MODE] LEGALER POLIZEI-MODUS AKTIVIERT (20 km/h gedrosselt)!");
                    } else if (s_cmd_line.equalsIgnoreCase("mode eco")) {
                        ModeManager::instance().setRideMode(MODE_ECO);
                        Serial.println("[MODE] Ride-Mode -> ECO");
                    } else if (s_cmd_line.equalsIgnoreCase("mode sport")) {
                        ModeManager::instance().setRideMode(MODE_SPORT);
                        Serial.println("[MODE] Ride-Mode -> SPORT");
                    } else if (s_cmd_line.equalsIgnoreCase("mode race")) {
                        ModeManager::instance().setRideMode(MODE_RACE);
                        Serial.println("[MODE] Ride-Mode -> RACE");
                    } else if (s_cmd_line.equalsIgnoreCase("vollgas") || s_cmd_line.equalsIgnoreCase("max")) {
                        if (telemetry.is_locked) {
                            Serial.println("[WARNUNG] System ist GESPERRT! Erst PIN eingeben.");
                        } else {
                            ModeLimits lim = ModeManager::instance().getActiveLimits();
                            float mode_phase = lim.max_phase_amps;
                            if (mode_phase > 60.0f) mode_phase = 60.0f; // safety max for bench
                            s_test_gas_current = mode_phase;
                            s_test_gas_duty = 0.0f;
                            s_test_gas_is_duty = false;
                            s_test_gas_active = true;
                            s_test_gas_start_ms = millis();
                            s_test_gas_duration_ms = 3000; // 3.0s full throttle
                            Vesc.sendCurrent(mode_phase);
                            Vesc.setSimThrottle(100.0f);
                            Renderer.markScreenDirty();
                            Serial.printf("[VOLLGAS] Sende %.1fA Phasenstrom (3.0s Vollgas im Modus %s, Tuning=%s)!\n",
                                          mode_phase, ModeManager::instance().getActiveModeName(),
                                          telemetry.tuning_mode ? "AN" : "AUS");
                        }
                    } else if (s_cmd_line.startsWith("gas ") || s_cmd_line.startsWith("cur ")) {
                        if (telemetry.is_locked) {
                            Serial.println("[WARNUNG] System ist GESPERRT! Erst PIN eingeben.");
                        } else {
                            String param = s_cmd_line.substring(4);
                            param.trim();
                            int space_idx = param.indexOf(' ');
                            float amps = param.toFloat();
                            float secs = 1.5f;
                            if (space_idx > 0) {
                                amps = param.substring(0, space_idx).toFloat();
                                secs = param.substring(space_idx + 1).toFloat();
                                if (secs > 5.0f) secs = 5.0f;
                                if (secs < 0.2f) secs = 0.2f;
                            }
                            if (amps > 60.0f) amps = 60.0f;
                            if (amps < 0.0f) amps = 0.0f;
                            if (amps > 0.0f) {
                                s_test_gas_current = amps;
                                s_test_gas_duty = 0.0f;
                                s_test_gas_is_duty = false;
                                s_test_gas_active = true;
                                s_test_gas_start_ms = millis();
                                s_test_gas_duration_ms = (uint32_t)(secs * 1000.0f);
                                Vesc.sendCurrent(amps);
                                Vesc.setSimThrottle(amps * 2.0f);
                                Serial.printf("[TEST GAS] Motor-Strom %.1fA fuer %.1fs gesendet!\n", amps, secs);
                            } else {
                                s_test_gas_active = false;
                                s_test_gas_start_ms = 0;
                                s_test_gas_duration_ms = 0;
                                s_test_gas_current = 0.0f;
                                s_test_gas_duty = 0.0f;
                                s_test_gas_is_duty = false;
                                Vesc.sendCurrent(0.0f);
                                Vesc.sendDuty(0.0f);
                                Vesc.setSimThrottle(0.0f);
                                Serial.println("[TEST GAS] Motor 0.0A gestoppt.");
                            }
                        }
                    } else if (s_cmd_line.startsWith("duty ")) {
                        if (telemetry.is_locked) {
                            Serial.println("[WARNUNG] System ist GESPERRT! Erst PIN eingeben.");
                        } else {
                            String param = s_cmd_line.substring(5);
                            param.trim();
                            int space_idx = param.indexOf(' ');
                            float duty_pct = param.toFloat();
                            float secs = 1.5f;
                            if (space_idx > 0) {
                                duty_pct = param.substring(0, space_idx).toFloat();
                                secs = param.substring(space_idx + 1).toFloat();
                                if (secs > 5.0f) secs = 5.0f;
                                if (secs < 0.2f) secs = 0.2f;
                            }
                            float max_duty = telemetry.tuning_mode ? 85.0f : 20.0f;
                            if (duty_pct > max_duty) duty_pct = max_duty;
                            if (duty_pct < 0.0f) duty_pct = 0.0f;
                            float duty_frac = duty_pct / 100.0f;
                            if (duty_frac > 0.0f) {
                                s_test_gas_duty = duty_frac;
                                s_test_gas_current = 0.0f;
                                s_test_gas_is_duty = true;
                                s_test_gas_active = true;
                                s_test_gas_start_ms = millis();
                                s_test_gas_duration_ms = (uint32_t)(secs * 1000.0f);
                                Vesc.sendDuty(duty_frac);
                                Vesc.setSimThrottle(duty_pct);
                                Serial.printf("[TEST DUTY] Motor Duty %.1f%% (%.3f) fuer %.1fs gesendet!\n", duty_pct, duty_frac, secs);
                            } else {
                                s_test_gas_active = false;
                                s_test_gas_start_ms = 0;
                                s_test_gas_duration_ms = 0;
                                s_test_gas_current = 0.0f;
                                s_test_gas_duty = 0.0f;
                                s_test_gas_is_duty = false;
                                Vesc.sendCurrent(0.0f);
                                Vesc.sendDuty(0.0f);
                                Vesc.setSimThrottle(0.0f);
                                Serial.println("[TEST DUTY] Motor 0% gestoppt.");
                            }
                        }
                    } else if (s_cmd_line.equalsIgnoreCase("stop") || s_cmd_line.equalsIgnoreCase("s")) {
                        s_test_gas_active = false;
                        s_test_gas_start_ms = 0;
                        s_test_gas_duration_ms = 0;
                        s_test_gas_current = 0.0f;
                        s_test_gas_duty = 0.0f;
                        s_test_gas_is_duty = false;
                        Vesc.sendCurrent(0.0f);
                        Vesc.sendDuty(0.0f);
                        Vesc.sendCurrentBrake(0.0f);
                        Vesc.setSimThrottle(0.0f);
                        Renderer.markScreenDirty();
                        Serial.println("[TEST STOP] Motor gestoppt.");
                    } else if (s_cmd_line.equalsIgnoreCase("status") || s_cmd_line.equalsIgnoreCase("i")) {
                        Serial.println("=== SYSTEM STATUS ===");
                        Serial.printf("  Lock: %s | PIN-Code: '%s'\n", telemetry.is_locked ? "GESPERRT" : "ENTSPERRT", Settings.get().pin_code);
                        Serial.printf("  Alarm: %s | Moved: %.2fm\n", telemetry.theft_alarm_active ? "AKTIV" : "INAKTIV", telemetry.locked_moved_meters);
                        Serial.printf("  VESC: %s | Spd: %.1f km/h | RPM: %.0f | MotI: %.1fA | BatI: %.1fA | Vin: %.1fV\n",
                                      telemetry.vesc_connected ? "VERBUNDEN" : "GETRENNT",
                                      telemetry.vesc_motor_speed, telemetry.rpm, telemetry.phase_amps, telemetry.current_amps, telemetry.voltage);
                        Serial.printf("  Mode: %s | Theme: %s\n", ModeManager::instance().getActiveModeName(), DashboardRenderer::getThemePalette(telemetry.color_theme).name);
                        Serial.println("=====================");
                    } else if (s_cmd_line.equalsIgnoreCase("theme")) {
                        uint8_t th = (Settings.get().color_theme + 1) % THEME_COUNT;
                        Settings.get().color_theme = th;
                        Settings.save();
                        telemetry.color_theme = th;
                        Renderer.invalidateCache();
                        Renderer.markScreenDirty();
                        Serial.printf("[SERIAL] Farb-Theme gewechselt auf: %s\n", DashboardRenderer::getThemePalette(th).name);
                    } else if (s_cmd_line.equalsIgnoreCase("help")) {
                        Serial.println("Befehle: unlock, lock, gas <A>, duty <%>, stop, status, theme, help");
                    }
                    s_cmd_line = "";
                }
            } else if (c >= 32 && c <= 126) {
                if (s_cmd_line.length() < 32) {
                    s_cmd_line += c;
                }
            }
        }
    }

    // 0. High-Priority Wireless OTA Firmware Update in Progress
    if (OtaManager::instance().isUpdating()) {
        OtaManager::instance().update();
        Renderer.render(telemetry);
        delay(15);
        return;
    }

    // Sync external tuning mode or ride mode changes from Captive Portal web UI (Core 0 -> Core 1)
    if (telemetry.tuning_mode != ModeManager::instance().isTuningMode()) {
        telemetry.tuning_mode = ModeManager::instance().isTuningMode();
        Buttons.setTuningMode(telemetry.tuning_mode);
        telemetry.mode_badge_until = now + 2500;
        if (!telemetry.tuning_mode && telemetry.screen == SCREEN_RACE_TIMER) {
            telemetry.screen = SCREEN_RIDE_DASH;
            telemetry.race_state = 0;
            telemetry.ride_mode = telemetry.saved_ride_mode;
            ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
            telemetry.boost_active = false;
            telemetry.race_popup_open = false;
            telemetry.race_dyno_open = false;
            Buttons.setRaceState(0);
            Buttons.setRacePopupOpen(false);
            Buttons.setRaceDynoOpen(false);
            Renderer.invalidateCache();
            Renderer.markScreenDirty();
        }
    }
    if (telemetry.ride_mode != ModeManager::instance().getRideMode()) {
        telemetry.ride_mode = ModeManager::instance().getRideMode();
        telemetry.mode_badge_until = now + 2500;
    }

    // Auto-unlock if PIN lock was disabled in Settings
    if (!Settings.get().pin_lock_enabled && telemetry.is_locked) {
        telemetry.is_locked = false;
        ModeManager::instance().setLocked(false);
        Renderer.markScreenDirty();
        Renderer.invalidateCache();
        Renderer.triggerNeedleSweep();
    }

    // VESC-App Auto-Unlock: Unlock if VESC App connects via WiFi Bridge, USB Bridge, or active bridge traffic
    bool vesc_app_connected = Vesc.isWifiBridgeActive() ||
                              Vesc.isUsbBridgeActive() ||
                              (Vesc.isBridgeActive() && (Vesc.getBridgePcToVescBytes() > 0 || Vesc.getBridgeWifiToVescBytes() > 0))
#if SIMULATION_MODE
                              || s_sim_vesc_app_connected
#endif
                              ;

    if (telemetry.is_locked && vesc_app_connected) {
        telemetry.is_locked = false;
        ModeManager::instance().setLocked(false);
        Renderer.markScreenDirty();
        Renderer.invalidateCache();
        Renderer.triggerNeedleSweep();
        Serial.println("[SECURITY] VESC-App Verbindung aktiv: Display & VESC entsperrt!");
    }

    // BLE Keyless-Go disabled as requested
    // BleKeylessManager::instance().update(telemetry, now);

    // 1. Process Touch & Button Navigation Inputs
    Buttons.setLocked(telemetry.is_locked);
    Buttons.setActiveScreen(telemetry.screen);
    Buttons.setRacePopupOpen(telemetry.race_popup_open);
    Buttons.setRaceDynoOpen(telemetry.race_dyno_open);
    Buttons.setRaceState(telemetry.race_state);
    Buttons.update();
    Renderer.updateRaceTimer(telemetry, now);

    // Boot Animation Screen Tap Handling: Auto-continues if connected, or skips search and reveals lines if offline
    static uint32_t s_boot_skip_ms = 0;
    if (Renderer.isBootAnimActive()) {
        int16_t tx = -1, ty = -1;
        if (Buttons.getLastTap(&tx, &ty) || Buttons.isTouching()) {
            Renderer.handleBootTouch(telemetry.vesc_connected);
            Buttons.clearNavAction();
            s_boot_skip_ms = now;
            Serial.println("[BOOT] Screen tapped during boot animation -> handleBootTouch executed.");
        }
    } else if (s_boot_skip_ms != 0 && now - s_boot_skip_ms < 300) {
        // Discard any residual tap from the boot skip touch so it doesn't trigger a PIN keypad button
        int16_t dummy_x, dummy_y;
        Buttons.getLastTap(&dummy_x, &dummy_y);
        Buttons.clearNavAction();
    }

    // PIN Lockscreen Input Processing (when locked and boot animation finished)
    if (telemetry.is_locked && !Renderer.isBootAnimActive()) {
        if (telemetry.pin_error) {
            if ((uint32_t)(millis() - telemetry.pin_error_start_ms) >= 800) {
                telemetry.pin_error = false;
                telemetry.pin_entry[0] = '\0'; // Clear buffer AFTER red error display window ends!
                Renderer.markScreenDirty();
            } else {
                // Debounce / spam immunity: Discard any taps while error indicator is showing
                int16_t dummy_x, dummy_y;
                Buttons.getLastTap(&dummy_x, &dummy_y);
            }
        }

        int16_t tx = -1, ty = -1;
        if (!telemetry.pin_error && Buttons.getLastTap(&tx, &ty)) {
            // Continuous column mapping without dead zones between buttons (x: 100..700)
            int c = -1;
            if (tx >= 100 && tx < 330) c = 0;
            else if (tx >= 330 && tx < 470) c = 1;
            else if (tx >= 470 && tx <= 700) c = 2;

            // Continuous row mapping without dead zones between rows (y: 120..460)
            int r = -1;
            if (ty >= 120 && ty < 227) r = 0;
            else if (ty >= 227 && ty < 297) r = 1;
            else if (ty >= 297 && ty < 367) r = 2;
            else if (ty >= 367 && ty <= 460) r = 3;

            if (r >= 0 && c >= 0) {
                static const char KEY_CHARS[4][3] = {
                    { '1', '2', '3' },
                    { '4', '5', '6' },
                    { '7', '8', '9' },
                    { '<', '0', '#' }
                };
                char key = KEY_CHARS[r][c];
                Serial.printf("[TOUCH PIN] tx=%d, ty=%d -> r=%d, c=%d, key='%c'\n", tx, ty, r, c, key);

                // Tactile / acoustic motor tick feedback on button press (silent to avoid deck thumps)
                // Vesc.triggerFocBeep(2600.0f, 0.03f, 1.8f);

                int len = strlen(telemetry.pin_entry);

                if (key == '<') { // DEL
                    if (len > 0) {
                        telemetry.pin_entry[len - 1] = '\0';
                        telemetry.pin_error = false;
                        Renderer.markScreenDirty();
                    }
                } else if (key == '#') { // OK / enter
                    if (strcmp(telemetry.pin_entry, Settings.get().pin_code) == 0) {
                        telemetry.is_locked = false;
                        telemetry.theft_alarm_active = false;
                        telemetry.locked_moved_meters = 0.0f;
                        telemetry.pin_error = false;
                        telemetry.pin_entry[0] = '\0';
                        ModeManager::instance().setLocked(false);
                        Vesc.sendFocPlayStop(); // Stop any active alarm tone immediately
                        Vesc.sendCurrentBrake(0.0f);
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        Renderer.triggerNeedleSweep();
                        Serial.println("[PIN] System UNLOCKED with valid PIN code! Alarm disarmed.");
                    } else {
                        telemetry.pin_error = true;
                        telemetry.pin_error_start_ms = millis();
                        telemetry.pin_error_end_ms = telemetry.pin_error_start_ms + 800;
                        // Keep pin_entry intact so all 4 dots are shown in bright red!
                        Renderer.markScreenDirty();
                        Serial.printf("[PIN] Invalid PIN code '%s' entered! (Expected: '%s')\n",
                                      telemetry.pin_entry, Settings.get().pin_code);
                    }
                } else if (key >= '0' && key <= '9') {
                    if (len < 4) {
                        telemetry.pin_entry[len] = key;
                        telemetry.pin_entry[len + 1] = '\0';
                        telemetry.pin_error = false;
                        len++;
                        Renderer.markScreenDirty();

                        if (len == 4) {
                            if (strcmp(telemetry.pin_entry, Settings.get().pin_code) == 0) {
                                telemetry.is_locked = false;
                                telemetry.theft_alarm_active = false;
                                telemetry.locked_moved_meters = 0.0f;
                                telemetry.pin_error = false;
                                telemetry.pin_entry[0] = '\0';
                                ModeManager::instance().setLocked(false);
                                Vesc.sendFocPlayStop(); // Stop any active alarm tone immediately
                                Vesc.sendCurrentBrake(0.0f);
                                Renderer.markScreenDirty();
                                Renderer.invalidateCache();
                                Renderer.triggerNeedleSweep();
                                Serial.println("[PIN] System UNLOCKED automatically upon 4 digits! Alarm disarmed.");
                            } else {
                                telemetry.pin_error = true;
                                telemetry.pin_error_start_ms = millis();
                                telemetry.pin_error_end_ms = telemetry.pin_error_start_ms + 800;
                                // Keep pin_entry intact so all 4 dots are shown in bright red!
                                Renderer.markScreenDirty();
                                Serial.printf("[PIN] Invalid PIN code '%s' entered! (Expected: '%s')\n",
                                              telemetry.pin_entry, Settings.get().pin_code);
                            }
                        }
                    }
                }
            }
        }
    }

    // Theft Protection & Movement Alarm Tracking (when locked)
    static bool s_was_locked = false;
    static uint32_t s_last_alarm_beep_ms = 0;
    static uint8_t s_alarm_beep_cadence = 0;
    static uint32_t s_last_movement_calc_ms = millis();
    static uint32_t s_lock_arm_start_ms = 0;

    if (telemetry.is_locked) {
        uint32_t dt_ms = now - s_last_movement_calc_ms;
        s_last_movement_calc_ms = now;
        if (dt_ms > 500) dt_ms = 50;
        float dt_sec = dt_ms / 1000.0f;

        if (!s_was_locked) {
            s_was_locked = true;
            telemetry.theft_alarm_active = false;
            telemetry.locked_moved_meters = 0.0f;
            s_lock_arm_start_ms = now; // 4s boot/lock arming grace period
            Serial.println("[SECURITY] Theft protection ARMED (4s grace window).");
        }

        // Only evaluate movement AFTER the 4-second boot/lock arming grace period
        if ((uint32_t)(now - s_lock_arm_start_ms) >= 4000) {
            // Measure actual physical vehicle movement:
            // 1. Motor wheel rotation speed (VESC telemetry):
            float raw_v = telemetry.vesc_motor_speed;
            if (telemetry.rpm > 15.0f && raw_v < 0.5f) {
                float wheel_circ_m = (Settings.get().wheel_diameter_in * 0.0254f) * M_PI;
                float gear_ratio = Settings.get().gear_ratio > 0.1f ? Settings.get().gear_ratio : 1.0f;
                raw_v = (telemetry.rpm / gear_ratio) * wheel_circ_m * 60.0f / 1000.0f;
            }

            // 2. High-quality GPS ground speed (only if 3D fix is solid, HDOP < 1.6, and speed >= 3.0 km/h to reject drift)
            float gps_v = 0.0f;
            if (telemetry.gps.valid && telemetry.gps.satellites >= 6 && telemetry.gps.hdop < 1.6f && telemetry.gps.speed_kmh >= 3.0f) {
                gps_v = telemetry.gps.speed_kmh;
            }

            // Only count if physical wheel rotation >= 1.2 km/h OR genuine GPS movement >= 3.0 km/h
            float move_v = 0.0f;
            if (raw_v >= 1.2f) {
                move_v = raw_v;
            } else if (gps_v >= 3.0f) {
                move_v = gps_v;
            }

            if (move_v > 0.0f) {
                telemetry.locked_moved_meters += (move_v * (1000.0f / 3600.0f)) * dt_sec;
            } else if (telemetry.locked_moved_meters > 0.0f && !telemetry.theft_alarm_active) {
                // Gentle decay if vehicle is completely stationary (prevents slow noise accumulation)
                telemetry.locked_moved_meters -= 0.15f * dt_sec;
                if (telemetry.locked_moved_meters < 0.0f) telemetry.locked_moved_meters = 0.0f;
            }

            // Trigger condition: "wenn es gesperrt ist und sich ein paar meter bewegt" -> >= 2.0 meters
            if (telemetry.locked_moved_meters >= 2.0f && !telemetry.theft_alarm_active) {
                telemetry.theft_alarm_active = true;
                Renderer.markScreenDirty();
                Serial.printf("[ALARM] DIEBSTAHLWARNUNG! Moved %.2f meters while locked! Triggering FOC Motor Siren.\n",
                              telemetry.locked_moved_meters);
            }
        }

        // While alarm is active: pulse loud VESC FOC siren through motor windings
        if (telemetry.theft_alarm_active) {
            if (now - s_last_alarm_beep_ms >= 350) {
                s_last_alarm_beep_ms = now;
                s_alarm_beep_cadence = (s_alarm_beep_cadence + 1) % 4;
                float freqs[4] = { 1600.0f, 2400.0f, 1800.0f, 2800.0f };
                Vesc.triggerFocBeep(freqs[s_alarm_beep_cadence], 0.30f, 6.0f);
            }
        }

        static uint32_t s_last_sec_dbg = 0;
        if (now - s_last_sec_dbg >= 1000) {
            s_last_sec_dbg = now;
            Serial.printf("[SEC_DBG] locked=%d, alarm=%d, moved=%.2fm, rpm=%.1f, v_mot=%.2f, v_gps=%.2f, sats=%d, screen=%d\n",
                          (int)telemetry.is_locked, (int)telemetry.theft_alarm_active, telemetry.locked_moved_meters,
                          telemetry.rpm, telemetry.vesc_motor_speed, telemetry.gps.speed_kmh,
                          (int)telemetry.gps.satellites, (int)telemetry.screen);
        }
    } else {
        if (s_was_locked) {
            s_was_locked = false;
            telemetry.theft_alarm_active = false;
            telemetry.locked_moved_meters = 0.0f;
            Vesc.sendFocPlayStop(); // Instantly silence motor on unlock
            Renderer.invalidateCache();
            Renderer.markScreenDirty();
        }
        s_last_movement_calc_ms = now;
    }

    telemetry.gps_enabled = Settings.get().gps_enabled;
    telemetry.incline_gforce_enabled = Settings.get().incline_gforce_enabled;
    telemetry.wifi_bridge_enabled = Settings.get().wifi_bridge_enabled;
    telemetry.setup_completed = Settings.get().setup_completed;

    // SCREEN_SETUP_WIZARD: Interactive First-Time Setup & Onboarding Handling
    if (telemetry.screen == SCREEN_SETUP_WIZARD) {
        int16_t tx = -1, ty = -1;
        if (Buttons.getLastTap(&tx, &ty)) {
            Renderer.handleWizardTouch(tx, ty, telemetry);
            Buttons.clearNavAction();
        }
    }

    // SCREEN_SETTINGS_MENU: Interactive Touch & Navigation Handling
    if (!telemetry.is_locked && telemetry.screen == SCREEN_SETTINGS_MENU) {
        int16_t tx = -1, ty = -1;
        if (Buttons.getLastTap(&tx, &ty)) {
            Renderer.handleSettingsTouch(tx, ty, telemetry);
            Buttons.clearNavAction(); // Clear any pending action to prevent double-invocation
        } else if (Buttons.isTouching() && Renderer.getSettingsTab() < 3) {
            int16_t cx = Buttons.getTouchX();
            int16_t cy = Buttons.getTouchY();
            Renderer.handleSettingsSliderDrag(cx, cy, telemetry);
        }
    }

    NavAction nav = Buttons.getNavAction();
    if ((telemetry.is_locked || Renderer.isBootAnimActive()) && nav != NAV_PIN_EMERGENCY_RESET) {
        nav = NAV_NONE;
    }

    if (nav != NAV_NONE) {
        if (nav == NAV_PIN_EMERGENCY_RESET) {
            strncpy(Settings.get().pin_code, "1234", sizeof(Settings.get().pin_code));
            Settings.save();
            telemetry.is_locked = false;
            telemetry.theft_alarm_active = false;
            telemetry.locked_moved_meters = 0.0f;
            telemetry.pin_error = false;
            telemetry.pin_entry[0] = '\0';
            ModeManager::instance().setLocked(false);
            Renderer.markScreenDirty();
            Renderer.invalidateCache();
            Renderer.triggerNeedleSweep();
            Serial.println("[SECURITY] NOTFALL-RESET: PIN auf 1234 zurueckgesetzt & System entsperrt!");
        } else if (Vesc.isBridgeActive()) {
            if (nav == NAV_BTN2_LONG || nav == NAV_BOTH_PRESSED || nav == NAV_BTN1_LONG || nav == NAV_BTN1_SHORT || nav == NAV_BTN2_SHORT || nav == NAV_SCREEN_DASH) {
                if (Vesc.isUsbBridgeActive()) Vesc.exitBridgeMode();
                if (Vesc.isWifiBridgeActive()) Vesc.exitWifiBridgeMode();
                telemetry.screen = SCREEN_RIDE_DASH;
                Renderer.markScreenDirty();
                Renderer.invalidateCache();
            }
        } else if (telemetry.screen == SCREEN_SETTINGS_MENU) {
            Renderer.handleMenuNav(nav, telemetry);
        } else {
            switch (nav) {
                case NAV_SCREEN_DASH:
                    if (telemetry.screen == SCREEN_RACE_TIMER) {
                        telemetry.race_state = 0;
                        telemetry.ride_mode = telemetry.saved_ride_mode;
                        ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
                        telemetry.boost_active = false;
                        telemetry.race_popup_open = false;
                        telemetry.race_dyno_open = false;
                        Buttons.setRaceState(0);
                        Buttons.setRacePopupOpen(false);
                        Buttons.setRaceDynoOpen(false);
                    }
                    telemetry.screen = SCREEN_RIDE_DASH;
                    Renderer.markScreenDirty();
                    Renderer.invalidateCache();
                    Serial.println("[NAV] Switched to SCREEN_RIDE_DASH (< DASH)");
                    break;
                case NAV_BTN1_SHORT: // Previous Screen (Swipe Right / Left Tap)
                    {
                        if (telemetry.screen == SCREEN_RT_GRAPHS || telemetry.screen == SCREEN_RACE_TIMER) {
                            if (telemetry.screen == SCREEN_RACE_TIMER) {
                                telemetry.race_state = 0;
                                telemetry.ride_mode = telemetry.saved_ride_mode;
                                ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
                                telemetry.boost_active = false;
                                telemetry.race_popup_open = false;
                                telemetry.race_dyno_open = false;
                                Buttons.setRaceState(0);
                                Buttons.setRacePopupOpen(false);
                                Buttons.setRaceDynoOpen(false);
                            }
                            telemetry.screen = SCREEN_RIDE_DASH;
                            Renderer.markScreenDirty();
                        } else {
#if ENABLE_LIGHTS_SCREEN
                            int cur = (int)telemetry.screen;
                            telemetry.screen = (DashboardScreen)((cur - 1 + 3) % 3);
#else
                            telemetry.screen = (telemetry.screen == SCREEN_RIDE_DASH) ? SCREEN_RT_GRAPHS : SCREEN_RIDE_DASH;
#endif
                        }
                        Serial.printf("[NAV] Switched to Screen ID %d (Prev)\n", (int)telemetry.screen);
                    }
                    break;

                case NAV_BTN2_SHORT: // Next Screen (Swipe Left / Right Tap)
                    {
                        if (telemetry.screen == SCREEN_RACE_TIMER) {
                            telemetry.race_state = 0;
                            telemetry.ride_mode = telemetry.saved_ride_mode;
                            ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
                            telemetry.boost_active = false;
                            telemetry.race_popup_open = false;
                            telemetry.race_dyno_open = false;
                            Buttons.setRaceState(0);
                            Buttons.setRacePopupOpen(false);
                            Buttons.setRaceDynoOpen(false);
                            telemetry.screen = SCREEN_RIDE_DASH;
                            Renderer.markScreenDirty();
                        } else if (telemetry.screen == SCREEN_RT_GRAPHS) {
                            telemetry.screen = SCREEN_RIDE_DASH;
                        } else {
#if ENABLE_LIGHTS_SCREEN
                            int cur = (int)telemetry.screen;
                            telemetry.screen = (DashboardScreen)((cur + 1) % 3);
#else
                            telemetry.screen = (telemetry.screen == SCREEN_RIDE_DASH) ? SCREEN_RT_GRAPHS : SCREEN_RIDE_DASH;
#endif
                        }
                        Serial.printf("[NAV] Switched to Screen ID %d (Next)\n", (int)telemetry.screen);
                    }
                    break;

                case NAV_SCREEN_STATS:
                    if (telemetry.screen == SCREEN_RACE_TIMER) {
                        telemetry.race_state = 0;
                        telemetry.ride_mode = telemetry.saved_ride_mode;
                        ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
                        telemetry.boost_active = false;
                        telemetry.race_popup_open = false;
                        telemetry.race_dyno_open = false;
                        Buttons.setRaceState(0);
                        Buttons.setRacePopupOpen(false);
                        Buttons.setRaceDynoOpen(false);
                    }
                    telemetry.screen = SCREEN_RT_GRAPHS;
                    Renderer.markScreenDirty();
                    Serial.println("[NAV] [ STATS ] button tapped -> Switched to Screen ID 2 (RT Graphs)");
                    break;

                case NAV_SCREEN_RACE:
                    if (!telemetry.tuning_mode) {
                        Serial.println("[NAV] NAV_SCREEN_RACE blocked: tuning_mode is false (police mode)");
                        break;
                    }
                    if (telemetry.screen != SCREEN_RACE_TIMER) {
                        telemetry.saved_ride_mode = telemetry.ride_mode;
                    }
                    telemetry.screen = SCREEN_RACE_TIMER;
                    if (!isRaceThermalBlocked(telemetry)) {
                        telemetry.ride_mode = MODE_RACE;
                        ModeManager::instance().setRideMode(MODE_RACE);
                        telemetry.boost_active = true;
                        telemetry.boost_start_ms = now;
                        ModeManager::instance().setBoostActive(true);
                    } else {
                        telemetry.boost_active = false;
                        ModeManager::instance().setBoostActive(false);
                    }
                    telemetry.boost_end_ms = now + 60000;
                    Renderer.markScreenDirty();
                    Serial.println("[NAV] Switched to SCREEN_RACE_TIMER");
                    break;

                case NAV_RACE_TOGGLE_POPUP:
                    telemetry.race_popup_open = !telemetry.race_popup_open;
                    if (telemetry.race_popup_open) telemetry.race_dyno_open = false;
                    Buttons.setRacePopupOpen(telemetry.race_popup_open);
                    Buttons.setRaceDynoOpen(telemetry.race_dyno_open);
                    Renderer.markScreenDirty();
                    break;

                case NAV_RACE_TOGGLE_DYNO:
                    telemetry.race_dyno_open = !telemetry.race_dyno_open;
                    if (telemetry.race_dyno_open) telemetry.race_popup_open = false;
                    Buttons.setRacePopupOpen(telemetry.race_popup_open);
                    Buttons.setRaceDynoOpen(telemetry.race_dyno_open);
                    Renderer.markScreenDirty();
                    break;

                case NAV_RACE_CLOSE_POPUP:
                    telemetry.race_popup_open = false;
                    telemetry.race_dyno_open = false;
                    Buttons.setRacePopupOpen(false);
                    Buttons.setRaceDynoOpen(false);
                    Renderer.markScreenDirty();
                    break;

                case NAV_RACE_RESET_RECORDS:
                    telemetry.race_records.reset();
                    Settings.saveRaceRecords(telemetry.race_records);
                    Renderer.markScreenDirty();
                    Serial.println("[RACE] Reset personal best records in NVS");
                    break;

                case NAV_RACE_START_LAUNCH:
                    if (isRaceThermalBlocked(telemetry)) {
                        Serial.println("[RACE] Launch blocked: Motor/ESC temperature > 65 C!");
                        break;
                    }
                    telemetry.ride_mode = MODE_RACE;
                    ModeManager::instance().setRideMode(MODE_RACE);
                    telemetry.boost_active = true;
                    telemetry.boost_start_ms = now;
                    ModeManager::instance().setBoostActive(true);
                    telemetry.boost_end_ms = now + 60000;
                    telemetry.race_state = 1; // COUNTDOWN (5s)
                    telemetry.race_countdown_start_ms = now;
                    telemetry.race_elapsed_sec = 0.0f;
                    telemetry.race_split_0_22 = 0.0f;
                    telemetry.race_split_0_50 = 0.0f;
                    telemetry.race_split_0_70 = 0.0f;
                    Buttons.setRaceState(1);
                    Serial.println("[RACE] 5-second launch countdown started!");
                    break;

                case NAV_RACE_ABORT:
                    telemetry.race_state = 0; // IDLE
                    telemetry.ride_mode = telemetry.saved_ride_mode;
                    ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
                    telemetry.boost_active = false;
                    ModeManager::instance().setBoostActive(false);
                    Buttons.setRaceState(0);
                    Serial.println("[RACE] Launch timer stopped/aborted (restored mode & boost off)");
                    break;

                case NAV_BTN1_LONG: // Long Press Top / Tap Settings: Open Settings Menu directly
                    if (telemetry.screen == SCREEN_RACE_TIMER) {
                        telemetry.race_state = 0;
                        telemetry.ride_mode = telemetry.saved_ride_mode;
                        ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
                        telemetry.boost_active = false;
                        ModeManager::instance().setBoostActive(false);
                        telemetry.race_popup_open = false;
                        telemetry.race_dyno_open = false;
                        Buttons.setRaceState(0);
                        Buttons.setRacePopupOpen(false);
                        Buttons.setRaceDynoOpen(false);
                    }
                    telemetry.screen = SCREEN_SETTINGS_MENU;
                    Serial.println("[NAV] Opened Settings Menu");
                    break;

                case NAV_BTN2_LONG: // Long Press Bottom / Trip Reset: Reset Trip Odometer & Statistics
                    telemetry.trip_km = 0.0f;
                    telemetry.stats.reset();
                    Battery.resetTrip();
                    Vesc.resetTripOdometer();
                    Serial.println("[NAV] Reset Trip Odometer and Statistics to 0.0");
                    break;

                case NAV_BOOST_TOGGLE:
                    if (!telemetry.tuning_mode) {
                        Serial.println("[NAV] Boost blocked: Tuning mode is OFF (Police Mode)");
                        break;
                    }
                    telemetry.boost_active = !telemetry.boost_active;
                    if (telemetry.boost_active) {
                        telemetry.boost_start_ms = now;
                        telemetry.boost_end_ms = now + 10000;
                    }
                    ModeManager::instance().setBoostActive(telemetry.boost_active);
                    if (telemetry.throttle_pct <= 0.01f && telemetry.speed_kmh <= 0.5f) {
                        telemetry.speed_kmh = 0.0f;
                        telemetry.power_watts = 0.0f;
                        telemetry.current_amps = 0.0f;
                        telemetry.phase_amps = 0.0f;
                    }
                    Serial.printf("[NAV] Toggled Hyper Boost: %s (Battery Limit: %.1f A)\n",
                                  telemetry.boost_active ? "ACTIVE (70A)" : "OFF (60A)",
                                  ModeManager::instance().getActiveLimits().max_battery_amps);
                    break;

                case NAV_TUNING_ENABLE:
                    telemetry.tuning_mode = true;
                    Buttons.setTuningMode(true);
                    ModeManager::instance().setTuningMode(true);
                    telemetry.mode_badge_until = now + 2500;
                    Serial.println("[NAV] Tuning Mode ACTIVATED via 4s hold on speed number!");
                    break;

                case NAV_TUNING_DISABLE:
                    telemetry.tuning_mode = false;
                    Buttons.setTuningMode(false);
                    ModeManager::instance().setTuningMode(false);
                    telemetry.boost_active = false;
                    ModeManager::instance().setBoostActive(false);
                    telemetry.mode_badge_until = now + 2500;
                    if (telemetry.screen == SCREEN_RACE_TIMER) {
                        telemetry.screen = SCREEN_RIDE_DASH;
                        telemetry.race_state = 0;
                        telemetry.ride_mode = telemetry.saved_ride_mode;
                        ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
                        telemetry.race_popup_open = false;
                        telemetry.race_dyno_open = false;
                        Buttons.setRaceState(0);
                        Buttons.setRacePopupOpen(false);
                        Buttons.setRaceDynoOpen(false);
                        Renderer.invalidateCache();
                        Renderer.markScreenDirty();
                    }
                    Serial.println("[NAV] Tuning Mode DEACTIVATED via single tap on speed number!");
                    break;

                case NAV_MODE_CYCLE:
                    telemetry.ride_mode = (RideMode)(((int)telemetry.ride_mode + 1) % MODE_COUNT);
                    ModeManager::instance().setRideMode(telemetry.ride_mode);
                    telemetry.mode_badge_until = now + 2500;
                    if (telemetry.throttle_pct <= 0.01f && telemetry.speed_kmh <= 0.5f) {
                        telemetry.speed_kmh = 0.0f;
                        telemetry.power_watts = 0.0f;
                        telemetry.current_amps = 0.0f;
                        telemetry.phase_amps = 0.0f;
                    }
                    Serial.printf("[NAV] Cycled Ride Mode: %d (%s, Tuning: %s)\n",
                                  (int)telemetry.ride_mode,
                                  ModeManager::instance().getActiveModeName(),
                                  telemetry.tuning_mode ? "ON" : "OFF");
                    break;

                case NAV_HAZARD_TOGGLE:
                    telemetry.hazard_active = !telemetry.hazard_active;
                    Serial.printf("[NAV] Toggled Hazard Flashers: %s\n", telemetry.hazard_active ? "ACTIVE" : "OFF");
                    break;

                case NAV_LIGHT_TOGGLE:
                    telemetry.lights_enabled = !telemetry.lights_enabled;
#if defined(PIN_HEADLIGHT_MOSFET) && (PIN_HEADLIGHT_MOSFET >= 0)
                    digitalWrite(PIN_HEADLIGHT_MOSFET, telemetry.lights_enabled ? HIGH : LOW);
#endif
                    Serial.printf("[NAV] Toggled Light Switch (Hauptschalter): %s (GPIO %d: %s)\n",
                                  telemetry.lights_enabled ? "ON" : "OFF",
                                  PIN_HEADLIGHT_MOSFET,
                                  telemetry.lights_enabled ? "HIGH" : "LOW");
                    break;

                case NAV_BOTH_PRESSED: // Fast Toggle Active Battery Profile
                    {
                        uint8_t next_p = (Settings.get().active_battery_profile == 0) ? 1 : 0;
                        Settings.get().active_battery_profile = next_p;
                        Battery.setActiveProfile(next_p);
                        Settings.save();
                        Serial.printf("[NAV] Fast Toggled Battery Profile: %d (%s)\n",
                                      next_p, Battery.getProfile(next_p).name);
                    }
                    break;

                default:
                    break;
            }
        }
    }

    // Boost timer expiration check (10s max duration on ride dash)
    if (telemetry.screen != SCREEN_RACE_TIMER && telemetry.boost_active && (uint32_t)(now - telemetry.boost_start_ms) >= 10000) {
        telemetry.boost_active = false;
        ModeManager::instance().setBoostActive(false);
        Serial.println("[TIMER] Hyper Boost timed out (10s limit reached, restored 60A battery limit)");
    }

    // Record waveform curves every 1000ms
    static uint32_t last_wave_sample_ms = 0;
    if (now - last_wave_sample_ms >= 1000) {
        last_wave_sample_ms = now;
        for (int i = 0; i < WAVE_HISTORY_LEN - 1; i++) {
            telemetry.speed_history[i] = telemetry.speed_history[i + 1];
            telemetry.power_history[i] = telemetry.power_history[i + 1];
        }
        telemetry.speed_history[WAVE_HISTORY_LEN - 1] = telemetry.speed_kmh;
        telemetry.power_history[WAVE_HISTORY_LEN - 1] = telemetry.power_watts;
    }

    // 2. Update Telemetry or Forward Bridge Traffic
    if (Vesc.isBridgeActive()) {
        Vesc.updateBridge();
    } else {
        Vesc.update(telemetry);
    }

    // 2b. Update Live GPS+BDS Satellite Tracker & Ground Speed Priority
    GpsTracker::instance().update(telemetry);

    // 2b2. Update Proactive Traction Control (TCS) & Virtual TPMS Engine
    static uint32_t s_last_tcs_update_ms = millis();
    uint32_t tcs_now = millis();
    float tcs_dt = (tcs_now - s_last_tcs_update_ms) / 1000.0f;
    if (tcs_dt < 0.001f) tcs_dt = 0.001f;
    if (tcs_dt > 0.1f) tcs_dt = 0.1f;
    s_last_tcs_update_ms = tcs_now;

    TcsTpmsManager::instance().update(telemetry, tcs_dt);

    static float s_last_applied_tcs_scale = 1.0f;
    if (fabsf(telemetry.tcs_throttle_scale - s_last_applied_tcs_scale) > 0.04f ||
        (telemetry.tcs_active != (s_last_applied_tcs_scale < 0.98f))) {
        s_last_applied_tcs_scale = telemetry.tcs_throttle_scale;
        float base_phase = ModeManager::instance().getActiveLimits().max_phase_amps;
        float scaled_phase = base_phase * telemetry.tcs_throttle_scale;
        Vesc.sendTcsPhaseLimit(scaled_phase);
    }

    // 2c. Update Headlight MOSFET hardware state if changed
    static bool s_last_hw_light = false;
    if (telemetry.lights_enabled != s_last_hw_light) {
        s_last_hw_light = telemetry.lights_enabled;
#if defined(PIN_HEADLIGHT_MOSFET) && (PIN_HEADLIGHT_MOSFET >= 0)
        digitalWrite(PIN_HEADLIGHT_MOSFET, s_last_hw_light ? HIGH : LOW);
#endif
    }

    // 2d. Speed fusion stats sync: if active ground speed exceeds recorded max, update record
    if (telemetry.speed_kmh > telemetry.stats.max_speed_kmh) {
        telemetry.stats.max_speed_kmh = telemetry.speed_kmh;
    }

    telemetry.uptime_sec = (now - boot_ms) / 1000;
    telemetry.dynamic_wh_km = Battery.getDynamicWhKm();
    telemetry.est_range_km = Battery.getEstRangeKm();
    telemetry.stats.dynamic_wh_km = telemetry.dynamic_wh_km;
    if (telemetry.voltage > 10.0f) {
        if (telemetry.stats.min_voltage <= 0.0f || telemetry.voltage < telemetry.stats.min_voltage) {
            telemetry.stats.min_voltage = telemetry.voltage;
        }
        if (telemetry.voltage > telemetry.stats.max_voltage) {
            telemetry.stats.max_voltage = telemetry.voltage;
        }
    }
    uint8_t current_fault = (uint8_t)FaultManager::instance().getActiveFault();
    if (current_fault != telemetry.fault_code) {
        telemetry.fault_code = current_fault;
        if (current_fault != 0) {
            telemetry.fault_start_ms = now;
        } else {
            telemetry.fault_start_ms = 0;
        }
        Renderer.markScreenDirty();
        Renderer.invalidateCache();
    }

    // 2d. FOC Audio Turbine Sound Engine (Option 3: Fixed Whoosh Chord, Dynamic Throttle + Speed Intensity)
    static uint32_t s_last_turbine_update_ms = 0;
    static float s_last_turbine_volts = 0.0f;
    static bool s_turbine_was_active = false;

    telemetry.color_theme = Settings.get().color_theme;
    bool turbine_enabled = Settings.get().turbine_sound_enabled;
    // Active if vehicle is unlocked, no alarm, and either moving or throttle applied
    bool is_driving = (!telemetry.is_locked && !telemetry.theft_alarm_active && 
                       (telemetry.speed_kmh >= 0.8f || telemetry.throttle_pct >= 3.0f));

    if (turbine_enabled && is_driving) {
        telemetry.turbine_sound_active = true;

        // Frequencies: Constant dual-tone turbine chord (eliminates pitch gliding / "singing"!)
        // Channel 0: 480 Hz (jet core turbine rumble)
        // Channel 1: 1120 Hz (air rush / turbine blade hiss)
        const float t_freq_base = 480.0f;
        const float t_freq_rush = 1120.0f;

        // Volume / Intensity: Option 3 - Dynamic modulation via Throttle + Speed
        // Gasgriff (throttle) gives instant punch; Speed adds sustained rush!
        float thr_norm = constrain(telemetry.throttle_pct / 100.0f, 0.0f, 1.0f);
        float spd_norm = constrain(telemetry.speed_kmh / 65.0f, 0.0f, 1.0f);

        // Voltage scaling:
        // Idle roll / low speed: ~1.2V subtle whoosh
        // Moderate cruise: ~2.8V - 3.8V
        // Full throttle acceleration: up to 6.2V powerful jet whoosh
        float t_volts = 1.2f + (thr_norm * 2.8f) + (spd_norm * 2.2f);
        if (t_volts > 6.2f) t_volts = 6.2f;

        telemetry.turbine_freq = t_freq_base;
        telemetry.turbine_volts = t_volts;

        // Throttled update to VESC (every 80ms or when voltage changes noticeably)
        // Does NOT alter motor limits, torque, duty, throttle or braking!
        if (now - s_last_turbine_update_ms >= 80 && 
            fabsf(t_volts - s_last_turbine_volts) >= 0.15f) {
            s_last_turbine_update_ms = now;
            s_last_turbine_volts = t_volts;
            s_turbine_was_active = true;
            Vesc.playFocTone(0, t_freq_base, t_volts);
            Vesc.playFocTone(1, t_freq_rush, t_volts * 0.70f);
        }
    } else {
        telemetry.turbine_sound_active = false;
        telemetry.turbine_freq = 0.0f;
        telemetry.turbine_volts = 0.0f;
        if (s_turbine_was_active) {
            s_turbine_was_active = false;
            s_last_turbine_volts = 0.0f;
            Vesc.sendFocPlayStop();
        }
    }

    // 3. Render at steady 30 FPS rate on ST7262 16-bit parallel RGB panel (throttled to 5 FPS in bridge mode)
    uint32_t render_interval = Vesc.isBridgeActive() ? 200 : 33;
    if (now - last_render_ms >= render_interval) {
        last_render_ms = now;
        Renderer.render(telemetry);
        if (Vesc.isBridgeActive()) {
            Vesc.updateBridge();
        }
    }

    // 4. Debug print over USB Serial every 2 seconds (only when bridge is inactive)
    if (!Vesc.isBridgeActive() && (now - last_log_ms >= 2000)) {
        last_log_ms = now;
        Serial.printf("[DASH] Screen: %d | Lock: %d | Spd: %.1f km/h | Duty: %.1f%% | Bat: %.1fV (%.0f%%) | Pwr: %.0fW | Cur: %.1fA | Est: %.1fkm\n",
                      (int)telemetry.screen, (int)telemetry.is_locked, telemetry.speed_kmh, telemetry.duty_cycle_pct, telemetry.voltage,
                      telemetry.battery_pct, telemetry.power_watts, telemetry.current_amps, telemetry.est_range_km);
    }

#if !defined(NATIVE_SIMULATOR)
    // Yield CPU Core 1 to FreeRTOS background tasks (TWAI driver, WiFi/BLE stack, FreeRTOS IDLE)
    // This reduces chip temperature, saves power, and prevents PSRAM bus contention with the ST7262 RGB DMA channel!
    if (millis() - last_render_ms < 5) {
        delay(2);
    } else {
        yield();
    }
#endif
}
