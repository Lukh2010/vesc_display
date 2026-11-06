#pragma once

#include <Arduino.h>

// Top-Level Screens
enum DashboardScreen {
    SCREEN_RIDE_DASH     = 0,  // Hyper VESC 35kW+ Pro Cluster (Video Style)
    SCREEN_LIGHTS        = 1,  // Controllable LEDs, Color Wheel & Hazard Screen
    SCREEN_RT_GRAPHS     = 2,  // Real-Time Waveform Curves (Speed & Power) & Controllers
    SCREEN_SETTINGS_MENU = 3,  // Interactive Settings & Diagnostics Menu
    SCREEN_VESC_BRIDGE   = 4,  // Transparent USB CDC <-> UART Bridge for VESC Tool
    SCREEN_WIFI_BRIDGE   = 5,  // Transparent Wi-Fi TCP <-> UART Bridge for VESC Tool
    SCREEN_ENERGY_STATS  = SCREEN_LIGHTS,
    SCREEN_PERF_STATS    = SCREEN_RT_GRAPHS,
    SCREEN_COUNT
};

// Main Dashboard Styles (Selected in Settings)
enum DashboardStyle {
    STYLE_HYPER_35KW     = 0,  // "Best VESC Screen Ever Made" Hyper-VESC 35kW+ Cluster
    STYLE_ANALOG_DIAL    = 1,  // Left-Hug Analog Duty Dial (0-120% Duty with FW Redline)
    STYLE_HORIZONTAL_BAR = 2,  // Horizontal Duty Bar Cluster
    STYLE_COUNT
};

// Ride Modes (Eco, Sport, Race - Video Exact)
enum RideMode {
    MODE_ECO    = 0,  // Efficient mode (Green)
    MODE_SPORT  = 1,  // Dynamic sport mode (Cyan)
    MODE_RACE   = 2,  // Unlimited 35kW+ full output (Hot Pink)
    MODE_COUNT  = 3,
    MODE_STREET = 1   // Alias for backwards compatibility
};

// Navigation & Touch Actions
enum NavAction {
    NAV_NONE = 0,
    NAV_BTN1_SHORT,    // Button 1 / Swipe Right (Prev Screen / Value -)
    NAV_BTN1_LONG,     // Button 1 Long / Tap Settings (Menu Back / Settings)
    NAV_BTN2_SHORT,    // Button 2 / Swipe Left (Next Screen / Value +)
    NAV_BTN2_LONG,     // Button 2 Long (Trip Reset)
    NAV_EDIT_DEC,      // Accelerated Decrement
    NAV_EDIT_INC,      // Accelerated Increment
    NAV_BOTH_PRESSED,  // Both buttons / Profile Toggle / Select
    NAV_BOOST_TOGGLE,  // Tap on [ BOOST ] button
    NAV_MODE_CYCLE,    // Tap on [ SPORT / RACE / ECO ] button
    NAV_HAZARD_TOGGLE, // Tap on [ ▲ HAZARD ] button
    NAV_SCREEN_STATS,  // Tap on [ STATS ] button (Open SCREEN_RT_GRAPHS)
    NAV_TUNING_ENABLE, // 4-second continuous hold on km/h number (enable tuning mode)
    NAV_TUNING_DISABLE // Single tap on km/h number when tuning mode is active (deactivate)
};

// Long-Term & Ride Statistics
struct TripStatistics {
    float trip_wh;           // Energy consumed this trip (Watt-hours)
    float trip_wh_km;        // Energy efficiency (Wh/km)
    float peak_current_amps; // Maximum battery current recorded (A)
    float peak_phase_amps;   // Maximum motor phase current recorded (A)
    float peak_power_watts;  // Maximum power recorded (W)
    float max_speed_kmh;     // Maximum speed reached (km/h)
    float avg_speed_kmh;     // Average speed during trip (km/h)
    uint32_t ride_time_sec;  // Elapsed moving/ride time in seconds
    float max_temp_motor;    // Peak motor temperature recorded (°C)
    float max_temp_esc;      // Peak ESC/Mosfet temperature recorded (°C)
    float min_voltage;       // Minimum pack voltage recorded under load (V)
    float max_voltage;       // Maximum pack voltage recorded (V)
    float dynamic_wh_km;     // Rolling window energy efficiency (Wh/km)

    void reset() {
        trip_wh = 0.0f;
        trip_wh_km = 0.0f;
        dynamic_wh_km = 0.0f;
        peak_current_amps = 0.0f;
        peak_phase_amps = 0.0f;
        peak_power_watts = 0.0f;
        max_speed_kmh = 0.0f;
        avg_speed_kmh = 0.0f;
        ride_time_sec = 0;
        max_temp_motor = 0.0f;
        max_temp_esc = 0.0f;
        min_voltage = 0.0f;
        max_voltage = 0.0f;
    }
};

