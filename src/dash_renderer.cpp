#include "dash_renderer.h"
#include "buttons.h"
#include "vesc_uart.h"
#include "fault_manager.h"
#include "mode_manager.h"
#include <math.h>
#include <lgfx/Fonts/GFXFF/FreeSansBold24pt7b.h>
#include <lgfx/Fonts/GFXFF/FreeSansBold18pt7b.h>
#include <lgfx/Fonts/GFXFF/FreeSansBold12pt7b.h>

DashboardRenderer Renderer;

// Modern High-Contrast Color Palette (Exact Match to Video 800x480)
static const uint16_t COLOR_BG            = 0x0000; // Pure Obsidian Black (as in video)
static const uint16_t COLOR_SURFACE       = 0x18E4; // Dark Card Surface Slate Charcoal
static const uint16_t COLOR_SURFACE_LIGHT = 0x2124; // Track background
static const uint16_t COLOR_BORDER        = 0x2965; // Crisp Border
static const uint16_t COLOR_WHITE         = 0xFFFF; // Pure White
static const uint16_t COLOR_LIGHT_GRAY    = 0xDEFB; // Crisp Bright Silver
static const uint16_t COLOR_MUTED_GRAY    = 0x9CD3; // Medium Silver-Gray
static const uint16_t COLOR_AMBER         = 0xFDC0; // High Power / Warning / Hazard Yellow
static const uint16_t COLOR_RED           = 0xF800; // Redline / Field Weakening / Alarm
static const uint16_t COLOR_GREEN         = 0x07E0; // Connected / Safe / Efficient / Eco
static const uint16_t COLOR_CYAN          = 0x07FF; // Electric Accent / Profile / Sport
static const uint16_t COLOR_RACE          = 0xFA08; // Hot Pink / Race Red (from video frame 00:05)
static const uint16_t COLOR_TRACK_TEAL    = 0x3BF0; // Muted Gauge Track Teal (RGB 59, 126, 128)
static const uint16_t COLOR_NEEDLE        = 0xFC00; // High-Visibility Sport Orange-Red

// Dynamic Color Helpers
static inline uint16_t getPowerColor(float watts) {
    if (watts >= 1500.0f) return COLOR_RED;
    if (watts >= 750.0f)  return COLOR_AMBER;
    return COLOR_CYAN;
}

static inline uint16_t getPhaseColor(float amps) {
    if (amps >= 55.0f) return COLOR_RED;
    if (amps >= 30.0f) return COLOR_AMBER;
    return COLOR_WHITE;
}

static inline uint16_t getBatteryColor(float pct) {
    if (pct < 20.0f) return COLOR_RED;
    if (pct < 40.0f) return COLOR_AMBER;
    return COLOR_GREEN;
}

static inline uint16_t getDutyColor(float duty) {
    if (duty >= 100.0f) return COLOR_RED;
    if (duty >= 75.0f)  return COLOR_AMBER;
    return COLOR_CYAN;
}

static inline uint16_t getTempColor(float temp) {
    if (temp >= 80.0f) return COLOR_RED;
    if (temp >= 60.0f) return COLOR_AMBER;
    return COLOR_WHITE;
}

static inline uint16_t getSpeedColor(float spd) {
    if (spd >= 45.0f) return COLOR_AMBER;
    if (spd >= 25.0f) return COLOR_CYAN;
    return COLOR_WHITE;
}

static inline uint16_t hueToRgb565(uint8_t hue) {
    uint8_t r, g, b;
    uint8_t region = hue / 43;
    uint8_t remainder = (hue - (region * 43)) * 6;

    uint8_t p = 0;
    uint8_t q = 255 - remainder;
    uint8_t t = remainder;

    switch (region) {
        case 0:  r = 255; g = t;   b = p;   break;
        case 1:  r = q;   g = 255; b = p;   break;
        case 2:  r = p;   g = 255; b = t;   break;
        case 3:  r = p;   g = q;   b = 255; break;
        case 4:  r = t;   g = p;   b = 255; break;
        default: r = 255; g = p;   b = q;   break;
    }
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

DashboardRenderer::DashboardRenderer()
    : _display(nullptr),
      _canvas(nullptr),
      _active_screen(SCREEN_RIDE_DASH),
      _active_style(STYLE_HYPER_35KW),
      _screen_dirty(true),
      _mode_banner_active(false),
      _settings_tab_idx(255),
      _last_rendered_tab(255),
      _settings_inited(false),
      _menu_dirty(true),
      _is_sweeping(false),
      _sweep_phase(0),
      _sweep_progress(0.0f),
      _sweep_duty(0.0f),
      _sweep_speed(0.0f),
      _last_sweep_tick_ms(0),
      _anim_speed(0.0f),
      _anim_power(0.0f),
      _anim_duty(0.0f),
      _anim_last_ms(0),
      _anim_inited(false),
      _hyper_inited(false),
      _stats_inited(false),
      _last_arc_frac(-1.0f),
      _last_arc_angle(135.0f),
      _last_arc_col(0),
      _pin_screen_drawn(false),
      _last_pin_len(-1),
      _last_pin_err(false),
      _last_theft_alarm(false),
      _charging_inited(false),
      _last_charge_pct(-1.0f),
      _last_charge_angle(135.0f),
      _last_charge_min(-1),
      _last_charge_w(-1),
      _last_charge_v_x10(-1),
      _pin_modal_open(false),
      _pin_modal_success(false),
      _pin_modal_success_ms(0),
      _wizard_step(0),
      _wizard_inited(false) {
    _wizard_pin_buf[0] = '\0';
    _pin_modal_buf[0] = '\0';
    _last_needle.valid = false;
    _cache.invalidate();
    _stats_cache.invalidate();
}

void DashboardRenderer::begin(DisplayDriver *display) {
    _display = display;
    _canvas = display->getCanvas();
    _cache.invalidate();
    _screen_dirty = true;
    _active_style = (DashboardStyle)Settings.get().dash_style;
}

void DashboardRenderer::drawCard(int x, int y, int w, int h, uint16_t bg, uint16_t border) {
    _canvas->fillRoundRect(x, y, w, h, 8, bg);
    _canvas->drawRoundRect(x, y, w, h, 8, border);
}

// Zero-flicker native text padding renderer with snug unit placement
void DashboardRenderer::drawValWithUnit(int x, int y, int numPadW, int unitPadW, const char *numStr, const char *unitStr, uint16_t numCol, uint16_t unitCol, uint16_t bgCol) {
    int nw = _canvas->textWidth(numStr, &fonts::Font6);
    int totalBoxW = numPadW + unitPadW;

    _canvas->setTextColor(numCol, bgCol);
    _canvas->setTextPadding(totalBoxW);
    _canvas->drawString(numStr, x, y, &fonts::Font6);

    _canvas->setTextColor(unitCol, bgCol);
    _canvas->setTextPadding(0);
    _canvas->drawString(unitStr, x + nw + 6, y + 14, &fonts::Font4);
}

// 32x32 Anti-aliased Satellite Icon (exact match to custom hardware icon):
// White body & solar panels with cyan radiating transmission waves.
static const uint16_t SATELLITE_ICON_32x32[32 * 32] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xAD55, 0x528A, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2945, 0xEF7D, 0x7BEF, 0xDEFB, 0x73AE, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2945, 0xFFFF, 0x39E7, 0x0000, 0x0000, 0xE75C, 0x73AE, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2965, 0xFFFF, 0x39C7, 0x0000, 0x0000, 0x0000, 0x0000, 0xE73C, 0x7BCF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xDF1B, 0x39C7, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xE73C, 0x73AE, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xE73C, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xE73C, 0x73AE, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x8C71, 0xCE79, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xE71C, 0x4A29, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CF3, 0xCE79, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9492, 0x8C71, 0x0000, 0xA514, 0xEF5D, 0xA534, 0x0000, 0x8C71, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CD3, 0xCE79, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x526A, 0xEF5D, 0x0000, 0xAD75, 0xBDD7, 0x0000, 0x9CD3, 0xFFFF, 0xA514, 0xE71C, 0x2945, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CF3, 0xCE59, 0x0000, 0x0000, 0x0000, 0x52AA, 0xFFFF, 0x0000, 0xA534, 0xBDD7, 0x0000, 0x0000, 0x0000, 0xA514, 0x9CD3, 0x528A, 0xDEFB, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CF3, 0xCE79, 0x0000, 0x5AEB, 0xE73C, 0x9CD3, 0xF7BE, 0xAD55, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CD3, 0xFFFF, 0x6B6D, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x8C51, 0xE73C, 0xD6BA, 0x0000, 0xA534, 0xBDF7, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xA534, 0xB596, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xAD75, 0xBDD7, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xCE59, 0x5AEB, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xB5B6, 0xB5B6, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xEF5D, 0x3166, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9492, 0xB5B6, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFDF, 0x4A49, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xCE79, 0x3186, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFDF, 0x41E8, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x41E8, 0x8410, 0x9CD3, 0x9492, 0x5AEB, 0x2945, 0xC618, 0xFFFF, 0x2104, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x39C7, 0x526A, 0xEF5D, 0xE73C, 0x738E, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x630C, 0xDEFB, 0xC638, 0x8C51, 0x632C, 0x738E, 0xA534, 0xF7BE, 0xEF7D, 0x528A, 0xEF7D, 0x2104, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xE73C, 0xC618, 0xBE17, 0xDEFB, 0x0000, 0x0000, 0xE75C, 0x73AE, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x8C51, 0xCE79, 0x2965, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x7BCF, 0xDEDB, 0x7BCF, 0xEF7D, 0x2124, 0x0000, 0x0000, 0x0000, 0xFFDF, 0x39A7, 0x5ACB, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0xE73C, 0x73AE, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x061A, 0x024A, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x01E8, 0x067C, 0x5AEB, 0xDEFB, 0x0000, 0x0000, 0xFFDF, 0x4A29, 0x39E7, 0xEF7D, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xE75C, 0x73AE, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x06FE, 0x024A, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x02AC, 0xC618, 0xCE79, 0xF7BE, 0xE73C, 0x528A, 0x0000, 0xDEFB, 0x2965, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xEF5D, 0x73AE, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x06FE, 0x024A, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xE71C, 0xA514, 0x0000, 0x0000, 0x0000, 0x0000, 0x73AE, 0xCE79, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xE73C, 0x6B4D,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x069D, 0x0209, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x630C, 0x94B2, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CF3, 0xCE79, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xF7DE,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0412, 0x073F, 0x01C8, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2124, 0xCE59, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CF3, 0xCE59, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CD3, 0xAD95,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x05FA, 0x0125, 0x06DE, 0x022A, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xCE59, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CF3, 0xCE79, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CF3, 0xCE59, 0x0000,
    0x0187, 0x0000, 0x0000, 0x026B, 0x0000, 0x067C, 0x0000, 0x0000, 0x071E, 0x024A, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xCE59, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CF3, 0xCE79, 0x0000, 0x0000, 0x9CF3, 0xCE79, 0x0000, 0x0000,
    0x0494, 0x03D0, 0x0000, 0x06DD, 0x0000, 0x01C7, 0x06FE, 0x04F5, 0x061A, 0x073F, 0x0229, 0x0000, 0x0000, 0x0000, 0x5ACB, 0xAD75, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x9CD3, 0xCE99, 0xB5B6, 0xC658, 0x0000, 0x0000, 0x0000,
    0x0145, 0x063B, 0x0000, 0x024A, 0x06BD, 0x0000, 0x0000, 0x028B, 0x0000, 0x0000, 0x06FE, 0x0229, 0x0000, 0x0000, 0xBDF7, 0x52AA, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x52AA, 0x6B6D, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0536, 0x03B0, 0x0000, 0x02EC, 0x071E, 0x038F, 0x0000, 0x0000, 0x0000, 0x0000, 0x071F, 0x01C8, 0x0166, 0xE71C, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x06DD, 0x034E, 0x0000, 0x0000, 0x0166, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x065B, 0x06FE, 0x2965, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x05F9, 0x0598, 0x01E8, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x02AB, 0x05F9, 0x0453, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};

static void drawMiniSatellite(LovyanGFX *canvas, int cx, int cy) {
    int x0 = cx - 16;
    int y0 = cy - 16;
    canvas->startWrite();
    for (int y = 0; y < 32; ++y) {
        int py = y0 + y;
        for (int x = 0; x < 32; ++x) {
            uint16_t col = SATELLITE_ICON_32x32[y * 32 + x];
            if (col != 0x0000) {
                canvas->writePixel(x0 + x, py, col);
            }
        }
    }
    canvas->endWrite();
}

void DashboardRenderer::triggerNeedleSweep() {
    _is_sweeping = true;
    _sweep_phase = 0;
    _sweep_progress = 0.0f;
    _sweep_duty = 0.0f;
    _sweep_speed = 0.0f;
    _last_sweep_tick_ms = millis();
    _screen_dirty = true;
    _cache.invalidate();
}

// Top-Level Screen Dispatcher
void DashboardRenderer::render(const DashTelemetry &telemetry) {
    // 0. First-Time Setup Wizard takes precedence on first boot (bypasses PIN lockscreen)
    if (!telemetry.setup_completed && telemetry.screen == SCREEN_SETUP_WIZARD) {
        // Fall through to screen dispatch
    } else if (telemetry.is_locked || ModeManager::instance().isLocked()) {
        renderPinLockScreen(telemetry);
        return;
    } else {
        _pin_screen_drawn = false;
    }

    // 1. Startup Speedometer Sweep Animation (0 -> 100 km/h -> 0)
    if (_is_sweeping) {
        uint32_t now = millis();
        float dt = (now - _last_sweep_tick_ms) / 1000.0f;
        _last_sweep_tick_ms = now;

        if (_sweep_phase == 0) { // Sweep up: 0 -> 100 km/h in 0.65s
            _sweep_progress += dt / 0.65f;
            if (_sweep_progress >= 1.0f) {
                _sweep_progress = 1.0f;
                _sweep_phase = 1;
            }
            float ease = _sweep_progress * _sweep_progress * (3.0f - 2.0f * _sweep_progress);
            _sweep_duty = ease * 120.0f;
            _sweep_speed = ease * 100.0f;
        } else if (_sweep_phase == 1) { // Sweep down: 100 -> 0 km/h in 0.55s
            _sweep_progress -= dt / 0.55f;
            if (_sweep_progress <= 0.0f) {
                _sweep_progress = 0.0f;
                _sweep_duty = 0.0f;
                _sweep_speed = 0.0f;
                _is_sweeping = false;
                _screen_dirty = true;
                _cache.invalidate();
            } else {
                float ease = _sweep_progress * _sweep_progress * (3.0f - 2.0f * _sweep_progress);
                _sweep_duty = ease * 120.0f;
                _sweep_speed = ease * 100.0f;
            }
        }
    }

    // 2. Detect Screen Switch
    if (telemetry.screen != _active_screen) {
        _active_screen = telemetry.screen;
        _screen_dirty = true;
        _cache.invalidate();
        _last_needle.valid = false;
    }

    // 3. Detect Style Switch in Ride Dashboard
    if (_active_screen == SCREEN_RIDE_DASH) {
        DashboardStyle st = (DashboardStyle)Settings.get().dash_style;
        if (st != _active_style) {
            _active_style = st;
            _screen_dirty = true;
            _cache.invalidate();
            _last_needle.valid = false;
        }
    }

    // 4. Dispatch Screen Rendering
    switch (_active_screen) {
        case SCREEN_RIDE_DASH:
            if (_screen_dirty) {
                _canvas->fillScreen(COLOR_BG);
                initRideDashboard(telemetry);
                _screen_dirty = false;
            }
            renderRideDashboard(telemetry);
            break;

        case SCREEN_LIGHTS:
            // Deprecated screen: redirect directly to main Ride Dash
            _active_screen = SCREEN_RIDE_DASH;
            _screen_dirty = true;
            initRideDashboard(telemetry);
            renderRideDashboard(telemetry);
            break;

        case SCREEN_RT_GRAPHS:
            if (_screen_dirty) {
                _canvas->fillScreen(COLOR_BG);
                initRtGraphsScreen(telemetry);
                _screen_dirty = false;
            }
            renderRtGraphsScreen(telemetry);
            break;

        case SCREEN_SETTINGS_MENU:
            if (_screen_dirty) {
                initSettingsScreen(telemetry);
                _screen_dirty = false;
            }
            renderSettingsScreen(telemetry);
            break;

        case SCREEN_VESC_BRIDGE:
            if (_screen_dirty) {
                _canvas->fillScreen(COLOR_BG);
                initVescBridgeScreen(telemetry);
                _screen_dirty = false;
            }
            renderVescBridgeScreen(telemetry);
            break;

        case SCREEN_WIFI_BRIDGE:
            if (_screen_dirty) {
                _canvas->fillScreen(COLOR_BG);
                initWifiBridgeScreen(telemetry);
                _screen_dirty = false;
            }
            renderWifiBridgeScreen(telemetry);
            break;

        case SCREEN_RACE_TIMER:
            if (_screen_dirty) {
                _canvas->fillScreen(COLOR_BG);
                initRaceTimerScreen(telemetry);
                _screen_dirty = false;
            }
            renderRaceTimerScreen(telemetry);
            break;

        case SCREEN_SETUP_WIZARD:
            if (_screen_dirty) {
                _canvas->fillScreen(COLOR_BG);
                initSetupWizard(telemetry);
                _screen_dirty = false;
            }
            renderSetupWizard(telemetry);
            break;

        case SCREEN_CHARGING_HUD:
            if (_screen_dirty) {
                _canvas->fillScreen(COLOR_BG);
                initChargingScreen();
                _screen_dirty = false;
            }
            renderChargingScreen(telemetry);
            break;

        default:
            break;
    }

    // Requirement 2: Visual Indicator (Small subtle red dot in top right corner)
    // When tuningMode == false (Stealth / Drossel Mode): permanently lit small red dot
    // When tuningMode == true (Tuning Mode): completely vanishes
    if (telemetry.screen != SCREEN_SETUP_WIZARD) {
        if (!telemetry.tuning_mode) {
            _canvas->fillCircle(790, 10, 4, COLOR_RED);
        } else {
            _canvas->fillCircle(790, 10, 5, COLOR_BG);
        }
    }
}

// ==============================================================================
// SCREEN 0: MAIN RIDE DASHBOARD
// ==============================================================================
void DashboardRenderer::initRideDashboard(const DashTelemetry &telemetry) {
    initHyper35kwStyle(telemetry);
}

void DashboardRenderer::renderRideDashboard(const DashTelemetry &telemetry) {
    renderHyper35kwStyle(telemetry);
    if (telemetry.fault_code != 0) {
        drawFaultBanner(telemetry.fault_code);
    }
}

// ==============================================================================
// STYLE 0: "BEST VESC SCREEN EVER MADE" HYPER-VESC 35kW+ PRO CLUSTER (800x480)
// ==============================================================================
static void drawHeroDigitText(LovyanGFX *canvas, const char *str, int x, int y, textdatum_t datum, uint16_t color, uint16_t bg_color) {
    canvas->setFont(&fonts::DejaVu72);
    canvas->setTextSize(1);
    canvas->setTextDatum(datum);
    canvas->setTextColor(color, bg_color);
    // Draw solid high-DPI automotive weight with 1:1 single-pixel precision (no 2x block doubling)
    canvas->drawString(str, x, y);
    canvas->drawString(str, x + 1, y);
    canvas->drawString(str, x + 2, y);
    canvas->drawString(str, x, y + 1);
    canvas->drawString(str, x + 1, y + 1);
}

[[maybe_unused]] static void drawLightSwitchBadge(LovyanGFX *canvas, bool enabled, int cx, int cy, int r) {
    canvas->fillCircle(cx, cy, r, COLOR_BG);
    uint16_t ringCol = enabled ? COLOR_CYAN : COLOR_BORDER;
    uint16_t bgCol   = enabled ? 0x0A4D : COLOR_SURFACE;
    uint16_t textCol = enabled ? COLOR_WHITE : COLOR_MUTED_GRAY;
    uint16_t subCol  = enabled ? COLOR_CYAN : COLOR_MUTED_GRAY;

    canvas->fillCircle(cx, cy, r - 2, bgCol);
    canvas->drawCircle(cx, cy, r, ringCol);
    canvas->drawCircle(cx, cy, r - 1, ringCol);

    canvas->setTextDatum(textdatum_t::middle_center);
    canvas->setFont(&fonts::Font0);
    canvas->setTextSize(1);
    canvas->setTextColor(subCol, bgCol);
    canvas->drawString("LICHT", cx, cy - 7);

    canvas->setFont(&fonts::Font2);
    canvas->setTextSize(1);
    canvas->setTextColor(textCol, bgCol);
    canvas->drawString(enabled ? "ON" : "OFF", cx, cy + 7);
}

static void drawGForceBadge(LovyanGFX *canvas, float g_force, int cx, int cy, int r) {
    canvas->fillCircle(cx, cy, r, COLOR_BG);
    uint16_t ringCol = COLOR_BORDER;
    uint16_t textCol = COLOR_WHITE;
    if (g_force > 0.15f) {
        ringCol = COLOR_GREEN;
        textCol = COLOR_GREEN;
    } else if (g_force < -0.15f) {
        ringCol = COLOR_RED;
        textCol = COLOR_RED;
    }
    canvas->fillCircle(cx, cy, r - 2, COLOR_SURFACE);
    canvas->drawCircle(cx, cy, r, ringCol);
    canvas->drawCircle(cx, cy, r - 1, ringCol);

    canvas->setFont(&fonts::Font2);
    canvas->setTextSize(1);
    canvas->setTextDatum(textdatum_t::middle_center);
    canvas->setTextColor(textCol, COLOR_SURFACE);
    char gBuf[12];
    snprintf(gBuf, sizeof(gBuf), "%+.1f", g_force);
    canvas->drawString(gBuf, cx, cy - 6);

    canvas->setFont(&fonts::Font0);
    canvas->setTextSize(1);
    canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    canvas->drawString("G-FORCE", cx, cy + 9);
}

static void drawInclineBadge(LovyanGFX *canvas, float incline_deg, int cx, int cy, int r) {
    canvas->fillCircle(cx, cy, r, COLOR_BG);
    uint16_t ringCol = COLOR_BORDER;
    if (incline_deg > 15.0f) {
        ringCol = COLOR_CYAN;
    } else if (incline_deg < -15.0f) {
        ringCol = COLOR_AMBER;
    }
    canvas->fillCircle(cx, cy, r - 2, COLOR_SURFACE);
    canvas->drawCircle(cx, cy, r, ringCol);
    canvas->drawCircle(cx, cy, r - 1, ringCol);

    canvas->setFont(&fonts::Font2);
    canvas->setTextSize(1);
    int inc = (int)roundf(incline_deg);
    char valBuf[12];
    if (inc > 0) {
        snprintf(valBuf, sizeof(valBuf), "+%d", inc);
    } else if (inc < 0) {
        snprintf(valBuf, sizeof(valBuf), "%d", inc);
    } else {
        snprintf(valBuf, sizeof(valBuf), "0");
    }

    int valW = canvas->textWidth(valBuf);
    int totalW = valW + 6;
    int startX = cx - (totalW / 2);
    canvas->setTextDatum(textdatum_t::middle_left);
    canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    canvas->drawString(valBuf, startX, cy - 6);
    canvas->drawCircle(startX + valW + 3, cy - 9, 2, COLOR_WHITE);

    canvas->setFont(&fonts::Font0);
    canvas->setTextSize(1);
    canvas->setTextDatum(textdatum_t::middle_center);
    canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    canvas->drawString("NEIGUNG", cx, cy + 9);
}

