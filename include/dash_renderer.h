#pragma once

#include <Arduino.h>
#include "display_driver.h"
#include "dash_types.h"
#include "settings_manager.h"
#include "battery_manager.h"
#include "board_config.h"
#include "ui_layout.h"

class DashboardRenderer {
public:
    enum UiFont : uint8_t {
        UIF_HERO_NUM = 0, // Roboto Bold 72px (digits, - . + % :)
        UIF_MED_NUM  = 1, // Roboto Bold 34px (digits, A-Z, k m h s % . : - + / °)
        UIF_HEADING  = 2, // Roboto Bold 22px (full ASCII + DE umlauts)
        UIF_BODY     = 3, // Roboto Medium 16px (full ASCII + DE umlauts)
        UIF_SMALL    = 4, // Roboto Medium 12px (full ASCII + DE umlauts)
        UIF_COUNT    = 5
    };

    enum SettingsTab : uint8_t {
        TAB_SPEED  = 0,
        TAB_DRAGY  = 1,
        TAB_DASH   = 2,
        TAB_SYSTEM = 3,
        TAB_PIN    = 4,
        TAB_COUNT  = 5
    };

    DashboardRenderer();
    void begin(DisplayDriver *display);
    void render(const DashTelemetry &telemetry);
    void triggerNeedleSweep();
    bool isSweeping() const { return _is_sweeping; }

    // Boot Animation Logic
    void triggerBootAnimation();
    bool isBootAnimActive() const { return _boot_anim_active; }
    void skipBootAnimation();
    void handleBootTouch(bool is_vesc_connected);
    bool isVescLinkSkipped() const { return _vesc_link_skipped; }
    uint8_t getBootAnimPhase() const { return _boot_anim_phase; }
    void setBootAnimElapsed(uint32_t elapsed_ms);
    void renderBootAnimation(uint32_t elapsed_ms, const DashTelemetry &telemetry);

    void markScreenDirty() { _screen_dirty = true; }
    void markMenuDirty() { _menu_dirty = true; }
    void invalidateCache() {
        _cache.invalidate();
        _stats_cache.invalidate();
        _hyper_inited = false;
        _stats_inited = false;
        _settings_inited = false;
        _last_arc_frac = -1.0f;
        _last_arc_angle = 135.0f;
        _last_arc_col = 0;
        _race_records_drawn = false;
        _race_dyno_drawn = false;
        _last_race_arc_angle = 130.0f;
        _last_race_speed = -1;
        _last_race_state = 255;
        _last_race_countdown = -1;
        _last_race_elapsed_sec = -1.0f;
        _last_race_split_0_22 = -999.0f;
        _last_race_split_0_50 = -999.0f;
        _last_race_split_0_70 = -999.0f;
        _last_race_g_force_x10 = -999;
        _race_plateau_start_ms = 0;
        _race_last_eval_ms = 0;
        _race_last_eval_speed_kmh = 0.0f;
        _mode_banner_drawn_mode = -1;
        _mode_banner_active = false;
        _pin_screen_drawn = false;
        _last_pin_bat_pct = -999;
        _last_pin_volt_x10 = -999;
        _last_pin_vesc_conn = -1;
        _last_ble_conn = false;
        _last_ble_bars = 255;
        _last_fault_flash = -1;
        _ota_inited = false;
        _last_ota_pct = -1;
        _last_ota_status[0] = '\0';
        _wizard_pin_warn = false;
    }

    // Color Theme & Palettes
    static const ColorThemePalette& getThemePalette(uint8_t theme_id);
    const ColorThemePalette& currentTheme() const;

    // Menu Navigation Handler (called by main when in SCREEN_SETTINGS_MENU)
    void handleMenuNav(NavAction action, DashTelemetry &telemetry);
    bool handleSettingsTouch(int16_t tx, int16_t ty, DashTelemetry &telemetry);
    bool handleSettingsSliderDrag(int16_t cx, int16_t cy, DashTelemetry &telemetry);
    bool handleWizardTouch(int16_t tx, int16_t ty, DashTelemetry &telemetry);
    void initSetupWizard(const DashTelemetry &telemetry);
    void resetSetupWizard();
    bool isInEditMode() const { return false; }
    uint8_t getSettingsTab() const { return _settings_tab_idx; }
    void setSettingsTab(uint8_t tab) { _settings_tab_idx = (tab < TAB_COUNT) ? tab : (uint8_t)TAB_DASH; _last_rendered_tab = 255; _menu_dirty = true; }

    // Race / Dragy Timer Logic
    void updateRaceTimer(DashTelemetry &telemetry, uint32_t now);

private:
    DisplayDriver *_display;
    LovyanGFX     *_canvas;

