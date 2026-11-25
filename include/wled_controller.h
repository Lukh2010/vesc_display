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
 * @brief Zero-heap 2-Segment Hardware-RMT WLED engine for WS2812B / SK6812 / WS2815 strips.
 *
 * Segment 0 (0 .. _rear_led_count - 1): Legal Rear Tail & Adaptive Brake Light (always active).
 * Segment 1 (_rear_led_count .. WLED_LED_COUNT - 1): Tuning Underglow (STRICTLY OFF in Police Mode).
 */
class WledController {
public:
    static constexpr uint8_t EFFECT_SOLID      = 0;
    static constexpr uint8_t EFFECT_SPEED_GLOW = 1;
    static constexpr uint8_t EFFECT_BREATHE    = 2;
    static constexpr uint8_t EFFECT_RAINBOW    = 3;
    static constexpr uint8_t EFFECT_SCANNER    = 4;
    static constexpr uint8_t EFFECT_MAX        = 4;

    static WledController& instance();

    void begin();
    void update(const DashTelemetry &telemetry, uint32_t now_ms);

    void setEnabled(bool en);
    bool isEnabled() const;

    void setBrightness(uint8_t bri);
    uint8_t getBrightness() const;

    void setCustomColor(uint8_t r, uint8_t g, uint8_t b);
    void setUseThemeColor(bool use_theme);
    bool isUsingThemeColor() const;

    uint8_t getColorR() const;
    uint8_t getColorG() const;
    uint8_t getColorB() const;

    void setEffect(uint8_t fx);
    uint8_t getEffect() const;

    void setRearLedCount(uint8_t count);
    uint8_t getRearLedCount() const;

    WledRgb getLedRgb(int idx) const;
    WledEffectMode getActiveMode() const { return _active_mode; }
    int getLedCount() const { return WLED_LED_COUNT; }

private:
    WledController();

    static WledRgb themePrimaryRgb(uint8_t color_theme);
    static WledRgb scaleRgb(WledRgb c, uint8_t scale);
    static WledRgb colorWheel(uint8_t pos);

    void renderEffect(const DashTelemetry &telemetry, uint32_t now_ms, uint32_t dt_ms);
    void renderUnderglowSegment(uint8_t rear_count, uint8_t effect, WledRgb base, uint8_t brightness,
                                const DashTelemetry &telemetry, uint32_t now_ms, uint32_t dt_ms);
    void flushHardware();

    WledRgb        _leds[WLED_LED_COUNT];
    bool           _initialized;
    bool           _enabled;
    bool           _rmt_ready;
    bool           _has_updated;
    uint8_t        _brightness;
    bool           _use_theme_color;
    uint8_t        _custom_r;
    uint8_t        _custom_g;
    uint8_t        _custom_b;
    uint8_t        _effect;
    uint8_t        _rear_led_count;
    WledEffectMode _active_mode;
    uint32_t       _last_update_ms;
    uint32_t       _anim_phase;
};
