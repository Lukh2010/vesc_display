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
    void invalidateCache() { _cache.invalidate(); }

    // Menu Navigation Handler (called by main when in SCREEN_SETTINGS_MENU)
    void handleMenuNav(NavAction action, DashTelemetry &telemetry);
    bool isInEditMode() const { return _menu_edit_mode; }

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

    // Screen 3: Interactive Settings Menu System
    void initSettingsScreen(const DashTelemetry &telemetry);
    void renderSettingsScreen(const DashTelemetry &telemetry);
    void renderRootMenu();
    void renderSubmenu(uint8_t sub_id, const DashTelemetry &telemetry);
    void adjustCurrentSetting(int direction, DashTelemetry &telemetry); // +1 or -1

    // Screen 4: Transparent USB CDC <-> UART Bridge for VESC Tool
    void initVescBridgeScreen(const DashTelemetry &telemetry);
    void renderVescBridgeScreen(const DashTelemetry &telemetry);

    // Screen 5: Transparent Wi-Fi TCP <-> UART Bridge for VESC Tool
    void initWifiBridgeScreen(const DashTelemetry &telemetry);
    void renderWifiBridgeScreen(const DashTelemetry &telemetry);

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

    // Menu Navigation State
    bool     _menu_in_sub;
    uint8_t  _menu_root_idx;
    uint8_t  _menu_sub_idx;
    bool     _menu_edit_mode;
    uint32_t _menu_last_activity_ms;
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

        void invalidate() {
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
            light_mode = 255;
            light_hue = 255;
            light_brightness = 255;
        }
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
};

extern DashboardRenderer Renderer;
