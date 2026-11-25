#pragma once

#include <Arduino.h>
#include "board_config.h"
#include "dash_types.h"

struct WledRgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

enum WledEffectMode : uint8_t {
    WLED_MODE_OFF = 0,
    WLED_MODE_THEFT_STROBE = 1,
    WLED_MODE_BRAKE_SOLID = 2,
    WLED_MODE_BRAKE_EMERGENCY = 3,
    WLED_MODE_LOCKED_BREATHE = 4,
    WLED_MODE_SPEED_GLOW = 5
};

/**
 * @brief Zero-heap Hardware-RMT WLED engine for WS2812B / SK6812 / WS2815 strips.
 *
 * Designed to execute at ~40 Hz inside Core 0 realtimeTask while remaining
 * 100% deterministic and testable under NATIVE_SIMULATOR.
 */
class WledController {
public:
    static WledController& instance();

    void begin();
    void update(const DashTelemetry &telemetry, uint32_t now_ms);

    void setEnabled(bool enabled);
    bool isEnabled() const { return _enabled; }

    void setBrightness(uint8_t brightness);
    uint8_t getBrightness() const { return _brightness; }

    WledRgb getLedRgb(int idx) const;
    WledEffectMode getActiveMode() const { return _active_mode; }
    int getLedCount() const { return WLED_LED_COUNT; }

private:
    WledController();

    static WledRgb themePrimaryRgb(uint8_t color_theme);
    static WledRgb scaleRgb(WledRgb c, uint8_t scale);

    void renderEffect(const DashTelemetry &telemetry, uint32_t now_ms, uint32_t dt_ms);
    void flushHardware();

    WledRgb        _leds[WLED_LED_COUNT];
    bool           _initialized;
    bool           _enabled;
    bool           _rmt_ready;
    bool           _has_updated;
    uint8_t        _brightness;
    WledEffectMode _active_mode;
    uint32_t       _last_update_ms;
    uint32_t       _anim_phase;
};
