#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "dash_types.h"

// 3 modes in both legal and tuned profiles: ECO, SPORT, RACE
enum ProfileModeIndex {
    PROFILE_MODE_ECO   = 0,
    PROFILE_MODE_SPORT = 1,
    PROFILE_MODE_RACE  = 2,
    PROFILE_MODE_COUNT = 3
};

struct ModeLimits {
    float max_speed_kmh;    // km/h (0 = unlimited)
    float max_power_watts;  // W (continuous cruising power limit, e.g. 500W at 22 km/h)
    float max_pull_watts;   // W (peak acceleration pull power, e.g. 1000W)
    float max_battery_amps; // A
    float max_phase_amps;   // A
};

struct VehicleModeConfig {
    ModeLimits legal_modes[PROFILE_MODE_COUNT]; // When tuning_mode == false: ECO (15 km/h), SPORT (20 km/h), RACE (22 km/h)
    ModeLimits tuned_modes[PROFILE_MODE_COUNT]; // When tuning_mode == true: ECO (35 km/h), SPORT (55 km/h), RACE (85+ km/h)
    int8_t     throttle_curve;                  // -50 to +50 (% curve / expo)
    uint8_t    battery_cells;                   // 10 to 24 (default 14)
    float      battery_cutoff_start;            // V (e.g. 46.2V)
    float      battery_cutoff_end;              // V (e.g. 42.0V)
};

class ModeManager {
public:
    static ModeManager& instance();

    void begin();
    void loadFromNvs();
    void saveToNvs();
    void resetDefaults();

    VehicleModeConfig& getConfig() { return _config; }
    const VehicleModeConfig& getConfig() const { return _config; }

    bool isTuningMode() const { return _tuning_mode; }
    void setTuningMode(bool enable);

    bool isBoostActive() const { return _boost_active; }
    void setBoostActive(bool active);

    bool isLocked() const { return _is_locked; }
    void setLocked(bool locked);

    RideMode getRideMode() const { return _ride_mode; }
    void setRideMode(RideMode mode);
    void cycleMode();

    // Returns active limits for the current ride mode and tuning state
    ModeLimits getActiveLimits() const;
    const char* getActiveModeName() const;

    bool checkAndClearLimitsDirty();
    void markLimitsDirty();

    // Sends current active limits to VESC
    void applyLimitsToVesc();
    void applyCurrentMode() { applyLimitsToVesc(); }

private:
    ModeManager();
    VehicleModeConfig _config;
    volatile bool _tuning_mode;
    volatile bool _boost_active;
    volatile bool _is_locked;
    volatile RideMode _ride_mode;
    volatile bool _limits_dirty;
    Preferences _prefs;

    void setFactoryDefaults();
};