    // Pre-loaded zero-allocation VLW anti-aliased smooth fonts
    lgfx::PointerWrapper _vlwData[UIF_COUNT];
    lgfx::VLWfont        _vlwFonts[UIF_COUNT];
    bool                 _vlwLoaded[UIF_COUNT];

    void initSmoothFonts();
    const lgfx::IFont* getUiFont(UiFont f) const;
    void useFont(UiFont f);
    void resetFont();
    void drawSmoothText(UiFont f, const char *str, int x, int y, textdatum_t datum, uint16_t fg, uint16_t bg, int padW = 0);

    // Screen 0: Main Ride Dashboard (renders selected style)
    void initRideDashboard(const DashTelemetry &telemetry);
    void renderRideDashboard(const DashTelemetry &telemetry);

    // Flagship: Hyper-VESC 35kW+ Pro Cluster
    void initHyper35kwStyle(const DashTelemetry &telemetry);
    void renderHyper35kwStyle(const DashTelemetry &telemetry);

#if ENABLE_LIGHTS_SCREEN
    // Screen 1: Controllable LEDs & Light Modules (Color Wheel & Hazard)
    void initLightsScreen(const DashTelemetry &telemetry);
    void renderLightsScreen(const DashTelemetry &telemetry);
    void drawColorWheel(int cx, int cy, int r_in, int r_out, uint8_t selected_hue);
#endif

    // Screen 2: Telemetry, Statistics & 60s Live Dual-Curve Sparkline
    void initRtGraphsScreen(const DashTelemetry &telemetry);
    void renderRtGraphsScreen(const DashTelemetry &telemetry);
    void drawStatsSparkline(const DashTelemetry &telemetry, const ColorThemePalette &pal);

    // Screen 3: Interactive Touch Mode & System Configurator
    void initSettingsScreen(const DashTelemetry &telemetry);
    void renderSettingsScreen(const DashTelemetry &telemetry);

    // Screen 4: Transparent USB CDC <-> UART Bridge for VESC Tool
    void initVescBridgeScreen(const DashTelemetry &telemetry);
    void renderVescBridgeScreen(const DashTelemetry &telemetry);

    // Screen 5: Transparent Wi-Fi TCP <-> UART Bridge for VESC Tool
    void initWifiBridgeScreen(const DashTelemetry &telemetry);
    void renderWifiBridgeScreen(const DashTelemetry &telemetry);

    // Screen 6: "RACE" Dragy / Launch Acceleration Timer (Auto-Rollout)
    void initRaceTimerScreen(const DashTelemetry &telemetry);
    void renderRaceTimerScreen(const DashTelemetry &telemetry);
    void drawRaceRecordsModal(const DashTelemetry &telemetry);
    void drawRaceDynoModal(const DashTelemetry &telemetry);
    void drawPinChangeModal();

    // Screen 7: First-Time Setup & Onboarding Wizard
    void renderSetupWizard(const DashTelemetry &telemetry);

    // Screen 9: Wireless OTA Firmware Update Progress HUD
    void initOtaScreen();
    void renderOtaScreen(int pct, const char *status);

    // PIN-Lockscreen & VESC Alarm Banner
    void renderPinLockScreen(const DashTelemetry &telemetry);
    void drawFaultBanner(uint8_t fault_code);

    // Common UI Helpers
    void drawCard(int x, int y, int w, int h, uint16_t bg, uint16_t border, int r = 10);
    void drawPillToggle(int x, int y, bool on, const ColorThemePalette &pal);
    void drawLockIcon(int cx, int cy, uint16_t col, uint16_t bg);

    // Screen State
    DashboardScreen _active_screen;
    DashboardStyle  _active_style;
    uint8_t         _active_theme;
    bool            _screen_dirty;
    bool            _mode_banner_active;
    int8_t          _mode_banner_drawn_mode;

    // Menu / Settings Configurator State
    uint8_t  _settings_tab_idx; // SettingsTab (0..4)
    uint8_t  _last_rendered_tab;
    bool     _settings_inited;
    bool     _menu_dirty;
    int8_t   _settings_last_tuning_mode;

    // Differential Render Cache
    struct RenderCache {
        int speed;
        int duty_x10;
        int duty_fill_w;
        uint16_t duty_col;
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

        // Hyper 35kW & Status Caches
        int ride_mode;
        int8_t banner_active;
        int8_t boost_active;
        int8_t lights_enabled;
        int8_t tcs_active;
        int8_t tpms_warning;

