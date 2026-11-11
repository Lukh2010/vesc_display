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
    if (Settings.get().pin_lock_enabled) {
        ModeManager::instance().setLocked(true);
        Serial.println("[SECURITY] Boot PIN-Lockscreen ACTIVE. VESC locked at 0A.");
    } else {
        ModeManager::instance().setLocked(false);
    }
    Serial.println("[OK] Mode Manager initialized: Stealth limits dispatched to VESC on boot.");

    // 9b. Initialize VESC Fault Blackbox Manager
    FaultManager::instance().begin();
    Serial.printf("[OK] Fault Manager initialized (%d logged faults).\n", FaultManager::instance().getFaultCount());

    Serial.println("[OK] Setup complete. Entering main render loop.");
}

void loop() {
    static uint32_t last_render_ms = 0;
    static uint32_t last_log_ms = 0;
    static uint32_t boot_ms = millis();
    uint32_t now = millis();

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
        }
        Renderer.markScreenDirty();
    }
    if (telemetry.ride_mode != ModeManager::instance().getRideMode()) {
        telemetry.ride_mode = ModeManager::instance().getRideMode();
        telemetry.mode_badge_until = now + 2500;
        Renderer.markScreenDirty();
    }

    // Auto-unlock if PIN lock was disabled in Settings
    if (!Settings.get().pin_lock_enabled && telemetry.is_locked) {
        telemetry.is_locked = false;
        ModeManager::instance().setLocked(false);
        Renderer.markScreenDirty();
        Renderer.invalidateCache();
        Renderer.triggerNeedleSweep();
    }

    // 1. Process Touch & Button Navigation Inputs
    Buttons.setActiveScreen(telemetry.screen);
    Buttons.setRacePopupOpen(telemetry.race_popup_open);
    Buttons.setRaceDynoOpen(telemetry.race_dyno_open);
    Buttons.setRaceState(telemetry.race_state);
    Buttons.update();
    Renderer.updateRaceTimer(telemetry, now);

    // PIN Lockscreen Input Processing (when locked)
    if (telemetry.is_locked) {
        if (telemetry.pin_error) {
            if (millis() >= telemetry.pin_error_end_ms) {
                telemetry.pin_error = false;
            } else {
                // Debounce / spam immunity: Discard any taps while error indicator is showing
                int16_t dummy_x, dummy_y;
                Buttons.getLastTap(&dummy_x, &dummy_y);
            }
        }

        int16_t tx = -1, ty = -1;
        if (!telemetry.pin_error && Buttons.getLastTap(&tx, &ty)) {
            // Continuous column mapping without dead zones between buttons
            int c = -1;
            if (tx >= 150 && tx < 330) c = 0;
            else if (tx >= 330 && tx < 470) c = 1;
            else if (tx >= 470 && tx <= 650) c = 2;

            // Continuous row mapping without dead zones between rows
            int r = -1;
            if (ty >= 140 && ty < 227) r = 0;
            else if (ty >= 227 && ty < 297) r = 1;
            else if (ty >= 297 && ty < 367) r = 2;
            else if (ty >= 367 && ty <= 445) r = 3;

            if (r >= 0 && c >= 0) {
                static const char KEY_CHARS[4][3] = {
                    { '1', '2', '3' },
                    { '4', '5', '6' },
                    { '7', '8', '9' },
                    { '<', '0', '#' }
                };
                char key = KEY_CHARS[r][c];
                Serial.printf("[TOUCH PIN] tx=%d, ty=%d -> r=%d, c=%d, key='%c'\n", tx, ty, r, c, key);
                int len = strlen(telemetry.pin_entry);

                if (key == '<') { // DEL
                    if (len > 0) {
                        telemetry.pin_entry[len - 1] = '\0';
                        telemetry.pin_error = false;
                    }
                } else if (key == '#') { // OK / enter
                    if (strcmp(telemetry.pin_entry, Settings.get().pin_code) == 0) {
                        telemetry.is_locked = false;
                        telemetry.pin_error = false;
                        telemetry.pin_entry[0] = '\0';
                        ModeManager::instance().setLocked(false);
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        Renderer.triggerNeedleSweep();
                        Serial.println("[PIN] System UNLOCKED with valid PIN code!");
                    } else {
                        telemetry.pin_error = true;
                        telemetry.pin_error_end_ms = millis() + 800;
                        telemetry.pin_entry[0] = '\0';
                        Serial.println("[PIN] Invalid PIN code entered!");
                    }
                } else if (key >= '0' && key <= '9') {
                    if (len < 4) {
                        telemetry.pin_entry[len] = key;
                        telemetry.pin_entry[len + 1] = '\0';
                        telemetry.pin_error = false;
                        len++;

                        if (len == 4) {
                            if (strcmp(telemetry.pin_entry, Settings.get().pin_code) == 0) {
                                telemetry.is_locked = false;
                                telemetry.pin_error = false;
                                telemetry.pin_entry[0] = '\0';
                                ModeManager::instance().setLocked(false);
                                Renderer.markScreenDirty();
                                Renderer.invalidateCache();
                                Renderer.triggerNeedleSweep();
                                Serial.println("[PIN] System UNLOCKED automatically upon 4 digits!");
                            } else {
                                telemetry.pin_error = true;
                                telemetry.pin_error_end_ms = millis() + 800;
                                telemetry.pin_entry[0] = '\0';
                                Serial.println("[PIN] Invalid PIN code entered!");
                            }
                        }
                    }
                }
            }
        }
    }

    // SCREEN_SETTINGS_MENU: Interactive Touch & Navigation Handling
    if (!telemetry.is_locked && telemetry.screen == SCREEN_SETTINGS_MENU) {
        int16_t tx = -1, ty = -1;
        if (Buttons.getLastTap(&tx, &ty)) {
            Renderer.handleSettingsTouch(tx, ty, telemetry);
            Buttons.clearNavAction(); // Clear any pending action to prevent double-invocation
        } else if (Buttons.isTouching()) {
            int16_t cx = Buttons.getTouchX();
            int16_t cy = Buttons.getTouchY();
            // Real-time slider drag: speed track y: 135..285, watt track y: 295..445, x: 120..680
            if (cx >= 120 && cx <= 680 && ((cy >= 135 && cy <= 285) || (cy >= 295 && cy <= 445))) {
                Renderer.handleSettingsTouch(cx, cy, telemetry);
            }
        }
    }

    NavAction nav = Buttons.getNavAction();
    if (telemetry.is_locked && nav != NAV_PIN_EMERGENCY_RESET) {
        nav = NAV_NONE;
    }

    if (nav != NAV_NONE) {
        if (nav == NAV_PIN_EMERGENCY_RESET) {
            strncpy(Settings.get().pin_code, "1234", sizeof(Settings.get().pin_code));
            Settings.save();
            telemetry.is_locked = false;
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
                                Buttons.setRaceState(0);
                                Buttons.setRacePopupOpen(false);
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
                        Renderer.markScreenDirty();
                        break;
                    }
                    telemetry.ride_mode = MODE_RACE;
                    ModeManager::instance().setRideMode(MODE_RACE);
                    telemetry.boost_active = true;
                    ModeManager::instance().setBoostActive(true);
                    telemetry.boost_end_ms = now + 60000;
                    telemetry.race_state = 1; // COUNTDOWN (5s)
                    telemetry.race_countdown_start_ms = now;
                    telemetry.race_elapsed_sec = 0.0f;
                    telemetry.race_split_0_22 = 0.0f;
                    telemetry.race_split_0_50 = 0.0f;
                    telemetry.race_split_0_70 = 0.0f;
                    Buttons.setRaceState(1);
                    Renderer.markScreenDirty();
                    Serial.println("[RACE] 5-second launch countdown started!");
                    break;

                case NAV_RACE_ABORT:
                    telemetry.race_state = 0; // IDLE
                    telemetry.ride_mode = telemetry.saved_ride_mode;
                    ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
                    telemetry.boost_active = false;
                    ModeManager::instance().setBoostActive(false);
                    Buttons.setRaceState(0);
                    Renderer.markScreenDirty();
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
                    Renderer.markScreenDirty();
                    Serial.println("[NAV] Reset Trip Odometer and Statistics to 0.0");
                    break;

                case NAV_BOOST_TOGGLE:
                    telemetry.boost_active = !telemetry.boost_active;
                    if (telemetry.boost_active) {
                        telemetry.boost_end_ms = now + 10000;
                    }
                    ModeManager::instance().setBoostActive(telemetry.boost_active);
                    if (telemetry.throttle_pct <= 0.01f && telemetry.speed_kmh <= 0.5f) {
                        telemetry.speed_kmh = 0.0f;
                        telemetry.power_watts = 0.0f;
                        telemetry.current_amps = 0.0f;
                        telemetry.phase_amps = 0.0f;
                    }
                    Renderer.markScreenDirty();
                    Serial.printf("[NAV] Toggled Hyper Boost: %s (Battery Limit: %.1f A)\n",
                                  telemetry.boost_active ? "ACTIVE (70A)" : "OFF (60A)",
                                  ModeManager::instance().getActiveLimits().max_battery_amps);
                    break;

                case NAV_TUNING_ENABLE:
                    telemetry.tuning_mode = true;
                    Buttons.setTuningMode(true);
                    ModeManager::instance().setTuningMode(true);
                    telemetry.mode_badge_until = now + 2500;
                    Renderer.markScreenDirty();
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
                    }
                    Renderer.markScreenDirty();
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
                    Renderer.markScreenDirty();
                    Serial.printf("[NAV] Cycled Ride Mode: %d (%s, Tuning: %s)\n",
                                  (int)telemetry.ride_mode,
                                  ModeManager::instance().getActiveModeName(),
                                  telemetry.tuning_mode ? "ON" : "OFF");
                    break;

                case NAV_HAZARD_TOGGLE:
                    telemetry.hazard_active = !telemetry.hazard_active;
                    Renderer.markScreenDirty();
                    Serial.printf("[NAV] Toggled Hazard Flashers: %s\n", telemetry.hazard_active ? "ACTIVE" : "OFF");
                    break;

                case NAV_LIGHT_TOGGLE:
                    telemetry.lights_enabled = !telemetry.lights_enabled;
                    Renderer.markScreenDirty();
                    Serial.printf("[NAV] Toggled Light Switch (Hauptschalter): %s\n", telemetry.lights_enabled ? "ON" : "OFF");
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
    if (telemetry.screen != SCREEN_RACE_TIMER && telemetry.boost_active && now > telemetry.boost_end_ms) {
        telemetry.boost_active = false;
        ModeManager::instance().setBoostActive(false);
        Renderer.markScreenDirty();
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
        Serial.printf("[DASH] Screen: %d | Spd: %.1f km/h | Duty: %.1f%% | Bat: %.1fV (%.0f%%) | Cur: %.1fA | Pha: %.1fA | Est: %.1fkm\n",
                      (int)telemetry.screen, telemetry.speed_kmh, telemetry.duty_cycle_pct, telemetry.voltage,
                      telemetry.battery_pct, telemetry.current_amps, telemetry.phase_amps, telemetry.est_range_km);
    }
}
