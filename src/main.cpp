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
#include "dash_log.h"
#include "serial_cli.h"
#if !defined(NATIVE_SIMULATOR)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

// Active Telemetry State (Core 1 UI / loop() working snapshot)
static DashTelemetry telemetry;

// Double-buffered Core 0 <-> Core 1 synchronization mailbox
static DashTelemetry s_shared_telem;
static volatile uint32_t s_trip_reset_req_gen = 0;
static volatile int8_t   s_pending_bat_profile = -1;
#if !defined(NATIVE_SIMULATOR)
static SemaphoreHandle_t s_telem_mutex = nullptr;
static TaskHandle_t      s_rt_task_handle = nullptr;
#endif

static void copyLiveTelemetry(DashTelemetry &dst, const DashTelemetry &src) {
    dst.speed_kmh                  = src.speed_kmh;
    dst.vesc_motor_speed           = src.vesc_motor_speed;
    dst.use_gps_speed              = src.use_gps_speed;
    memcpy(dst.clock_str, src.clock_str, sizeof(dst.clock_str));
    dst.gps                        = src.gps;
    dst.g_force                    = src.g_force;
    dst.incline_deg                = src.incline_deg;
    dst.duty_cycle_pct             = src.duty_cycle_pct;
    dst.rpm                        = src.rpm;
    dst.throttle_pct               = src.throttle_pct;

    dst.voltage                    = src.voltage;
    dst.current_amps               = src.current_amps;
    dst.phase_amps                 = src.phase_amps;
    dst.power_watts                = src.power_watts;
    dst.amphours_used              = src.amphours_used;

    dst.temp_fet_rear              = src.temp_fet_rear;
    dst.temp_mot_rear              = src.temp_mot_rear;
    dst.temp_fet_front             = src.temp_fet_front;
    dst.temp_mot_front             = src.temp_mot_front;
    dst.temp_motor                 = src.temp_motor;
    dst.temp_esc                   = src.temp_esc;

    dst.battery_profile_id         = src.battery_profile_id;
    dst.battery_pct                = src.battery_pct;
    dst.remaining_wh               = src.remaining_wh;
    dst.battery_health_soh         = src.battery_health_soh;
    dst.est_range_km               = src.est_range_km;
    dst.dynamic_wh_km              = src.dynamic_wh_km;

    dst.trip_km                    = src.trip_km;
    dst.odo_km                     = src.odo_km;
    dst.stats                      = src.stats;

    dst.tcs_active                 = src.tcs_active;
    dst.tcs_slip_kmh               = src.tcs_slip_kmh;
    dst.tcs_throttle_scale         = src.tcs_throttle_scale;
    dst.tpms_warning               = src.tpms_warning;
    dst.tpms_drift_pct             = src.tpms_drift_pct;
    dst.calibrated_wheel_diam_inch = src.calibrated_wheel_diam_inch;

    dst.vesc1_max_power            = src.vesc1_max_power;
    dst.vesc1_max_phase            = src.vesc1_max_phase;
    dst.vesc1_max_speed            = src.vesc1_max_speed;
    dst.vesc2_max_power            = src.vesc2_max_power;
    dst.vesc2_max_phase            = src.vesc2_max_phase;
    dst.vesc2_max_speed            = src.vesc2_max_speed;

    dst.vesc_connected             = src.vesc_connected;
#if SIMULATION_MODE
    dst.fault_code                 = src.fault_code;
#endif
}

static void copyUiControlState(DashTelemetry &dst, const DashTelemetry &src) {
    dst.is_locked              = src.is_locked;
    dst.ride_mode              = src.ride_mode;
    dst.saved_ride_mode        = src.saved_ride_mode;
    dst.tuning_mode            = src.tuning_mode;
    dst.boost_active           = src.boost_active;
    dst.gps_enabled            = src.gps_enabled;
    dst.incline_gforce_enabled = src.incline_gforce_enabled;
    dst.wifi_bridge_enabled    = src.wifi_bridge_enabled;
    dst.lights_enabled         = src.lights_enabled;
    dst.screen                 = src.screen;
    dst.uptime_sec             = src.uptime_sec;
}

