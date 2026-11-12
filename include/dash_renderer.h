#pragma once

#include <Arduino.h>
#include "display_driver.h"
#include "dash_types.h"
#include "settings_manager.h"
#include "battery_manager.h"

class DashboardRenderer {
public:
    DashboardRenderer();
    void begin(DisplayDriver *display);
    void render(const DashTelemetry &telemetry);
    void triggerNeedleSweep();
    bool isSweeping() const { return _is_sweeping; }
    void markScreenDirty() { _screen_dirty = true; }
    void invalidateCache() { _cache.invalidate(); _stats_cache.invalidate(); _hyper_inited = false; _stats_inited = false; _last_arc_frac = -1.0f; _last_arc_angle = 135.0f; _last_arc_col = 0; }

    // Menu Navigation Handler (called by main when in SCREEN_SETTINGS_MENU)
    void handleMenuNav(NavAction action, DashTelemetry &telemetry);
    bool handleSettingsTouch(int16_t tx, int16_t ty, DashTelemetry &telemetry);
    bool handleSettingsSliderDrag(int16_t cx, int16_t cy, DashTelemetry &telemetry);
    bool handleWizardTouch(int16_t tx, int16_t ty, DashTelemetry &telemetry);
    void initSetupWizard(const DashTelemetry &telemetry);
    void resetSetupWizard();
    bool isInEditMode() const { return false; }
    uint8_t getSettingsTab() const { return _settings_tab_idx; }
    void setSettingsTab(uint8_t tab) { _settings_tab_idx = (tab < 6) ? tab : 0; _menu_dirty = true; }

    // Race / Dragy Timer Logic
    void updateRaceTimer(DashTelemetry &telemetry, uint32_t now);

private:
    DisplayDriver *_display;
    LovyanGFX     *_canvas;

    // Screen 0: Main Ride Dashboard (renders selected style)
    void initRideDashboard(const DashTelemetry &telemetry);
    void renderRideDashboard(const DashTelemetry &telemetry);

    // Flagship: Hyper-VESC 35kW+ Pro Cluster
    void initHyper35kwStyle(const DashTelemetry &telemetry);
    void renderHyper35kwStyle(const DashTelemetry &telemetry);

    // Screen 1: Controllable LEDs & Light Modules (Color Wheel & Hazard)
    void initLightsScreen(const DashTelemetry &telemetry);
    void renderLightsScreen(const DashTelemetry &telemetry);
    void drawColorWheel(int cx, int cy, int r_in, int r_out, uint8_t selected_hue);

    // Screen 2: Real-Time Waveform Curves (Speed & Power) & Dual Controllers
    void initRtGraphsScreen(const DashTelemetry &telemetry);
    void renderRtGraphsScreen(const DashTelemetry &telemetry);
    void drawWaveformGraph(int x, int y, int w, int h, const float *data, int count, float max_val, uint16_t line_col, uint16_t fill_col, const char *label, const char *val_str, uint16_t val_col);

    // Screen 3: Interactive Touch Mode Configurator
    void initSettingsScreen(const DashTelemetry &telemetry);
    void renderSettingsScreen(const DashTelemetry &telemetry);

    // Screen 4: Transparent USB CDC <-> UART Bridge for VESC Tool
    void initVescBridgeScreen(const DashTelemetry &telemetry);
    void renderVescBridgeScreen(const DashTelemetry &telemetry);

    // Screen 5: Transparent Wi-Fi TCP <-> UART Bridge for VESC Tool
    void initWifiBridgeScreen(const DashTelemetry &telemetry);
    void renderWifiBridgeScreen(const DashTelemetry &telemetry);

    // Screen 6: "RACE" Dragy / Launch Acceleration Timer
    void initRaceTimerScreen(const DashTelemetry &telemetry);
    void renderRaceTimerScreen(const DashTelemetry &telemetry);
    void drawRaceRecordsModal(const DashTelemetry &telemetry);
    void drawRaceDynoModal(const DashTelemetry &telemetry);
    void drawPinChangeModal();

    // Screen 7: First-Time Setup & Onboarding Wizard
    void renderSetupWizard(const DashTelemetry &telemetry);

    // PIN-Lockscreen & VESC Alarm Banner (Requirements 1 & 7)
    void renderPinLockScreen(const DashTelemetry &telemetry);
    void drawFaultBanner(uint8_t fault_code);

    // Common UI Helpers
    void drawCard(int x, int y, int w, int h, uint16_t bg, uint16_t border);
    void drawValWithUnit(int x, int y, int numPadW, int unitPadW, const char *numStr, const char *unitStr, uint16_t numCol, uint16_t unitCol, uint16_t bgCol = 0x0821);
    void drawThrottleBar(int x, int y, int w, int h, float pct);

    // Screen State
    DashboardScreen _active_screen;
    DashboardStyle  _active_style;
    bool            _screen_dirty;
    bool            _mode_banner_active;

    // Menu / Settings Configurator State
    uint8_t  _settings_tab_idx; // 0 = ECO, 1 = SPORT, 2 = RACE, 3 = DRAGY, 4 = DASH, 5 = PIN
    uint8_t  _last_rendered_tab;
    bool     _settings_inited;
    bool     _menu_dirty;

