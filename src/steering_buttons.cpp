#include "steering_buttons.h"
#include <Arduino.h>
#include <string.h>

// Default Segway G3 Max 5-button resistor ladder voltage windows (typical 3.3V reference)
static const SteeringBtnThreshold DEFAULT_THRESHOLDS[] = {
    { SteeringBtn::BTN_UP,          300,  700 }, // ~500 mV
    { SteeringBtn::BTN_DOWN,        800, 1200 }, // ~1000 mV
    { SteeringBtn::BTN_MODE,       1300, 1700 }, // ~1500 mV
    { SteeringBtn::BTN_POWER_LIGHT,1800, 2200 }, // ~2000 mV
    { SteeringBtn::BTN_CUSTOM,     2300, 2700 }  // ~2500 mV
};

SteeringButtons& SteeringButtons::instance() {
    static SteeringButtons inst;
    return inst;
}

SteeringButtons::SteeringButtons()
    : _adc_pin(PIN_STEERING_ADC),
      _sim_mv(3300),
      _sim_active(false),
      _threshold_count(0),
      _last_raw_btn(SteeringBtn::NONE),
      _debounce_start_ms(0),
      _debouncing(false),
      _stable_btn(SteeringBtn::NONE),
      _active_btn(SteeringBtn::NONE),
      _press_start_ms(0),
      _long_reported(false)
{
    setThresholds(DEFAULT_THRESHOLDS, sizeof(DEFAULT_THRESHOLDS) / sizeof(DEFAULT_THRESHOLDS[0]));
}

void SteeringButtons::begin(int adc_pin) {
    _adc_pin = adc_pin;
    reset();
}

void SteeringButtons::reset() {
    _sim_active = false;
    _sim_mv = 3300;
    _last_raw_btn = SteeringBtn::NONE;
    _debounce_start_ms = 0;
    _debouncing = false;
    _stable_btn = SteeringBtn::NONE;
    _active_btn = SteeringBtn::NONE;
    _press_start_ms = 0;
    _long_reported = false;
}

void SteeringButtons::setThresholds(const SteeringBtnThreshold* table, size_t count) {
    if (!table || count == 0) return;
    if (count > MAX_THRESHOLDS) {
        count = MAX_THRESHOLDS;
    }
    memcpy(_thresholds, table, count * sizeof(SteeringBtnThreshold));
    _threshold_count = count;
}

SteeringBtn SteeringButtons::resolveButton(uint16_t mv) const {
    for (size_t i = 0; i < _threshold_count; ++i) {
        if (mv >= _thresholds[i].min_mv && mv <= _thresholds[i].max_mv) {
            return _thresholds[i].btn;
        }
    }
    return SteeringBtn::NONE;
}

void SteeringButtons::setSimMilliVolts(uint16_t mv) {
    _sim_mv = mv;
    _sim_active = true;
}

uint16_t SteeringButtons::readMilliVolts() {
    if (_sim_active) {
        return _sim_mv;
    }
#if !defined(NATIVE_SIMULATOR) && (ENABLE_STEERING_BUTTONS != 0)
    if (_adc_pin >= 0) {
        return (uint16_t)analogReadMilliVolts(_adc_pin);
    }
#endif
    return 3300;
}

SteeringBtnEvent SteeringButtons::makeEvent(SteeringBtn btn, bool is_long) {
    switch (btn) {
        case SteeringBtn::BTN_CUSTOM:
            return is_long ? SteeringBtnEvent::CUSTOM_LONG : SteeringBtnEvent::CUSTOM_SHORT;
        case SteeringBtn::BTN_POWER_LIGHT:
            return is_long ? SteeringBtnEvent::POWER_LIGHT_LONG : SteeringBtnEvent::POWER_LIGHT_SHORT;
        case SteeringBtn::BTN_MODE:
            return is_long ? SteeringBtnEvent::MODE_LONG : SteeringBtnEvent::MODE_SHORT;
        case SteeringBtn::BTN_UP:
            return is_long ? SteeringBtnEvent::UP_LONG : SteeringBtnEvent::UP_SHORT;
        case SteeringBtn::BTN_DOWN:
            return is_long ? SteeringBtnEvent::DOWN_LONG : SteeringBtnEvent::DOWN_SHORT;
        default:
            return SteeringBtnEvent::NONE;
    }
}

SteeringBtnEvent SteeringButtons::poll(uint32_t now_ms) {
    if (now_ms == 0) {
        now_ms = millis();
    }

    uint16_t mv = readMilliVolts();
    SteeringBtn raw_btn = resolveButton(mv);

    // Software debounce filter (30ms)
    if (raw_btn != _last_raw_btn) {
        _last_raw_btn = raw_btn;
        _debounce_start_ms = now_ms;
        _debouncing = true;
    } else if (_debouncing) {
        if ((now_ms - _debounce_start_ms) >= DEBOUNCE_MS) {
            _stable_btn = _last_raw_btn;
            _debouncing = false;
        }
    }

    SteeringBtnEvent ev = SteeringBtnEvent::NONE;

    // Press & hold tracking
    if (_active_btn == SteeringBtn::NONE) {
        if (_stable_btn != SteeringBtn::NONE) {
            _active_btn = _stable_btn;
            _press_start_ms = now_ms;
            _long_reported = false;
        }
    } else {
        // Button currently pressed
        if (_stable_btn == _active_btn) {
            // Check long press threshold (>= 450ms)
            if (!_long_reported && (now_ms - _press_start_ms) >= LONG_PRESS_MS) {
                _long_reported = true;
                ev = makeEvent(_active_btn, true);
            }
        } else {
            // Button released or changed
            if (!_long_reported) {
                ev = makeEvent(_active_btn, false);
            }
            _active_btn = SteeringBtn::NONE;
            _long_reported = false;
            // Immediate transition if another stable button is active
            if (_stable_btn != SteeringBtn::NONE) {
                _active_btn = _stable_btn;
                _press_start_ms = now_ms;
                _long_reported = false;
            }
        }
    }

    return ev;
}

const char* SteeringButtons::getButtonName(SteeringBtn btn) {
    switch (btn) {
        case SteeringBtn::BTN_CUSTOM:      return "CUSTOM";
        case SteeringBtn::BTN_POWER_LIGHT: return "POWER_LIGHT";
        case SteeringBtn::BTN_MODE:        return "MODE";
        case SteeringBtn::BTN_UP:          return "UP";
        case SteeringBtn::BTN_DOWN:        return "DOWN";
        default:                           return "NONE";
    }
}

const char* SteeringButtons::getEventName(SteeringBtnEvent ev) {
    switch (ev) {
        case SteeringBtnEvent::CUSTOM_SHORT:      return "CUSTOM_SHORT";
        case SteeringBtnEvent::CUSTOM_LONG:       return "CUSTOM_LONG";
        case SteeringBtnEvent::POWER_LIGHT_SHORT: return "POWER_LIGHT_SHORT";
        case SteeringBtnEvent::POWER_LIGHT_LONG:  return "POWER_LIGHT_LONG";
        case SteeringBtnEvent::MODE_SHORT:        return "MODE_SHORT";
        case SteeringBtnEvent::MODE_LONG:         return "MODE_LONG";
        case SteeringBtnEvent::UP_SHORT:          return "UP_SHORT";
        case SteeringBtnEvent::UP_LONG:           return "UP_LONG";
        case SteeringBtnEvent::DOWN_SHORT:        return "DOWN_SHORT";
        case SteeringBtnEvent::DOWN_LONG:         return "DOWN_LONG";
        default:                                  return "NONE";
    }
}
