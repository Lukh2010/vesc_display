#include "wled_controller.h"
#include <math.h>
#include <string.h>

#if !defined(NATIVE_SIMULATOR)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
static portMUX_TYPE s_wled_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
#define WLED_CFG_ENTER() portENTER_CRITICAL(&s_wled_cfg_mux)
#define WLED_CFG_EXIT()  portEXIT_CRITICAL(&s_wled_cfg_mux)
#else
#define WLED_CFG_ENTER() do {} while (0)
#define WLED_CFG_EXIT()  do {} while (0)
#endif

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
      _use_theme_color(true),
      _custom_r(56),
      _custom_g(189),
      _custom_b(248),
      _effect(EFFECT_SPEED_GLOW),
      _rear_led_count((WLED_REAR_LED_COUNT > WLED_LED_COUNT) ? WLED_LED_COUNT : WLED_REAR_LED_COUNT),
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

WledRgb WledController::colorWheel(uint8_t pos) {
    pos = (uint8_t)(255U - pos);
    if (pos < 85U) {
        return { (uint8_t)(255U - pos * 3U), 0U, (uint8_t)(pos * 3U) };
    }
    if (pos < 170U) {
        pos = (uint8_t)(pos - 85U);
        return { 0U, (uint8_t)(pos * 3U), (uint8_t)(255U - pos * 3U) };
    }
    pos = (uint8_t)(pos - 170U);
    return { (uint8_t)(pos * 3U), (uint8_t)(255U - pos * 3U), 0U };
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
        Serial.printf("[WLED] Hardware RMT initialized on GPIO %d (%d LEDs: Rear=%u, Underglow=%u @ 800 kHz).\n",
                      PIN_WLED_DATA, WLED_LED_COUNT,
                      (unsigned)_rear_led_count,
                      (unsigned)(WLED_LED_COUNT - _rear_led_count));
    } else {
        Serial.printf("[WLED] WARN: rmtInit failed on GPIO %d.\n", PIN_WLED_DATA);
    }
#else
    _rmt_ready = false;
#endif
}

void WledController::setEnabled(bool en) {
    WLED_CFG_ENTER();
    _enabled = en;
    WLED_CFG_EXIT();
    if (!en) {
        memset(_leds, 0, sizeof(_leds));
        _active_mode = WLED_MODE_OFF;
        flushHardware();
    }
}

bool WledController::isEnabled() const {
    WLED_CFG_ENTER();
    bool v = _enabled;
    WLED_CFG_EXIT();
    return v;
}

void WledController::setBrightness(uint8_t bri) {
    WLED_CFG_ENTER();
    _brightness = bri;
    WLED_CFG_EXIT();
    if (bri == 0) {
        memset(_leds, 0, sizeof(_leds));
        flushHardware();
    }
}

uint8_t WledController::getBrightness() const {
    WLED_CFG_ENTER();
    uint8_t v = _brightness;
    WLED_CFG_EXIT();
    return v;
}

void WledController::setCustomColor(uint8_t r, uint8_t g, uint8_t b) {
    WLED_CFG_ENTER();
    _use_theme_color = false;
    _custom_r = r;
    _custom_g = g;
    _custom_b = b;
    WLED_CFG_EXIT();
}

void WledController::setUseThemeColor(bool use_theme) {
    WLED_CFG_ENTER();
    _use_theme_color = use_theme;
    WLED_CFG_EXIT();
}

bool WledController::isUsingThemeColor() const {
    WLED_CFG_ENTER();
    bool v = _use_theme_color;
    WLED_CFG_EXIT();
    return v;
}

uint8_t WledController::getColorR() const {
    WLED_CFG_ENTER();
    uint8_t v = _custom_r;
    WLED_CFG_EXIT();
    return v;
}

uint8_t WledController::getColorG() const {
    WLED_CFG_ENTER();
    uint8_t v = _custom_g;
    WLED_CFG_EXIT();
    return v;
}

uint8_t WledController::getColorB() const {
    WLED_CFG_ENTER();
    uint8_t v = _custom_b;
    WLED_CFG_EXIT();
    return v;
}

void WledController::setEffect(uint8_t fx) {
    if (fx > EFFECT_MAX) fx = EFFECT_MAX;
    WLED_CFG_ENTER();
    _effect = fx;
    WLED_CFG_EXIT();
}

uint8_t WledController::getEffect() const {
    WLED_CFG_ENTER();
    uint8_t v = _effect;
    WLED_CFG_EXIT();
    return v;
}

void WledController::setRearLedCount(uint8_t count) {
    if (count > WLED_LED_COUNT) count = WLED_LED_COUNT;
    WLED_CFG_ENTER();
    _rear_led_count = count;
    WLED_CFG_EXIT();
}

