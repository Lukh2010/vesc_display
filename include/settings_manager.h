#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "dash_types.h"

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