// Runs the ~200 Hz real-time VESC/GPS/TCS/Limit control step (on Core 0 under FreeRTOS)
static void stepRealtimeSubsystem(DashTelemetry &rt_tel, uint32_t now) {
    if (Vesc.isBridgeActive()) {
        Vesc.updateBridge();
    } else {
        Vesc.update(rt_tel);
    }

    GpsTracker::instance().update(rt_tel);

    static uint32_t s_last_tcs_update_ms = millis();
    float tcs_dt = (now - s_last_tcs_update_ms) / 1000.0f;
    if (tcs_dt < 0.001f) tcs_dt = 0.001f;
    if (tcs_dt > 0.1f) tcs_dt = 0.1f;
    s_last_tcs_update_ms = now;

    TcsTpmsManager::instance().update(rt_tel, tcs_dt);

    // Push pending mode/lock/tuning limits BEFORE TCS phase scaling so TCS is never overwritten
    if (ModeManager::instance().checkAndClearLimitsDirty()) {
        ModeManager::instance().applyLimitsToVesc();
    }

    static float s_last_applied_tcs_scale = 1.0f;
    static RideMode s_tcs_last_mode = MODE_ECO;
    static bool s_tcs_last_boost = false;
    static bool s_tcs_last_tuning = false;
    if (rt_tel.ride_mode != s_tcs_last_mode || rt_tel.boost_active != s_tcs_last_boost || rt_tel.tuning_mode != s_tcs_last_tuning) {
        s_tcs_last_mode = rt_tel.ride_mode;
        s_tcs_last_boost = rt_tel.boost_active;
        s_tcs_last_tuning = rt_tel.tuning_mode;
        s_last_applied_tcs_scale = 1.0f;
    }
    if (!rt_tel.is_locked && !ModeManager::instance().isLocked() && TcsTpmsManager::instance().isTcsEnabled()) {
        if (fabsf(rt_tel.tcs_throttle_scale - s_last_applied_tcs_scale) > 0.04f ||
            (rt_tel.tcs_active != (s_last_applied_tcs_scale < 0.98f))) {
            s_last_applied_tcs_scale = rt_tel.tcs_throttle_scale;
            float base_phase = ModeManager::instance().getActiveLimits().max_phase_amps;
            float scaled_phase = base_phase * rt_tel.tcs_throttle_scale;
            Vesc.sendTcsPhaseLimit(scaled_phase);
        }
    } else {
        s_last_applied_tcs_scale = 1.0f;
    }

    if (rt_tel.speed_kmh > rt_tel.stats.max_speed_kmh) {
        rt_tel.stats.max_speed_kmh = rt_tel.speed_kmh;
    }
    rt_tel.dynamic_wh_km = Battery.getDynamicWhKm();
    rt_tel.est_range_km = Battery.getEstRangeKm();
    rt_tel.stats.dynamic_wh_km = rt_tel.dynamic_wh_km;
    if (rt_tel.voltage > 10.0f) {
        if (rt_tel.stats.min_voltage <= 0.0f || rt_tel.voltage < rt_tel.stats.min_voltage) {
            rt_tel.stats.min_voltage = rt_tel.voltage;
        }
        if (rt_tel.voltage > rt_tel.stats.max_voltage) {
            rt_tel.stats.max_voltage = rt_tel.voltage;
        }
    }
}