uint8_t WledController::getRearLedCount() const {
    WLED_CFG_ENTER();
    uint8_t v = _rear_led_count;
    WLED_CFG_EXIT();
    return v;
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

    renderEffect(telemetry, now_ms, dt_ms);
    flushHardware();
#endif
}

void WledController::renderEffect(const DashTelemetry &telemetry, uint32_t now_ms, uint32_t dt_ms) {
    // Atomic snapshot of runtime configuration (Core 1 HTTP /json/state <-> Core 0 render)
    WLED_CFG_ENTER();
    bool enabled         = _enabled;
    uint8_t brightness   = _brightness;
    bool use_theme       = _use_theme_color;
    WledRgb custom_rgb   = { _custom_r, _custom_g, _custom_b };
    uint8_t effect       = _effect;
    uint8_t rear_count   = _rear_led_count;
    WLED_CFG_EXIT();

    if (rear_count > WLED_LED_COUNT) rear_count = WLED_LED_COUNT;

    if (!enabled) {
        memset(_leds, 0, sizeof(_leds));
        _active_mode = WLED_MODE_OFF;
        return;
    }

    if (brightness == 0) {
        memset(_leds, 0, sizeof(_leds));
        // Still classify active mode logically if helpful, or OFF? Keep mode classification clean below or 0 LEDs:
        // All LEDs are strictly (0,0,0) when brightness == 0.
        if (telemetry.theft_alarm_active) _active_mode = WLED_MODE_THEFT_STROBE;
        else if (telemetry.g_force < -0.65f) _active_mode = WLED_MODE_BRAKE_EMERGENCY;
        else if (telemetry.current_amps < -2.0f || telemetry.g_force < -0.45f) _active_mode = WLED_MODE_BRAKE_SOLID;
        else if (telemetry.is_locked) _active_mode = WLED_MODE_LOCKED_BREATHE;
        else _active_mode = WLED_MODE_SPEED_GLOW;
        return;
    }

    // Priority 1: Theft Alarm Strobe (highest priority — BOTH segments flash Red/Blue @ 100 ms)
    if (telemetry.theft_alarm_active) {
        _active_mode = WLED_MODE_THEFT_STROBE;
        bool blue_phase = ((now_ms / 100U) & 1U) != 0U;
        WledRgb strobe = blue_phase ? WledRgb{ 0, 0, 255 } : WledRgb{ 255, 0, 0 };
        WledRgb out = scaleRgb(strobe, brightness);
        for (int i = 0; i < WLED_LED_COUNT; i++) {
            _leds[i] = out;
        }
        return;
    }

    bool emergency_brake = (telemetry.g_force < -0.65f);
    bool is_braking = emergency_brake || (telemetry.current_amps < -2.0f) || (telemetry.g_force < -0.45f);
    const WledRgb theme_rgb = themePrimaryRgb(telemetry.color_theme);

    // Priority 2: Locked / Parked (without alarm)
    // Segment 0: subtle low-power breathing; Segment 1: strictly OFF
    if (telemetry.is_locked) {
        _active_mode = WLED_MODE_LOCKED_BREATHE;
        float phase_rad = ((float)(now_ms % 4000U) / 4000.0f) * 6.2831853f;
        float wave = 0.5f - 0.5f * cosf(phase_rad); // 0.0 -> 1.0 -> 0.0 over 4s
        uint8_t breath_scale = (uint8_t)(12.0f + wave * 36.0f);
        WledRgb rear_c = scaleRgb(scaleRgb(theme_rgb, breath_scale), brightness);
        for (int i = 0; i < rear_count; i++) {
            _leds[i] = rear_c;
        }
        for (int i = rear_count; i < WLED_LED_COUNT; i++) {
            _leds[i] = { 0, 0, 0 };
        }
        return;
    }

    // -------------------------------------------------------------------------
    // Active Riding (Police Mode !tuning_mode OR Tuning Mode tuning_mode)
    // Segment 0 (0 .. rear_count - 1): Legal Rear Tail & Adaptive Brake Light
    // -------------------------------------------------------------------------
    WledRgb rear_color;
    if (emergency_brake) {
        _active_mode = WLED_MODE_BRAKE_EMERGENCY;
        bool pulse_high = ((now_ms / 60U) & 1U) == 0U;
        rear_color = scaleRgb({ (uint8_t)(pulse_high ? 255 : 64), 0, 0 }, brightness);
    } else if (is_braking) {
        _active_mode = WLED_MODE_BRAKE_SOLID;
        rear_color = scaleRgb({ 255, 0, 0 }, brightness);
    } else {
        _active_mode = WLED_MODE_SPEED_GLOW;
        rear_color = scaleRgb({ 90, 0, 0 }, brightness); // Subtle legal tail-light red
    }

    for (int i = 0; i < rear_count; i++) {
        _leds[i] = rear_color;
    }

    // -------------------------------------------------------------------------
    // Segment 1 (rear_count .. WLED_LED_COUNT - 1): Underglow (STRICTLY Tuning Only!)
    // -------------------------------------------------------------------------
    if (!telemetry.tuning_mode) {
        // Police / Legal Mode: 100% Stealth — underglow strip ALWAYS strictly OFF
        for (int i = rear_count; i < WLED_LED_COUNT; i++) {
            _leds[i] = { 0, 0, 0 };
        }
        return;
    }

    WledRgb ug_base = use_theme ? theme_rgb : custom_rgb;
    renderUnderglowSegment(rear_count, effect, ug_base, brightness, telemetry, now_ms, dt_ms);
}

