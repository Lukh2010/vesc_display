#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "dash_types.h"

class SettingsManager {
public:
    SettingsManager();
    void begin();
    void save();
    void saveOdo(float odo_km);
    void resetToDefaults();

    DashSettings& get() { return _settings; }
    const DashSettings& get() const { return _settings; }

    RaceRecords getRaceRecords() const { return _race_records; }
    void saveRaceRecords(const RaceRecords &records);
    void resetRaceRecords();

private:
    DashSettings _settings;
    DashSettings _last_saved;
    RaceRecords  _race_records;
    RaceRecords  _last_saved_race;
    bool         _has_saved_snapshot;
    Preferences  _prefs;
    void loadDefaults();
    void saveInternal(bool force);
};

extern SettingsManager Settings;
