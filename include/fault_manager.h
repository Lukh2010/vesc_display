#pragma once

#include <Arduino.h>
#include <Preferences.h>

#define MAX_FAULT_HISTORY 10

enum VescFaultCode {
    FAULT_NONE = 0,
    FAULT_OVER_VOLTAGE = 1,
    FAULT_UNDER_VOLTAGE = 2,
    FAULT_DRV = 3,
    FAULT_ABS_OVER_CURRENT = 4,
    FAULT_OVER_TEMP_FET = 5,
    FAULT_OVER_TEMP_MOTOR = 6,
    FAULT_GATE_DRIVER_OVER_VOLTAGE = 7,
    FAULT_GATE_DRIVER_UNDER_VOLTAGE = 8,
    FAULT_MCU_UNDER_VOLTAGE = 9,
    FAULT_BOOTING_FROM_WATCHDOG_RESET = 10,
    FAULT_ENCODER_SPI = 11,
    FAULT_ENCODER_SINCOS_BELOW_MIN_AMPLITUDE = 12,
    FAULT_ENCODER_SINCOS_ABOVE_MAX_AMPLITUDE = 13,
    FAULT_FLASH_CORRUPTION = 14,
    FAULT_HIGH_OFFSET_CURRENT_SENSOR_1 = 15,
    FAULT_HIGH_OFFSET_CURRENT_SENSOR_2 = 16,
    FAULT_HIGH_OFFSET_CURRENT_SENSOR_3 = 17,
    FAULT_UNBALANCED_CURRENTS = 18,
    FAULT_BRK = 19,
    FAULT_RESOLVER_LOT = 20,
    FAULT_RESOLVER_DOS = 21,
    FAULT_RESOLVER_LOS = 22,
    FAULT_FLASH_CORRUPTION_APP_CFG = 23,
    FAULT_FLASH_CORRUPTION_MC_CFG = 24,
    FAULT_ENCODER_NO_MAGNET = 25
};

struct FaultRecord {
    uint8_t  code;
    uint32_t timestamp_s;
    float    voltage;
    float    current;
    float    phase_current;
    float    temp_fet;
    float    temp_mot;
};

class FaultManager {
public:
    static FaultManager& instance();

    void begin();
    void recordFault(uint8_t code, float v, float i, float phase_i, float fet_temp, float mot_temp, uint32_t uptime_s);

    uint8_t getFaultCount() const { return _fault_count; }
    FaultRecord getFault(uint8_t index) const;
    void clearFaults();

    uint8_t getActiveFault() const { return _active_fault; }
    void setActiveFault(uint8_t code) { _active_fault = code; }

    static const char* getFaultTitleDe(uint8_t code);
    static const char* getFaultDescriptionDe(uint8_t code);

private:
    FaultManager();
    Preferences _prefs;
    FaultRecord _history[MAX_FAULT_HISTORY];
    uint8_t     _fault_count;
    uint8_t     _active_fault;
    uint32_t    _last_logged_time_ms;
    uint8_t     _last_logged_code;

    void loadFromNvs();
    void saveToNvs();
};
