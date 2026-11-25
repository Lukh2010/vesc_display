#include "wled_controller.h"
#include <math.h>
#include <string.h>

#if !defined(NATIVE_SIMULATOR) && ENABLE_WLED
#include <esp32-hal-rmt.h>
#endif

static constexpr uint32_t WLED_UPDATE_INTERVAL_MS = 25; // ~40 Hz target rate (30-50 Hz)

#if !defined(NATIVE_SIMULATOR) && ENABLE_WLED
// 10 MHz RMT clock -> 1 tick = 100 ns (800 kHz WS2812B/SK6812/WS2815 timing)
static constexpr uint32_t WLED_RMT_FREQ_HZ = 10000000UL;
// Static DMA/RMT symbol buffer (zero heap): 24 bits per RGB LED
static rmt_data_t s_wled_rmt_symbols[WLED_LED_COUNT * 24];

static inline rmt_data_t makeWs2812Symbol(bool bit_one) {
    rmt_data_t sym;
    if (bit_one) {
        sym.duration0 = 8; // 800 ns HIGH
        sym.level0    = 1;
        sym.duration1 = 5; // 500 ns LOW
        sym.level1    = 0;
    } else {
        sym.duration0 = 4; // 400 ns HIGH
        sym.level0    = 1;
        sym.duration1 = 9; // 900 ns LOW
        sym.level1    = 0;
    }
    return sym;
}
#endif

WledController& WledController::instance() {
    static WledController inst;
    return inst;
}

WledController::WledController()
    : _initialized(false),
      _enabled(true),
      _rmt_ready(false),
      _has_updated(false),
      _brightness(180),
      _active_mode(WLED_MODE_OFF),
      _last_update_ms(0),
      _anim_phase(0) {
    memset(_leds, 0, sizeof(_leds));
}

WledRgb WledController::themePrimaryRgb(uint8_t color_theme) {
    switch (color_theme) {
        case THEME_ARCTIC_LIGHT:   return {   2, 132, 199 }; // #0284C7 Deep Sky Blue
        case THEME_CARBON_STEALTH: return { 160, 195, 235 }; // Cool Titanium Steel
        case THEME_NIGHT_DIM:      return { 184, 122,  40 }; // #B87A28 Warm Amber
        case THEME_OBSIDIAN_DARK:
        default:                   return {  56, 189, 248 }; // #38BDF8 Ice Sky
    }
}

WledRgb WledController::scaleRgb(WledRgb c, uint8_t scale) {
    if (scale == 255) return c;
    if (scale == 0) return { 0, 0, 0 };
    return {
        (uint8_t)(((uint16_t)c.r * (uint16_t)scale) / 255U),
        (uint8_t)(((uint16_t)c.g * (uint16_t)scale) / 255U),
        (uint8_t)(((uint16_t)c.b * (uint16_t)scale) / 255U)
    };
}

void WledController::begin() {
    memset(_leds, 0, sizeof(_leds));
    _active_mode = WLED_MODE_OFF;
    _has_updated = false;
    _last_update_ms = 0;
    _anim_phase = 0;
    _initialized = true;

#if !defined(NATIVE_SIMULATOR) && ENABLE_WLED && (PIN_WLED_DATA >= 0)
    _rmt_ready = rmtInit(PIN_WLED_DATA, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, WLED_RMT_FREQ_HZ);
    if (_rmt_ready) {
        rmtSetEOT(PIN_WLED_DATA, LOW);
        flushHardware();
        Serial.printf("[WLED] Hardware RMT initialized on GPIO %d (%d LEDs @ 800 kHz).\n",
                      PIN_WLED_DATA, WLED_LED_COUNT);
    } else {
        Serial.printf("[WLED] WARN: rmtInit failed on GPIO %d.\n", PIN_WLED_DATA);
    }
#else
    _rmt_ready = false;
#endif
}

void WledController::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        memset(_leds, 0, sizeof(_leds));
        _active_mode = WLED_MODE_OFF;
        flushHardware();
    }
}

void WledController::setBrightness(uint8_t brightness) {
    _brightness = brightness;
}

WledRgb WledController::getLedRgb(int idx) const {
    if (idx < 0 || idx >= WLED_LED_COUNT) {
        return { 0, 0, 0 };
    }
    return _leds[idx];
}

void WledController::update(const DashTelemetry &telemetry, uint32_t now_ms) {
#if !ENABLE_WLED
    (void)telemetry;
    (void)now_ms;
    memset(_leds, 0, sizeof(_leds));
    _active_mode = WLED_MODE_OFF;
    return;
#else
    if (!_initialized) {
        begin();
    }

    if (_has_updated && (uint32_t)(now_ms - _last_update_ms) < WLED_UPDATE_INTERVAL_MS) {
        return;
    }

    uint32_t dt_ms = _has_updated ? (uint32_t)(now_ms - _last_update_ms) : WLED_UPDATE_INTERVAL_MS;
    if (dt_ms > 200) dt_ms = 200;
    _last_update_ms = now_ms;
    _has_updated = true;

    if (!_enabled) {
        memset(_leds, 0, sizeof(_leds));
        _active_mode = WLED_MODE_OFF;
        flushHardware();
        return;
    }

    renderEffect(telemetry, now_ms, dt_ms);
    flushHardware();
#endif
}