#define WAVE_HISTORY_LEN 60

// Core Telemetry Data Structure
struct DashTelemetry {
    // Speed & Inverter State
    float speed_kmh;           // Vehicle speed (km/h)
    float duty_cycle_pct;      // Inverter Duty Cycle (0.0% to 120.0%, >100% = Field Weakening)
    float rpm;                 // Motor mechanical RPM
    float throttle_pct;        // Throttle input percentage (0 - 100%)

    // Electrical (Dual/Single VESC telemetry)
    float voltage;             // Battery pack voltage (V)
    float current_amps;        // Battery pack current (A)
    float phase_amps;          // Motor phase current (A)
    float power_watts;         // Power (W = V * I)
    float amphours_used;       // Amp-hours consumed (Ah)

    // Multi-ESC Dual Motor & Controller Thermals (Rear & Front)
    float temp_fet_rear;       // Rear ESC Mosfet temp (°C)
    float temp_mot_rear;       // Rear Motor temp (°C)
    float temp_fet_front;      // Front ESC Mosfet temp (°C)
    float temp_mot_front;      // Front Motor temp (°C)

    // Backward-compatible aliases
    float temp_motor;          // Primary motor temp (°C)
    float temp_esc;            // Primary ESC temp (°C)

    // Smart Battery & Health State
    uint8_t battery_profile_id;// 0 = Profile 1, 1 = Profile 2
    float   battery_pct;       // Hybrid Fused State of Charge (0 - 100%)
    float   remaining_wh;      // Remaining energy in pack (Wh)
    float   battery_health_soh;// Learned battery health State of Health (SoH %)
    float   est_range_km;      // Projected remaining range (km)
    float   dynamic_wh_km;     // Dynamic rolling consumption (Wh/km)

    // Odometry & Statistics
    float trip_km;             // Trip distance (km)
    float odo_km;              // Total lifetime odometer (km)
    TripStatistics stats;      // Ride & energy statistics

    // VESC Diagnostics & Fault Codes (Requirement 1)
    uint8_t  fault_code;       // VESC mc_fault_code (0 = none)
    uint32_t fault_start_ms;   // Timestamp when fault was raised

    // Security & Boot PIN-Lockscreen (Requirement 3 & 7)
    bool     is_locked;        // true = Lockscreen active, VESC 0A output blocked
    char     pin_entry[8];     // Currently entered digits buffer
    bool     pin_error;        // Flash red / error shake on invalid PIN
    uint32_t pin_error_end_ms; // Error indicator display expiry

    // Ride Modes & Hyper Boost
    RideMode ride_mode;        // Active Ride Mode: ECO, STREET, SPORT, RACE
    bool     tuning_mode;      // false = Stealth / Drossel Mode (Default, red dot on), true = Custom Tuning (red dot off)
    bool     boost_active;     // Temporary 120% power boost active
    uint32_t boost_end_ms;     // Expiration timestamp of boost
    uint32_t mode_badge_until; // Temporary HUD mode banner display timer

    // Controllable LEDs & Light Modules (WS2812B / Neopixel - 9 Effects 3x3 Grid)
    uint8_t light_mode;        // 0=SOLID, 1=BREATHE, 2=WAVE, 3=RAINBOW, 4=COMET, 5=SPARKLE, 6=STROBE, 7=POLICE, 8=WHITE
    uint8_t light_hue;         // 0 - 255 Hue on color wheel
    uint8_t light_brightness;  // 0 - 255 PWM/RGB brightness
    bool    hazard_active;     // Flashing hazard lights active
    bool    lights_enabled;    // Master light power switch

    // Real-Time Waveform History (for Screen 2 RT graphs)
    float speed_history[WAVE_HISTORY_LEN];
    float power_history[WAVE_HISTORY_LEN];
    uint8_t history_count;
    uint32_t last_history_sample_ms;

    // Dual VESC Controller Peak Specs (from video)
    float vesc1_max_power;
    float vesc1_max_phase;
    float vesc1_max_speed;
    float vesc2_max_power;
    float vesc2_max_phase;
    float vesc2_max_speed;

    // Custom Rider / Scooter Tag (default: empty)
    char rider_tag[24];

    // System Status
    DashboardScreen screen;    // Currently active screen
    bool vesc_connected;       // UART/CAN communication healthy
    uint32_t uptime_sec;       // System run time in seconds
};