        // Clock & GPS Satellite Caches
        char clock_str[12];
        int8_t gps_valid;
        int8_t gps_enabled;
        int8_t incline_gforce_enabled;
        int8_t clock_visible;
        uint8_t show_gforce;
        uint8_t show_incline;
        uint8_t show_light_btn;
        int8_t tuning_mode_dot;

        void invalidate() {
            clock_str[0] = '\0';
            gps_valid = -1;
            gps_enabled = -1;
            incline_gforce_enabled = -1;
            clock_visible = -1;
            show_gforce = 255;
            show_incline = 255;
            show_light_btn = 255;
            tuning_mode_dot = -1;
            speed = -999;
            duty_x10 = -999;
            duty_fill_w = -1;
            duty_col = 0;
            throttle_pct = -1;
            throttle_fill_w = -1;
            battery_pct = -999;
            voltage_x10 = -999;
            current_x10 = -9999;
            phase_x10 = -9999;
            watts = -99999;
            remaining_wh = -1;
            est_range = -999;
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
            banner_active = -1;
            boost_active = -1;
            lights_enabled = -1;
            tcs_active = -1;
            tpms_warning = -1;
        }
    } _cache;

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
        int8_t tuning_mode;
        uint32_t sparkline_sig;

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
            tuning_mode = -1;
            sparkline_sig = 0xFFFFFFFF;
        }
    } _stats_cache;

    // PIN Lockscreen Render State
    bool     _pin_screen_drawn;
    int      _last_pin_len;
    bool     _last_pin_err;
    bool     _last_theft_alarm;
    bool     _last_ble_conn;
    uint8_t  _last_ble_bars;
    int8_t   _last_pin_active_key;
    int      _last_pin_bat_pct;
    int      _last_pin_volt_x10;
    int8_t   _last_pin_vesc_conn;

    // PIN Change Modal State (in Settings)
    bool     _pin_modal_open;
    char     _pin_modal_buf[8];
    bool     _pin_modal_success;
    uint32_t _pin_modal_success_ms;

    // Wizard State
    uint8_t  _wizard_step;       // 0=Welcome, 1=PIN, 2=Hardware
    char     _wizard_pin_buf[8]; // PIN input buffer
    bool     _wizard_inited;
    bool     _wizard_pin_warn;
    int      _last_wizard_step;
    int      _last_wizard_pin_len;
    int8_t   _last_wizard_gps;
    int8_t   _last_wizard_incline;
    int8_t   _last_wizard_wifi;
    bool     _wifi_bridge_inited;

    // Fault Banner & OTA Differential Cache
    uint8_t  _last_fault_code;
    int8_t   _last_fault_flash;
    bool     _ota_inited;
    int      _last_ota_pct;
    char     _last_ota_status[64];

    // Off-Screen Backbuffer Selective Dirty-Rect Tracking
    struct DirtyRect {
        int16_t x, y, w, h;
    };
    static constexpr uint8_t MAX_DIRTY_RECTS = 16;
    DirtyRect _dirty_rects[MAX_DIRTY_RECTS];
    uint8_t   _dirty_rect_count;
    bool      _dirty_full;

    void clearDirtyTracking() { _dirty_rect_count = 0; _dirty_full = false; }
    void markFullDirty() { _dirty_full = true; _dirty_rect_count = 0; }
    void addDirtyRect(int16_t x, int16_t y, int16_t w, int16_t h);
    void flushDirtyToDisplay();

    // Race Screen Differential Cache (Zero Flicker & Modal Isolation)
    bool     _race_records_drawn;
    bool     _race_dyno_drawn;
    float    _last_race_arc_angle;
    int      _last_race_speed;
    uint8_t  _last_race_state;
    int      _last_race_countdown;
    float    _last_race_elapsed_sec;
    float    _last_race_split_0_22;
    float    _last_race_split_0_50;
    float    _last_race_split_0_70;
    int      _last_race_g_force_x10;
    uint32_t _race_plateau_start_ms;
    uint32_t _race_last_eval_ms;
    float    _race_last_eval_speed_kmh;
    uint8_t  _pb_split1_kmh;
    uint8_t  _pb_split2_kmh;
    uint8_t  _pb_split3_kmh;

    // Boot Animation State
    bool     _boot_anim_active;
    uint8_t  _boot_anim_phase; // 0=idle/done, 1=Phase 1 (Logo), 2=Phase 2 (Console), 3=Complete
    uint32_t _boot_anim_start_ms;
    uint8_t  _boot_log_lines_drawn;
    bool     _vesc_link_skipped;
    uint32_t _vesc_skip_click_ms;
    uint32_t _vesc_connected_at_ms;
};

extern DashboardRenderer Renderer;