static void drawThermalsWithDegrees(LovyanGFX *canvas, const char *prefix, float fet, float mot, int x, int y, bool centered = false) {
    canvas->setFont(&fonts::Font2);
    canvas->setTextSize(1);
    canvas->setTextDatum(textdatum_t::top_left);
    canvas->setTextColor(COLOR_WHITE, COLOR_BG);

    char buf[16];
    if (prefix && prefix[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s  FET", prefix);
    } else {
        snprintf(buf, sizeof(buf), "FET");
    }

    char fetVal[8];
    snprintf(fetVal, sizeof(fetVal), "%.0f", fet);

    char motVal[8];
    snprintf(motVal, sizeof(motVal), "%.0f", mot);

    int bufW = canvas->textWidth(buf);
    int fetW = canvas->textWidth(fetVal);
    int motLabelW = canvas->textWidth("MOT");
    int motW = canvas->textWidth(motVal);

    int totalW = bufW + 6 + fetW + 2 + 16 + motLabelW + 6 + motW + 2 + 4;
    int curX = centered ? (x - (totalW / 2)) : x;

    canvas->drawString(buf, curX, y);
    curX += bufW + 6;

    canvas->drawString(fetVal, curX, y);
    curX += fetW + 2;
    canvas->drawCircle(curX + 2, y + 3, 2, COLOR_WHITE);
    curX += 16;

    canvas->drawString("MOT", curX, y);
    curX += motLabelW + 6;

    canvas->drawString(motVal, curX, y);
    curX += motW + 2;
    canvas->drawCircle(curX + 2, y + 3, 2, COLOR_WHITE);
}

void DashboardRenderer::initHyper35kwStyle(const DashTelemetry &telemetry) {
    _canvas->fillScreen(COLOR_BG);
    _mode_banner_active = (millis() < telemetry.mode_badge_until);
    _hyper_inited = false;
    _cache.invalidate();
    _last_arc_frac = -1.0f;
    _last_arc_col = 0;

    int cx = 240, cy = 215;
    int r_in = 135, r_out = 162;
    float mid_r = 148.5f;
    float deg2rad = 3.14159265f / 180.0f;

    // 0. Top-Left [ STATS ] Navigation Touch Button
    if (telemetry.fault_code == 0) {
        int sX = 24, sY = 18, sW = 92, sH = 36;
        _canvas->fillRoundRect(sX, sY, sW, sH, 10, COLOR_BG);
        _canvas->drawRoundRect(sX, sY, sW, sH, 10, COLOR_WHITE);
        _canvas->drawRoundRect(sX + 1, sY + 1, sW - 2, sH - 2, 9, 0x2965);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->drawString("STATS", sX + sW / 2, sY + sH / 2);
    }

    // 1. Speedometer Gauge Background Arc (270 degree sweep, open at bottom: 135° to 405°)
    _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, 405.0f, COLOR_TRACK_TEAL);
    int s_cap_x = cx + (int)roundf(cosf(135.0f * deg2rad) * mid_r);
    int s_cap_y = cy + (int)roundf(sinf(135.0f * deg2rad) * mid_r);
    int e_cap_x = cx + (int)roundf(cosf(405.0f * deg2rad) * mid_r);
    int e_cap_y = cy + (int)roundf(sinf(405.0f * deg2rad) * mid_r);
    _canvas->fillCircle(s_cap_x, s_cap_y, 13, COLOR_TRACK_TEAL);
    _canvas->fillCircle(e_cap_x, e_cap_y, 13, COLOR_TRACK_TEAL);

    // Static label "km/h" under hero speed
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextSize(1);
    _canvas->setTextDatum(textdatum_t::top_center);
    _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
    _canvas->drawString("km/h", cx, 245);

    // Static label "W" next to hero power
    if (telemetry.fault_code == 0) {
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->drawString("W", 730, 86);
    }

    // Static label "DUTY"
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextSize(1);
    _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->drawString("DUTY", 445, 124);

    // Static duty track bar
    _canvas->fillRoundRect(445, 150, 320, 10, 5, COLOR_TRACK_TEAL);

    // Static battery track bar
    _canvas->fillRoundRect(445, 218, 320, 14, 7, COLOR_TRACK_TEAL);

    // Static label "RESTREICHWEITE" & "TRIP"
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
    _canvas->drawString("RESTREICHWEITE", 445, 298);
    _canvas->drawString("TRIP", 615, 298);
}

void DashboardRenderer::renderHyper35kwStyle(const DashTelemetry &telemetry) {
    bool banner_now = (millis() < telemetry.mode_badge_until);
    if (banner_now != _mode_banner_active) {
        _mode_banner_active = banner_now;
        _canvas->fillRect(410, 170, 370, 110, COLOR_BG);
        _cache.battery_pct = -1;
        _cache.voltage_x10 = -1;
        _cache.phase_x10 = -1;
        _cache.current_x10 = -1;
    }

    uint32_t now = millis();
    if (!_anim_inited || _screen_dirty) {
        _anim_speed = telemetry.speed_kmh;
        _anim_power = telemetry.power_watts;
        _anim_duty  = telemetry.duty_cycle_pct;
        _anim_last_ms = now;
        _anim_inited = true;
    } else {
        float dt = (now - _anim_last_ms) / 1000.0f;
        _anim_last_ms = now;
        if (dt < 0.001f) dt = 0.016f;
        if (dt > 0.1f) dt = 0.1f;
        float k = 1.0f - expf(-18.0f * dt);
        float k_pwr = 1.0f - expf(-12.0f * dt);
        _anim_speed += (telemetry.speed_kmh - _anim_speed) * k;
        _anim_power += (telemetry.power_watts - _anim_power) * k_pwr;
        _anim_duty  += (telemetry.duty_cycle_pct - _anim_duty) * k;
    }
    if (telemetry.speed_kmh == 0.0f) {
        _anim_speed = 0.0f;
        if (telemetry.power_watts == 0.0f) _anim_power = 0.0f;
    }
    if (fabsf(_anim_power) < 8.0f && telemetry.speed_kmh < 0.5f) _anim_power = 0.0f;
    if (telemetry.duty_cycle_pct == 0.0f) _anim_duty = 0.0f;

    int cx = 240, cy = 215;
    int r_in = 135, r_out = 162;
    float mid_r = 148.5f;
    float deg2rad = 3.14159265f / 180.0f;
    int s_cap_x = cx + (int)roundf(cosf(135.0f * deg2rad) * mid_r);
    int s_cap_y = cy + (int)roundf(sinf(135.0f * deg2rad) * mid_r);
    int e_cap_x = cx + (int)roundf(cosf(405.0f * deg2rad) * mid_r);
    int e_cap_y = cy + (int)roundf(sinf(405.0f * deg2rad) * mid_r);

    float spd = _is_sweeping ? _sweep_speed : telemetry.speed_kmh;
    if (spd < 0.8f) spd = 0.0f; // Deadband to filter sensor noise at standstill
    float spd_anim = (_is_sweeping) ? _sweep_speed : ((_anim_speed > 0.0f) ? _anim_speed : spd);
    if (spd_anim < 0.8f) spd_anim = 0.0f;
    float frac = spd_anim / 100.0f;
    if (frac > 1.0f) frac = 1.0f;

    uint16_t arcCol = COLOR_WHITE;
    if (telemetry.boost_active) {
        arcCol = COLOR_CYAN;
    } else if (spd >= 85.0f) {
        arcCol = COLOR_RED;
    } else if (spd >= 60.0f) {
        arcCol = COLOR_AMBER;
    }

    float target_angle = 135.0f + (frac * 270.0f);
    if (frac <= 0.005f) target_angle = 135.0f;

    // 1. Speedometer Active Arc: Incremental differential update (Zero flicker / no tripping!)
    if (!_hyper_inited) {
        _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, 405.0f, COLOR_TRACK_TEAL);
        _canvas->fillCircle(s_cap_x, s_cap_y, 13, COLOR_TRACK_TEAL);
        _canvas->fillCircle(e_cap_x, e_cap_y, 13, COLOR_TRACK_TEAL);
        if (target_angle > 135.0f) {
            _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, target_angle, arcCol);
            _canvas->fillCircle(s_cap_x, s_cap_y, 13, arcCol);
            int tip_x = cx + (int)roundf(cosf(target_angle * deg2rad) * mid_r);
            int tip_y = cy + (int)roundf(sinf(target_angle * deg2rad) * mid_r);
            _canvas->fillCircle(tip_x, tip_y, 13, arcCol);
            _canvas->fillCircle(tip_x, tip_y, 5, COLOR_WHITE);
        }
        _last_arc_angle = target_angle;
        _last_arc_col = arcCol;
    } else if (arcCol != _last_arc_col) {
        // Redraw active arc on color change
        if (target_angle < _last_arc_angle) {
            // Speed decreasing across color boundary: erase obsolete arc & old tip
            _canvas->fillArc(cx, cy, r_in, r_out, target_angle, _last_arc_angle + 1.0f, COLOR_TRACK_TEAL);
            int old_tip_x = cx + (int)roundf(cosf(_last_arc_angle * deg2rad) * mid_r);
            int old_tip_y = cy + (int)roundf(sinf(_last_arc_angle * deg2rad) * mid_r);
            _canvas->fillCircle(old_tip_x, old_tip_y, 13, COLOR_TRACK_TEAL);
        }
        if (target_angle > 135.0f) {
            _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, target_angle, arcCol);
            _canvas->fillCircle(s_cap_x, s_cap_y, 13, arcCol);
            int tip_x = cx + (int)roundf(cosf(target_angle * deg2rad) * mid_r);
            int tip_y = cy + (int)roundf(sinf(target_angle * deg2rad) * mid_r);
            _canvas->fillCircle(tip_x, tip_y, 13, arcCol);
            _canvas->fillCircle(tip_x, tip_y, 5, COLOR_WHITE);
        } else {
            _canvas->fillCircle(s_cap_x, s_cap_y, 13, COLOR_TRACK_TEAL);
        }
        _last_arc_angle = target_angle;
        _last_arc_col = arcCol;
    } else if (fabsf(target_angle - _last_arc_angle) >= 0.8f) {
        if (target_angle > _last_arc_angle) {
            // Speed INCREASING: Extend arc forward seamlessly (zero erase!)
            if (_last_arc_angle <= 135.0f) {
                _canvas->fillCircle(s_cap_x, s_cap_y, 13, arcCol);
            }
            _canvas->fillArc(cx, cy, r_in, r_out, _last_arc_angle - 0.5f, target_angle, arcCol);
            int tip_x = cx + (int)roundf(cosf(target_angle * deg2rad) * mid_r);
            int tip_y = cy + (int)roundf(sinf(target_angle * deg2rad) * mid_r);
            _canvas->fillCircle(tip_x, tip_y, 13, arcCol);
            _canvas->fillCircle(tip_x, tip_y, 5, COLOR_WHITE);
        } else {
            // Speed DECREASING: Retract arc cleanly
            _canvas->fillArc(cx, cy, r_in, r_out, target_angle, _last_arc_angle + 0.5f, COLOR_TRACK_TEAL);
            int old_tip_x = cx + (int)roundf(cosf(_last_arc_angle * deg2rad) * mid_r);
            int old_tip_y = cy + (int)roundf(sinf(_last_arc_angle * deg2rad) * mid_r);
            _canvas->fillCircle(old_tip_x, old_tip_y, 13, COLOR_TRACK_TEAL);

            if (target_angle > 135.0f) {
                int tip_x = cx + (int)roundf(cosf(target_angle * deg2rad) * mid_r);
                int tip_y = cy + (int)roundf(sinf(target_angle * deg2rad) * mid_r);
                _canvas->fillCircle(tip_x, tip_y, 13, arcCol);
                _canvas->fillCircle(tip_x, tip_y, 5, COLOR_WHITE);
            } else {
                _canvas->fillCircle(s_cap_x, s_cap_y, 13, COLOR_TRACK_TEAL);
            }
        }
        _last_arc_angle = target_angle;
    }

    // 1b. Top Status Header: Clock in Center (on exact same height as STATS button: y=36)
    bool clock_changed = strcmp(_cache.clock_str, telemetry.clock_str) != 0;
    bool gps_en_changed = (_cache.gps_enabled != telemetry.gps_enabled);

    if (!_hyper_inited || clock_changed || gps_en_changed) {
        strncpy(_cache.clock_str, telemetry.clock_str, sizeof(_cache.clock_str));
        _cache.gps_enabled = telemetry.gps_enabled;

        // Clear center top area (x=340 to 460, y=16 to 56)
        _canvas->fillRect(340, 16, 120, 40, COLOR_BG);

        if (telemetry.gps_enabled) {
            // Render Clock ("14:35") in middle-center on exact same height as STATS button
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextSize(1);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
            _canvas->drawString(telemetry.clock_str[0] ? telemetry.clock_str : "--:--", 400, 36);
        }
    }

    // 2. Rider Tag (if set)
    if (telemetry.rider_tag[0] != '\0' && !_hyper_inited) {
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::top_center);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->drawString(telemetry.rider_tag, cx, 110);
    }

    // 3. Hero Digital Speed Readout (Only redrawn when integer speed, sweep or GPS status changes)
    int cur_spd_int = (int)(spd + 0.5f);
    bool gps_connected = telemetry.gps_enabled && (telemetry.gps.valid && telemetry.gps.satellites >= 4);
    if (!_hyper_inited || cur_spd_int != _cache.speed || _is_sweeping ||
        telemetry.boost_active != _cache.boost_active || gps_connected != _cache.gps_valid ||
        gps_en_changed) {
        _cache.speed = cur_spd_int;
        _cache.gps_valid = gps_connected;
        _canvas->fillRect(cx - 100, 140, 200, 165, COLOR_BG);
        char spdBuf[16];
        snprintf(spdBuf, sizeof(spdBuf), "%d", cur_spd_int);
        uint16_t spdCol = telemetry.boost_active ? COLOR_CYAN : ((spd >= 85.0f) ? COLOR_RED : COLOR_WHITE);
        drawHeroDigitText(_canvas, spdBuf, cx, 195, textdatum_t::middle_center, spdCol, COLOR_BG);

        _canvas->setFont(&fonts::Font4);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::top_center);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->drawString("km/h", cx, 245);

        // Under "km/h": Draw small satellite icon ONLY when GPS is enabled and connected, otherwise nothing!
        if (gps_connected) {
            drawMiniSatellite(_canvas, cx, 285);
        }
    }

    // 4. Controller & Motor Thermals (Centered under Speedometer dial)
    int cur_fet = (int)(telemetry.temp_esc + 0.5f);
    int cur_mot = (int)(telemetry.temp_motor + 0.5f);
    if (!_hyper_inited || cur_fet != _cache.temp_esc || cur_mot != _cache.temp_motor) {
        _cache.temp_esc = cur_fet;
        _cache.temp_motor = cur_mot;
        _canvas->fillRect(150, 395, 180, 45, COLOR_BG);
        drawThermalsWithDegrees(_canvas, "", telemetry.temp_esc, telemetry.temp_motor, cx, 405, true);
    }

    // 5. Right Column: Hero Power Readout (Only redrawn when watts change)
    int cur_watts = (int)roundf(_anim_power);
    if (telemetry.speed_kmh < 0.5f && fabsf(_anim_power) < 8.0f) {
        cur_watts = 0;
    }
    if (!_hyper_inited || cur_watts != _cache.watts || telemetry.boost_active != _cache.boost_active) {
        _cache.watts = cur_watts;
        if (telemetry.fault_code == 0) {
            _canvas->fillRect(460, 42, 310, 78, COLOR_BG);
            char pStr[16];
            snprintf(pStr, sizeof(pStr), "%d", cur_watts);
            uint16_t pwrCol = (cur_watts < -10) ? COLOR_GREEN : (telemetry.boost_active ? COLOR_CYAN : COLOR_WHITE);
            drawHeroDigitText(_canvas, pStr, 720, 50, textdatum_t::top_right, pwrCol, COLOR_BG);

            // Redraw "W" unit label so it never gets clipped or erased
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextSize(1);
            _canvas->setTextDatum(textdatum_t::top_left);
            _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
            _canvas->drawString("W", 730, 86);
        }
    }

    // 6. Right Column: Duty Cycle Section
    float duty = telemetry.duty_cycle_pct;
    if (duty < 0.0f) duty = 0.0f;
    float duty_anim = (_anim_duty > 0.0f) ? _anim_duty : duty;
    if (duty_anim < 0.0f) duty_anim = 0.0f;
    bool is_fw = (duty > 100.0f);
    uint16_t dutyCol = is_fw ? COLOR_RED : COLOR_CYAN;
    int cur_duty = (int)(duty + 0.5f);
    float dFrac = (duty_anim > 100.0f) ? 1.0f : (duty_anim / 100.0f);
    int cur_d_fill = (int)(dFrac * 320.0f);

    if (!_hyper_inited || cur_duty != _cache.duty_x10 || cur_d_fill != _cache.duty_fill_w) {
        _cache.duty_x10 = cur_duty;
        _cache.duty_fill_w = cur_d_fill;
        _canvas->fillRect(680, 122, 90, 26, COLOR_BG);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextSize(1);
        _canvas->setTextColor(dutyCol, COLOR_BG);
        _canvas->setTextDatum(textdatum_t::top_right);
        char dBuf[16];
        snprintf(dBuf, sizeof(dBuf), "%d%%", cur_duty);
        _canvas->drawString(dBuf, 765, 124);

        _canvas->fillRoundRect(445, 150, 320, 10, 5, COLOR_TRACK_TEAL);
        if (cur_d_fill > 0) {
            _canvas->fillRoundRect(445, 150, cur_d_fill, 10, 5, dutyCol);
            if (cur_d_fill >= 6) {
                _canvas->fillRoundRect(445 + cur_d_fill - 4, 150, 4, 10, 2, COLOR_WHITE);
            }
        }
    }

    // 7. Right Column: Dynamic Mode Banner Overlay OR Battery / Currents Readout
    const char *mNames[] = {"ECO", "SPORT", "RACE"};
    uint16_t mCols[] = {COLOR_GREEN, COLOR_CYAN, COLOR_RACE};
    int mIdx = (int)telemetry.ride_mode % 3;
    uint16_t mCol = mCols[mIdx];
    uint16_t bannerCol = mCol;

    if (banner_now) {
        // Redraw banner card while active
        int bX = 410, bY = 170, bW = 365, bH = 108;
        _canvas->fillRect(bX, bY, bW, bH, COLOR_BG);
        int cX = 430, cY = 174, cW = 335, cH = 100;
        _canvas->fillRoundRect(cX, cY, cW, cH, 14, COLOR_BG);
        _canvas->drawRoundRect(cX, cY, cW, cH, 14, bannerCol);
        _canvas->drawRoundRect(cX + 1, cY + 1, cW - 2, cH - 2, 13, bannerCol);

        _canvas->setFont(&fonts::Font2);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
        const char *tagStr = telemetry.tuning_mode ? "RIDE MODE ACTIVATED" : "DRIVE MODE ACTIVE";
        _canvas->drawString(tagStr, cX + cW / 2, cY + 22);

        _canvas->setFont(&fonts::FreeSansBold24pt7b);
        _canvas->setTextSize(1);
        _canvas->setTextColor(bannerCol, COLOR_BG);
        _canvas->drawString(mNames[mIdx], cX + cW / 2, cY + 54);

        int32_t rem_ms = (int32_t)(telemetry.mode_badge_until - millis());
        float anim_p = (rem_ms > 0 && rem_ms <= 2000) ? ((float)rem_ms / 2000.0f) : 1.0f;
        int pBarW = (int)(anim_p * (cW - 40));
        _canvas->fillRoundRect(cX + 20, cY + cH - 12, cW - 40, 3, 1, 0x18C6);
        if (pBarW > 0) {
            _canvas->fillRoundRect(cX + 20, cY + cH - 12, pBarW, 3, bannerCol);
        }
    } else {
        int cur_bat = (int)(telemetry.battery_pct + 0.5f);
        int cur_v10 = (int)(telemetry.voltage * 10.0f + 0.5f);
        int cur_pha = (int)(telemetry.phase_amps + 0.5f);
        int cur_a = (int)(telemetry.current_amps + 0.5f);

        if (!_hyper_inited || cur_bat != _cache.battery_pct || cur_v10 != _cache.voltage_x10) {
            _cache.battery_pct = cur_bat;
            _cache.voltage_x10 = cur_v10;
            _canvas->fillRect(440, 170, 140, 42, COLOR_BG);
            char batBuf[16];
            snprintf(batBuf, sizeof(batBuf), "%d%%", cur_bat);
            _canvas->setFont(&fonts::FreeSansBold18pt7b);
            _canvas->setTextSize(1);
            _canvas->setTextDatum(textdatum_t::top_left);
            _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
            _canvas->drawString(batBuf, 445, 174);

            _canvas->fillRect(650, 170, 125, 42, COLOR_BG);
            char vBuf[16];
            snprintf(vBuf, sizeof(vBuf), "%.1f V", telemetry.voltage);
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextDatum(textdatum_t::top_right);
            _canvas->drawString(vBuf, 765, 180);

            _canvas->fillRoundRect(445, 218, 320, 14, 7, COLOR_TRACK_TEAL);
            int bFillW = (int)((telemetry.battery_pct / 100.0f) * 320.0f);
            if (bFillW > 320) bFillW = 320;
            if (bFillW > 0) {
                uint16_t bCol = (telemetry.battery_pct > 25.0f) ? COLOR_WHITE : ((telemetry.battery_pct > 12.0f) ? COLOR_AMBER : COLOR_RED);
                _canvas->fillRoundRect(445, 218, bFillW, 14, 7, bCol);
                if (bFillW >= 8) {
                    _canvas->fillRoundRect(445 + bFillW - 5, 218, 5, 14, 3, COLOR_WHITE);
                }
            }
        }

        if (!_hyper_inited || cur_pha != _cache.phase_x10 || cur_a != _cache.current_x10) {
            _cache.phase_x10 = cur_pha;
            _cache.current_x10 = cur_a;
            _canvas->fillRect(435, 246, 345, 34, COLOR_BG);
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextSize(1);
            _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
            _canvas->setTextDatum(textdatum_t::top_left);
            char motBuf[32];
            snprintf(motBuf, sizeof(motBuf), "MOTOR %d A", cur_pha);
            _canvas->drawString(motBuf, 445, 250);

            _canvas->setTextDatum(textdatum_t::top_right);
            char bBuf[32];
            snprintf(bBuf, sizeof(bBuf), "BATTERY %d A", cur_a);
            _canvas->drawString(bBuf, 765, 250);
        }
    }

    // 8. Range Readout (Only redrawn when range changes)
    int cur_range = (int)(telemetry.est_range_km + 0.5f);
    if (!_hyper_inited || cur_range != _cache.est_range) {
        _cache.est_range = cur_range;
        _canvas->fillRect(445, 322, 160, 36, COLOR_BG);
        _canvas->setFont(&fonts::FreeSansBold18pt7b);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->setTextDatum(textdatum_t::top_left);
        char rngBuf[16];
        if (telemetry.est_range_km < 0.0f) {
            snprintf(rngBuf, sizeof(rngBuf), "-- km");
        } else {
            snprintf(rngBuf, sizeof(rngBuf), "%d km", cur_range);
        }
        _canvas->drawString(rngBuf, 445, 322);
    }

    // 8b. Trip Distance Readout (Above [ MODE ] button at x=615..765, y=298..360)
    int cur_trip_x10 = (int)roundf(telemetry.trip_km * 10.0f);
    if (!_hyper_inited || cur_trip_x10 != _cache.trip_x10) {
        _cache.trip_x10 = cur_trip_x10;
        _canvas->fillRect(615, 322, 150, 36, COLOR_BG);
        _canvas->setFont(&fonts::FreeSansBold18pt7b);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->setTextDatum(textdatum_t::top_left);
        char tripBuf[16];
        snprintf(tripBuf, sizeof(tripBuf), "%.1f km", telemetry.trip_km);
        _canvas->drawString(tripBuf, 615, 322);
    }

    // 9. Right Column: Bottom Touch Action Buttons (Only redrawn when state changes)
    if (!_hyper_inited || telemetry.boost_active != _cache.boost_active) {
        _cache.boost_active = telemetry.boost_active;
        _canvas->setTextDatum(textdatum_t::top_left);
        if (telemetry.boost_active) {
            _canvas->fillRoundRect(445, 380, 150, 72, 18, COLOR_CYAN);
            _canvas->drawRoundRect(445, 380, 150, 72, 18, COLOR_WHITE);
            _canvas->drawRoundRect(446, 381, 148, 70, 17, COLOR_WHITE);
            _canvas->setTextColor(COLOR_BG, COLOR_CYAN);
        } else {
            _canvas->fillRoundRect(445, 380, 150, 72, 18, COLOR_BG);
            _canvas->drawRoundRect(445, 380, 150, 72, 18, COLOR_WHITE);
            _canvas->drawRoundRect(446, 381, 148, 70, 17, 0x2965);
            _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        }
        _canvas->drawCenterString("BOOST", 520, 404, &fonts::Font4);
    }

    if (!_hyper_inited || (int)telemetry.ride_mode != _cache.ride_mode || banner_now != _cache.tuning_mode) {
        _cache.ride_mode = (int)telemetry.ride_mode;
        _cache.tuning_mode = banner_now;
        const char *btnLabel = mNames[mIdx];
        uint16_t btnCol = mCol;
        if (banner_now) {
            _canvas->fillRoundRect(615, 380, 150, 72, 18, bannerCol);
            _canvas->drawRoundRect(615, 380, 150, 72, 18, COLOR_WHITE);
            _canvas->setTextColor(COLOR_BG, bannerCol);
        } else {
            _canvas->fillRoundRect(615, 380, 150, 72, 18, COLOR_BG);
            _canvas->drawRoundRect(615, 380, 150, 72, 18, btnCol);
            _canvas->drawRoundRect(616, 381, 148, 70, 17, btnCol);
            _canvas->setTextColor(btnCol, COLOR_BG);
        }
        _canvas->drawCenterString(btnLabel, 690, 404, &fonts::Font4);
    }

    // 9. Bottom Left: G-Force Badge and Incline Badge
    bool inc_changed = (_cache.incline_gforce_enabled != telemetry.incline_gforce_enabled);
    if (!_hyper_inited || inc_changed) {
        _cache.incline_gforce_enabled = telemetry.incline_gforce_enabled;
        _canvas->fillRect(15, 380, 135, 75, COLOR_BG); // clear bottom-left badges area
    }

    if (telemetry.incline_gforce_enabled) {
        int cur_g10 = (int)roundf(telemetry.g_force * 10.0f);
        if (!_hyper_inited || inc_changed || cur_g10 != _cache.g_force_x10) {
            _cache.g_force_x10 = cur_g10;
            drawGForceBadge(_canvas, telemetry.g_force, 50, 416, 28);
        }

        int cur_inc_x10 = (int)roundf(telemetry.incline_deg * 10.0f);
        if (!_hyper_inited || inc_changed || cur_inc_x10 != _cache.incline_deg) {
            _cache.incline_deg = cur_inc_x10;
            drawInclineBadge(_canvas, telemetry.incline_deg, 114, 416, 28);
        }
    }

    _hyper_inited = true;
}