#if !defined(NATIVE_SIMULATOR)
static void realtimeTask(void *pvParameters) {
    (void)pvParameters;
    DashTelemetry rt_tel;
    if (s_telem_mutex && xSemaphoreTake(s_telem_mutex, portMAX_DELAY) == pdTRUE) {
        rt_tel = s_shared_telem;
        xSemaphoreGive(s_telem_mutex);
    } else {
        rt_tel = telemetry;
    }

    uint32_t applied_trip_gen = s_trip_reset_req_gen;

    for (;;) {
        if (OtaManager::instance().isUpdating()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t req_gen = applied_trip_gen;
        int8_t req_bat_profile = -1;
        if (s_telem_mutex && xSemaphoreTake(s_telem_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            copyUiControlState(rt_tel, s_shared_telem);
            req_gen = s_trip_reset_req_gen;
            req_bat_profile = s_pending_bat_profile;
            s_pending_bat_profile = -1;
            xSemaphoreGive(s_telem_mutex);
        }

        if (req_bat_profile >= 0) {
            Battery.setActiveProfile((uint8_t)req_bat_profile);
        }
        if (req_gen != applied_trip_gen) {
            applied_trip_gen = req_gen;
            rt_tel.trip_km = 0.0f;
            rt_tel.stats.reset();
            Battery.resetTrip();
            Vesc.resetTripOdometer();
        }

        stepRealtimeSubsystem(rt_tel, millis());

        if (s_telem_mutex && xSemaphoreTake(s_telem_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (s_trip_reset_req_gen == applied_trip_gen) {
                copyLiveTelemetry(s_shared_telem, rt_tel);
            }
            xSemaphoreGive(s_telem_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
#endif

// Safe cleanup helper whenever leaving SCREEN_RACE_TIMER via any navigation or safety path
static void exitRaceScreenCleanup(DashTelemetry &tel) {
    tel.race_state = 0;
    tel.ride_mode = tel.saved_ride_mode;
    ModeManager::instance().setRideMode(tel.saved_ride_mode);
    tel.boost_active = false;
    ModeManager::instance().setBoostActive(false);
    tel.race_popup_open = false;
    tel.race_dyno_open = false;
    Buttons.setRaceState(0);
    Buttons.setRacePopupOpen(false);
    Buttons.setRaceDynoOpen(false);
}

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
    telemetry.odo_km              = Settings.get().odo_km;
    telemetry.color_theme         = Settings.get().color_theme;
    telemetry.stats.reset();
    telemetry.screen              = SCREEN_RIDE_DASH;
    telemetry.vesc_connected      = false;
    telemetry.uptime_sec          = 0;
    telemetry.ride_mode           = ModeManager::instance().getRideMode();
    telemetry.saved_ride_mode     = telemetry.ride_mode;
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
    TcsTpmsManager::instance().setTcsEnabled(Settings.get().tcs_enabled);

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
    ModeManager::instance().setBatteryCells(Settings.get().battery_cells);
    // Sync telemetry ride_mode after ModeManager::begin() so loop() does not trigger a spurious boot mode banner
    telemetry.ride_mode = ModeManager::instance().getRideMode();
    telemetry.saved_ride_mode = telemetry.ride_mode;
    telemetry.mode_badge_until = 0;
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

#if !defined(NATIVE_SIMULATOR)
    s_shared_telem = telemetry;
    s_telem_mutex = xSemaphoreCreateMutex();
    if (s_telem_mutex != nullptr) {
        ModeManager::instance().setAsyncVescPush(true);
        BaseType_t ok = xTaskCreatePinnedToCore(realtimeTask, "rt_vesc_tcs", 8192, nullptr, 3, &s_rt_task_handle, 0);
        if (ok == pdPASS) {
            Serial.println("[OK] Core 0 Real-Time Control Task started (~200 Hz: VESC, GPS, TCS/TPMS).");
        } else {
            ModeManager::instance().setAsyncVescPush(false);
            Serial.println("[WARN] Failed to create Core 0 task; running telemetry inline on Core 1.");
        }
    }
#endif

    Serial.println("[OK] Setup complete. Entering main render loop.");
}

void loop() {
    static uint32_t last_render_ms = 0;
    static uint32_t last_log_ms = 0;
    static uint32_t boot_ms = millis();
    uint32_t now = millis();

#if !defined(NATIVE_SIMULATOR)
    // Pull latest Core 0 live telemetry snapshot into Core 1 working state
    if (s_rt_task_handle && s_telem_mutex && xSemaphoreTake(s_telem_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        copyLiveTelemetry(telemetry, s_shared_telem);
        xSemaphoreGive(s_telem_mutex);
    }
#endif

    // Interactive Serial Command Console via USB (115200 Baud)
    // Active both in SIMULATION_MODE and on real hardware (when USB bridge is inactive)
    handleSerialCli(telemetry, now);

    // 0. High-Priority Wireless OTA Firmware Update in Progress
    if (OtaManager::instance().isUpdating()) {
        static uint32_t s_last_ota_render_ms = 0;
        OtaManager::instance().update();
        uint32_t ota_now = millis();
        if ((uint32_t)(ota_now - s_last_ota_render_ms) >= 100) { // Max 10 Hz during flash writes
            s_last_ota_render_ms = ota_now;
            Renderer.render(telemetry);
        }
        delay(5);
        return;
    }

    // Sync external tuning mode or ride mode changes from Captive Portal web UI (Core 0 -> Core 1)
    if (telemetry.tuning_mode != ModeManager::instance().isTuningMode()) {
        telemetry.tuning_mode = ModeManager::instance().isTuningMode();
        Buttons.setTuningMode(telemetry.tuning_mode);
        telemetry.mode_badge_until = now + 2500;
        if (!telemetry.tuning_mode && telemetry.screen == SCREEN_RACE_TIMER) {
            telemetry.screen = SCREEN_RIDE_DASH;
            exitRaceScreenCleanup(telemetry);
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

    // Backlight Sleep / Wake Management while Locked (saves power; first wake touch is NOT consumed as PIN input)
    static bool s_backlight_sleeping = false;
    static bool s_wake_ignore_until_release = false;
    static uint32_t s_lock_idle_ms = millis();
    static bool s_lock_idle_was_locked = false;

    if (telemetry.is_locked && !Renderer.isBootAnimActive() && !OtaManager::instance().isUpdating()) {
        if (!s_lock_idle_was_locked) {
            s_lock_idle_was_locked = true;
            s_lock_idle_ms = now;
        }
        bool lock_motion = (fabsf(telemetry.vesc_motor_speed) >= 0.8f) || (fabsf(telemetry.rpm) > 15.0f);
        if (telemetry.theft_alarm_active || lock_motion) {
            s_lock_idle_ms = now;
            if (s_backlight_sleeping) {
                uint8_t br = (uint8_t)map(Settings.get().brightness_pct, 0, 100, 0, 255);
                Display.setBrightness(br);
                s_backlight_sleeping = false;
                Renderer.markScreenDirty();
            }
        } else if (s_backlight_sleeping) {
            int16_t wx = -1, wy = -1;
            bool tapped = Buttons.getLastTap(&wx, &wy);
            if (Buttons.isTouching() || tapped) {
                uint8_t br = (uint8_t)map(Settings.get().brightness_pct, 0, 100, 0, 255);
                Display.setBrightness(br);
                s_backlight_sleeping = false;
                s_lock_idle_ms = now;
                if (Buttons.isTouching()) {
                    s_wake_ignore_until_release = true;
                }
                Buttons.clearNavAction();
                Renderer.markScreenDirty();
            }
        } else {
            if (Buttons.isTouching()) {
                s_lock_idle_ms = now;
            } else if ((uint32_t)(now - s_lock_idle_ms) >= 60000UL) {
                Display.setBrightness(0);
                s_backlight_sleeping = true;
            }
        }

        if (s_wake_ignore_until_release) {
            int16_t dx = -1, dy = -1;
            Buttons.getLastTap(&dx, &dy);
            Buttons.clearNavAction();
            if (!Buttons.isTouching()) {
                s_wake_ignore_until_release = false;
            }
        }
    } else {
        s_lock_idle_was_locked = false;
        s_wake_ignore_until_release = false;
        if (s_backlight_sleeping) {
            uint8_t br = (uint8_t)map(Settings.get().brightness_pct, 0, 100, 0, 255);
            Display.setBrightness(br);
            s_backlight_sleeping = false;
            Renderer.markScreenDirty();
        }
    }

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
                DASH_LOGF("[TOUCH PIN] tx=%d, ty=%d -> r=%d, c=%d, key='%c'\n", tx, ty, r, c, key);

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
            // Measure actual physical vehicle movement (fabsf catches reverse pushing too!):
            // 1. Motor wheel rotation speed (VESC telemetry):
            float raw_v = fabsf(telemetry.vesc_motor_speed);
            float abs_rpm = fabsf(telemetry.rpm);
            if (abs_rpm > 15.0f && raw_v < 0.5f) {
                float wheel_circ_m = (Settings.get().wheel_diameter_in * 0.0254f) * M_PI;
                float gear_ratio = Settings.get().gear_ratio > 0.1f ? Settings.get().gear_ratio : 1.0f;
                raw_v = (abs_rpm / gear_ratio) * wheel_circ_m * 60.0f / 1000.0f;
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
            DASH_LOGF("[SEC_DBG] locked=%d, alarm=%d, moved=%.2fm, rpm=%.1f, v_mot=%.2f, v_gps=%.2f, sats=%d, screen=%d\n",
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
        } else if (Buttons.isTouching() && Renderer.getSettingsTab() <= DashboardRenderer::TAB_SPEED) {
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
                        exitRaceScreenCleanup(telemetry);
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
                                exitRaceScreenCleanup(telemetry);
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
                            exitRaceScreenCleanup(telemetry);
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
                        exitRaceScreenCleanup(telemetry);
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
                    telemetry.race_state = 1; // ARMED (Auto-Rollout on speed >= 1.0 km/h)
                    telemetry.race_countdown_start_ms = now;
                    telemetry.race_launch_start_ms = 0;
                    telemetry.race_elapsed_sec = 0.0f;
                    telemetry.race_split_0_22 = 0.0f;
                    telemetry.race_split_0_50 = 0.0f;
                    telemetry.race_split_0_70 = 0.0f;
                    telemetry.race_dyno.reset();
                    Buttons.setRaceState(1);
                    Serial.println("[RACE] Launch Control ARMED — auto-rollout starts at >= 1.0 km/h!");
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
                        exitRaceScreenCleanup(telemetry);
                    }
                    telemetry.screen = SCREEN_SETTINGS_MENU;
                    Serial.println("[NAV] Opened Settings Menu");
                    break;

                case NAV_BTN2_LONG: // Long Press Bottom / Trip Reset: Reset Trip Odometer & Statistics
                    telemetry.trip_km = 0.0f;
                    telemetry.stats.reset();
#if !defined(NATIVE_SIMULATOR)
                    if (s_rt_task_handle && s_telem_mutex && xSemaphoreTake(s_telem_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                        s_shared_telem.trip_km = 0.0f;
                        s_shared_telem.stats.reset();
                        s_trip_reset_req_gen++;
                        xSemaphoreGive(s_telem_mutex);
                    } else {
                        Battery.resetTrip();
                        Vesc.resetTripOdometer();
                    }
#else
                    Battery.resetTrip();
                    Vesc.resetTripOdometer();
#endif
                    Serial.println("[NAV] Reset Trip Odometer and Statistics to 0.0");
                    break;

                case NAV_BOOST_TOGGLE:
                    if (!telemetry.tuning_mode) {
                        telemetry.boost_active = false;
                        ModeManager::instance().setBoostActive(false);
                        Serial.println("[NAV] Hyper Boost blocked in legal mode (!tuning_mode).");
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
                        exitRaceScreenCleanup(telemetry);
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
#if defined(PIN_HEADLIGHT_MOSFET) && (PIN_HEADLIGHT_MOSFET >= 0)
                    digitalWrite(PIN_HEADLIGHT_MOSFET, telemetry.lights_enabled ? HIGH : LOW);
#endif
                    Renderer.markScreenDirty();
                    Serial.printf("[NAV] Toggled Light Switch (Hauptschalter): %s (GPIO %d: %s)\n",
                                  telemetry.lights_enabled ? "ON" : "OFF",
                                  PIN_HEADLIGHT_MOSFET,
                                  telemetry.lights_enabled ? "HIGH" : "LOW");
                    break;

                case NAV_BOTH_PRESSED: // Fast Toggle Active Battery Profile
                    {
                        uint8_t next_p = (Settings.get().active_battery_profile == 0) ? 1 : 0;
                        Settings.get().active_battery_profile = next_p;
#if !defined(NATIVE_SIMULATOR)
                        if (s_rt_task_handle && s_telem_mutex && xSemaphoreTake(s_telem_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                            s_pending_bat_profile = (int8_t)next_p;
                            xSemaphoreGive(s_telem_mutex);
                        } else {
                            Battery.setActiveProfile(next_p);
                        }
#else
                        Battery.setActiveProfile(next_p);
#endif
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
        Renderer.markScreenDirty();
        Serial.println("[TIMER] Hyper Boost timed out (10s limit reached, restored 60A battery limit)");
    }

    // 2. Real-time VESC / GPS / TCS subsystem (Core 0 task on ESP32; inline fallback on simulator)
    telemetry.uptime_sec = (now - boot_ms) / 1000;
#if !defined(NATIVE_SIMULATOR)
    if (s_rt_task_handle && s_telem_mutex) {
        if (xSemaphoreTake(s_telem_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            copyUiControlState(s_shared_telem, telemetry);
            xSemaphoreGive(s_telem_mutex);
        }
    } else {
        stepRealtimeSubsystem(telemetry, now);
    }
#else
    stepRealtimeSubsystem(telemetry, now);
#endif

    // 2b3. Smart Periodic Odometer Persistence (avoids NVS wear)
    static float s_last_saved_odo_km = -1.0f;
    if (s_last_saved_odo_km < 0.0f) {
        s_last_saved_odo_km = telemetry.odo_km;
    }
    float odo_delta = telemetry.odo_km - s_last_saved_odo_km;
    if (odo_delta >= 0.5f || (telemetry.speed_kmh < 0.5f && odo_delta >= 0.1f)) {
        Settings.saveOdo(telemetry.odo_km);
        s_last_saved_odo_km = telemetry.odo_km;
    }

    // 2c. Update Headlight MOSFET hardware state if changed
    static bool s_last_hw_light = false;
    if (telemetry.lights_enabled != s_last_hw_light) {
        s_last_hw_light = telemetry.lights_enabled;
#if defined(PIN_HEADLIGHT_MOSFET) && (PIN_HEADLIGHT_MOSFET >= 0)
        digitalWrite(PIN_HEADLIGHT_MOSFET, s_last_hw_light ? HIGH : LOW);
#endif
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

    // 2e. 5-Minute Idle Auto-Lock (automatically locks stationary untouched scooter after 5 min)
    static uint32_t s_last_activity_ms = millis();
    static bool s_autolock_was_locked = true;
    if (telemetry.is_locked) {
        s_autolock_was_locked = true;
        s_last_activity_ms = now;
    } else {
        if (s_autolock_was_locked) {
            s_autolock_was_locked = false;
            s_last_activity_ms = now;
        }
        bool is_active = (telemetry.speed_kmh >= 0.8f) ||
                         (telemetry.vesc_motor_speed >= 0.8f) ||
                         (telemetry.throttle_pct >= 2.0f) ||
                         Buttons.isTouching() ||
                         Buttons.isBtn1Down() ||
                         Buttons.isBtn2Down() ||
                         Vesc.isBridgeActive() ||
                         (telemetry.screen == SCREEN_SETUP_WIZARD);
        if (is_active) {
            s_last_activity_ms = now;
        } else if (Settings.get().pin_lock_enabled &&
                   !Renderer.isBootAnimActive() &&
                   !OtaManager::instance().isUpdating() &&
                   (uint32_t)(now - s_last_activity_ms) >= 300000UL) {
            if (telemetry.screen == SCREEN_RACE_TIMER) {
                exitRaceScreenCleanup(telemetry);
            }
            telemetry.is_locked = true;
            telemetry.theft_alarm_active = false;
            telemetry.boost_active = false;
            telemetry.pin_entry[0] = '\0';
            telemetry.pin_error = false;
            ModeManager::instance().setBoostActive(false);
            ModeManager::instance().setLocked(true);
            Buttons.setLocked(true);
            telemetry.screen = SCREEN_RIDE_DASH;
            Renderer.markScreenDirty();
            Renderer.invalidateCache();
            Display.setBrightness(0);
            s_backlight_sleeping = true;
            s_autolock_was_locked = true;
            Serial.println("[SECURITY] Auto-locked after 5 minutes of inactivity (Backlight OFF).");
        }
    }

    // Universal Race-Screen Exit Safety Guard (covers any indirect screen transition)
    static DashboardScreen s_prev_screen_guard = SCREEN_RIDE_DASH;
    if (s_prev_screen_guard == SCREEN_RACE_TIMER && telemetry.screen != SCREEN_RACE_TIMER) {
        exitRaceScreenCleanup(telemetry);
    }
    s_prev_screen_guard = telemetry.screen;

    // 3. Render at steady 30 FPS rate on ST7262 16-bit parallel RGB panel (throttled to 5 FPS in bridge mode)
    uint32_t render_interval = Vesc.isBridgeActive() ? 200 : 33;
    if (now - last_render_ms >= render_interval) {
        last_render_ms = now;
        if (!s_backlight_sleeping) {
            Renderer.render(telemetry);
        }
#if defined(NATIVE_SIMULATOR)
        if (Vesc.isBridgeActive()) {
            Vesc.updateBridge();
        }
#endif
    }

    // 4. Debug print over USB Serial every 2 seconds (only when bridge is inactive)
    if (!Vesc.isBridgeActive() && (now - last_log_ms >= 2000)) {
        last_log_ms = now;
        DASH_LOGF("[DASH] Screen: %d | Lock: %d | Spd: %.1f km/h | Duty: %.1f%% | Bat: %.1fV (%.0f%%) | Pwr: %.0fW | Cur: %.1fA | Est: %.1fkm\n",
                  (int)telemetry.screen, (int)telemetry.is_locked, telemetry.speed_kmh, telemetry.duty_cycle_pct, telemetry.voltage,
                  telemetry.battery_pct, telemetry.power_watts, telemetry.current_amps, telemetry.est_range_km);
    }

#if !defined(NATIVE_SIMULATOR)
    // Yield CPU Core 1 cleanly via FreeRTOS vTaskDelay (avoids 100% spin from bare yield())
    delay(Vesc.isBridgeActive() ? 1 : 4);
#endif
}
