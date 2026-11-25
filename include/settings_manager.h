#pragma once

#include <Arduino.h>
#include <Preferences.h>
#if !defined(NATIVE_SIMULATOR)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif
#include "dash_types.h"

class SettingsManager {
public:
    SettingsManager();
    void begin();
    void save();
    void saveOdo(float odo_km);
    void resetToDefaults();

    // High-precision meter odometer methods
    uint32_t getOdoMeters() const { return _odo_meters; }
    void setOdoMeters(uint32_t m);
    void addOdoMeters(float delta_m);

    DashSettings& get() { return _settings; }
    const DashSettings& get() const { return _settings; }

    RaceRecords getRaceRecords() const { return _race_records; }
    void saveRaceRecords(const RaceRecords &records);
    void resetRaceRecords();

private:
    DashSettings _settings;
    DashSettings _last_saved;
    uint32_t     _odo_meters;
    uint32_t     _last_saved_odo_m;
    float        _odo_accum_fraction_m;
    RaceRecords  _race_records;
    RaceRecords  _last_saved_race;
    bool         _has_saved_snapshot;
#if !defined(NATIVE_SIMULATOR)
    mutable SemaphoreHandle_t _mutex;
#endif
    Preferences  _prefs;
    void loadDefaults();
    void saveInternal(bool force);
};

extern SettingsManager Settings;