// ==============================================================================
// ==============================================================================
// SCREEN 1: CONTROLLABLE LEDS & LIGHT MODULES (800x480 Optimized - Video Exact)
// ==============================================================================
void DashboardRenderer::initLightsScreen(const DashTelemetry &telemetry) {
    _canvas->fillScreen(COLOR_BG);
}

void DashboardRenderer::drawColorWheel(int cx, int cy, int r_in, int r_out, uint8_t selected_hue) {
    for (int deg = 0; deg < 360; deg++) {
        uint8_t h = (uint8_t)(deg * 255 / 360);
        uint16_t col = hueToRgb565(h);
        _canvas->fillArc(cx, cy, r_in, r_out, (float)deg, (float)(deg + 1.2f), col);
    }
}

void DashboardRenderer::renderLightsScreen(const DashTelemetry &telemetry) {
    _canvas->fillScreen(COLOR_BG);

    // 1. Header
    _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
    _canvas->drawString("LIGHTS", 35, 20, &fonts::Font4);

    // Top Right: Master Toggle Pill Switch
    if (telemetry.lights_enabled) {
        _canvas->fillRoundRect(685, 18, 68, 32, 16, COLOR_WHITE);
        _canvas->fillCircle(735, 34, 12, 0x1082);
    } else {
        _canvas->fillRoundRect(685, 18, 68, 32, 16, COLOR_BG);
        _canvas->drawRoundRect(685, 18, 68, 32, 16, COLOR_MUTED_GRAY);
        _canvas->fillCircle(703, 34, 12, COLOR_MUTED_GRAY);
    }

    // 2. Color Wheel: Donut Ring
    int cx = 220, cy = 230;
    int r_in = 88, r_out = 125;
    drawColorWheel(cx, cy, r_in, r_out, telemetry.light_hue);

    // Inner Dark Separator Ring
    _canvas->fillCircle(cx, cy, r_in - 1, COLOR_BG);

    // Inner Solid Circle: Swatch of current color (no text inside)
    uint16_t curCol = telemetry.lights_enabled ? hueToRgb565(telemetry.light_hue) : 0x18C3;
    _canvas->fillCircle(cx, cy, 70, curCol);

    // Ring Selector Handle on the Donut
    float sel_deg = (float)telemetry.light_hue * 360.0f / 255.0f;
    float sel_rad = sel_deg * (3.14159265f / 180.0f);
    float mid_r = (r_in + r_out) / 2.0f;
    int hx = cx + (int)roundf(cosf(sel_rad) * mid_r);
    int hy = cy + (int)roundf(sinf(sel_rad) * mid_r);
    _canvas->fillCircle(hx, hy, 12, COLOR_WHITE);
    _canvas->drawCircle(hx, hy, 12, COLOR_BG);
    _canvas->drawCircle(hx, hy, 13, COLOR_BG);

    // 3. Bottom-Left: Hazard Toggle Switch
    if (telemetry.hazard_active) {
        _canvas->fillRoundRect(90, 405, 65, 30, 15, COLOR_AMBER);
        _canvas->fillCircle(137, 420, 11, COLOR_BG);
    } else {
        _canvas->fillRoundRect(90, 405, 65, 30, 15, COLOR_BG);
        _canvas->drawRoundRect(90, 405, 65, 30, 15, COLOR_MUTED_GRAY);
        _canvas->fillCircle(107, 420, 11, COLOR_MUTED_GRAY);
    }
    _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
    _canvas->drawString("HAZARD", 168, 412, &fonts::Font4);

    // 4. 9 Effect Preset Buttons (3x3 Grid from video frame)
    const char *fxNames[] = {
        "SOLID",   "BREATHE", "WAVE",
        "RAINBOW", "COMET",   "SPARKLE",
        "STROBE",  "POLICE",  "WHITE"
    };

    for (int i = 0; i < 9; i++) {
        int col = i % 3;
        int row = i / 3;
        int bx = 415 + col * 120;
        int by = 65 + row * 56;
        int bw = 105;
        int bh = 48;

        bool active = (i == telemetry.light_mode);
        if (active) {
            _canvas->fillRoundRect(bx, by, bw, bh, 12, COLOR_WHITE);
            _canvas->setTextColor(COLOR_BG, COLOR_WHITE);
        } else {
            _canvas->fillRoundRect(bx, by, bw, bh, 12, 0x1082);
            _canvas->drawRoundRect(bx, by, bw, bh, 12, 0x39C7);
            _canvas->setTextColor(COLOR_WHITE, 0x1082);
        }
        _canvas->drawCenterString(fxNames[i], bx + bw / 2, by + 16, &fonts::Font2);
    }

    // 5. Brightness Slider
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
    _canvas->drawString("BRIGHTNESS", 415, 250, &fonts::Font2);

    _canvas->fillRoundRect(415, 280, 345, 8, 4, 0x2124);
    int bFillW = (int)((telemetry.light_brightness / 255.0f) * 345.0f);
    if (bFillW > 0) {
        _canvas->fillRoundRect(415, 280, bFillW, 8, 4, COLOR_WHITE);
    }
    int thumbX = 415 + bFillW;
    _canvas->fillCircle(thumbX, 284, 11, COLOR_WHITE);
    _canvas->drawCircle(thumbX, 284, 11, 0x1082);

    // 6. Hazard Button
    int hzX = 415, hzY = 330, hzW = 345, hzH = 82;
    uint16_t hzBg = telemetry.hazard_active ? COLOR_AMBER : COLOR_BG;
    uint16_t hzFg = telemetry.hazard_active ? COLOR_BG : COLOR_AMBER;

    _canvas->fillRoundRect(hzX, hzY, hzW, hzH, 18, hzBg);
    _canvas->drawRoundRect(hzX, hzY, hzW, hzH, 18, COLOR_AMBER);
    _canvas->drawRoundRect(hzX + 1, hzY + 1, hzW - 2, hzH - 2, 17, COLOR_AMBER);

    // Warning Triangle Icon + Text
    _canvas->setTextColor(hzFg, hzBg);
    int iconX = hzX + hzW / 2 - 75;
    int iconY = hzY + 41;
    _canvas->fillTriangle(iconX, iconY - 14, iconX - 13, iconY + 9, iconX + 13, iconY + 9, hzFg);
    _canvas->fillCircle(iconX, iconY + 5, 1, hzBg);
    _canvas->drawFastVLine(iconX, iconY - 7, 8, hzBg);
    _canvas->drawCenterString("HAZARD", hzX + hzW / 2 + 15, hzY + 28, &fonts::Font4);
}

