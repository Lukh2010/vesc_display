#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef ENABLE_STEERING_BUTTONS
#define ENABLE_STEERING_BUTTONS 0
#endif

#ifndef PIN_STEERING_ADC
#define PIN_STEERING_ADC -1
#endif

// Button Identifiers on Segway G3 Max 5-button handlebar block
enum class SteeringBtn : uint8_t {
    NONE = 0,
    BTN_CUSTOM,
    BTN_POWER_LIGHT,
    BTN_MODE,
    BTN_UP,
    BTN_DOWN
};

// Event Types (Short tap < 450ms vs Long press >= 450ms)
enum class SteeringBtnEvent : uint8_t {
    NONE = 0,
    CUSTOM_SHORT,
    CUSTOM_LONG,
    POWER_LIGHT_SHORT,
    POWER_LIGHT_LONG,
    MODE_SHORT,
    MODE_LONG,
    UP_SHORT,
    UP_LONG,
    DOWN_SHORT,
    DOWN_LONG
};

// ADC Voltage window definition in millivolts
struct SteeringBtnThreshold {
    SteeringBtn btn;
    uint16_t min_mv;
    uint16_t max_mv;
};

class SteeringButtons {
public:
    static constexpr uint32_t DEBOUNCE_MS   = 30;
    static constexpr uint32_t LONG_PRESS_MS = 450;
    static constexpr size_t   MAX_THRESHOLDS = 8;

    static SteeringButtons& instance();

    SteeringButtons();

    void begin(int adc_pin = PIN_STEERING_ADC);
    void reset();

    // Set custom threshold window table
    void setThresholds(const SteeringBtnThreshold* table, size_t count);

    // Evaluates raw mV against configured threshold windows
    SteeringBtn resolveButton(uint16_t mv) const;

    // Periodic poll - returns any newly fired button event
    SteeringBtnEvent poll(uint32_t now_ms = 0);

    // Simulation / testing injection
    void setSimMilliVolts(uint16_t mv);
    uint16_t getSimMilliVolts() const { return _sim_mv; }

    SteeringBtn getActiveButton() const { return _active_btn; }
    bool isButtonPressed() const { return _active_btn != SteeringBtn::NONE; }

    static const char* getEventName(SteeringBtnEvent ev);
    static const char* getButtonName(SteeringBtn btn);

private:
    uint16_t readMilliVolts();
    SteeringBtnEvent makeEvent(SteeringBtn btn, bool is_long);

    int _adc_pin;
    uint16_t _sim_mv;
    bool _sim_active;

    SteeringBtnThreshold _thresholds[MAX_THRESHOLDS];
    size_t _threshold_count;

    // Debounce & press tracking
    SteeringBtn _last_raw_btn;
    uint32_t    _debounce_start_ms;
    bool        _debouncing;
    SteeringBtn _stable_btn;

    SteeringBtn _active_btn;
    uint32_t    _press_start_ms;
    bool        _long_reported;
};