void WledController::renderEffect(const DashTelemetry &telemetry, uint32_t now_ms, uint32_t dt_ms) {
    // Priority 1: Theft Alarm Strobe (100 ms Red / Blue alternation)
    if (telemetry.theft_alarm_active) {
        _active_mode = WLED_MODE_THEFT_STROBE;
        bool blue_phase = ((now_ms / 100U) & 1U) != 0U;
        WledRgb strobe = blue_phase ? WledRgb{ 0, 0, 255 } : WledRgb{ 255, 0, 0 };
        WledRgb out = scaleRgb(strobe, _brightness);
        for (int i = 0; i < WLED_LED_COUNT; i++) {
            _leds[i] = out;
        }
        return;
    }

    // Priority 2: Adaptive Brake Light (Regen < -2.0A or Decel < -0.45G; Emergency < -0.65G)
    bool emergency_brake = (telemetry.g_force < -0.65f);
    bool is_braking = emergency_brake || (telemetry.current_amps < -2.0f) || (telemetry.g_force < -0.45f);
    if (is_braking) {
        if (emergency_brake) {
            _active_mode = WLED_MODE_BRAKE_EMERGENCY;
            bool pulse_high = ((now_ms / 60U) & 1U) == 0U;
            WledRgb c = scaleRgb({ (uint8_t)(pulse_high ? 255 : 64), 0, 0 }, _brightness);
            for (int i = 0; i < WLED_LED_COUNT; i++) {
                _leds[i] = c;
            }
        } else {
            _active_mode = WLED_MODE_BRAKE_SOLID;
            WledRgb c = scaleRgb({ 255, 0, 0 }, _brightness);
            for (int i = 0; i < WLED_LED_COUNT; i++) {
                _leds[i] = c;
            }
        }
        return;
    }

    const WledRgb base = themePrimaryRgb(telemetry.color_theme);

    // Priority 3a: Parked / Locked Subtle Breathing (no alarm)
    if (telemetry.is_locked) {
        _active_mode = WLED_MODE_LOCKED_BREATHE;
        float phase_rad = ((float)(now_ms % 4000U) / 4000.0f) * 6.2831853f;
        float wave = 0.5f - 0.5f * cosf(phase_rad); // 0.0 -> 1.0 -> 0.0 over 4s
        uint8_t breath_scale = (uint8_t)(12.0f + wave * 36.0f); // Subtle low-power glow
        WledRgb c = scaleRgb(scaleRgb(base, breath_scale), _brightness);
        for (int i = 0; i < WLED_LED_COUNT; i++) {
            _leds[i] = c;
        }
        return;
    }

    // Priority 3b: Speed-Glow & Theme-Sync (ambient underglow + smooth speed-scaled runner wave)
    _active_mode = WLED_MODE_SPEED_GLOW;
    float spd = telemetry.speed_kmh;
    if (spd < 0.0f) spd = 0.0f;
    float spd_norm = spd / 60.0f;
    if (spd_norm > 1.0f) spd_norm = 1.0f;

    // Advance phase smoothly proportional to dt_ms and vehicle speed
    uint32_t phase_step = (dt_ms * (uint32_t)(18.0f + spd_norm * 90.0f)) / 10U;
    _anim_phase += phase_step;

    float base_gain = 0.35f + 0.55f * spd_norm;
    float runner_amp = 0.10f + 0.35f * spd_norm;
    float phase_offset = (float)(_anim_phase % 10000U) * 0.006283185f;

    for (int i = 0; i < WLED_LED_COUNT; i++) {
        float led_angle = ((float)i / (float)WLED_LED_COUNT) * 12.5663706f - phase_offset;
        float wave = 0.5f + 0.5f * sinf(led_angle);
        float gain = base_gain + runner_amp * (wave - 0.3f);
        if (gain < 0.15f) gain = 0.15f;
        if (gain > 1.0f)  gain = 1.0f;
        uint8_t sc = (uint8_t)(gain * 255.0f + 0.5f);
        _leds[i] = scaleRgb(scaleRgb(base, sc), _brightness);
    }
}

void WledController::flushHardware() {
#if !defined(NATIVE_SIMULATOR) && ENABLE_WLED && (PIN_WLED_DATA >= 0)
    if (!_rmt_ready) return;
    if (!rmtTransmitCompleted(PIN_WLED_DATA)) {
        return;
    }

    // Encode pixels in standard WS2812B GRB bit order (MSB first)
    size_t idx = 0;
    for (int i = 0; i < WLED_LED_COUNT; i++) {
        const uint8_t grb[3] = { _leds[i].g, _leds[i].r, _leds[i].b };
        for (int c = 0; c < 3; c++) {
            uint8_t byte_val = grb[c];
            for (int bit = 7; bit >= 0; bit--) {
                s_wled_rmt_symbols[idx++] = makeWs2812Symbol((byte_val & (1U << bit)) != 0);
            }
        }
    }

    rmtWriteAsync(PIN_WLED_DATA, s_wled_rmt_symbols, idx);
#endif
}