// ==============================================================================
// SCREEN 2: REAL-TIME WAVEFORM CURVES & OSCILLOSCOPE (800x480 Optimized - Video Exact)
// ==============================================================================
void DashboardRenderer::initRtGraphsScreen(const DashTelemetry &telemetry) {
    _canvas->fillScreen(COLOR_BG);

    // Top Bar: Back Button [< DASH], [ RACE ] Button, Center Title, Reset Trip Button
    _canvas->fillRoundRect(20, 12, 110, 36, 18, 0x18C3);
    _canvas->drawRoundRect(20, 12, 110, 36, 18, 0x39C7);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font2);
    _canvas->drawString("< DASH", 75, 30);

    // Styled [ RACE ] touch button (only visible when tuning mode is active)
    if (telemetry.tuning_mode) {
        _canvas->fillRoundRect(140, 12, 100, 36, 18, 0x9000);
        _canvas->drawRoundRect(140, 12, 100, 36, 18, COLOR_RED);
        _canvas->setTextColor(COLOR_WHITE, 0x9000);
        _canvas->setFont(&fonts::Font2);
        _canvas->drawString("RACE", 190, 30);
    }

    _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    _canvas->setFont(&fonts::Font4);
    _canvas->drawString("FAHRDATEN & STATISTIK", 430, 30);

    _canvas->fillRoundRect(640, 12, 140, 36, 18, 0x18C3);
    _canvas->drawRoundRect(640, 12, 140, 36, 18, 0x39C7);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->setFont(&fonts::Font2);
    _canvas->drawString("RESET TRIP", 710, 30);

    // Top Summary Card: Trip & Movement Overview
    drawCard(20, 56, 760, 92, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("STRECKE", 36, 66);
    _canvas->drawString("FAHRZEIT", 225, 66);
    _canvas->drawString("MAX SPEED", 415, 66);
    _canvas->drawString("ENERGIE (TRIP)", 605, 66);

    // Middle Performance Tile Grid (6 Large Tiles):
    // Row 1: y = 160, h = 142
    // Tile 1: MAX LEISTUNG (PEAK WATT)
    drawCard(20, 160, 244, 142, COLOR_SURFACE, 0xD400); // Glowing gold border
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
    _canvas->drawString("MAX LEISTUNG (PEAK)", 32, 172);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Spitzenleistung ab Akku", 32, 268);

    // Tile 2: PEAK AKKU-STROM
    drawCard(278, 160, 244, 142, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("PEAK AKKU-STROM", 290, 172);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Maximaler Batterie-Strom", 290, 268);

    // Tile 3: PEAK PHASENSTROM
    drawCard(536, 160, 244, 142, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_RACE, COLOR_SURFACE);
    _canvas->drawString("PEAK PHASENSTROM", 548, 172);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Max. Motor-Drehmoment", 548, 268);

    // Row 2: y = 314, h = 142
    // Tile 4: VERBRAUCH & EFFIZIENZ
    drawCard(20, 314, 244, 142, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("VERBRAUCH & EFFIZIENZ", 32, 326);

    // Tile 5: AKKU-SPANNUNG (MIN/MAX)
    drawCard(278, 314, 244, 142, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("AKKU-SPANNUNG (MIN/MAX)", 290, 326);

    // Tile 6: MAX TEMPERATUR
    drawCard(536, 314, 244, 142, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
    _canvas->drawString("MAX TEMPERATUR", 548, 326);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Motor / Inverter FETs", 548, 422);

    _stats_cache.invalidate();
    _stats_inited = false;
}

void DashboardRenderer::drawWaveformGraph(int x, int y, int w, int h, const float *data, int count, float max_val, uint16_t line_col, uint16_t fill_col, const char *label, const char *val_str, uint16_t val_col) {
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString(label, x, y, &fonts::Font2);
    _canvas->setTextColor(val_col, COLOR_SURFACE);
    _canvas->drawRightString(val_str, x + w, y, &fonts::Font4);

    int plot_x = x + 35;
    int plot_w = w - 35;
    int plot_y = y + h - 10;
    int plot_h = h - 35;

    // Y Axis scale labels
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    if (max_val >= 1000.0f) {
        _canvas->drawString("12k", x, y + 18, &fonts::Font0);
        _canvas->drawString("6k",  x, y + 18 + plot_h / 2, &fonts::Font0);
        _canvas->drawString("0",   x, plot_y - 8, &fonts::Font0);
    } else {
        _canvas->drawString("100", x, y + 18, &fonts::Font0);
        _canvas->drawString("50",  x, y + 18 + plot_h / 2, &fonts::Font0);
        _canvas->drawString("0",   x, plot_y - 8, &fonts::Font0);
    }

    // Grid baseline and midline
    _canvas->drawFastHLine(plot_x, plot_y, plot_w, COLOR_SURFACE_LIGHT);
    _canvas->drawFastHLine(plot_x, y + 18 + plot_h / 2, plot_w, 0x18C3);

    if (count <= 1 || max_val <= 0.0f) return;

    float dx = (float)plot_w / (float)(count - 1);
    int prev_px = plot_x;
    float v0 = data[0];
    if (v0 > max_val) v0 = max_val;
    if (v0 < 0.0f) v0 = 0.0f;
    int prev_py = plot_y - (int)((v0 / max_val) * plot_h);

    for (int i = 1; i < count; i++) {
        float v = data[i];
        if (v > max_val) v = max_val;
        if (v < 0.0f) v = 0.0f;
        int curr_px = plot_x + (int)(i * dx);
        int curr_py = plot_y - (int)((v / max_val) * plot_h);

        // Fill solid trapezoid area under curve
        _canvas->fillTriangle(prev_px, prev_py, curr_px, curr_py, prev_px, plot_y, fill_col);
        _canvas->fillTriangle(curr_px, curr_py, curr_px, plot_y, prev_px, plot_y, fill_col);

        // Clean glowing curve line
        _canvas->drawLine(prev_px, prev_py, curr_px, curr_py, line_col);
        _canvas->drawLine(prev_px, prev_py - 1, curr_px, curr_py - 1, line_col);

        prev_px = curr_px;
        prev_py = curr_py;
    }

    // Glowing tip dot at latest sample
    _canvas->fillCircle(prev_px, prev_py, 4, line_col);
    _canvas->fillCircle(prev_px, prev_py, 2, COLOR_WHITE);
}

void DashboardRenderer::renderRtGraphsScreen(const DashTelemetry &telemetry) {
    _canvas->setTextDatum(textdatum_t::top_left);

    // Col 1: STRECKE (Trip km & ODO km)
    int cur_trip = (int)(telemetry.trip_km * 10.0f + 0.5f);
    int cur_odo  = (int)(telemetry.odo_km * 10.0f + 0.5f);
    if (!_stats_inited || cur_trip != _stats_cache.trip_x10 || cur_odo != _stats_cache.odo_x10) {
        _stats_cache.trip_x10 = cur_trip;
        _stats_cache.odo_x10 = cur_odo;
        _canvas->fillRect(36, 86, 170, 56, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char distBuf[16]; snprintf(distBuf, sizeof(distBuf), "%.1f km", telemetry.trip_km);
        _canvas->drawString(distBuf, 36, 86);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        char odoBuf[16]; snprintf(odoBuf, sizeof(odoBuf), "ODO: %.1f km", telemetry.odo_km);
        _canvas->drawString(odoBuf, 36, 122);
    }

    // Col 2: FAHRZEIT (Ride time & Avg speed)
    uint32_t sec = telemetry.stats.ride_time_sec;
    int cur_avg = (int)(telemetry.stats.avg_speed_kmh * 10.0f + 0.5f);
    if (!_stats_inited || sec != _stats_cache.ride_sec || cur_avg != _stats_cache.avg_spd_x10) {
        _stats_cache.ride_sec = sec;
        _stats_cache.avg_spd_x10 = cur_avg;
        _canvas->fillRect(225, 86, 170, 56, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char timeBuf[16];
        if (sec >= 3600) {
            snprintf(timeBuf, sizeof(timeBuf), "%u:%02u:%02u", (unsigned int)(sec / 3600), (unsigned int)((sec % 3600) / 60), (unsigned int)(sec % 60));
        } else {
            snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u", (unsigned int)(sec / 60), (unsigned int)(sec % 60));
        }
        _canvas->drawString(timeBuf, 225, 86);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        char avgSpd[16]; snprintf(avgSpd, sizeof(avgSpd), "Avg: %.1f km/h", telemetry.stats.avg_speed_kmh);
        _canvas->drawString(avgSpd, 225, 122);
    }

    // Col 3: MAX SPEED (Max speed & Live speed)
    int cur_max_spd  = (int)(telemetry.stats.max_speed_kmh * 10.0f + 0.5f);
    int cur_live_spd = (int)(telemetry.speed_kmh * 10.0f + 0.5f);
    if (!_stats_inited || cur_max_spd != _stats_cache.max_spd_x10 || cur_live_spd != _stats_cache.live_spd_x10) {
        _stats_cache.max_spd_x10 = cur_max_spd;
        _stats_cache.live_spd_x10 = cur_live_spd;
        _canvas->fillRect(415, 86, 170, 56, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char maxSpd[16]; snprintf(maxSpd, sizeof(maxSpd), "%.1f km/h", telemetry.stats.max_speed_kmh);
        _canvas->drawString(maxSpd, 415, 86);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        char liveSpd[16]; snprintf(liveSpd, sizeof(liveSpd), "Live: %.1f km/h", telemetry.speed_kmh);
        _canvas->drawString(liveSpd, 415, 122);
    }

    // Col 4: ENERGIE (Trip Wh & Remaining Wh)
    int cur_wh  = (int)(telemetry.stats.trip_wh + 0.5f);
    int cur_rem = (int)(telemetry.remaining_wh + 0.5f);
    if (!_stats_inited || cur_wh != _stats_cache.trip_wh || cur_rem != _stats_cache.rem_wh) {
        _stats_cache.trip_wh = cur_wh;
        _stats_cache.rem_wh = cur_rem;
        _canvas->fillRect(605, 86, 160, 56, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char whBuf[16]; snprintf(whBuf, sizeof(whBuf), "%.0f Wh", telemetry.stats.trip_wh);
        _canvas->drawString(whBuf, 605, 86);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        char remWh[16]; snprintf(remWh, sizeof(remWh), "Rest: %.0f Wh", telemetry.remaining_wh);
        _canvas->drawString(remWh, 605, 122);
    }

    // Tile 1: MAX LEISTUNG (PEAK WATT & PS)
    int cur_pwr = (int)(telemetry.stats.peak_power_watts + 0.5f);
    if (!_stats_inited || cur_pwr != _stats_cache.peak_watts) {
        _stats_cache.peak_watts = cur_pwr;
        _canvas->fillRect(32, 202, 220, 60, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char pwrBuf[16]; snprintf(pwrBuf, sizeof(pwrBuf), "%.0f W", telemetry.stats.peak_power_watts);
        _canvas->drawString(pwrBuf, 32, 202);

        float ps_stat = telemetry.stats.peak_power_watts * 0.00135962f;
        char psBuf[16]; snprintf(psBuf, sizeof(psBuf), "%.1f PS", ps_stat);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
        _canvas->drawString(psBuf, 32, 238);
    }

    // Tile 2: PEAK AKKU-STROM
    int cur_curr = (int)(telemetry.stats.peak_current_amps * 10.0f + 0.5f);
    if (!_stats_inited || cur_curr != _stats_cache.peak_curr_x10) {
        _stats_cache.peak_curr_x10 = cur_curr;
        _canvas->fillRect(290, 202, 220, 42, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char curBuf[16]; snprintf(curBuf, sizeof(curBuf), "%.1f A", telemetry.stats.peak_current_amps);
        _canvas->drawString(curBuf, 290, 202);
    }

    // Tile 3: PEAK PHASENSTROM
    int cur_pha = (int)(telemetry.stats.peak_phase_amps * 10.0f + 0.5f);
    if (!_stats_inited || cur_pha != _stats_cache.peak_phase_x10) {
        _stats_cache.peak_phase_x10 = cur_pha;
        _canvas->fillRect(548, 202, 220, 42, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char phaBuf[16]; snprintf(phaBuf, sizeof(phaBuf), "%.1f A", telemetry.stats.peak_phase_amps);
        _canvas->drawString(phaBuf, 548, 202);
    }

    // Tile 4: VERBRAUCH & EFFIZIENZ
    int cur_eff = (int)(telemetry.stats.trip_wh_km * 10.0f + 0.5f);
    float dynWh = (telemetry.stats.dynamic_wh_km > 0.0f) ? telemetry.stats.dynamic_wh_km : telemetry.dynamic_wh_km;
    int cur_dyn = (int)(dynWh * 10.0f + 0.5f);
    if (!_stats_inited || cur_eff != _stats_cache.trip_wh_km_x10 || cur_dyn != _stats_cache.dyn_wh_km_x10) {
        _stats_cache.trip_wh_km_x10 = cur_eff;
        _stats_cache.dyn_wh_km_x10 = cur_dyn;
        _canvas->fillRect(32, 356, 220, 68, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char effBuf[24]; snprintf(effBuf, sizeof(effBuf), "%.1f Wh/km", telemetry.stats.trip_wh_km);
        _canvas->drawString(effBuf, 32, 356);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        char dynBuf[32]; snprintf(dynBuf, sizeof(dynBuf), "Rollend (2km): %.1f Wh/km", dynWh);
        _canvas->drawString(dynBuf, 32, 400);
    }

    // Tile 5: AKKU-SPANNUNG (MIN/MAX)
    int cur_min_v = (int)(telemetry.stats.min_voltage * 10.0f + 0.5f);
    int cur_max_v = (int)(telemetry.stats.max_voltage * 10.0f + 0.5f);
    int cur_live_v = (int)(telemetry.voltage * 10.0f + 0.5f);
    if (!_stats_inited || cur_min_v != _stats_cache.min_v_x10 || cur_max_v != _stats_cache.max_v_x10 || cur_live_v != _stats_cache.live_v_x10) {
        _stats_cache.min_v_x10 = cur_min_v;
        _stats_cache.max_v_x10 = cur_max_v;
        _stats_cache.live_v_x10 = cur_live_v;
        _canvas->fillRect(290, 356, 220, 68, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char vBuf[32]; snprintf(vBuf, sizeof(vBuf), "%.1fV / %.1fV", telemetry.stats.min_voltage, telemetry.stats.max_voltage);
        _canvas->drawString(vBuf, 290, 356);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        char vSub[32]; snprintf(vSub, sizeof(vSub), "Live: %.1fV (Sag Check)", telemetry.voltage);
        _canvas->drawString(vSub, 290, 400);
    }

    // Tile 6: MAX TEMPERATUR
    int cur_max_mot = (int)(telemetry.stats.max_temp_motor + 0.5f);
    int cur_max_esc = (int)(telemetry.stats.max_temp_esc + 0.5f);
    if (!_stats_inited || cur_max_mot != _stats_cache.max_temp_mot || cur_max_esc != _stats_cache.max_temp_esc) {
        _stats_cache.max_temp_mot = cur_max_mot;
        _stats_cache.max_temp_esc = cur_max_esc;
        _canvas->fillRect(548, 356, 220, 42, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char tmpBuf[32]; snprintf(tmpBuf, sizeof(tmpBuf), "%.0f C / %.0f C", telemetry.stats.max_temp_motor, telemetry.stats.max_temp_esc);
        _canvas->drawString(tmpBuf, 548, 356);
    }

    _stats_inited = true;
}

void DashboardRenderer::drawFaultBanner(uint8_t fault_code) {
    uint32_t now = millis();
    bool flash = ((now / 350) % 2) == 0;
    uint16_t bg_col = flash ? 0x8800 : 0xC800;      // Flashing deep red / crimson
    uint16_t border_col = flash ? 0xF800 : 0xFFFF;  // Flashing neon red / white

    // Draw high-visibility warning box across top of screen
    drawCard(20, 8, 760, 56, bg_col, border_col);

    // Hazard Symbol / Alert Icon [ ! ]
    _canvas->fillRoundRect(28, 14, 44, 44, 8, 0xF800);
    _canvas->drawRoundRect(28, 14, 44, 44, 8, COLOR_WHITE);
    _canvas->setTextColor(COLOR_WHITE, 0xF800);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->drawString("!", 50, 36);

    // Line 1: Bold Fault Title in German
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, bg_col);
    char titleBuf[64];
    snprintf(titleBuf, sizeof(titleBuf), "VESC ALARM: %s", FaultManager::getFaultTitleDe(fault_code));
    _canvas->drawString(titleBuf, 84, 13);

    // Line 2: Actionable Description in German
    _canvas->setTextColor(flash ? 0xFFE0 : COLOR_WHITE, bg_col); // Yellow/white text
    _canvas->setFont(&fonts::Font2);
    char descBuf[96];
    snprintf(descBuf, sizeof(descBuf), "%s (MOTOR ABGESCHALTET)", FaultManager::getFaultDescriptionDe(fault_code));
    _canvas->drawString(descBuf, 84, 38);
}

void DashboardRenderer::renderPinLockScreen(const DashTelemetry &telemetry) {
    int entered_len = strlen(telemetry.pin_entry);
    bool err = telemetry.pin_error && (millis() < telemetry.pin_error_end_ms);
    bool alarm = telemetry.theft_alarm_active;

    if (alarm != _last_theft_alarm) {
        _last_theft_alarm = alarm;
        _pin_screen_drawn = false;
    }

    if (!_pin_screen_drawn) {
        _pin_screen_drawn = true;
        _screen_dirty = false;
        _last_pin_len = -1;
        _last_pin_err = !err;

        _canvas->fillScreen(0x0823); // Deep midnight obsidian background

        if (alarm) {
            // Loud Warning Flashing Border
            _canvas->drawRect(0, 0, 800, 480, COLOR_RED);
            _canvas->drawRect(1, 1, 798, 478, COLOR_RED);
            _canvas->drawRect(2, 2, 796, 476, COLOR_RED);
            _canvas->drawRect(3, 3, 794, 474, COLOR_RED);

            // Header Badge with Alarm Alert
            drawCard(180, 16, 440, 50, COLOR_RED, COLOR_WHITE);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextColor(COLOR_WHITE, COLOR_RED);
            _canvas->drawString("! DIEBSTAHL-ALARM !", 400, 41);

            // Prompt Subtitle
            _canvas->setFont(&fonts::Font2);
            _canvas->setTextColor(COLOR_RED, 0x0823);
            char moveBuf[64];
            snprintf(moveBuf, sizeof(moveBuf), "BEWEGUNG ERKANNT (%.1fm) - FOC MOTOR-SIRENE AKTIV!", telemetry.locked_moved_meters);
            _canvas->drawString(moveBuf, 400, 80);
        } else {
            // Standard Header Badge with Lock Icon
            drawCard(250, 16, 300, 50, 0x1084, COLOR_CYAN);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextColor(COLOR_CYAN, 0x1084);
            _canvas->drawString("PIN-SPERRE", 400, 41);

            // Prompt Subtitle
            _canvas->setFont(&fonts::Font2);
            _canvas->setTextColor(COLOR_WHITE, 0x0823);
            _canvas->drawString("SYSTEM GESPERRT - VESC 0A MOTORBLOCKIERT", 400, 80);
        }

        // Keypad Grid: 3 columns x 4 rows
        static const char *KEYS[4][3] = {
            { "1", "2", "3" },
            { "4", "5", "6" },
            { "7", "8", "9" },
            { "DEL", "0", "OK" }
        };

        int col_x[3] = { 205, 345, 485 };
        int row_y[4] = { 165, 235, 305, 375 };

        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 3; c++) {
                int bx = col_x[c];
                int by = row_y[r];
                const char *label = KEYS[r][c];

                uint16_t btn_bg = 0x141F;
                uint16_t btn_border = 0x2965;
                uint16_t txt_col = COLOR_WHITE;

                if (r == 3 && c == 0) {
                    btn_border = 0xB800; // Red border
                    txt_col = 0xFCA5;   // Light coral
                } else if (r == 3 && c == 2) {
                    btn_border = 0x05E0; // Green border
                    txt_col = COLOR_GREEN;
                }

                drawCard(bx, by, 110, 54, btn_bg, btn_border);
                _canvas->setTextDatum(textdatum_t::middle_center);
                _canvas->setFont(&fonts::Font4);
                _canvas->setTextColor(txt_col, btn_bg);
                _canvas->drawString(label, bx + 55, by + 27);
            }
        }

        // Footer Info
        _canvas->setFont(&fonts::Font2);
        if (alarm) {
            _canvas->setTextColor(COLOR_RED, 0x0823);
            _canvas->drawString("FOC Akustik-Sirene aktiv  |  Antrieb gesperrt", 400, 455);
        } else {
            _canvas->setTextColor(0x52AA, 0x0823);
            _canvas->drawString("VESC Wegfahrsperre aktiv  |  Antrieb blockiert", 400, 455);
        }
    }

    // Only update PIN Circles and Status Hint if state changed (Zero Flicker!)
    if (entered_len != _last_pin_len || err != _last_pin_err) {
        _last_pin_len = entered_len;
        _last_pin_err = err;

        // Erase only dots and message area
        _canvas->fillRect(200, 96, 400, 62, 0x0823);

        int dot_xs[4] = { 325, 375, 425, 475 };
        int dot_y = 114;

        for (int i = 0; i < 4; i++) {
            if (i < entered_len) {
                uint16_t dot_col = err ? COLOR_RED : COLOR_CYAN;
                _canvas->fillCircle(dot_xs[i], dot_y, 13, dot_col);
                _canvas->fillCircle(dot_xs[i], dot_y, 5, COLOR_WHITE);
            } else {
                _canvas->fillCircle(dot_xs[i], dot_y, 13, 0x1084);
                _canvas->drawCircle(dot_xs[i], dot_y, 13, err ? COLOR_RED : 0x39C7);
            }
        }

        // Status / Hint below PIN dots
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font2);
        if (err) {
            _canvas->setTextColor(COLOR_RED, 0x0823);
            _canvas->drawString("FALSCHER PIN-CODE! BITTE WIEDERHOLEN", 400, 142);
        } else if (alarm) {
            _canvas->setTextColor(COLOR_RED, 0x0823);
            _canvas->drawString("PIN EINGEBEN ZUM ENTSPERREN & ALARM AUS", 400, 142);
        } else {
            _canvas->setTextColor(COLOR_MUTED_GRAY, 0x0823);
            _canvas->drawString("4-stelligen Entsperrcode eingeben", 400, 142);
        }
    }
}

// ==============================================================================
// SCREEN 3: TOUCH RIDE MODE CONFIGURATOR (800x480 Optimized)
// ==============================================================================
void DashboardRenderer::initSettingsScreen(const DashTelemetry &telemetry) {
    if (_settings_tab_idx >= 6) {
        _settings_tab_idx = (uint8_t)telemetry.ride_mode;
        if (_settings_tab_idx >= 6) _settings_tab_idx = 0;
    }
    _pin_modal_open = false;
    _settings_inited = false;
    _last_rendered_tab = 255;
    _menu_dirty = true;
}

void DashboardRenderer::renderSettingsScreen(const DashTelemetry &telemetry) {
    if (!_menu_dirty) return;
    _menu_dirty = false;

    bool tabChanged = (_last_rendered_tab != _settings_tab_idx);

    if (!_settings_inited) {
        _canvas->fillScreen(COLOR_BG);

        // 1. Header Bar: [< DASH], Title, [FERTIG]
        _canvas->fillRoundRect(20, 12, 110, 36, 18, 0x18C3);
        _canvas->drawRoundRect(20, 12, 110, 36, 18, 0x39C7);
        _canvas->setTextColor(COLOR_WHITE, 0x18C3);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font2);
        _canvas->drawString("< DASH", 75, 30);

        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->drawString("EINSTELLUNGEN", 400, 30);

        _canvas->fillRoundRect(670, 12, 110, 36, 18, 0x18C3);
        _canvas->drawRoundRect(670, 12, 110, 36, 18, 0x39C7);
        _canvas->setTextColor(COLOR_WHITE, 0x18C3);
        _canvas->setFont(&fonts::Font2);
        _canvas->drawString("FERTIG", 725, 30);

        _settings_inited = true;
    }

    // 2. Touch Tabs: [ ECO ] | [ SPORT ] | [ RACE ] | [ DRAGY ] | [ DASH ] | [ PIN ]
    const char *tabNames[6] = { "ECO", "SPORT", "RACE", "DRAGY", "DASH", "PIN" };
    const char *tabSub[6]   = { "Sparsam", "Dynamisch", "Boost", "Splits", "Anzeige", "Sicherheit" };
    uint16_t tabCols[6]     = { COLOR_GREEN, COLOR_CYAN, COLOR_RACE, COLOR_AMBER, 0x07FF, COLOR_WHITE };
    uint16_t tabBgCols[6]   = { 0x0200, 0x0188, 0x4002, 0x31A0, 0x01E8, 0x18C3 };

    int tabX[6] = { 20, 148, 276, 404, 532, 660 };
    int tabW[6] = { 120, 120, 120, 120, 120, 120 };
    int tabH = 54;
    int tabY = 62;

    if (tabChanged) {
        _last_rendered_tab = _settings_tab_idx;

        for (int i = 0; i < 6; i++) {
            bool isSel = (i == _settings_tab_idx);
            uint16_t borderCol = isSel ? tabCols[i] : COLOR_BORDER;
            uint16_t bgCol     = isSel ? tabBgCols[i] : COLOR_SURFACE;
            uint16_t txtCol    = isSel ? tabCols[i] : COLOR_MUTED_GRAY;

            _canvas->fillRoundRect(tabX[i], tabY, tabW[i], tabH, 10, bgCol);
            _canvas->drawRoundRect(tabX[i], tabY, tabW[i], tabH, 10, borderCol);
            if (isSel) {
                _canvas->drawRoundRect(tabX[i] + 1, tabY + 1, tabW[i] - 2, tabH - 2, 9, borderCol);
            }

            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextColor(txtCol, bgCol);
            _canvas->drawString(tabNames[i], tabX[i] + tabW[i] / 2, tabY + 20);

            _canvas->setFont(&fonts::Font0);
            _canvas->setTextColor(isSel ? COLOR_WHITE : 0x52AA, bgCol);
            _canvas->drawString(tabSub[i], tabX[i] + tabW[i] / 2, tabY + 41);
        }

        // Clear content area below the tabs with background color
        _canvas->fillRect(0, 122, 800, 358, COLOR_BG);
    }

    if (_settings_tab_idx == 3) {
        // DRAGY SPLIT TARGET CONFIGURATION (y: 130..450)
        auto drawSplitCol = [&](int col_idx, const char *title, uint8_t split_spd, uint16_t col) {
            int cx = 20 + col_idx * 262;
            int cw = 236, ch = 315;
            drawCard(cx, 130, cw, ch, COLOR_SURFACE, COLOR_BORDER);

            _canvas->setFont(&fonts::Font4);
            _canvas->setTextDatum(textdatum_t::top_left);
            _canvas->setTextColor(col, COLOR_SURFACE);
            _canvas->drawString(title, cx + 18, 145);

            _canvas->setFont(&fonts::Font0);
            _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
            char subBuf[32];
            snprintf(subBuf, sizeof(subBuf), "ZIEL: 0 - %d km/h", split_spd);
            _canvas->drawString(subBuf, cx + 18, 175);

            // Big Speed Number in Center
            char numBuf[16];
            snprintf(numBuf, sizeof(numBuf), "%d", split_spd);
            drawHeroDigitText(_canvas, numBuf, cx + cw / 2, 235, textdatum_t::middle_center, col, COLOR_SURFACE);

            _canvas->setFont(&fonts::Font2);
            _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
            _canvas->setTextDatum(textdatum_t::top_center);
            _canvas->drawString("km/h Zielwert", cx + cw / 2, 280);

            // [-] Button
            _canvas->fillRoundRect(cx + 18, 350, 92, 54, 10, 0x18C3);
            _canvas->drawRoundRect(cx + 18, 350, 92, 54, 10, 0x39C7);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextColor(COLOR_WHITE, 0x18C3);
            _canvas->drawString("-", cx + 18 + 46, 350 + 27);

            // [+] Button
            _canvas->fillRoundRect(cx + cw - 110, 350, 92, 54, 10, 0x18C3);
            _canvas->drawRoundRect(cx + cw - 110, 350, 92, 54, 10, 0x39C7);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextColor(COLOR_WHITE, 0x18C3);
            _canvas->drawString("+", cx + cw - 110 + 46, 350 + 27);
        };

        drawSplitCol(0, "SPLIT 1", Settings.get().race_split1_kmh, COLOR_GREEN);
        drawSplitCol(1, "SPLIT 2", Settings.get().race_split2_kmh, COLOR_CYAN);
        drawSplitCol(2, "SPLIT 3 (ZIEL)", Settings.get().race_split3_kmh, COLOR_RACE);

        // Footer Strip
        _canvas->setFont(&fonts::Font0);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
        _canvas->drawString("Split-Ziele fuer Launch Control & Bestzeiten  |  Auto-Save bei [< DASH]", 400, 462);
        return;
    }

    if (_settings_tab_idx == 4) {
        // DASHBOARD & HARDWARE OPTIONS (y: 130..450)
        // Card 1: GPS & Live-Uhrzeit (y: 130..220)
        drawCard(20, 130, 760, 92, COLOR_SURFACE, COLOR_BORDER);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        _canvas->drawString("GPS & LIVE-UHRZEIT", 40, 144);

        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("Zeigt die Uhrzeit oben mittig & Satelliten-Icon bei 3D-Fix unter km/h.", 40, 174);
        _canvas->setFont(&fonts::Font0);
        _canvas->drawString("Deaktivieren, wenn kein GPS verbaut ist (spart NMEA UART Polling).", 40, 196);

        bool gpsOn = Settings.get().gps_enabled;
        _canvas->fillRoundRect(550, 144, 210, 58, 12, gpsOn ? 0x03E0 : 0x18C3);
        _canvas->drawRoundRect(550, 144, 210, 58, 12, gpsOn ? COLOR_GREEN : 0x39C7);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(gpsOn ? COLOR_WHITE : COLOR_MUTED_GRAY, gpsOn ? 0x03E0 : 0x18C3);
        _canvas->drawString(gpsOn ? "[ AKTIV (AN) ]" : "[ AUS ]", 655, 173);

        // Card 2: Neigung & G-Force (y: 232..322)
        drawCard(20, 232, 760, 92, COLOR_SURFACE, COLOR_BORDER);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        _canvas->drawString("NEIGUNG & G-FORCE (IMU)", 40, 246);

        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("Zeigt Nickwinkel & Beschleunigungskraefte unten links im Dashboard.", 40, 276);
        _canvas->setFont(&fonts::Font0);
        _canvas->drawString("Deaktivieren fuer absolut minimalistischen, aufgeraeumten Look.", 40, 298);

        bool incOn = Settings.get().incline_gforce_enabled;
        _canvas->fillRoundRect(550, 246, 210, 58, 12, incOn ? 0x01E8 : 0x18C3);
        _canvas->drawRoundRect(550, 246, 210, 58, 12, incOn ? COLOR_CYAN : 0x39C7);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(incOn ? COLOR_WHITE : COLOR_MUTED_GRAY, incOn ? 0x01E8 : 0x18C3);
        _canvas->drawString(incOn ? "[ ANZEIGEN ]" : "[ VERSTECKT ]", 655, 275);

        // Card 3: Kein Bluetooth? (WiFi-Bridge) (y: 334..424)
        drawCard(20, 334, 760, 92, COLOR_SURFACE, COLOR_BORDER);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
        _canvas->drawString("KEIN BLUETOOTH-MODUL? (WIFI-BRIDGE)", 40, 348);

        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("Startet SoftAP 'VESC-DASH-AP' fuer drahtlose VESC Tool Verbindung.", 40, 376);
        _canvas->drawString("TCP-Port 6510. Ideal wenn kein Bluetooth-Modul verbaut ist.", 40, 398);

        _canvas->fillRoundRect(550, 348, 210, 58, 12, 0x31A0);
        _canvas->drawRoundRect(550, 348, 210, 58, 12, COLOR_AMBER);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, 0x31A0);
        _canvas->drawString("[ BRIDGE STARTEN ]", 655, 377);

        // Bottom Button: Setup-Assistent wiederholen
        _canvas->fillRoundRect(240, 436, 320, 38, 10, 0x18C3);
        _canvas->drawRoundRect(240, 436, 320, 38, 10, 0x39C7);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_LIGHT_GRAY, 0x18C3);
        _canvas->drawString("SETUP-ASSISTENT WIEDERHOLEN", 400, 455);
        return;
    }

    if (_settings_tab_idx == 5) {
        // PIN / SECURITY SETTINGS (y: 130..450)
        // Card 1: PIN-Sperre beim Start (y: 130..270)
        drawCard(20, 130, 760, 140, COLOR_SURFACE, COLOR_BORDER);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        _canvas->drawString("PIN-SPERRE BEIM EINSCHALTEN", 40, 148);

        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("Blockiert Motor & Gasgriff auf 0A beim Start bis zur PIN-Eingabe", 40, 180);

        // Toggle Button
        bool pin_en = Settings.get().pin_lock_enabled;
        uint16_t tog_bg  = pin_en ? 0x0380 : 0x18C3;
        uint16_t tog_brd = pin_en ? COLOR_GREEN : 0x39C7;
        uint16_t tog_txt = pin_en ? COLOR_GREEN : COLOR_MUTED_GRAY;
        _canvas->fillRoundRect(500, 155, 255, 52, 12, tog_bg);
        _canvas->drawRoundRect(500, 155, 255, 52, 12, tog_brd);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(tog_txt, tog_bg);
        _canvas->drawString(pin_en ? "AKTIVIERT" : "DEAKTIVIERT", 500 + 127, 155 + 26);

        // Card 2: PIN Code Aendern (y: 285..440)
        drawCard(20, 285, 760, 155, COLOR_SURFACE, COLOR_BORDER);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
        _canvas->drawString("4-STELLIGER SICHERHEITS-PIN", 40, 302);

        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("PIN-Code direkt hier am Display neu festlegen", 40, 332);

        // [ PIN AENDERN ] Button
        _canvas->fillRoundRect(40, 365, 290, 54, 12, 0x1084);
        _canvas->drawRoundRect(40, 365, 290, 54, 12, COLOR_CYAN);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, 0x1084);
        _canvas->drawString("PIN JETZT AENDERN", 40 + 145, 365 + 27);

        // Security Info Box
        _canvas->fillRoundRect(360, 350, 400, 74, 8, 0x0823);
        _canvas->drawRoundRect(360, 350, 400, 74, 8, 0x2965);
        _canvas->setFont(&fonts::Font0);
        _canvas->setTextColor(COLOR_CYAN, 0x0823);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->drawString("SICHERHEITSHINWEIS:", 375, 358);
        _canvas->setTextColor(COLOR_MUTED_GRAY, 0x0823);
        _canvas->drawString("Der PIN blockiert alle VESC-Leistungsausgaenge auf 0A.", 375, 378);
        _canvas->drawString("Den gewaehlten 4-stelligen Code gut merken.", 375, 396);

        // Footer Strip
        _canvas->setFont(&fonts::Font0);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
        _canvas->drawString("Sicherheitseinstellungen 100% autark im NVS Flash  |  Zurueck mit [< DASH]", 400, 462);

        if (_pin_modal_open) {
            drawPinChangeModal();
        }
        return;
    }

    // 3. Current Mode Configuration Data
    VehicleModeConfig &cfg = ModeManager::instance().getConfig();
    uint8_t m = (_settings_tab_idx < 3) ? _settings_tab_idx : 1;
    ModeLimits &lim = cfg.tuned_modes[m];
    uint16_t activeCol = tabCols[m];

    // Ensure bounds
    if (lim.max_speed_kmh < 15.0f) lim.max_speed_kmh = 15.0f;
    if (lim.max_speed_kmh > 95.0f) lim.max_speed_kmh = 95.0f;
    if (lim.max_power_watts < 250.0f) lim.max_power_watts = 250.0f;
    if (lim.max_power_watts > 25000.0f) lim.max_power_watts = 25000.0f;

    // 4. Card 1: Max Speed (km/h) (y: 130..280)
    drawCard(20, 130, 760, 150, COLOR_SURFACE, COLOR_BORDER);

    _canvas->setFont(&fonts::Font2);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("MAXIMALE GESCHWINDIGKEIT", 40, 145);

    char spdStr[32];
    snprintf(spdStr, sizeof(spdStr), "%.0f km/h", lim.max_speed_kmh);
    _canvas->setTextDatum(textdatum_t::top_right);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(activeCol, COLOR_SURFACE);
    _canvas->drawString(spdStr, 740, 142);

    // Button [-]
    _canvas->fillRoundRect(40, 182, 65, 54, 10, 0x18C3);
    _canvas->drawRoundRect(40, 182, 65, 54, 10, 0x39C7);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->drawString("-", 72, 209);

    // Button [+]
    _canvas->fillRoundRect(695, 182, 65, 54, 10, 0x18C3);
    _canvas->drawRoundRect(695, 182, 65, 54, 10, 0x39C7);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->drawString("+", 727, 209);

    // Slider track
    _canvas->fillRoundRect(125, 196, 550, 26, 8, 0x1084);
    _canvas->drawRoundRect(125, 196, 550, 26, 8, 0x2965);
    float fracSpd = (lim.max_speed_kmh - 15.0f) / (95.0f - 15.0f);
    if (fracSpd < 0.0f) fracSpd = 0.0f;
    if (fracSpd > 1.0f) fracSpd = 1.0f;
    int fillWSpd = (int)(fracSpd * 546.0f);
    if (fillWSpd > 4) {
        _canvas->fillRoundRect(127, 198, fillWSpd, 22, 6, activeCol);
    }
    int knobX = 127 + fillWSpd;
    _canvas->fillCircle(knobX, 209, 14, COLOR_WHITE);
    _canvas->drawCircle(knobX, 209, 14, activeCol);

    _canvas->setFont(&fonts::Font0);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->drawString("15 km/h", 125, 230);
    _canvas->setTextDatum(textdatum_t::top_right);
    _canvas->drawString("95 km/h", 675, 230);

    // 5. Card 2: Max Watt (y: 295..445)
    drawCard(20, 295, 760, 150, COLOR_SURFACE, COLOR_BORDER);

    _canvas->setFont(&fonts::Font2);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("MAXIMALE LEISTUNG (WATT)", 40, 310);

    char wattStr[32];
    if (lim.max_power_watts >= 1000.0f) {
        snprintf(wattStr, sizeof(wattStr), "%.1f kW (%.0f W)", lim.max_power_watts / 1000.0f, lim.max_power_watts);
    } else {
        snprintf(wattStr, sizeof(wattStr), "%.0f W", lim.max_power_watts);
    }
    _canvas->setTextDatum(textdatum_t::top_right);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(activeCol, COLOR_SURFACE);
    _canvas->drawString(wattStr, 740, 307);

    // Button [-]
    _canvas->fillRoundRect(40, 347, 65, 54, 10, 0x18C3);
    _canvas->drawRoundRect(40, 347, 65, 54, 10, 0x39C7);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->drawString("-", 72, 374);

    // Button [+]
    _canvas->fillRoundRect(695, 347, 65, 54, 10, 0x18C3);
    _canvas->drawRoundRect(695, 347, 65, 54, 10, 0x39C7);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->drawString("+", 727, 374);

    // Slider track
    _canvas->fillRoundRect(125, 361, 550, 26, 8, 0x1084);
    _canvas->drawRoundRect(125, 361, 550, 26, 8, 0x2965);
    float fracWatt = (lim.max_power_watts - 250.0f) / (25000.0f - 250.0f);
    if (fracWatt < 0.0f) fracWatt = 0.0f;
    if (fracWatt > 1.0f) fracWatt = 1.0f;
    int fillWWatt = (int)(fracWatt * 546.0f);
    if (fillWWatt > 4) {
        _canvas->fillRoundRect(127, 363, fillWWatt, 22, 6, activeCol);
    }
    int knobXW = 127 + fillWWatt;
    _canvas->fillCircle(knobXW, 374, 14, COLOR_WHITE);
    _canvas->drawCircle(knobXW, 374, 14, activeCol);

    _canvas->setFont(&fonts::Font0);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->drawString("250 W", 125, 395);
    _canvas->setTextDatum(textdatum_t::top_right);
    _canvas->drawString("25000 W", 675, 395);

    // 6. Footer Strip
    _canvas->setFont(&fonts::Font0);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
    _canvas->drawString("Phasen- & Batteriestrom auf Maximum / Headroom  |  Auto-Save bei [< DASH]", 400, 462);

    if (_pin_modal_open) {
        drawPinChangeModal();
    }
}

void DashboardRenderer::drawPinChangeModal() {
    int mx = 160, my = 35, mw = 480, mh = 410;
    // Dark modal surface card with glowing cyan border
    drawCard(mx, my, mw, mh, 0x0823, COLOR_CYAN);

    // Header Title
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_CYAN, 0x0823);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->drawString("NEUEN PIN FESTLEGEN", mx + 20, my + 16);

    // Top-Right [ X ] Close / Cancel Button
    _canvas->fillRoundRect(mx + mw - 48, my + 12, 36, 32, 8, 0x2124);
    _canvas->drawRoundRect(mx + mw - 48, my + 12, 36, 32, 8, COLOR_MUTED_GRAY);
    _canvas->setTextColor(COLOR_WHITE, 0x2124);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->drawString("X", mx + mw - 30, my + 28);

    if (_pin_modal_success) {
        // Success state!
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_GREEN, 0x0823);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->drawString("PIN ERFOLGREICH GESPEICHERT!", mx + mw / 2, my + 130);

        char savedBuf[32];
        snprintf(savedBuf, sizeof(savedBuf), "Neuer PIN: %s", Settings.get().pin_code);
        _canvas->setFont(&fonts::Font6);
        _canvas->setTextColor(COLOR_WHITE, 0x0823);
        _canvas->drawString(savedBuf, mx + mw / 2, my + 200);

        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, 0x0823);
        _canvas->drawString("Im NVS Flash gesichert. Schliesst in Kuerze...", mx + mw / 2, my + 260);
        return;
    }

    // Subtitle
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_WHITE, 0x0823);
    _canvas->setTextDatum(textdatum_t::top_center);
    _canvas->drawString("Gib 4 neue Ziffern ein:", mx + mw / 2, my + 54);

    // 4 PIN Dots (y: 82)
    int entered_len = strlen(_pin_modal_buf);
    int dot_xs[4] = { mx + mw / 2 - 60, mx + mw / 2 - 20, mx + mw / 2 + 20, mx + mw / 2 + 60 };
    int dot_y = my + 92;
    for (int i = 0; i < 4; i++) {
        if (i < entered_len) {
            _canvas->fillCircle(dot_xs[i], dot_y, 11, COLOR_CYAN);
            _canvas->fillCircle(dot_xs[i], dot_y, 4, COLOR_WHITE);
        } else {
            _canvas->fillCircle(dot_xs[i], dot_y, 11, 0x1084);
            _canvas->drawCircle(dot_xs[i], dot_y, 11, 0x39C7);
        }
    }

    // Keypad (3 cols x 4 rows)
    static const char *KEYS[4][3] = {
        { "1", "2", "3" },
        { "4", "5", "6" },
        { "7", "8", "9" },
        { "DEL", "0", "OK" }
    };
    int kw = 96, kh = 46;
    int kx0 = mx + (mw - (3 * kw + 2 * 14)) / 2;
    int ky0 = my + 125;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            int bx = kx0 + c * (kw + 14);
            int by = ky0 + r * (kh + 8);
            uint16_t bg = 0x141F, border = 0x2965, txt = COLOR_WHITE;
            if (r == 3 && c == 0) { border = 0xB800; txt = 0xFCA5; }
            else if (r == 3 && c == 2) { border = 0x05E0; txt = COLOR_GREEN; }

            drawCard(bx, by, kw, kh, bg, border);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextColor(txt, bg);
            _canvas->drawString(KEYS[r][c], bx + kw / 2, by + kh / 2);
        }
    }
}