    // Differential Render Cache
    struct RenderCache {
        int speed;
        int duty_x10;
        int duty_fill_w;
        int throttle_pct;
        int throttle_fill_w;
        int battery_pct;
        int voltage_x10;
        int current_x10;
        int phase_x10;
        int watts;
        int remaining_wh;
        int est_range;
        int temp_motor;
        int temp_esc;
        int g_force_x10;
        int incline_deg;
        int trip_x10;
        int odo;
        int uptime;
        bool vesc_connected;
        uint8_t batt_prof;

        // Statistics Screen Caches
        int peak_watts;
        int peak_curr_x10;
        int peak_phase_x10;
        int max_spd_x10;
        int avg_spd_x10;
        int eff_x10;
        int soh_x10;
        uint32_t ride_sec;

        // Hyper 35kW & Lights Caches
        int ride_mode;
        bool tuning_mode;
        bool boost_active;
        bool hazard_active;
        uint8_t light_mode;
        uint8_t light_hue;
        uint8_t light_brightness;

        // Clock & GPS Satellite Caches
        char clock_str[12];
        uint8_t gps_sats;
        bool gps_valid;
        bool use_gps_speed;
        bool gps_enabled;
        bool incline_gforce_enabled;

        void invalidate() {
            clock_str[0] = '\0';
            gps_sats = 255;
            gps_valid = false;
            use_gps_speed = false;
            gps_enabled = true;
            incline_gforce_enabled = true;
            speed = -999;
            duty_x10 = -999;
            duty_fill_w = -1;
            throttle_pct = -1;
            throttle_fill_w = -1;
            battery_pct = -1;
            voltage_x10 = -1;
            current_x10 = -1;
            phase_x10 = -1;
            watts = -99999;
            remaining_wh = -1;
            est_range = -1;
            temp_motor = -999;
            temp_esc = -999;
            g_force_x10 = -999;
            incline_deg = -999;
            trip_x10 = -1;
            odo = -1;
            uptime = -1;
            vesc_connected = false;
            batt_prof = 255;

            peak_watts = -99999;
            peak_curr_x10 = -1;
            peak_phase_x10 = -1;
            max_spd_x10 = -1;
            avg_spd_x10 = -1;
            eff_x10 = -1;
            soh_x10 = -1;
            ride_sec = 0xFFFFFFFF;

            ride_mode = -1;
            tuning_mode = false;
            boost_active = false;
            hazard_active = false;
            lights_enabled = false;
            light_mode = 255;
            light_hue = 255;
            light_brightness = 255;
        }
        bool lights_enabled;
    } _cache;

    // Analog Needle State
    struct NeedleCoords {
        int tipX, tipY, b1X, b1Y, b2X, b2Y;
        bool valid;
    } _last_needle;

    // Needle & Speed Sweep Animation State
    bool     _is_sweeping;
    uint8_t  _sweep_phase;
    float    _sweep_progress;
    float    _sweep_duty;
    float    _sweep_speed;
    uint32_t _last_sweep_tick_ms;

    // VESC Bridge Display State
    uint32_t _bridge_last_pc_bytes;
    uint32_t _bridge_last_vesc_bytes;
    uint32_t _bridge_last_wifi_rx;
    uint32_t _bridge_last_wifi_tx;
    int8_t   _bridge_last_wifi_conn;

    // 60 FPS Fluid Animation State
    float    _anim_speed;
    float    _anim_power;
    float    _anim_duty;
    uint32_t _anim_last_ms;
    bool     _anim_inited;

    // Hyper 35kW Zero-Flicker Cache
    bool     _hyper_inited;
    bool     _stats_inited;
    float    _last_arc_frac;
    float    _last_arc_angle;
    uint16_t _last_arc_col;

    // Statistics Screen Differential Cache (Zero Flicker)
    struct StatsCache {
        int trip_x10;
        int odo_x10;
        uint32_t ride_sec;
        int avg_spd_x10;
        int max_spd_x10;
        int live_spd_x10;
        int trip_wh;
        int rem_wh;
        int peak_watts;
        int peak_curr_x10;
        int peak_phase_x10;
        int trip_wh_km_x10;
        int dyn_wh_km_x10;
        int min_v_x10;
        int max_v_x10;
        int live_v_x10;
        int max_temp_mot;
        int max_temp_esc;

        void invalidate() {
            trip_x10 = -1;
            odo_x10 = -1;
            ride_sec = 0xFFFFFFFF;
            avg_spd_x10 = -1;
            max_spd_x10 = -1;
            live_spd_x10 = -1;
            trip_wh = -1;
            rem_wh = -1;
            peak_watts = -1;
            peak_curr_x10 = -1;
            peak_phase_x10 = -1;
            trip_wh_km_x10 = -1;
            dyn_wh_km_x10 = -1;
            min_v_x10 = -1;
            max_v_x10 = -1;
            live_v_x10 = -1;
            max_temp_mot = -999;
            max_temp_esc = -999;
        }
    } _stats_cache;

    // PIN Lockscreen Render State
    bool     _pin_screen_drawn;
    int      _last_pin_len;
    bool     _last_pin_err;

    // PIN Change Modal State (in Settings)
    bool     _pin_modal_open;
    char     _pin_modal_buf[8];
    bool     _pin_modal_success;
    uint32_t _pin_modal_success_ms;

    // Wizard State
    uint8_t  _wizard_step;       // 0=Welcome, 1=PIN, 2=Hardware
    char     _wizard_pin_buf[8]; // PIN input buffer
    bool     _wizard_inited;
};

extern DashboardRenderer Renderer;
