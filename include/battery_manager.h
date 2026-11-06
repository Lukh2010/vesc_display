#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "dash_types.h"

struct BatteryProfile {
    char     name[16];
    float    nominal_wh;        // e.g. 1300.0 Wh (52V 25Ah)
    float    learned_wh;        // True learned full capacity
    float    total_wh_discharged; // Lifetime energy delivered
    uint16_t charge_cycles;     // Estimated full cycle equivalents
    float    health_soh_pct;    // State of Health % (learned_wh / nominal_wh * 100)
};

class BatteryManager {
public:
    BatteryManager();
    void begin();
    void update(DashTelemetry &telemetry, float dt);
    
    // Profile Management
    uint8_t getActiveProfileIndex() const { return _active_profile_id; }
    void setActiveProfile(uint8_t id);
    BatteryProfile& getProfile(uint8_t id) { return _profiles[id % 2]; }
    const BatteryProfile& getProfile(uint8_t id) const { return _profiles[id % 2]; }
    void saveProfiles();
    void resetProfileLearning(uint8_t id);

    // Live Metrics
    float getFusedSocPct() const { return _fused_soc_pct; }
    float getRemainingWh() const { return _remaining_wh; }
    float getEstRangeKm() const { return _est_range_km; }
    float getDynamicWhKm() const { return _dynamic_wh_km; }
    float getOcvSoc(float v_pack);
    void resetTrip();

private:
    BatteryProfile _profiles[2];
    uint8_t        _active_profile_id;
    Preferences    _prefs;

    // Filter & Algorithm States
    float    _fused_soc_pct;
    float    _remaining_wh;
    float    _est_range_km;
    uint32_t _rest_start_ms;
    bool     _is_resting;
    float    _cycle_start_v;
    float    _cycle_wh_accum;
    bool     _tracking_full_cycle;

    // Rolling Dynamic Consumption Window (last ~2km)
    static const int ROLLING_SECTORS = 10;
    float    _sector_wh[ROLLING_SECTORS];
    float    _sector_km[ROLLING_SECTORS];
    int      _current_sector;
    float    _dist_in_current_sector;
    float    _wh_in_current_sector;
    float    _dynamic_wh_km;

    void  loadDefaults();
};

extern BatteryManager Battery;
