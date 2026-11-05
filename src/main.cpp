#include <Arduino.h>
#include "board_config.h"
#include "dash_types.h"
#include "display_driver.h"
#include "dash_renderer.h"
#include "settings_manager.h"
#include "battery_manager.h"
#include "buttons.h"
#include "vesc_uart.h"

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
    telemetry.duty_cycle_pct      = 0.0f;
    telemetry.rpm                 = 0.0f;
    telemetry.throttle_pct        = 0.0f;
    telemetry.voltage             = 0.0f;
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

    // 5. Initialize Touch and Input Handling (GT911 I2C SDA=8, SCL=9)
    Buttons.begin();
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
    if (Settings.get().dash_style == STYLE_ANALOG_DIAL) {
        Renderer.triggerNeedleSweep();
    }

    // 8. Initialize VESC UART or Simulation Engine
    Vesc.begin();
#if SIMULATION_MODE
    Serial.println("[MODE] Self-Test Simulation Mode Active.");
    Serial.println("       Set SIMULATION_MODE in include/board_config.h to 0 for live Flipsky / VESC UART.");
#else
    Serial.println("[MODE] Live VESC UART Mode Active (TX: GPIO 43, RX: GPIO 44).");
#endif

    Serial.println("[OK] Setup complete. Entering main render loop.");
}

void loop() {
    static uint32_t last_render_ms = 0;
    static uint32_t last_log_ms = 0;
    static uint32_t boot_ms = millis();
    uint32_t now = millis();

    // 1. Process Touch & Button Navigation Inputs
    Buttons.update();
    NavAction nav = Buttons.getNavAction();

    if (nav != NAV_NONE) {
        if (Vesc.isBridgeActive()) {
            if (nav == NAV_BTN2_LONG || nav == NAV_BOTH_PRESSED || nav == NAV_BTN1_LONG || nav == NAV_BTN1_SHORT || nav == NAV_BTN2_SHORT) {
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
                case NAV_BTN1_SHORT: // Previous Screen (Swipe Right / Left Tap): Ride Dash <-> Perf Stats <-> Energy Stats
                    {
                        int cur = (int)telemetry.screen;
                        telemetry.screen = (DashboardScreen)((cur - 1 + 3) % 3);
                        if (telemetry.screen == SCREEN_RIDE_DASH && Settings.get().dash_style == STYLE_ANALOG_DIAL) {
                            Renderer.triggerNeedleSweep();
                        }
                        Serial.printf("[NAV] Switched to Screen ID %d (Prev)\n", (int)telemetry.screen);
                    }
                    break;

                case NAV_BTN2_SHORT: // Next Screen (Swipe Left / Right Tap): Ride Dash -> Energy Stats -> Perf Stats -> Ride Dash
                    {
                        int cur = (int)telemetry.screen;
                        telemetry.screen = (DashboardScreen)((cur + 1) % 3);
                        if (telemetry.screen == SCREEN_RIDE_DASH && Settings.get().dash_style == STYLE_ANALOG_DIAL) {
                            Renderer.triggerNeedleSweep();
                        }
                        Serial.printf("[NAV] Switched to Screen ID %d (Next)\n", (int)telemetry.screen);
                    }
                    break;

                case NAV_BTN1_LONG: // Long Press Top / Tap Settings: Open Settings Menu directly
                    telemetry.screen = SCREEN_SETTINGS_MENU;
                    Serial.println("[NAV] Opened Settings Menu");
                    break;

                case NAV_BTN2_LONG: // Long Press Bottom / Trip Reset: Reset Trip Odometer & Statistics
                    telemetry.trip_km = 0.0f;
                    telemetry.stats.reset();
                    Serial.println("[NAV] Reset Trip Odometer and Statistics to 0.0");
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

    // 2. Update Telemetry or Forward Bridge Traffic
    if (Vesc.isBridgeActive()) {
        Vesc.updateBridge();
    } else {
        Vesc.update(telemetry);
    }
    telemetry.uptime_sec = (now - boot_ms) / 1000;

    // 3. Render at smooth ~30 FPS rate (throttled to 5 FPS in bridge mode to maximize USB/UART throughput)
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