void DashboardRenderer::handleMenuNav(NavAction action, DashTelemetry &telemetry) {
    if (action == NAV_NONE) return;

    if (action == NAV_SCREEN_DASH || action == NAV_MENU_BACK) {
        // Auto-save and apply limits
        VehicleModeConfig &cfg = ModeManager::instance().getConfig();
        uint8_t m = (_settings_tab_idx < 3) ? _settings_tab_idx : 1;
        for (int i = 0; i < 3; i++) {
            if (cfg.tuned_modes[i].max_battery_amps < 60.0f) cfg.tuned_modes[i].max_battery_amps = 60.0f;
            if (cfg.tuned_modes[i].max_phase_amps < 120.0f) cfg.tuned_modes[i].max_phase_amps = 120.0f;
            if (cfg.tuned_modes[i].max_pull_watts < cfg.tuned_modes[i].max_power_watts) {
                cfg.tuned_modes[i].max_pull_watts = cfg.tuned_modes[i].max_power_watts * 1.25f;
            }
            if (!ModeManager::instance().isTuningMode()) {
                cfg.legal_modes[i].max_speed_kmh = cfg.tuned_modes[i].max_speed_kmh;
                cfg.legal_modes[i].max_power_watts = cfg.tuned_modes[i].max_power_watts;
                cfg.legal_modes[i].max_pull_watts = cfg.tuned_modes[i].max_pull_watts;
                cfg.legal_modes[i].max_battery_amps = cfg.tuned_modes[i].max_battery_amps;
                cfg.legal_modes[i].max_phase_amps = cfg.tuned_modes[i].max_phase_amps;
            }
        }

        ModeManager::instance().saveToNvs();
        Settings.get().max_speed_kmh = (uint8_t)roundf(cfg.tuned_modes[m].max_speed_kmh);
        Settings.save();
        ModeManager::instance().applyCurrentMode();

        _settings_tab_idx = 255;
        _settings_inited = false;
        _last_rendered_tab = 255;
        telemetry.screen = SCREEN_RIDE_DASH;
        _screen_dirty = true;
        _cache.invalidate();
        return;
    }

    VehicleModeConfig &cfg = ModeManager::instance().getConfig();
    uint8_t m = (_settings_tab_idx < 3) ? _settings_tab_idx : 1;
    ModeLimits &lim = cfg.tuned_modes[m];

    if (action == NAV_BOTH_PRESSED) {
        // Cycle tab: ECO -> SPORT -> RACE -> DRAGY -> PIN -> ECO
        _settings_tab_idx = (_settings_tab_idx + 1) % 5;
        _menu_dirty = true;
        return;
    }

    if (action == NAV_EDIT_INC || action == NAV_BTN2_SHORT) {
        lim.max_speed_kmh += 1.0f;
        if (lim.max_speed_kmh > 95.0f) lim.max_speed_kmh = 95.0f;
        _menu_dirty = true;
        return;
    }

    if (action == NAV_EDIT_DEC || action == NAV_BTN1_SHORT) {
        lim.max_speed_kmh -= 1.0f;
        if (lim.max_speed_kmh < 15.0f) lim.max_speed_kmh = 15.0f;
        _menu_dirty = true;
        return;
    }
}

bool DashboardRenderer::handleSettingsTouch(int16_t tx, int16_t ty, DashTelemetry &telemetry) {
    if (tx < 0 || ty < 0) return false;

    // 0. PIN Change Modal Touch Handling
    if (_pin_modal_open) {
        int mx = 160, my = 35, mw = 480, mh = 410;
        // Top-Right [ X ] button
        if (tx >= mx + mw - 55 && tx <= mx + mw && ty >= my && ty <= my + 50) {
            _pin_modal_open = false;
            _last_rendered_tab = 255;
            _menu_dirty = true;
            return true;
        }

        if (_pin_modal_success) {
            _pin_modal_open = false;
            _last_rendered_tab = 255;
            _menu_dirty = true;
            return true;
        }

        // Tap outside modal closes it
        if (tx < mx || tx > mx + mw || ty < my || ty > my + mh) {
            _pin_modal_open = false;
            _last_rendered_tab = 255;
            _menu_dirty = true;
            return true;
        }

        // Keypad (3 cols x 4 rows)
        int kw = 96, kh = 46;
        int kx0 = mx + (mw - (3 * kw + 2 * 14)) / 2;
        int ky0 = my + 125;

        int c = -1;
        for (int col = 0; col < 3; col++) {
            int bx = kx0 + col * (kw + 14);
            if (tx >= bx && tx <= bx + kw) { c = col; break; }
        }
        int r = -1;
        for (int row = 0; row < 4; row++) {
            int by = ky0 + row * (kh + 8);
            if (ty >= by && ty <= by + kh) { r = row; break; }
        }

        if (r >= 0 && c >= 0) {
            static const char KEY_MAP[4][3] = {
                { '1', '2', '3' },
                { '4', '5', '6' },
                { '7', '8', '9' },
                { '<', '0', '#' }
            };
            char key = KEY_MAP[r][c];
            int len = strlen(_pin_modal_buf);
            if (key == '<') { // DEL
                if (len > 0) {
                    _pin_modal_buf[len - 1] = '\0';
                    _menu_dirty = true;
                }
            } else if (key == '#') { // OK
                if (len == 4) {
                    strncpy(Settings.get().pin_code, _pin_modal_buf, sizeof(Settings.get().pin_code));
                    Settings.save();
                    _pin_modal_success = true;
                    _pin_modal_success_ms = millis() + 1400;
                    _menu_dirty = true;
                }
            } else if (key >= '0' && key <= '9') {
                if (len < 4) {
                    _pin_modal_buf[len] = key;
                    _pin_modal_buf[len + 1] = '\0';
                    len++;
                    _menu_dirty = true;
                    if (len == 4) {
                        strncpy(Settings.get().pin_code, _pin_modal_buf, sizeof(Settings.get().pin_code));
                        Settings.save();
                        _pin_modal_success = true;
                        _pin_modal_success_ms = millis() + 1400;
                    }
                }
            }
        }
        return true;
    }

    // 1. TOP BAR (y <= 55): Return to Dash & Auto-Save
    if (ty <= 55) {
        if (tx <= 180 || tx >= 620) {
            handleMenuNav(NAV_SCREEN_DASH, telemetry);
            return true;
        }
        return false;
    }

    // 2. TABS (y: 58..125) - 6 Tabs: ECO, SPORT, RACE, DRAGY, DASH, PIN
    if (ty >= 58 && ty <= 125) {
        if (tx >= 10 && tx < 138) {
            _settings_tab_idx = 0; // ECO
            _menu_dirty = true;
            return true;
        } else if (tx >= 138 && tx < 266) {
            _settings_tab_idx = 1; // SPORT
            _menu_dirty = true;
            return true;
        } else if (tx >= 266 && tx < 394) {
            _settings_tab_idx = 2; // RACE
            _menu_dirty = true;
            return true;
        } else if (tx >= 394 && tx < 522) {
            _settings_tab_idx = 3; // DRAGY
            _menu_dirty = true;
            return true;
        } else if (tx >= 522 && tx < 650) {
            _settings_tab_idx = 4; // DASH
            _menu_dirty = true;
            return true;
        } else if (tx >= 650 && tx <= 790) {
            _settings_tab_idx = 5; // PIN
            _menu_dirty = true;
            return true;
        }
    }

    // Tab 3: DRAGY SPLITS CONFIGURATION
    if (_settings_tab_idx == 3) {
        if (ty >= 335 && ty <= 425) {
            // Col 0: Split 1
            if (tx >= 25 && tx <= 135) { // [-]
                if (Settings.get().race_split1_kmh > 10) {
                    Settings.get().race_split1_kmh--;
                    Settings.save();
                    _menu_dirty = true;
                }
                return true;
            } else if (tx >= 140 && tx <= 250) { // [+]
                if (Settings.get().race_split1_kmh < Settings.get().race_split2_kmh - 1) {
                    Settings.get().race_split1_kmh++;
                    Settings.save();
                    _menu_dirty = true;
                }
                return true;
            }
            // Col 1: Split 2
            else if (tx >= 285 && tx <= 395) { // [-]
                if (Settings.get().race_split2_kmh > Settings.get().race_split1_kmh + 1) {
                    Settings.get().race_split2_kmh--;
                    Settings.save();
                    _menu_dirty = true;
                }
                return true;
            } else if (tx >= 400 && tx <= 510) { // [+]
                if (Settings.get().race_split2_kmh < Settings.get().race_split3_kmh - 1) {
                    Settings.get().race_split2_kmh++;
                    Settings.save();
                    _menu_dirty = true;
                }
                return true;
            }
            // Col 2: Split 3
            else if (tx >= 550 && tx <= 660) { // [-]
                if (Settings.get().race_split3_kmh > Settings.get().race_split2_kmh + 1) {
                    Settings.get().race_split3_kmh--;
                    Settings.save();
                    _menu_dirty = true;
                }
                return true;
            } else if (tx >= 665 && tx <= 775) { // [+]
                if (Settings.get().race_split3_kmh < 130) {
                    Settings.get().race_split3_kmh++;
                    Settings.save();
                    _menu_dirty = true;
                }
                return true;
            }
        }
        return false;
    }

    // Tab 4: DASHBOARD & HARDWARE OPTIONS
    if (_settings_tab_idx == 4) {
        // Card 1: GPS & Live-Uhrzeit (button: x: 530..770, y: 135..210)
        if (tx >= 530 && tx <= 770 && ty >= 135 && ty <= 210) {
            Settings.get().gps_enabled = !Settings.get().gps_enabled;
            Settings.save();
            telemetry.gps_enabled = Settings.get().gps_enabled;
            _menu_dirty = true;
            return true;
        }
        // Card 2: Neigung & G-Force (button: x: 530..770, y: 235..315)
        if (tx >= 530 && tx <= 770 && ty >= 235 && ty <= 315) {
            Settings.get().incline_gforce_enabled = !Settings.get().incline_gforce_enabled;
            Settings.save();
            telemetry.incline_gforce_enabled = Settings.get().incline_gforce_enabled;
            _menu_dirty = true;
            return true;
        }
        // Card 3: Kein Bluetooth? (WiFi-Bridge) (button: x: 530..770, y: 340..420)
        if (tx >= 530 && tx <= 770 && ty >= 340 && ty <= 420) {
            Vesc.enterWifiBridgeMode();
            telemetry.screen = SCREEN_WIFI_BRIDGE;
            _screen_dirty = true;
            _menu_dirty = true;
            return true;
        }
        // Button: Setup-Assistent wiederholen (x: 230..570, y: 430..475)
        if (tx >= 230 && tx <= 570 && ty >= 430 && ty <= 475) {
            resetSetupWizard();
            telemetry.wizard_step = 0;
            telemetry.wizard_pin_buf[0] = '\0';
            telemetry.screen = SCREEN_SETUP_WIZARD;
            _screen_dirty = true;
            return true;
        }
        return false;
    }

    // Tab 5: PIN & SECURITY
    if (_settings_tab_idx == 5) {
        // Card 1: Toggle PIN lock on boot (button bounds: x: 480..770, y: 145..220)
        if (tx >= 480 && tx <= 770 && ty >= 145 && ty <= 220) {
            Settings.get().pin_lock_enabled = !Settings.get().pin_lock_enabled;
            Settings.save();
            _menu_dirty = true;
            return true;
        }
        // Card 2: [ PIN JETZT AENDERN ] Button (tx: 30..350, ty: 350..430)
        if (tx >= 30 && tx <= 350 && ty >= 350 && ty <= 430) {
            _pin_modal_open = true;
            _pin_modal_buf[0] = '\0';
            _pin_modal_success = false;
            _menu_dirty = true;
            return true;
        }
        return false;
    }

    VehicleModeConfig &cfg = ModeManager::instance().getConfig();
    uint8_t m = (_settings_tab_idx < 3) ? _settings_tab_idx : 1;
    ModeLimits &lim = cfg.tuned_modes[m];

    // 3. SPEED SLIDER & BUTTONS (y: 135..285)
    if (ty >= 135 && ty <= 285) {
        // [-] Button
        if (tx >= 30 && tx <= 115) {
            lim.max_speed_kmh -= 1.0f;
            if (lim.max_speed_kmh < 15.0f) lim.max_speed_kmh = 15.0f;
            _menu_dirty = true;
            return true;
        }
        // [+] Button
        if (tx >= 685 && tx <= 770) {
            lim.max_speed_kmh += 1.0f;
            if (lim.max_speed_kmh > 95.0f) lim.max_speed_kmh = 95.0f;
            _menu_dirty = true;
            return true;
        }
        // Slider Track tap
        if (tx >= 120 && tx <= 680) {
            float frac = (float)(tx - 125) / 550.0f;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            lim.max_speed_kmh = roundf(15.0f + frac * (95.0f - 15.0f));
            _menu_dirty = true;
            return true;
        }
    }

    // 4. WATT SLIDER & BUTTONS (y: 295..445)
    if (ty >= 295 && ty <= 445) {
        // [-] Button
        if (tx >= 30 && tx <= 115) {
            lim.max_power_watts -= (lim.max_power_watts > 5000.0f) ? 500.0f : 250.0f;
            if (lim.max_power_watts < 250.0f) lim.max_power_watts = 250.0f;
            lim.max_pull_watts = lim.max_power_watts * 1.25f;
            _menu_dirty = true;
            return true;
        }
        // [+] Button
        if (tx >= 685 && tx <= 770) {
            lim.max_power_watts += (lim.max_power_watts >= 5000.0f) ? 500.0f : 250.0f;
            if (lim.max_power_watts > 25000.0f) lim.max_power_watts = 25000.0f;
            lim.max_pull_watts = lim.max_power_watts * 1.25f;
            _menu_dirty = true;
            return true;
        }
        // Slider Track tap
        if (tx >= 120 && tx <= 680) {
            float frac = (float)(tx - 125) / 550.0f;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            float rawWatt = 250.0f + frac * (25000.0f - 250.0f);
            lim.max_power_watts = roundf(rawWatt / 50.0f) * 50.0f;
            if (lim.max_power_watts < 250.0f) lim.max_power_watts = 250.0f;
            if (lim.max_power_watts > 25000.0f) lim.max_power_watts = 25000.0f;
            lim.max_pull_watts = lim.max_power_watts * 1.25f;
            _menu_dirty = true;
            return true;
        }
    }

    return false;
}

bool DashboardRenderer::handleSettingsSliderDrag(int16_t cx, int16_t cy, DashTelemetry &telemetry) {
    if (_pin_modal_open) return false;
    // Slider drag only exists on Mode limit tabs (ECO:0, SPORT:1, RACE:2)
    if (_settings_tab_idx >= 3) return false;

    VehicleModeConfig &cfg = ModeManager::instance().getConfig();
    ModeLimits &lim = cfg.tuned_modes[_settings_tab_idx];

    // Speed Slider track y: 175..245, x: 120..680
    if (cy >= 175 && cy <= 245 && cx >= 120 && cx <= 680) {
        float frac = (float)(cx - 125) / 550.0f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        float new_spd = roundf(15.0f + frac * (95.0f - 15.0f));
        if (new_spd != lim.max_speed_kmh) {
            lim.max_speed_kmh = new_spd;
            _menu_dirty = true;
            return true;
        }
    }
    // Watt Slider track y: 340..410, x: 120..680
    else if (cy >= 340 && cy <= 410 && cx >= 120 && cx <= 680) {
        float frac = (float)(cx - 125) / 550.0f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        float rawWatt = 250.0f + frac * (25000.0f - 250.0f);
        float new_watt = roundf(rawWatt / 50.0f) * 50.0f;
        if (new_watt < 250.0f) new_watt = 250.0f;
        if (new_watt > 25000.0f) new_watt = 25000.0f;
        if (new_watt != lim.max_power_watts) {
            lim.max_power_watts = new_watt;
            lim.max_pull_watts = lim.max_power_watts * 1.25f;
            _menu_dirty = true;
            return true;
        }
    }
    return false;
}

