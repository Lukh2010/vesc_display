#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "dash_types.h"

struct DashSettings {
    uint8_t dash_style;          // 0 = STYLE_ANALOG_DIAL, 1 = STYLE_HORIZONTAL_BAR
    uint8_t brightness_pct;      // 20 to 100% (default 90%)
    uint16_t max_battery_amps;   // 5 to 200 A (default 28A)
    uint16_t max_phase_amps;     // 10 to 300 A (default 55A)
    uint8_t  field_weak_amps;    // 0 to 20 A (step 1A, default 10A)
    uint8_t  max_speed_kmh;      // 0 = Unlimited, 25, 32, 45, 60
    float   throttle_ramp_sec;   // 0.1 to 1.0 s (default 0.2s)
    float   wheel_diameter_in;   // 20.0 to 29.0 in (default 26.0)
    uint8_t motor_pole_pairs;    // 8 to 24 (default 15)
    float   gear_ratio;          // 1.0 to 4.0 (default 1.0)
    uint8_t active_battery_profile; // 0 = Profile 1, 1 = Profile 2
    float   voltage_trim_v;         // -2.0 to +2.0 V (default -0.3V)
    uint8_t throttle_adc_channel;   // 0 = ADC1, 1 = ADC2 (default 0 = ADC1)
    bool    pin_lock_enabled;       // Enable 4-digit PIN lock at startup (default true)
    char    pin_code[8];            // 4-digit PIN code (default "1234")
    uint8_t race_split1_kmh;        // Target speed for Split 1 (default 22 km/h)
    uint8_t race_split2_kmh;        // Target speed for Split 2 (default 50 km/h)
    uint8_t race_split3_kmh;        // Target speed for Split 3 (default 70 km/h)
    bool    gps_enabled;            // Enable ATGM336H GPS & live clock (default true)
    bool    incline_gforce_enabled; // Enable IMU pitch/incline & G-force gauges (default true)
    bool    wifi_bridge_enabled;    // SoftAP WiFi bridge enabled (default false)
    bool    setup_completed;        // First-time setup wizard completed (default false)
    uint8_t color_theme;            // DashTheme: 0=Cyber Cyan, 1=Synthwave, 2=Toxic Green, 3=Racing Red, 4=Clean Ice
    bool    turbine_sound_enabled;  // FOC audio turbine whine while driving (default true)
};

class SettingsManager {
public:
    SettingsManager();
    void begin();
    void save();
    void resetToDefaults();

    DashSettings& get() { return _settings; }
    const DashSettings& get() const { return _settings; }

    RaceRecords getRaceRecords() const { return _race_records; }
    void saveRaceRecords(const RaceRecords &records);
    void resetRaceRecords();

private:
    DashSettings _settings;
    RaceRecords  _race_records;
    Preferences  _prefs;
    void loadDefaults();
};

extern SettingsManager Settings;