void WledController::renderUnderglowSegment(uint8_t rear_count, uint8_t effect, WledRgb base, uint8_t brightness,
                                            const DashTelemetry &telemetry, uint32_t now_ms, uint32_t dt_ms) {
    const int seg_len = WLED_LED_COUNT - (int)rear_count;
    if (seg_len <= 0) return;

    switch (effect) {
        case EFFECT_SOLID: {
            WledRgb out = scaleRgb(base, brightness);
            for (int i = rear_count; i < WLED_LED_COUNT; i++) {
                _leds[i] = out;
            }
            break;
        }

        case EFFECT_BREATHE: {
            float phase_rad = ((float)(now_ms % 3200U) / 3200.0f) * 6.2831853f;
            float wave = 0.5f - 0.5f * cosf(phase_rad); // 0.0 -> 1.0 -> 0.0
            uint8_t sc = (uint8_t)(38.0f + wave * 217.0f);
            WledRgb out = scaleRgb(scaleRgb(base, sc), brightness);
            for (int i = rear_count; i < WLED_LED_COUNT; i++) {
                _leds[i] = out;
            }
            break;
        }

        case EFFECT_RAINBOW: {
            uint8_t base_hue = (uint8_t)((now_ms / 18U) & 0xFFU);
            for (int k = 0; k < seg_len; k++) {
                uint8_t hue = (uint8_t)(base_hue + (k * 256) / seg_len);
                _leds[rear_count + k] = scaleRgb(colorWheel(hue), brightness);
            }
            break;
        }

        case EFFECT_SCANNER: {
            // Larson / Knight-Rider ping-pong comet with soft tail
            int span = (seg_len > 1) ? (seg_len - 1) : 1;
            uint32_t period_steps = (uint32_t)span * 2U;
            uint32_t step = (now_ms / 30U) % period_steps;
            int head = (step <= (uint32_t)span) ? (int)step : (int)(period_steps - step);
            for (int k = 0; k < seg_len; k++) {
                int dist = (k >= head) ? (k - head) : (head - k);
                uint8_t sc = 0;
                if (dist == 0) sc = 255;
                else if (dist == 1) sc = 170;
                else if (dist == 2) sc = 96;
                else if (dist == 3) sc = 48;
                else if (dist == 4) sc = 20;
                else sc = 8; // Very dim background floor
                _leds[rear_count + k] = scaleRgb(scaleRgb(base, sc), brightness);
            }
            break;
        }

        case EFFECT_SPEED_GLOW:
        default: {
            float spd = telemetry.speed_kmh;
            if (spd < 0.0f) spd = 0.0f;
            float spd_norm = spd / 60.0f;
            if (spd_norm > 1.0f) spd_norm = 1.0f;

            uint32_t phase_step = (dt_ms * (uint32_t)(18.0f + spd_norm * 90.0f)) / 10U;
            _anim_phase += phase_step;

            float base_gain = 0.35f + 0.55f * spd_norm;
            float runner_amp = 0.10f + 0.35f * spd_norm;
            float phase_offset = (float)(_anim_phase % 10000U) * 0.006283185f;

            for (int k = 0; k < seg_len; k++) {
                float led_angle = ((float)k / (float)seg_len) * 12.5663706f - phase_offset;
                float wave = 0.5f + 0.5f * sinf(led_angle);
                float gain = base_gain + runner_amp * (wave - 0.3f);
                if (gain < 0.15f) gain = 0.15f;
                if (gain > 1.0f)  gain = 1.0f;
                uint8_t sc = (uint8_t)(gain * 255.0f + 0.5f);
                _leds[rear_count + k] = scaleRgb(scaleRgb(base, sc), brightness);
            }
            break;
        }
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