// ==============================================================================
// SCREEN 4: VESC TOOL USB PASSTHROUGH BRIDGE (800x480 Optimized)
// ==============================================================================
void DashboardRenderer::initVescBridgeScreen(const DashTelemetry &telemetry) {
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextPadding(0);

    // 1. Header Bar (y: 12..52)
    // Left: [< DASH] Button
    _canvas->fillRoundRect(20, 12, 110, 38, 8, 0x18C3);
    _canvas->drawRoundRect(20, 12, 110, 38, 8, 0x39C7);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setTextColor(COLOR_CYAN, 0x18C3);
    _canvas->drawString("< DASH", 75, 31);

    // Center Title
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    _canvas->drawString("VESC TOOL USB PASSTHROUGH", 400, 31);

    // Right Baud Badge
    _canvas->fillRoundRect(620, 12, 160, 38, 8, 0x0821);
    _canvas->drawRoundRect(620, 12, 160, 38, 8, COLOR_CYAN);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_GREEN, 0x0821);
    _canvas->drawString("115200 BAUD", 700, 31);

    _canvas->drawFastHLine(20, 58, 760, COLOR_BORDER);

    // 2. Left Card: PC / VESC Tool (w=370, h=315)
    drawCard(20, 68, 370, 315, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("PC / VESC TOOL (USB)", 36, 76);

    _canvas->drawFastHLine(34, 108, 342, COLOR_SURFACE_LIGHT);

    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->drawString("Port: USB CDC (/dev/ttyACM0)", 36, 120);
    _canvas->drawString("Status: Virtueller COM-Port aktiv", 36, 150);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("EMPFANGENE BYTES VOM PC:", 36, 195);

    // 3. Right Card: VESC Controller (w=370, h=315)
    drawCard(410, 68, 370, 315, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextColor(COLOR_GREEN, COLOR_SURFACE);
    _canvas->drawString("VESC MOTOR CONTROLLER", 426, 76);

    _canvas->drawFastHLine(424, 108, 342, COLOR_SURFACE_LIGHT);

    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->drawString("Pins: TX=43, RX=44 (Serial1)", 426, 120);
    _canvas->drawString("Hardware: VESC UART Interface", 426, 150);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("GESENDETE BYTES AN VESC:", 426, 195);

    // 4. Bottom Exit / Help Banner (w=760, h=72, y: 395..467)
    _canvas->fillRoundRect(20, 395, 760, 72, 10, 0x18C3);
    _canvas->drawRoundRect(20, 395, 760, 72, 10, COLOR_AMBER);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_AMBER, 0x18C3);
    _canvas->drawString("[ USB-BRIDGE BEENDEN & ZURUECK ]", 400, 420);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, 0x18C3);
    _canvas->drawString("Tippen kehrt zum Dashboard zurueck", 400, 448);

    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextPadding(0);

    _bridge_last_pc_bytes = 0xFFFFFFFF;
    _bridge_last_vesc_bytes = 0xFFFFFFFF;
}

void DashboardRenderer::renderVescBridgeScreen(const DashTelemetry &telemetry) {
    uint32_t pc_bytes = Vesc.getBridgePcToVescBytes();
    uint32_t vesc_bytes = Vesc.getBridgeVescToPcBytes();

    _canvas->setFont(&fonts::Font6);
    _canvas->setTextDatum(textdatum_t::top_left);

    if (pc_bytes != _bridge_last_pc_bytes) {
        _bridge_last_pc_bytes = pc_bytes;
        char buf[32];
        if (pc_bytes < 1024) snprintf(buf, sizeof(buf), "%u B", (unsigned)pc_bytes);
        else if (pc_bytes < 1048576) snprintf(buf, sizeof(buf), "%.1f KB", pc_bytes / 1024.0f);
        else snprintf(buf, sizeof(buf), "%.2f MB", pc_bytes / 1048576.0f);

        _canvas->fillRect(36, 230, 330, 45, COLOR_SURFACE);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->drawString(buf, 36, 230);
    }

    if (vesc_bytes != _bridge_last_vesc_bytes) {
        _bridge_last_vesc_bytes = vesc_bytes;
        char buf[32];
        if (vesc_bytes < 1024) snprintf(buf, sizeof(buf), "%u B", (unsigned)vesc_bytes);
        else if (vesc_bytes < 1048576) snprintf(buf, sizeof(buf), "%.1f KB", vesc_bytes / 1024.0f);
        else snprintf(buf, sizeof(buf), "%.2f MB", vesc_bytes / 1048576.0f);

        _canvas->fillRect(426, 230, 330, 45, COLOR_SURFACE);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->drawString(buf, 426, 230);
    }
}

// ==============================================================================
// SCREEN 5: VESC TOOL WIRELESS WI-FI BRIDGE (800x480 Optimized)
// ==============================================================================
void DashboardRenderer::initWifiBridgeScreen(const DashTelemetry &telemetry) {
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextPadding(0);

    // 1. Header Bar (y: 12..52)
    // Left: [< DASH] Button
    _canvas->fillRoundRect(20, 12, 110, 38, 8, 0x18C3);
    _canvas->drawRoundRect(20, 12, 110, 38, 8, 0x39C7);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setTextColor(COLOR_CYAN, 0x18C3);
    _canvas->drawString("< DASH", 75, 31);

    // Center Title
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    _canvas->drawString("VESC TOOL WI-FI BRIDGE", 400, 31);

    // Right Port Badge
    _canvas->fillRoundRect(650, 12, 130, 38, 8, 0x0821);
    _canvas->drawRoundRect(650, 12, 130, 38, 8, COLOR_CYAN);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_GREEN, 0x0821);
    _canvas->drawString("TCP 6510", 715, 31);

    _canvas->drawFastHLine(20, 58, 760, COLOR_BORDER);

    // 2. Left Card: Wi-Fi Access Point Details (w=370, h=315)
    drawCard(20, 68, 370, 315, COLOR_SURFACE, COLOR_BORDER);
    _canvas->fillCircle(38, 88, 5, COLOR_GREEN);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("WLAN ACCESS POINT", 52, 76);

    _canvas->drawFastHLine(34, 108, 342, COLOR_SURFACE_LIGHT);

    // Parameter List
    int rowY = 120;
    int rowH = 34;

    auto drawRow = [&](const char *label, const char *val, uint16_t valCol) {
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString(label, 36, rowY);

        _canvas->setTextDatum(textdatum_t::top_right);
        _canvas->setTextColor(valCol, COLOR_SURFACE);
        _canvas->drawString(val, 376, rowY);
        rowY += rowH;
    };

    drawRow("Netzwerk (SSID):", "VESC-DASH-AP", COLOR_WHITE);
    drawRow("IP-Adresse:", "192.168.4.1", COLOR_WHITE);
    drawRow("TCP-Port:", "6510 (Standard)", COLOR_WHITE);
    drawRow("Verschluesselung:", "Offen (Kein PIN)", COLOR_GREEN);
    drawRow("WLAN-Frequenz:", "2.4 GHz SoftAP", COLOR_WHITE);

    // Small info box inside left card
    _canvas->fillRoundRect(34, 304, 342, 65, 8, 0x0821);
    _canvas->drawRoundRect(34, 304, 342, 65, 8, 0x18E3);
    _canvas->setFont(&fonts::Font0);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextColor(COLOR_CYAN, 0x0821);
    _canvas->drawString("DRAHTLOS-PROGRAMMIERUNG:", 44, 312);
    _canvas->setTextColor(COLOR_MUTED_GRAY, 0x0821);
    _canvas->drawString("Ermoeglicht kabellose VESC-Tool Verbindung", 44, 330);
    _canvas->drawString("fuer Mobile App & PC ohne USB-Kabel.", 44, 348);

    // 3. Right Card: VESC Tool Client Connection & Traffic Throughput (w=370, h=315)
    drawCard(410, 68, 370, 315, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextColor(COLOR_GREEN, COLOR_SURFACE);
    _canvas->drawString("VESC TOOL CLIENT", 426, 76);

    _canvas->drawFastHLine(424, 108, 342, COLOR_SURFACE_LIGHT);

    // 4. Bottom Exit / Help Banner (w=760, h=72, y: 395..467)
    _canvas->fillRoundRect(20, 395, 760, 72, 10, 0x18C3);
    _canvas->drawRoundRect(20, 395, 760, 72, 10, COLOR_AMBER);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_AMBER, 0x18C3);
    _canvas->drawString("[ WI-FI BEENDEN & ZURUECK ]", 400, 420);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, 0x18C3);
    _canvas->drawString("Tippen schaltet WLAN ab und kehrt zum Dashboard zurueck", 400, 448);

    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextPadding(0);

    _bridge_last_wifi_rx = 0xFFFFFFFF;
    _bridge_last_wifi_tx = 0xFFFFFFFF;
    _bridge_last_wifi_conn = -1;
}

void DashboardRenderer::renderWifiBridgeScreen(const DashTelemetry &telemetry) {
    bool connected = Vesc.isWifiClientConnected();
    int8_t conn_state = connected ? 1 : 0;
    uint32_t rx_bytes = Vesc.getBridgeWifiToVescBytes();
    uint32_t tx_bytes = Vesc.getBridgeVescToWifiBytes();

    if (conn_state != _bridge_last_wifi_conn) {
        _bridge_last_wifi_conn = conn_state;
        // Clear interior of right card
        _canvas->fillRect(414, 112, 362, 265, COLOR_SURFACE);

        if (connected) {
            // Status badge (green)
            _canvas->fillRoundRect(426, 122, 338, 38, 8, 0x0380);
            _canvas->drawRoundRect(426, 122, 338, 38, 8, COLOR_GREEN);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->setFont(&fonts::Font2);
            _canvas->setTextColor(COLOR_WHITE, 0x0380);
            _canvas->drawString("STATUS: VERBUNDEN", 426 + 169, 141);

            _canvas->setTextDatum(textdatum_t::top_left);
            _canvas->setFont(&fonts::Font2);
            _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
            char ipBuf[40];
            snprintf(ipBuf, sizeof(ipBuf), "Client-IP: %s", Vesc.getWifiClientIp());
            _canvas->drawString(ipBuf, 426, 172);

            _canvas->drawFastHLine(426, 202, 338, COLOR_SURFACE_LIGHT);

            _canvas->drawString("APP -> VESC:", 426, 220);
            _canvas->drawString("VESC -> APP:", 426, 258);

            _bridge_last_wifi_rx = 0xFFFFFFFF;
            _bridge_last_wifi_tx = 0xFFFFFFFF;
        } else {
            // Status badge (amber)
            _canvas->fillRoundRect(426, 120, 338, 36, 8, 0x31A0);
            _canvas->drawRoundRect(426, 120, 338, 36, 8, COLOR_AMBER);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->setFont(&fonts::Font2);
            _canvas->setTextColor(COLOR_WHITE, 0x31A0);
            _canvas->drawString("WARTET AUF VESC TOOL...", 426 + 169, 138);

            // Instructions
            _canvas->setTextDatum(textdatum_t::top_left);
            _canvas->setFont(&fonts::Font2);
            _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
            _canvas->drawString("1. Mit WLAN 'VESC-DASH-AP' verbinden", 426, 170);
            _canvas->drawString("2. VESC Tool oeffnen (PC/Handy)", 426, 204);
            _canvas->drawString("3. Verbindungsart 'TCP' waehlen", 426, 238);
            _canvas->drawString("4. Host: 192.168.4.1  Port: 6510", 426, 272);
            _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
            _canvas->drawString("5. Auf 'Connect' tippen!", 426, 306);
        }
    }

    if (connected) {
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        if (rx_bytes != _bridge_last_wifi_rx) {
            _bridge_last_wifi_rx = rx_bytes;
            char buf[32];
            if (rx_bytes < 1024) snprintf(buf, sizeof(buf), "%u B", (unsigned)rx_bytes);
            else if (rx_bytes < 1048576) snprintf(buf, sizeof(buf), "%.1f KB", rx_bytes / 1024.0f);
            else snprintf(buf, sizeof(buf), "%.2f MB", rx_bytes / 1048576.0f);

            _canvas->fillRect(570, 216, 190, 30, COLOR_SURFACE);
            _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
            _canvas->drawString(buf, 570, 216);
        }

        if (tx_bytes != _bridge_last_wifi_tx) {
            _bridge_last_wifi_tx = tx_bytes;
            char buf[32];
            if (tx_bytes < 1024) snprintf(buf, sizeof(buf), "%u B", (unsigned)tx_bytes);
            else if (tx_bytes < 1048576) snprintf(buf, sizeof(buf), "%.1f KB", tx_bytes / 1024.0f);
            else snprintf(buf, sizeof(buf), "%.2f MB", tx_bytes / 1048576.0f);

            _canvas->fillRect(570, 254, 190, 30, COLOR_SURFACE);
            _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
            _canvas->drawString(buf, 570, 254);
        }
    }
}

// ==============================================================================
// SCREEN 6: "RACE" DRAGY / LAUNCH ACCELERATION TIMER (800x480)
// ==============================================================================
void DashboardRenderer::updateRaceTimer(DashTelemetry &telemetry, uint32_t now) {
    if (telemetry.screen != SCREEN_RACE_TIMER) {
        if (telemetry.race_state != 0) {
            telemetry.race_state = 0;
            telemetry.boost_active = false;
            ModeManager::instance().setBoostActive(false);
            Buttons.setRaceState(0);
        }
        return;
    }

    // Continuous Thermal Protection: Abort launch if temperatures exceed 65 °C during run
    if (isRaceThermalBlocked(telemetry)) {
        if (telemetry.race_state == 1 || telemetry.race_state == 2) {
            telemetry.race_state = 0; // IDLE / ABORT
            telemetry.boost_active = false;
            ModeManager::instance().setBoostActive(false);
            telemetry.ride_mode = telemetry.saved_ride_mode;
            ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
            Buttons.setRaceState(0);
            markScreenDirty();
            return;
        }
    }

    if (telemetry.race_state == 1) { // 1 = COUNTDOWN (5s)
        telemetry.boost_active = true;
        ModeManager::instance().setBoostActive(true);
        telemetry.boost_end_ms = now + 10000;
        uint32_t elapsed_cd = now - telemetry.race_countdown_start_ms;
        telemetry.race_elapsed_sec = (float)elapsed_cd / 1000.0f;
        if (elapsed_cd >= 5000) {
            telemetry.race_state = 2; // 2 = MEASURING
            telemetry.race_launch_start_ms = now;
            telemetry.race_elapsed_sec = 0.0f;
            telemetry.race_split_0_22 = 0.0f;
            telemetry.race_split_0_50 = 0.0f;
            telemetry.race_split_0_70 = 0.0f;
            telemetry.race_dyno.reset();
            telemetry.race_dyno.last_sample_ms = now;
            Buttons.setRaceState(2);
            markScreenDirty();
        }
    } else if (telemetry.race_state == 2) { // 2 = MEASURING
        telemetry.boost_active = true;
        ModeManager::instance().setBoostActive(true);
        telemetry.boost_end_ms = now + 10000;
        telemetry.race_elapsed_sec = (float)(now - telemetry.race_launch_start_ms) / 1000.0f;

        // Sample Dyno Data (every 100ms)
        if (now - telemetry.race_dyno.last_sample_ms >= 100) {
            telemetry.race_dyno.last_sample_ms = now;
            if (telemetry.race_dyno.sample_count < RACE_DYNO_SAMPLES) {
                int idx = telemetry.race_dyno.sample_count++;
                telemetry.race_dyno.samples_power[idx] = telemetry.power_watts;
                telemetry.race_dyno.samples_speed[idx] = telemetry.speed_kmh;
                telemetry.race_dyno.samples_g[idx] = telemetry.g_force;
            }
        }
        if (telemetry.power_watts > telemetry.race_dyno.peak_watts) {
            telemetry.race_dyno.peak_watts = telemetry.power_watts;
        }
        if (telemetry.g_force > telemetry.race_dyno.peak_g_force) {
            telemetry.race_dyno.peak_g_force = telemetry.g_force;
        }
        if (telemetry.speed_kmh > telemetry.race_dyno.peak_speed_kmh) {
            telemetry.race_dyno.peak_speed_kmh = telemetry.speed_kmh;
        }
        if (telemetry.phase_amps > telemetry.race_dyno.peak_phase_amps) {
            telemetry.race_dyno.peak_phase_amps = telemetry.phase_amps;
        }

        // Check configured Dragy splits
        uint8_t sp1 = Settings.get().race_split1_kmh;
        uint8_t sp2 = Settings.get().race_split2_kmh;
        uint8_t sp3 = Settings.get().race_split3_kmh;

        // Check Split 1
        if (telemetry.race_split_0_22 <= 0.0f && telemetry.speed_kmh >= (float)sp1) {
            telemetry.race_split_0_22 = (telemetry.race_elapsed_sec > 0.01f) ? telemetry.race_elapsed_sec : 0.01f;
            if (telemetry.race_records.best_0_22_sec <= 0.01f || telemetry.race_split_0_22 < telemetry.race_records.best_0_22_sec) {
                telemetry.race_records.best_0_22_sec = telemetry.race_split_0_22;
                Settings.saveRaceRecords(telemetry.race_records);
            }
        }

        // Check Split 2
        if (telemetry.race_split_0_50 <= 0.0f && telemetry.speed_kmh >= (float)sp2) {
            telemetry.race_split_0_50 = (telemetry.race_elapsed_sec > 0.01f) ? telemetry.race_elapsed_sec : 0.01f;
            if (telemetry.race_records.best_0_50_sec <= 0.01f || telemetry.race_split_0_50 < telemetry.race_records.best_0_50_sec) {
                telemetry.race_records.best_0_50_sec = telemetry.race_split_0_50;
                Settings.saveRaceRecords(telemetry.race_records);
            }
        }

        // Check Split 3 (Final Target Split -> Finishes Run)
        if (telemetry.race_split_0_70 <= 0.0f && telemetry.speed_kmh >= (float)sp3) {
            telemetry.race_split_0_70 = (telemetry.race_elapsed_sec > 0.01f) ? telemetry.race_elapsed_sec : 0.01f;
            if (telemetry.race_records.best_0_70_sec <= 0.01f || telemetry.race_split_0_70 < telemetry.race_records.best_0_70_sec) {
                telemetry.race_records.best_0_70_sec = telemetry.race_split_0_70;
                Settings.saveRaceRecords(telemetry.race_records);
            }
            telemetry.race_state = 3; // FINISHED
            telemetry.boost_active = false;
            ModeManager::instance().setBoostActive(false);
            telemetry.ride_mode = telemetry.saved_ride_mode;
            ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
            Buttons.setRaceState(3);
            markScreenDirty();
        }

        // Auto-finish when rider comes to a standstill after an acceleration pull
        if (telemetry.race_dyno.peak_speed_kmh >= 25.0f && telemetry.race_elapsed_sec > 2.0f && telemetry.speed_kmh < 2.0f) {
            telemetry.race_state = 3; // FINISHED
            telemetry.boost_active = false;
            ModeManager::instance().setBoostActive(false);
            telemetry.ride_mode = telemetry.saved_ride_mode;
            ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
            Buttons.setRaceState(3);
            markScreenDirty();
        }

        // 15-second measurement timeout cap (automatic race end)
        if (telemetry.race_elapsed_sec >= 15.0f) {
            telemetry.race_state = 3; // FINISHED
            telemetry.boost_active = false;
            ModeManager::instance().setBoostActive(false);
            telemetry.ride_mode = telemetry.saved_ride_mode;
            ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
            Buttons.setRaceState(3);
            markScreenDirty();
        }
    }
}

void DashboardRenderer::initRaceTimerScreen(const DashTelemetry &telemetry) {
    _canvas->fillScreen(COLOR_BG);
    _cache.invalidate();

    // Top-Left [< DASH] Button (x=20, y=12, w=110, h=36, r=18)
    _canvas->fillRoundRect(20, 12, 110, 36, 18, COLOR_SURFACE);
    _canvas->drawRoundRect(20, 12, 110, 36, 18, COLOR_BORDER);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font2);
    _canvas->drawString("< DASH", 75, 30);

    // Top [ DIAGRAMM ] Button (x=475, y=12, w=140, h=36, r=18)
    _canvas->fillRoundRect(475, 12, 140, 36, 18, COLOR_SURFACE);
    _canvas->drawRoundRect(475, 12, 140, 36, 18, COLOR_CYAN);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->drawString("DIAGRAMM", 545, 30);

    // Top-Right [ BESTZEITEN ] Button (x=630, y=12, w=150, h=36, r=18)
    _canvas->fillRoundRect(630, 12, 150, 36, 18, COLOR_SURFACE);
    _canvas->drawRoundRect(630, 12, 150, 36, 18, COLOR_BORDER);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    _canvas->setFont(&fonts::Font2);
    _canvas->drawString("BESTZEITEN", 705, 30);

    // Speedometer Center Arc Gauge (280 degrees sweep: 130° to 410°)
    int cx = 310, cy = 255;
    int r_in = 130, r_out = 158;
    float mid_r = 144.0f;
    float deg2rad = 3.14159265f / 180.0f;
    _canvas->fillArc(cx, cy, r_in, r_out, 130.0f, 410.0f, COLOR_SURFACE_LIGHT);
    int s_cap_x = cx + (int)roundf(cosf(130.0f * deg2rad) * mid_r);
    int s_cap_y = cy + (int)roundf(sinf(130.0f * deg2rad) * mid_r);
    int e_cap_x = cx + (int)roundf(cosf(410.0f * deg2rad) * mid_r);
    int e_cap_y = cy + (int)roundf(sinf(410.0f * deg2rad) * mid_r);
    _canvas->fillCircle(s_cap_x, s_cap_y, 14, COLOR_SURFACE_LIGHT);
    _canvas->fillCircle(e_cap_x, e_cap_y, 14, COLOR_SURFACE_LIGHT);

    // Static label "km/h" under hero speed
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextSize(1);
    _canvas->setTextDatum(textdatum_t::top_center);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
    _canvas->drawString("km/h", cx, cy + 30);

    // Corner Card: Clean Run Splits Card (x=530, y=70, w=250, h=390)
    drawCard(530, 70, 250, 390, COLOR_SURFACE, COLOR_BORDER);

    // Clean, spaced-out split rows with subtle dividers (no "RUN SPLITS" banner)
    char label1[24], label2[24], label3[24];
    snprintf(label1, sizeof(label1), "0 - %d km/h", Settings.get().race_split1_kmh);
    snprintf(label2, sizeof(label2), "0 - %d km/h", Settings.get().race_split2_kmh);
    snprintf(label3, sizeof(label3), "0 - %d km/h", Settings.get().race_split3_kmh);

    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->drawString(label1, 548, 100);
    _canvas->drawFastHLine(548, 185, 214, COLOR_SURFACE_LIGHT);

    _canvas->drawString(label2, 548, 205);
    _canvas->drawFastHLine(548, 290, 214, COLOR_SURFACE_LIGHT);

    _canvas->drawString(label3, 548, 310);
}

