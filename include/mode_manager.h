#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "dash_types.h"
#if !defined(NATIVE_SIMULATOR)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

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
    uint8_t    battery_cells;                   // 10 to 24 (default 13)
    float      battery_cutoff_start;            // V (e.g. 42.9V for 13S)
    float      battery_cutoff_end;              // V (e.g. 39.0V for 13S)
};

static constexpr uint32_t MODE_CONFIG_BLOB_MAGIC = 0x564D4342; // "VMCB" (Vehicle Mode Config Blob)
static constexpr uint16_t MODE_CONFIG_BLOB_VERSION = 5;

struct VehicleModeConfigBlob {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    VehicleModeConfig config;
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
    VehicleModeConfig getConfigSnapshot() const;

    // Synchronizes battery series cell count (10..24) with Settings and recalculates cutoffs
    void setBatteryCells(uint8_t cells);
    uint8_t getBatteryCells() const;

    bool isTuningMode() const;
    void setTuningMode(bool enable);

    bool isBoostActive() const;
    void setBoostActive(bool active);

    bool isLocked() const;
    void setLocked(bool locked);

    RideMode getRideMode() const;
    void setRideMode(RideMode mode);
    void cycleMode();

    // Returns active limits for the current ride mode and tuning state
    ModeLimits getActiveLimits() const;
    const char* getActiveModeName() const;

    bool checkAndClearLimitsDirty();
    void markLimitsDirty();
    void setAsyncVescPush(bool enabled);

    // Sends current active limits to VESC
    void applyLimitsToVesc();
    void applyCurrentMode() { applyLimitsToVesc(); }

private:
    ModeManager();
    VehicleModeConfig _config;
    VehicleModeConfig _last_saved_config;
    bool _has_saved_snapshot;
    volatile bool _tuning_mode;
    volatile bool _boost_active;
    volatile bool _is_locked;
    volatile RideMode _ride_mode;
    volatile bool _limits_dirty;
    volatile bool _async_vesc_push;
#if !defined(NATIVE_SIMULATOR)
    mutable SemaphoreHandle_t _mutex;
#endif
    Preferences _prefs;

    void setFactoryDefaults();
};