void DashboardRenderer::renderRaceTimerScreen(const DashTelemetry &telemetry) {
    int cx = 310, cy = 255;
    int r_in = 130, r_out = 158;
    float mid_r = 144.0f;
    float deg2rad = 3.14159265f / 180.0f;

    // 1. Top-Center Timer Display
    _canvas->fillRect(135, 10, 335, 46, COLOR_BG);
    if (telemetry.race_state == 2 || telemetry.race_state == 3) {
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "%.2f", telemetry.race_elapsed_sec);
        int nw = _canvas->textWidth(timeBuf, &fonts::Font6);
        int sw = _canvas->textWidth("s", &fonts::Font4);
        int totalW = nw + 6 + sw;
        int startX = 305 - totalW / 2;

        _canvas->setFont(&fonts::Font6);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->setTextDatum(textdatum_t::middle_left);
        _canvas->drawString(timeBuf, startX, 30);

        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
        _canvas->setTextDatum(textdatum_t::bottom_left);
        _canvas->drawString("s", startX + nw + 6, 42);
    } else if (telemetry.race_state == 1) {
        int rem_sec = 5 - (int)telemetry.race_elapsed_sec;
        if (rem_sec < 1) rem_sec = 1;
        if (rem_sec > 5) rem_sec = 5;
        char cdTitle[24];
        snprintf(cdTitle, sizeof(cdTitle), "LAUNCH IN %d s", rem_sec);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_AMBER, COLOR_BG);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->drawString(cdTitle, 305, 30);
    } else {
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_BG);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->drawString("LAUNCH CONTROL", 305, 30);
    }

    // 2. Speedometer Arc
    float spd = telemetry.speed_kmh;
    if (spd < 0.8f) spd = 0.0f;
    float frac = spd / 100.0f;
    if (frac > 1.0f) frac = 1.0f;
    float target_angle = 130.0f + (frac * 280.0f);
    if (frac <= 0.005f) target_angle = 130.0f;

    uint16_t arcCol = COLOR_WHITE;

    // Redraw arc track and active fill
    _canvas->fillArc(cx, cy, r_in, r_out, 130.0f, 410.0f, COLOR_SURFACE_LIGHT);
    int s_cap_x = cx + (int)roundf(cosf(130.0f * deg2rad) * mid_r);
    int s_cap_y = cy + (int)roundf(sinf(130.0f * deg2rad) * mid_r);
    int e_cap_x = cx + (int)roundf(cosf(410.0f * deg2rad) * mid_r);
    int e_cap_y = cy + (int)roundf(sinf(410.0f * deg2rad) * mid_r);
    _canvas->fillCircle(s_cap_x, s_cap_y, 14, COLOR_SURFACE_LIGHT);
    _canvas->fillCircle(e_cap_x, e_cap_y, 14, COLOR_SURFACE_LIGHT);
    if (target_angle > 130.0f) {
        _canvas->fillArc(cx, cy, r_in, r_out, 130.0f, target_angle, arcCol);
        _canvas->fillCircle(s_cap_x, s_cap_y, 14, arcCol);
        int tip_x = cx + (int)roundf(cosf(target_angle * deg2rad) * mid_r);
        int tip_y = cy + (int)roundf(sinf(target_angle * deg2rad) * mid_r);
        _canvas->fillCircle(tip_x, tip_y, 14, arcCol);
        _canvas->fillCircle(tip_x, tip_y, 5, COLOR_WHITE);
    }

    // Digital Speed in Center
    _canvas->fillRect(cx - 95, cy - 80, 190, 75, COLOR_BG);
    char spdBuf[16];
    snprintf(spdBuf, sizeof(spdBuf), "%.0f", spd);
    drawHeroDigitText(_canvas, spdBuf, cx, cy - 40, textdatum_t::middle_center, COLOR_WHITE, COLOR_BG);

    // 3. Center State Action / Countdown / Status
    _canvas->fillRect(cx - 105, cy + 65, 210, 62, COLOR_BG);
    if (telemetry.race_state == 0) { // IDLE / READY
        bool overheat = isRaceThermalBlocked(telemetry);
        if (overheat) {
            _canvas->fillRoundRect(cx - 95, cy + 72, 190, 42, 21, 0x3000);
            _canvas->drawRoundRect(cx - 95, cy + 72, 190, 42, 21, COLOR_AMBER);
            _canvas->setTextColor(COLOR_AMBER, 0x3000);
            _canvas->setFont(&fonts::Font2);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->drawString("TEMP > 65C", cx, cy + 93);
        } else {
            _canvas->fillRoundRect(cx - 85, cy + 72, 170, 42, 21, COLOR_SURFACE);
            _canvas->drawRoundRect(cx - 85, cy + 72, 170, 42, 21, COLOR_BORDER);
            _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
            _canvas->setFont(&fonts::Font2);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->drawString("START LAUNCH", cx, cy + 93);
        }
    } else if (telemetry.race_state == 1) { // COUNTDOWN (5s)
        int rem_sec = 5 - (int)telemetry.race_elapsed_sec;
        if (rem_sec < 1) rem_sec = 1;
        if (rem_sec > 5) rem_sec = 5;
        char cdBuf[16];
        if (rem_sec == 1) {
            snprintf(cdBuf, sizeof(cdBuf), "1 ... GO!");
        } else {
            snprintf(cdBuf, sizeof(cdBuf), "%d ...", rem_sec);
        }

        _canvas->fillRoundRect(cx - 85, cy + 72, 170, 42, 21, 0x3000);
        _canvas->drawRoundRect(cx - 85, cy + 72, 170, 42, 21, COLOR_RED);
        _canvas->setTextColor(COLOR_WHITE, 0x3000);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->drawString(cdBuf, cx, cy + 93);
    } else if (telemetry.race_state == 2) { // MEASURING
        _canvas->fillRoundRect(cx - 85, cy + 72, 170, 42, 21, 0x3000);
        _canvas->drawRoundRect(cx - 85, cy + 72, 170, 42, 21, COLOR_RED);
        _canvas->setTextColor(COLOR_WHITE, 0x3000);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->drawString("STOP / ABORT", cx, cy + 93);
    } else { // FINISHED
        _canvas->fillRoundRect(cx - 85, cy + 72, 170, 42, 21, COLOR_SURFACE);
        _canvas->drawRoundRect(cx - 85, cy + 72, 170, 42, 21, COLOR_WHITE);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->drawString("NEUER LAUNCH", cx, cy + 93);
    }

    // 4. Update Corner Card Split Values (Clean time display without *PB*)
    auto drawSplit = [&](int y, float split_val, bool is_active) {
        _canvas->fillRect(548, y, 215, 36, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        char sBuf[32];
        if (split_val > 0.0f) {
            snprintf(sBuf, sizeof(sBuf), "%.2f s", split_val);
            _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
            _canvas->drawString(sBuf, 548, y);
        } else if (telemetry.race_state == 2 && is_active) {
            snprintf(sBuf, sizeof(sBuf), "%.2f s", telemetry.race_elapsed_sec);
            _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
            _canvas->drawString(sBuf, 548, y);
        } else {
            _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
            _canvas->drawString("--.-- s", 548, y);
        }
    };

    bool active_0_22 = (telemetry.race_split_0_22 <= 0.0f);
    bool active_0_50 = (telemetry.race_split_0_22 > 0.0f && telemetry.race_split_0_50 <= 0.0f);
    bool active_0_70 = (telemetry.race_split_0_50 > 0.0f && telemetry.race_split_0_70 <= 0.0f);

    drawSplit(130, telemetry.race_split_0_22, active_0_22);
    drawSplit(235, telemetry.race_split_0_50, active_0_50);
    drawSplit(340, telemetry.race_split_0_70, active_0_70);

    // Bottom-Left G-Force Badge
    drawGForceBadge(_canvas, telemetry.g_force, 50, 416, 28);

    // 5. Popups & Modals (if open)
    if (telemetry.race_popup_open) {
        drawRaceRecordsModal(telemetry);
    } else if (telemetry.race_dyno_open) {
        drawRaceDynoModal(telemetry);
    }
}

void DashboardRenderer::drawRaceRecordsModal(const DashTelemetry &telemetry) {
    int mx = 140, my = 70, mw = 520, mh = 340;
    // Dark modal surface card with glowing gold border
    drawCard(mx, my, mw, mh, 0x10A2, COLOR_AMBER);

    // Modal Header
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_AMBER, 0x10A2);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->drawString("PERSOENLICHE BESTZEITEN", mx + 24, my + 18);

    // Top-Right Close Button [ X ]
    _canvas->fillRoundRect(mx + mw - 55, my + 14, 40, 34, 8, 0x2124);
    _canvas->drawRoundRect(mx + mw - 55, my + 14, 40, 34, 8, COLOR_MUTED_GRAY);
    _canvas->setTextColor(COLOR_WHITE, 0x2124);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->drawString("X", mx + mw - 35, my + 31);

    // Display rows for 0-22, 0-50, 0-70 km/h
    auto drawRecordRow = [&](int y, const char *label, float best_sec) {
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, 0x10A2);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->drawString(label, mx + 30, y);

        char valBuf[32];
        if (best_sec > 0.01f) {
            snprintf(valBuf, sizeof(valBuf), "%.2f s", best_sec);
            _canvas->setTextColor(COLOR_GREEN, 0x10A2);
        } else {
            snprintf(valBuf, sizeof(valBuf), "Keine Daten");
            _canvas->setTextColor(COLOR_MUTED_GRAY, 0x10A2);
        }
        _canvas->setTextDatum(textdatum_t::top_right);
        _canvas->drawString(valBuf, mx + mw - 40, y);

        // Subtle divider
        _canvas->drawFastHLine(mx + 30, y + 36, mw - 70, 0x2124);
    };

    char rec1[24], rec2[24], rec3[24];
    snprintf(rec1, sizeof(rec1), "0 - %d km/h", Settings.get().race_split1_kmh);
    snprintf(rec2, sizeof(rec2), "0 - %d km/h", Settings.get().race_split2_kmh);
    snprintf(rec3, sizeof(rec3), "0 - %d km/h", Settings.get().race_split3_kmh);

    drawRecordRow(my + 70, rec1, telemetry.race_records.best_0_22_sec);
    drawRecordRow(my + 130, rec2, telemetry.race_records.best_0_50_sec);
    drawRecordRow(my + 190, rec3, telemetry.race_records.best_0_70_sec);

    // Bottom Action Buttons: [ RESET ] (left) and [ SCHLIESSEN ] (right)
    int btnY = my + 265;
    // [ RESET ] button
    _canvas->fillRoundRect(mx + 35, btnY, 150, 48, 12, 0x3000);
    _canvas->drawRoundRect(mx + 35, btnY, 150, 48, 12, COLOR_RED);
    _canvas->setTextColor(COLOR_WHITE, 0x3000);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->drawString("RESET", mx + 110, btnY + 24);

    // [ SCHLIESSEN ] button
    _canvas->fillRoundRect(mx + mw - 215, btnY, 180, 48, 12, 0x18C3);
    _canvas->drawRoundRect(mx + mw - 215, btnY, 180, 48, 12, 0x39C7);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->setFont(&fonts::Font2);
    _canvas->drawString("SCHLIESSEN", mx + mw - 125, btnY + 24);
}

void DashboardRenderer::drawRaceDynoModal(const DashTelemetry &telemetry) {
    int mx = 100, my = 45, mw = 600, mh = 390;
    // Dark modal surface card with cyan border
    drawCard(mx, my, mw, mh, 0x10A2, COLOR_CYAN);

    // Modal Header
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_CYAN, 0x10A2);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->drawString("LEISTUNGSDIAGRAMM & TELEMETRIE", mx + 20, my + 14);

    // Top-Right Close Button [ X ]
    _canvas->fillRoundRect(mx + mw - 50, my + 12, 38, 32, 8, 0x2124);
    _canvas->drawRoundRect(mx + mw - 50, my + 12, 38, 32, 8, COLOR_MUTED_GRAY);
    _canvas->setTextColor(COLOR_WHITE, 0x2124);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->drawString("X", mx + mw - 31, my + 28);

    // Determine Peak & Summary Statistics
    // Fall back to realistic scooter specs if no run has been recorded yet
    float peak_w  = (telemetry.race_dyno.peak_watts > 100.0f) ? telemetry.race_dyno.peak_watts : 3520.0f;
    float peak_kw = peak_w / 1000.0f;
    float peak_ps = peak_w * 0.00135962f;
    float peak_g  = (telemetry.race_dyno.peak_g_force > 0.05f) ? telemetry.race_dyno.peak_g_force : 1.15f;
    float peak_i  = (telemetry.race_dyno.peak_phase_amps > 5.0f) ? telemetry.race_dyno.peak_phase_amps : 85.0f;
    float t_70    = (telemetry.race_split_0_70 > 0.01f) ? telemetry.race_split_0_70 :
                    (telemetry.race_records.best_0_70_sec > 0.01f ? telemetry.race_records.best_0_70_sec : 4.25f);

    // 4 Stat Tiles: MAX WATT, MAX G-FORCE, 0-70 KM/H, MAX PHASEN-I
    int tile_y = my + 50;
    int tile_w = 132;
    int tile_h = 52;
    int tile_gap = 10;
    int tile_x0 = mx + 16;

    auto drawStatTile = [&](int idx, const char *label, const char *val, const char *sub_val, uint16_t valColor) {
        int tx = tile_x0 + idx * (tile_w + tile_gap);
        _canvas->fillRoundRect(tx, tile_y, tile_w, tile_h, 8, 0x18C3);
        _canvas->drawRoundRect(tx, tile_y, tile_w, tile_h, 8, 0x2965);
        _canvas->setFont(&fonts::Font0);
        _canvas->setTextColor(COLOR_MUTED_GRAY, 0x18C3);
        _canvas->setTextDatum(textdatum_t::top_center);
        _canvas->drawString(label, tx + tile_w / 2, tile_y + 3);

        if (sub_val && sub_val[0] != '\0') {
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextColor(valColor, 0x18C3);
            _canvas->setTextDatum(textdatum_t::top_center);
            _canvas->drawString(val, tx + tile_w / 2, tile_y + 13);

            _canvas->setFont(&fonts::Font0);
            _canvas->setTextColor(COLOR_AMBER, 0x18C3);
            _canvas->setTextDatum(textdatum_t::top_center);
            _canvas->drawString(sub_val, tx + tile_w / 2, tile_y + 39);
        } else {
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextColor(valColor, 0x18C3);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->drawString(val, tx + tile_w / 2, tile_y + 31);
        }
    };

    char b1[16], sub_b1[16], b2[16], b3[16], b4[16];
    if (peak_kw >= 1.0f) {
        snprintf(b1, sizeof(b1), "%.1f kW", peak_kw);
    } else {
        snprintf(b1, sizeof(b1), "%.0f W", peak_w);
    }
    snprintf(sub_b1, sizeof(sub_b1), "%.1f PS", peak_ps);
    snprintf(b2, sizeof(b2), "+%.2f G", peak_g);
    snprintf(b3, sizeof(b3), "%.2f s", t_70);
    snprintf(b4, sizeof(b4), "%.0f A", peak_i);

    drawStatTile(0, "MAX WATT", b1, sub_b1, COLOR_CYAN);
    drawStatTile(1, "MAX G-FORCE", b2, nullptr, COLOR_AMBER);
    char dynoSplitLabel[24];
    snprintf(dynoSplitLabel, sizeof(dynoSplitLabel), "0 - %d KM/H", Settings.get().race_split3_kmh);
    drawStatTile(2, dynoSplitLabel, b3, nullptr, COLOR_GREEN);
    drawStatTile(3, "MAX PHASEN-I", b4, nullptr, COLOR_WHITE);

    // Dyno Chart Area
    int gx = mx + 55;
    int gy = my + 115;
    int gw = mw - 110; // 490px
    int gh = 185;

    // Background of chart
    _canvas->fillRect(gx, gy, gw, gh, 0x0841);
    _canvas->drawRect(gx, gy, gw, gh, 0x2965);

    // Grid lines (horizontal: 25%, 50%, 75%)
    for (int i = 1; i <= 3; i++) {
        int y_line = gy + (gh * i) / 4;
        _canvas->drawFastHLine(gx, y_line, gw, 0x18C3);
    }
    // Vertical grid lines (0, 20, 40, 60, 80 km/h)
    for (int i = 1; i <= 3; i++) {
        int x_line = gx + (gw * i) / 4;
        _canvas->drawFastVLine(x_line, gy, gh, 0x18C3);
    }

    // Y-Axis Left (Power in kW, Cyan) - Dynamic scaling up to 15 kW (e.g. 88V 21S battery packs)
    float max_kw_scale = 6.0f;
    if (peak_kw > 10.0f) max_kw_scale = 15.0f;
    else if (peak_kw > 7.5f) max_kw_scale = 10.0f;
    else if (peak_kw > 5.5f) max_kw_scale = 8.0f;
    _canvas->setFont(&fonts::Font0);
    _canvas->setTextColor(COLOR_CYAN, 0x10A2);
    _canvas->setTextDatum(textdatum_t::middle_right);
    char kwBuf[16];
    snprintf(kwBuf, sizeof(kwBuf), "%.1fkW", max_kw_scale);
    _canvas->drawString(kwBuf, gx - 5, gy + 4);
    snprintf(kwBuf, sizeof(kwBuf), "%.1fkW", max_kw_scale * 0.5f);
    _canvas->drawString(kwBuf, gx - 5, gy + gh / 2);
    _canvas->drawString("0 kW", gx - 5, gy + gh - 4);

    // Y-Axis Right (G-Force, Amber) - Dynamic scaling up to 2.5G
    float max_g_scale = 1.5f;
    if (peak_g > 1.8f) max_g_scale = 2.5f;
    else if (peak_g > 1.35f) max_g_scale = 2.0f;
    _canvas->setTextColor(COLOR_AMBER, 0x10A2);
    _canvas->setTextDatum(textdatum_t::middle_left);
    char gBuf[16];
    snprintf(gBuf, sizeof(gBuf), "%.1f G", max_g_scale);
    _canvas->drawString(gBuf, gx + gw + 5, gy + 4);
    snprintf(gBuf, sizeof(gBuf), "%.2f G", max_g_scale * 0.5f);
    _canvas->drawString(gBuf, gx + gw + 5, gy + gh / 2);
    _canvas->drawString("0.0 G", gx + gw + 5, gy + gh - 4);

    // X-Axis Speed Labels under chart
    _canvas->setFont(&fonts::Font0);
    _canvas->setTextColor(COLOR_MUTED_GRAY, 0x10A2);
    _canvas->setTextDatum(textdatum_t::top_center);
    _canvas->drawString("0 km/h", gx, gy + gh + 4);
    _canvas->drawString("25 km/h", gx + gw / 4, gy + gh + 4);
    _canvas->drawString("50 km/h", gx + gw / 2, gy + gh + 4);
    _canvas->drawString("75 km/h", gx + (gw * 3) / 4, gy + gh + 4);
    _canvas->drawString("100 km/h", gx + gw, gy + gh + 4);

    // Draw Curves
    bool has_real_data = (telemetry.race_dyno.sample_count >= 5);
    int num_points = has_real_data ? telemetry.race_dyno.sample_count : 40;

    int prev_px = -1, prev_py_power = -1, prev_py_g = -1;

    for (int i = 0; i < num_points; i++) {
        float spd_val = 0.0f;
        float pwr_val = 0.0f;
        float g_val   = 0.0f;

        if (has_real_data) {
            spd_val = telemetry.race_dyno.samples_speed[i];
            pwr_val = telemetry.race_dyno.samples_power[i];
            g_val   = telemetry.race_dyno.samples_g[i];
        } else {
            // Preview / baseline simulation:
            // Speed 0 to 80 km/h
            float t_norm = (num_points > 1) ? ((float)i / (float)(num_points - 1)) : 0.0f; // 0.0 .. 1.0
            spd_val = 80.0f * (1.0f - expf(-2.2f * t_norm)); // Ramping speed
            // Power curve: rises rapidly to peak_kw, stays high
            float p_frac = (t_norm < 0.25f) ? (t_norm / 0.25f) : (1.0f - 0.12f * (t_norm - 0.25f));
            pwr_val = peak_kw * 1000.0f * p_frac;
            // G-Force curve: peaks instantly at launch, decays with speed
            g_val = peak_g * expf(-1.1f * t_norm);
        }

        // Map speed to X coordinate (0 to 100 km/h)
        float x_ratio = spd_val / 100.0f;
        if (x_ratio < 0.0f) x_ratio = 0.0f;
        if (x_ratio > 1.0f) x_ratio = 1.0f;
        int px = gx + (int)(x_ratio * (gw - 2));

        // Map Power to Y coordinate
        float p_kw = pwr_val / 1000.0f;
        float p_ratio = p_kw / max_kw_scale;
        if (p_ratio < 0.0f) p_ratio = 0.0f;
        if (p_ratio > 1.0f) p_ratio = 1.0f;
        int py_pwr = gy + gh - 2 - (int)(p_ratio * (gh - 4));

        // Map G-Force to Y coordinate
        float g_ratio = g_val / max_g_scale;
        if (g_ratio < 0.0f) g_ratio = 0.0f;
        if (g_ratio > 1.0f) g_ratio = 1.0f;
        int py_g = gy + gh - 2 - (int)(g_ratio * (gh - 4));

        if (i > 0 && prev_px >= 0) {
            // Power curve (Cyan, 2px thick)
            _canvas->drawLine(prev_px, prev_py_power, px, py_pwr, COLOR_CYAN);
            _canvas->drawLine(prev_px, prev_py_power + 1, px, py_pwr + 1, COLOR_CYAN);

            // G-Force curve (Amber, 2px thick)
            _canvas->drawLine(prev_px, prev_py_g, px, py_g, COLOR_AMBER);
            _canvas->drawLine(prev_px, prev_py_g + 1, px, py_g + 1, COLOR_AMBER);
        }

        prev_px = px;
        prev_py_power = py_pwr;
        prev_py_g = py_g;
    }

    // Legend & Mode Tag under Chart
    int leg_y = gy + gh + 18;
    // Cyan line sample
    _canvas->fillRoundRect(gx + 10, leg_y + 4, 18, 4, 2, COLOR_CYAN);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_CYAN, 0x10A2);
    _canvas->setTextDatum(textdatum_t::middle_left);
    _canvas->drawString("Leistung (kW)", gx + 34, leg_y + 6);

    // Amber line sample
    _canvas->fillRoundRect(gx + 160, leg_y + 4, 18, 4, 2, COLOR_AMBER);
    _canvas->setTextColor(COLOR_AMBER, 0x10A2);
    _canvas->setTextDatum(textdatum_t::middle_left);
    _canvas->drawString("Beschleunigung (+G)", gx + 184, leg_y + 6);

    // Run indicator note
    _canvas->setFont(&fonts::Font0);
    _canvas->setTextColor(COLOR_MUTED_GRAY, 0x10A2);
    _canvas->setTextDatum(textdatum_t::middle_right);
    _canvas->drawString(has_real_data ? "[Live Dyno Telemetrie]" : "[Vorschau / Letzter Lauf]", gx + gw, leg_y + 6);

    // Bottom Action Button: [ SCHLIESSEN ]
    int btnW = 200, btnH = 40;
    int btnX = mx + (mw - btnW) / 2;
    int btnY = my + mh - 50;
    _canvas->fillRoundRect(btnX, btnY, btnW, btnH, 12, 0x18C3);
    _canvas->drawRoundRect(btnX, btnY, btnW, btnH, 12, 0x39C7);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->drawString("SCHLIESSEN", btnX + btnW / 2, btnY + btnH / 2);
}

// ==============================================================================
// SCREEN 7: FIRST-TIME SETUP & ONBOARDING WIZARD (800x480 Optimized)
// ==============================================================================
void DashboardRenderer::resetSetupWizard() {
    _wizard_inited = false;
    _wizard_step = 0;
    _wizard_pin_buf[0] = '\0';
    _screen_dirty = true;
}

void DashboardRenderer::initSetupWizard(const DashTelemetry &telemetry) {
    if (!_wizard_inited) {
        _wizard_step = telemetry.wizard_step;
        _wizard_pin_buf[0] = '\0';
        _wizard_inited = true;
    }
}

void DashboardRenderer::renderSetupWizard(const DashTelemetry &telemetry) {
    _canvas->fillScreen(COLOR_BG);

    if (_wizard_step == 0) {
        // STEP 1 OF 3: WELCOME & NAVIGATION / TUNING GUIDE
        _canvas->fillRoundRect(20, 14, 760, 52, 12, 0x0188); // Dark Cyan
        _canvas->drawRoundRect(20, 14, 760, 52, 12, COLOR_CYAN);
        _canvas->setTextColor(COLOR_WHITE, 0x0188);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->drawString("WILLKOMMEN ZUM VESC HYPER DISPLAY", 400, 30);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_CYAN, 0x0188);
        _canvas->drawString("Ersteinrichtung - Schritt 1 von 3: Schnellanleitung", 400, 52);

        // Card 1: Wie oeffne ich die Einstellungen?
        drawCard(20, 78, 760, 130, COLOR_SURFACE, COLOR_BORDER);
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->drawString("WIE OEFFNE ICH DIE EINSTELLUNGEN?", 38, 92);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font2);
        _canvas->drawString("Im Fahr-Dashboard oben rechts auf das Batterie / Profil-Badge tippen!", 38, 126);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("Hier kannst du Geschwindigkeit (km/h), Leistung (Watt), Display & PIN anpassen.", 38, 154);
        _canvas->drawString("Oder jederzeit zurueckkehren mit einem Klick auf [< DASH] oben links.", 38, 178);

        // Card 2: Wie aktiviere ich Tuning & Race-Modus?
        drawCard(20, 220, 760, 148, COLOR_SURFACE, 0x8004);
        _canvas->setTextColor(COLOR_RACE, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->drawString("WIE AKTIVIERE ICH TUNING & DEN RACE-MODUS?", 38, 234);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font2);
        _canvas->drawString("GEHEIMER TRICK: Halte die grosse km/h Geschwindigkeits-Zahl fuer 4s gedrueckt!", 38, 268);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("Der diskrete rote Statuspunkt oben rechts erlischt -> Tuning & Dragy Splits sind aktiv.", 38, 296);
        _canvas->setTextColor(COLOR_GREEN, COLOR_SURFACE);
        _canvas->drawString("Sofortige Drosselung (Polizei-Modus): Ein Einzeltipp auf die km/h Zahl sperrt auf 20 km/h!", 38, 324);

        // Bottom Button: Weiter zu PIN Festlegen
        _canvas->fillRoundRect(200, 386, 400, 64, 16, 0x01E8);
        _canvas->drawRoundRect(200, 386, 400, 64, 16, COLOR_CYAN);
        _canvas->setTextColor(COLOR_WHITE, 0x01E8);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->drawString("WEITER: PIN FESTLEGEN  ->", 400, 418);
    }
    else if (_wizard_step == 1) {
        // STEP 2 OF 3: CHOOSE SECURITY PIN
        _canvas->fillRoundRect(20, 14, 760, 52, 12, 0x18C3);
        _canvas->drawRoundRect(20, 14, 760, 52, 12, 0x39C7);
        _canvas->setTextColor(COLOR_WHITE, 0x18C3);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->drawString("SICHERHEITS-PIN WAEHLEN", 370, 30);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, 0x18C3);
        _canvas->drawString("Ersteinrichtung - Schritt 2 von 3: Wegfahrsperre", 370, 52);

        // Top-Right [ UEBERSPRINGEN ] button
        _canvas->fillRoundRect(615, 20, 155, 40, 8, 0x0821);
        _canvas->drawRoundRect(615, 20, 155, 40, 8, COLOR_CYAN);
        _canvas->setTextColor(COLOR_CYAN, 0x0821);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->drawString("UEBERSPRINGEN >", 692, 40);

        // PIN Keypad Card
        int mx = 180, my = 76, mw = 440, mh = 388;
        drawCard(mx, my, mw, mh, COLOR_SURFACE, COLOR_BORDER);

        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->drawString("Gib deinen gewuenschten 4-stelligen PIN ein:", 400, 98);

        // 4 Digit Boxes
        int pinLen = strlen(_wizard_pin_buf);
        for (int i = 0; i < 4; i++) {
            int bx = 265 + i * 70;
            int by = 118;
            _canvas->fillRoundRect(bx, by, 58, 48, 8, 0x0821);
            _canvas->drawRoundRect(bx, by, 58, 48, 8, (i < pinLen) ? COLOR_CYAN : COLOR_BORDER);
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextColor(COLOR_WHITE, 0x0821);
            if (i < pinLen) {
                char chStr[2] = { _wizard_pin_buf[i], '\0' };
                _canvas->drawString(chStr, bx + 29, by + 24);
            } else {
                _canvas->drawString("-", bx + 29, by + 24);
            }
        }

        // Keypad buttons
        int kw = 84, kh = 44;
        int kx0 = mx + (mw - (3 * kw + 2 * 14)) / 2;
        int ky0 = 176;

        static const char KEY_LBL[4][3] = {
            { '1', '2', '3' },
            { '4', '5', '6' },
            { '7', '8', '9' },
            { '<', '0', '#' }
        };

        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 3; c++) {
                int bx = kx0 + c * (kw + 14);
                int by = ky0 + r * (kh + 8);
                char key = KEY_LBL[r][c];
                uint16_t bg = (key == '#') ? ((pinLen == 4) ? 0x03E0 : 0x18C3) : ((key == '<') ? 0x4002 : 0x18C3);
                uint16_t border = (key == '#') ? ((pinLen == 4) ? COLOR_GREEN : COLOR_BORDER) : ((key == '<') ? COLOR_RED : 0x39C7);
                _canvas->fillRoundRect(bx, by, kw, kh, 8, bg);
                _canvas->drawRoundRect(bx, by, kw, kh, 8, border);
                _canvas->setTextColor(COLOR_WHITE, bg);
                _canvas->setFont(&fonts::Font4);
                char str[8] = { key, '\0' };
                if (key == '<') strcpy(str, "DEL");
                else if (key == '#') strcpy(str, (pinLen == 4) ? "OK" : "SKIP");
                _canvas->drawString(str, bx + kw / 2, by + kh / 2);
            }
        }

        // Bottom continue / skip button
        if (pinLen == 4) {
            _canvas->fillRoundRect(mx + 20, 396, mw - 40, 52, 12, 0x03E0);
            _canvas->drawRoundRect(mx + 20, 396, mw - 40, 52, 12, COLOR_GREEN);
            _canvas->setTextColor(COLOR_WHITE, 0x03E0);
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->drawString("PIN UEBERNEHMEN & WEITER ->", 400, 422);
        } else {
            _canvas->fillRoundRect(mx + 20, 396, mw - 40, 52, 12, 0x18C3);
            _canvas->drawRoundRect(mx + 20, 396, mw - 40, 52, 12, 0x39C7);
            _canvas->setTextColor(COLOR_CYAN, 0x18C3);
            _canvas->setFont(&fonts::Font4);
            _canvas->setTextDatum(textdatum_t::middle_center);
            _canvas->drawString("UEBERSPRINGEN (OHNE PIN) ->", 400, 422);
        }
    }
    else if (_wizard_step == 2) {
        // STEP 3 OF 3: DASHBOARD FEATURES & HARDWARE
        _canvas->fillRoundRect(20, 14, 760, 52, 12, 0x0188);
        _canvas->drawRoundRect(20, 14, 760, 52, 12, COLOR_CYAN);
        _canvas->setTextColor(COLOR_WHITE, 0x0188);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->drawString("DASHBOARD FEATURES ANPASSEN", 400, 30);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_CYAN, 0x0188);
        _canvas->drawString("Ersteinrichtung - Schritt 3 von 3: Hardware-Ausstattung", 400, 52);

        // Toggle 1: GPS & Live-Uhrzeit
        drawCard(20, 76, 760, 84, COLOR_SURFACE, COLOR_BORDER);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->drawString("GPS & LIVE-UHRZEIT", 38, 88);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("Zeigt die Uhrzeit oben zentriert & Satelliten-Icon bei 3D-Fix unter km/h.", 38, 120);

        bool gpsOn = Settings.get().gps_enabled;
        _canvas->fillRoundRect(550, 90, 205, 54, 10, gpsOn ? 0x03E0 : 0x18C3);
        _canvas->drawRoundRect(550, 90, 205, 54, 10, gpsOn ? COLOR_GREEN : COLOR_BORDER);
        _canvas->setTextColor(gpsOn ? COLOR_WHITE : COLOR_MUTED_GRAY, gpsOn ? 0x03E0 : 0x18C3);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->drawString(gpsOn ? "[ AKTIV (AN) ]" : "[ AUS ]", 652, 117);

        // Toggle 2: Neigung & G-Force
        drawCard(20, 170, 760, 84, COLOR_SURFACE, COLOR_BORDER);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->drawString("NEIGUNG & G-FORCE (IMU)", 38, 182);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("Zeigt Gyro-Nickwinkel & Beschleunigungskraefte unten links.", 38, 214);

        bool incOn = Settings.get().incline_gforce_enabled;
        _canvas->fillRoundRect(550, 184, 205, 54, 10, incOn ? 0x01E8 : 0x18C3);
        _canvas->drawRoundRect(550, 184, 205, 54, 10, incOn ? COLOR_CYAN : COLOR_BORDER);
        _canvas->setTextColor(incOn ? COLOR_WHITE : COLOR_MUTED_GRAY, incOn ? 0x01E8 : 0x18C3);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->drawString(incOn ? "[ ANZEIGEN ]" : "[ AUS ]", 652, 211);

        // Toggle 3: Kein Bluetooth? (WiFi-Bridge)
        drawCard(20, 264, 760, 84, COLOR_SURFACE, COLOR_BORDER);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
        _canvas->drawString("KEIN BLUETOOTH-MODUL (WIFI-BRIDGE)", 38, 276);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("Aktiviert die SoftAP WiFi-Bridge als kabellosen Fallback fuer VESC Tool.", 38, 308);

        bool wifiOn = Settings.get().wifi_bridge_enabled;
        _canvas->fillRoundRect(550, 278, 205, 54, 10, wifiOn ? 0x31A0 : 0x18C3);
        _canvas->drawRoundRect(550, 278, 205, 54, 10, wifiOn ? COLOR_AMBER : COLOR_BORDER);
        _canvas->setTextColor(wifiOn ? COLOR_WHITE : COLOR_MUTED_GRAY, wifiOn ? 0x31A0 : 0x18C3);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->drawString(wifiOn ? "[ AKTIVIERT ]" : "[ STANDARD (BLE) ]", 652, 305);

        // Final Button: Setup abschliessen & losfahren
        _canvas->fillRoundRect(160, 372, 480, 68, 16, 0x03E0);
        _canvas->drawRoundRect(160, 372, 480, 68, 16, COLOR_GREEN);
        _canvas->setTextColor(COLOR_WHITE, 0x03E0);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->drawString("SETUP ABSCHLIESSEN & STARTEN", 400, 406);
    }
}

bool DashboardRenderer::handleWizardTouch(int16_t tx, int16_t ty, DashTelemetry &telemetry) {
    if (tx < 0 || ty < 0) return false;

    if (_wizard_step == 0) {
        // Next button: x = 180..620, y = 380..460
        if (tx >= 180 && tx <= 620 && ty >= 380 && ty <= 460) {
            _wizard_step = 1;
            telemetry.wizard_step = 1;
            _screen_dirty = true;
            return true;
        }
    }
    else if (_wizard_step == 1) {
        int mx = 180, mw = 440;

        // 1. Top-Right [ UEBERSPRINGEN > ] Button
        if (tx >= 600 && tx <= 780 && ty >= 14 && ty <= 68) {
            Settings.get().pin_lock_enabled = false;
            Settings.save();
            _wizard_step = 2;
            telemetry.wizard_step = 2;
            _screen_dirty = true;
            return true;
        }

        // 2. Keypad buttons
        int c = -1;
        if (tx >= 240 && tx < 351) c = 0;
        else if (tx >= 351 && tx < 449) c = 1;
        else if (tx >= 449 && tx <= 560) c = 2;

        int r = -1;
        if (ty >= 160 && ty < 224) r = 0;
        else if (ty >= 224 && ty < 276) r = 1;
        else if (ty >= 276 && ty < 328) r = 2;
        else if (ty >= 328 && ty <= 385) r = 3;

        if (r >= 0 && c >= 0) {
            static const char KEY_LBL[4][3] = {
                { '1', '2', '3' },
                { '4', '5', '6' },
                { '7', '8', '9' },
                { '<', '0', '#' }
            };
            char key = KEY_LBL[r][c];
            int len = strlen(_wizard_pin_buf);
            if (key == '<') { // DEL
                if (len > 0) {
                    _wizard_pin_buf[len - 1] = '\0';
                    strncpy(telemetry.wizard_pin_buf, _wizard_pin_buf, sizeof(telemetry.wizard_pin_buf));
                    _screen_dirty = true;
                }
            } else if (key == '#') { // OK / Continue / Skip
                if (len == 4) {
                    strncpy(Settings.get().pin_code, _wizard_pin_buf, sizeof(Settings.get().pin_code));
                    Settings.get().pin_lock_enabled = true;
                    Settings.save();
                    _wizard_step = 2;
                    telemetry.wizard_step = 2;
                    _screen_dirty = true;
                } else if (len == 0) {
                    Settings.get().pin_lock_enabled = false;
                    Settings.save();
                    _wizard_step = 2;
                    telemetry.wizard_step = 2;
                    _screen_dirty = true;
                }
            } else if (key >= '0' && key <= '9') {
                if (len < 4) {
                    _wizard_pin_buf[len] = key;
                    _wizard_pin_buf[len + 1] = '\0';
                    strncpy(telemetry.wizard_pin_buf, _wizard_pin_buf, sizeof(telemetry.wizard_pin_buf));
                    _screen_dirty = true;
                }
            }
            return true;
        }

        // 3. Bottom continue / skip button
        if (tx >= mx + 10 && tx <= mx + mw - 10 && ty >= 390 && ty <= 455) {
            if (strlen(_wizard_pin_buf) == 4) {
                strncpy(Settings.get().pin_code, _wizard_pin_buf, sizeof(Settings.get().pin_code));
                Settings.get().pin_lock_enabled = true;
            } else {
                Settings.get().pin_lock_enabled = false;
            }
            Settings.save();
            _wizard_step = 2;
            telemetry.wizard_step = 2;
            _screen_dirty = true;
            return true;
        }
    }
    else if (_wizard_step == 2) {
        // Toggle 1: GPS (y: 80..155)
        if (tx >= 530 && tx <= 770 && ty >= 80 && ty <= 155) {
            Settings.get().gps_enabled = !Settings.get().gps_enabled;
            Settings.save();
            telemetry.gps_enabled = Settings.get().gps_enabled;
            _screen_dirty = true;
            return true;
        }
        // Toggle 2: Neigung & G-Force (y: 175..250)
        if (tx >= 530 && tx <= 770 && ty >= 175 && ty <= 250) {
            Settings.get().incline_gforce_enabled = !Settings.get().incline_gforce_enabled;
            Settings.save();
            telemetry.incline_gforce_enabled = Settings.get().incline_gforce_enabled;
            _screen_dirty = true;
            return true;
        }
        // Toggle 3: WiFi Bridge (y: 270..345)
        if (tx >= 530 && tx <= 770 && ty >= 270 && ty <= 345) {
            Settings.get().wifi_bridge_enabled = !Settings.get().wifi_bridge_enabled;
            Settings.save();
            telemetry.wifi_bridge_enabled = Settings.get().wifi_bridge_enabled;
            _screen_dirty = true;
            return true;
        }
        // Final button: Setup abschliessen & losfahren (y: 365..450)
        if (tx >= 140 && tx <= 660 && ty >= 365 && ty <= 450) {
            Settings.get().setup_completed = true;
            Settings.save();
            telemetry.setup_completed = true;
            telemetry.screen = SCREEN_RIDE_DASH;
            _wizard_inited = false;
            _wizard_step = 0;
            _wizard_pin_buf[0] = '\0';
            _screen_dirty = true;
            invalidateCache();
            triggerNeedleSweep();
            return true;
        }
    }

    return false;
}

// ==============================================================================
// SCREEN 8: ANIMATED BATTERY CHARGING HUD WITH DRIVE LOCKOUT
// ==============================================================================
void DashboardRenderer::initChargingScreen() {
    _charging_inited = true;
    _last_charge_pct = -1.0f;
    _last_charge_angle = 135.0f;
    _last_charge_min = -1;
    _last_charge_w = -1;
    _last_charge_v_x10 = -1;

    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setTextPadding(0);
    _canvas->fillScreen(COLOR_BG);

    // 1. Navigation Button [< DASH] at (x=20, y=14, w=110, h=42)
    drawCard(20, 14, 110, 42, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->drawString("[< DASH]", 75, 35);

    // 2. Header Badge
    drawCard(240, 14, 320, 42, 0x0A24, COLOR_GREEN);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_GREEN, 0x0A24);
    _canvas->drawString("AKKU-LADEMODUS", 400, 35);

    // 3. Subtitle / Lockout Notice (Comfortable vertical gap above gauge)
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(0xFCA5, COLOR_BG);
    _canvas->drawString("FAHRSPERRE AKTIV - MOTORBLOCKIERUNG", 400, 68);

    // 4. Center Circular Gauge Track (matching speedometer 270° arc!)
    int cx = 400, cy = 224;
    int r_in = 114, r_out = 138;
    float mid_r = 126.0f;
    float deg2rad = 3.14159265f / 180.0f;
    int s_cap_x = cx + (int)roundf(cosf(135.0f * deg2rad) * mid_r);
    int s_cap_y = cy + (int)roundf(sinf(135.0f * deg2rad) * mid_r);
    int e_cap_x = cx + (int)roundf(cosf(405.0f * deg2rad) * mid_r);
    int e_cap_y = cy + (int)roundf(sinf(405.0f * deg2rad) * mid_r);

    _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, 405.0f, COLOR_TRACK_TEAL);
    _canvas->fillCircle(s_cap_x, s_cap_y, 12, COLOR_TRACK_TEAL);
    _canvas->fillCircle(e_cap_x, e_cap_y, 12, COLOR_TRACK_TEAL);

    // 5. Bottom Info Card Outline
    drawCard(120, 380, 560, 82, 0x0823, 0x1945);
}

void DashboardRenderer::renderChargingScreen(const DashTelemetry &telemetry) {
    int cx = 400, cy = 224;
    int r_in = 114, r_out = 138;
    float mid_r = 126.0f;
    float deg2rad = 3.14159265f / 180.0f;
    int s_cap_x = cx + (int)roundf(cosf(135.0f * deg2rad) * mid_r);
    int s_cap_y = cy + (int)roundf(sinf(135.0f * deg2rad) * mid_r);

    float pct = telemetry.battery_pct;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    float frac = pct / 100.0f;

    float target_angle = 135.0f + (frac * 270.0f);
    if (target_angle < 135.0f) target_angle = 135.0f;
    if (target_angle > 405.0f) target_angle = 405.0f;

    uint16_t arcCol = (pct >= 90.0f) ? COLOR_GREEN : ((pct >= 25.0f) ? COLOR_CYAN : COLOR_AMBER);

    // 1. Arc & Center Percentage Update
    if (fabsf(pct - _last_charge_pct) >= 0.5f || !_charging_inited) {
        _last_charge_pct = pct;

        // Clear active arc track cleanly
        _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, 405.0f, COLOR_TRACK_TEAL);
        _canvas->fillCircle(s_cap_x, s_cap_y, 12, COLOR_TRACK_TEAL);
        int e_cap_x = cx + (int)roundf(cosf(405.0f * deg2rad) * mid_r);
        int e_cap_y = cy + (int)roundf(sinf(405.0f * deg2rad) * mid_r);
        _canvas->fillCircle(e_cap_x, e_cap_y, 12, COLOR_TRACK_TEAL);

        if (target_angle > 135.0f) {
            _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, target_angle, arcCol);
            _canvas->fillCircle(s_cap_x, s_cap_y, 12, arcCol);
            int tip_x = cx + (int)roundf(cosf(target_angle * deg2rad) * mid_r);
            int tip_y = cy + (int)roundf(sinf(target_angle * deg2rad) * mid_r);
            _canvas->fillCircle(tip_x, tip_y, 12, arcCol);
            _canvas->fillCircle(tip_x, tip_y, 5, COLOR_WHITE);
        }

        // 2. Center Battery Percentage & Unit (sized safely within r_in=114px)
        _canvas->fillRect(cx - 75, cy - 50, 150, 105, COLOR_BG);
        char pctBuf[16];
        snprintf(pctBuf, sizeof(pctBuf), "%d", (int)(pct + 0.5f));
        drawHeroDigitText(_canvas, pctBuf, cx, cy - 14, textdatum_t::middle_center, arcCol, COLOR_BG);

        _canvas->setFont(&fonts::Font4);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->drawString("%", cx, cy + 30);

        // Live Voltage & Power
        char rateBuf[48];
        snprintf(rateBuf, sizeof(rateBuf), "%.1f V  |  %.0f W  |  %.1f A",
                 telemetry.voltage, telemetry.charge_power_watts, telemetry.charge_current_amps);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
        _canvas->drawString(rateBuf, cx, cy + 48);
    }

    // 3. Bottom Card Update
    int est_min = (int)(telemetry.charge_est_minutes + 0.5f);
    int pwr_w = (int)(telemetry.charge_power_watts + 0.5f);
    int v_x10 = (int)(telemetry.voltage * 10.0f + 0.5f);

    if (est_min != _last_charge_min || pwr_w != _last_charge_w || v_x10 != _last_charge_v_x10) {
        _last_charge_min = est_min;
        _last_charge_w = pwr_w;
        _last_charge_v_x10 = v_x10;

        _canvas->fillRect(124, 384, 552, 74, 0x0823);

        char timeBuf[64];
        if (pct >= 99.5f) {
            snprintf(timeBuf, sizeof(timeBuf), "AKKU VOLL GELADEN (100%%)");
        } else if (est_min < 60) {
            snprintf(timeBuf, sizeof(timeBuf), "RESTZEIT: ca. %d min bis voll", (est_min > 0) ? est_min : 1);
        } else {
            int h = est_min / 60;
            int m = est_min % 60;
            snprintf(timeBuf, sizeof(timeBuf), "RESTZEIT: ca. %dh %02dm bis voll", h, m);
        }

        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_GREEN, 0x0823);
        _canvas->drawString(timeBuf, 400, 406);

        char capBuf[80];
        snprintf(capBuf, sizeof(capBuf), "Ladevorgang aktiv (%.0f W)  |  %.1fV  |  Wegfahrsperre aktiv",
                 telemetry.charge_power_watts, telemetry.voltage);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, 0x0823);
        _canvas->drawString(capBuf, 400, 436);
    }
}
