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
      _menu_in_sub(false),
      _menu_root_idx(0),
      _menu_sub_idx(0),
      _menu_edit_mode(false),
      _menu_last_activity_ms(0),
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
      _anim_inited(false) {
    _last_needle.valid = false;
    _cache.invalidate();
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
    // 0. Boot PIN-Lockscreen check (Requirement 3 & 7)
    if (telemetry.is_locked || ModeManager::instance().isLocked()) {
        renderPinLockScreen(telemetry);
        return;
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
            if (_screen_dirty) {
                _canvas->fillScreen(COLOR_BG);
                initLightsScreen(telemetry);
                _screen_dirty = false;
            }
            renderLightsScreen(telemetry);
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
                _canvas->fillScreen(COLOR_BG);
                initSettingsScreen(telemetry);
                _screen_dirty = false;
                _menu_dirty = true;
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

        default:
            break;
    }

    // Requirement 2: Visual Indicator (Small subtle red dot in top right corner)
    // When tuningMode == false (Stealth / Drossel Mode): permanently lit small red dot
    // When tuningMode == true (Tuning Mode): completely vanishes
    if (!telemetry.tuning_mode) {
        _canvas->fillCircle(790, 10, 4, COLOR_RED);
    } else {
        _canvas->fillCircle(790, 10, 5, COLOR_BG);
    }
}

// ==============================================================================
// SCREEN 0: MAIN RIDE DASHBOARD
// ==============================================================================
void DashboardRenderer::initRideDashboard(const DashTelemetry &telemetry) {
    if (_active_style == STYLE_HYPER_35KW) {
        initHyper35kwStyle(telemetry);
    } else if (_active_style == STYLE_ANALOG_DIAL) {
        initLeftHugAnalogStyle(telemetry);
    } else {
        initHorizontalBarStyle(telemetry);
    }
}

void DashboardRenderer::renderRideDashboard(const DashTelemetry &telemetry) {
    if (_active_style == STYLE_HYPER_35KW) {
        renderHyper35kwStyle(telemetry);
    } else if (_active_style == STYLE_ANALOG_DIAL) {
        renderLeftHugAnalogStyle(telemetry);
    } else {
        renderHorizontalBarStyle(telemetry);
    }
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
}

void DashboardRenderer::renderHyper35kwStyle(const DashTelemetry &telemetry) {
    bool banner_now = (millis() < telemetry.mode_badge_until);
    if (banner_now != _mode_banner_active) {
        _mode_banner_active = banner_now;
        _canvas->fillScreen(COLOR_BG);
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
        _anim_speed += (telemetry.speed_kmh - _anim_speed) * k;
        _anim_power += (telemetry.power_watts - _anim_power) * k;
        _anim_duty  += (telemetry.duty_cycle_pct - _anim_duty) * k;
    }
    if (telemetry.speed_kmh == 0.0f) _anim_speed = 0.0f;
    if (telemetry.power_watts == 0.0f) _anim_power = 0.0f;
    if (telemetry.duty_cycle_pct == 0.0f) _anim_duty = 0.0f;

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

    // Speedometer Active Arc with 60 FPS Fluid Interpolation
    float spd = _is_sweeping ? _sweep_speed : telemetry.speed_kmh;
    if (spd < 0.0f) spd = 0.0f;
    float spd_anim = (_is_sweeping) ? _sweep_speed : ((_anim_speed > 0.0f) ? _anim_speed : spd);
    if (spd_anim < 0.0f) spd_anim = 0.0f;
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

    if (frac > 0.005f) {
        float sweep_end = 135.0f + (frac * 270.0f);
        _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, sweep_end, arcCol);
        _canvas->fillCircle(s_cap_x, s_cap_y, 13, arcCol);
        int tip_x = cx + (int)roundf(cosf(sweep_end * deg2rad) * mid_r);
        int tip_y = cy + (int)roundf(sinf(sweep_end * deg2rad) * mid_r);
        _canvas->fillCircle(tip_x, tip_y, 13, arcCol);
        // Seamless bright inner tracer dot (radius 5 nested cleanly inside track)
        _canvas->fillCircle(tip_x, tip_y, 5, COLOR_WHITE);
    }

    // 2. Optional Rider Tag inside top of gauge (only drawn if set)
    if (telemetry.rider_tag[0] != '\0') {
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::top_center);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->drawString(telemetry.rider_tag, cx, 110);
    }

    // 4. Hero Digital Speed (Smooth Modern 1:1 Native Resolution Vector Font - No 2x Pixel Doubling)
    _canvas->fillRect(cx - 110, 140, 220, 102, COLOR_BG);
    char spdBuf[16];
    snprintf(spdBuf, sizeof(spdBuf), "%.0f", spd);
    uint16_t spdCol = telemetry.boost_active ? COLOR_CYAN : ((spd >= 85.0f) ? COLOR_RED : COLOR_WHITE);
    drawHeroDigitText(_canvas, spdBuf, cx, 195, textdatum_t::middle_center, spdCol, COLOR_BG);

    _canvas->setFont(&fonts::Font4);
    _canvas->setTextSize(1);
    _canvas->setTextDatum(textdatum_t::top_center);
    _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
    _canvas->drawString("km/h", cx, 245);

    // 5. Controller & Motor Thermals (Centered under Speedometer dial)
    _canvas->fillRect(100, 390, 280, 56, COLOR_BG);
    drawThermalsWithDegrees(_canvas, "", telemetry.temp_esc, telemetry.temp_motor, cx, 405, true);

    // 6. Right Column: Top Distance / Odometer (Removed - clean blank space above power)
    if (telemetry.fault_code == 0) {
        _canvas->fillRect(640, 10, 135, 40, COLOR_BG);

        // 7. Right Column: Hero Power Readout (with Emerald Green Regen indication - Smooth 1:1 Font)
        _canvas->fillRect(460, 42, 315, 78, COLOR_BG);
        float pwr = telemetry.power_watts;
        char pStr[16];
        snprintf(pStr, sizeof(pStr), "%.0f", pwr);
        uint16_t pwrCol = (pwr < -10.0f) ? COLOR_GREEN : (telemetry.boost_active ? COLOR_CYAN : COLOR_WHITE);
        drawHeroDigitText(_canvas, pStr, 720, 50, textdatum_t::top_right, pwrCol, COLOR_BG);

        _canvas->setFont(&fonts::Font4);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(pwrCol, COLOR_BG);
        _canvas->drawString("W", 726, 86);
    } else {
        // When VESC fault banner is active across the top, keep power area blank
        _canvas->fillRect(460, 42, 315, 78, COLOR_BG);
    }

    // 8. Right Column: Duty Cycle Section (with leading-edge glow)
    _canvas->fillRect(440, 122, 335, 46, COLOR_BG);
    float duty = telemetry.duty_cycle_pct;
    if (duty < 0.0f) duty = 0.0f;
    float duty_anim = (_anim_duty > 0.0f) ? _anim_duty : duty;
    if (duty_anim < 0.0f) duty_anim = 0.0f;
    bool is_fw = (duty > 100.0f);
    uint16_t dutyCol = is_fw ? COLOR_RED : COLOR_CYAN;

    _canvas->setFont(&fonts::Font4);
    _canvas->setTextSize(1);
    _canvas->setTextColor(dutyCol, COLOR_BG);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->drawString("DUTY", 445, 124);

    _canvas->setTextDatum(textdatum_t::top_right);
    char dBuf[16];
    snprintf(dBuf, sizeof(dBuf), "%.0f%%", duty);
    _canvas->drawString(dBuf, 765, 124);

    // Duty Progress Bar (320px wide from x=445 to x=765)
    _canvas->fillRoundRect(445, 150, 320, 10, 5, COLOR_TRACK_TEAL);
    float dFrac = (duty_anim > 100.0f) ? 1.0f : (duty_anim / 100.0f);
    int dFillW = (int)(dFrac * 320.0f);
    if (dFillW > 0) {
        _canvas->fillRoundRect(445, 150, dFillW, 10, 5, dutyCol);
        if (dFillW >= 6) {
            _canvas->fillRoundRect(445 + dFillW - 4, 150, 4, 10, 2, COLOR_WHITE);
        }
    }

    // 9. Right Column: Dynamic Mode Banner Overlay OR Battery / Currents Readout
    const char *mNames[] = {"ECO", "SPORT", "RACE"};
    uint16_t mCols[] = {COLOR_GREEN, COLOR_CYAN, COLOR_RACE};
    int mIdx = (int)telemetry.ride_mode % 3;
    uint16_t mCol = mCols[mIdx];

    const char *tagStr = telemetry.tuning_mode ? "RIDE MODE ACTIVATED" : "DRIVE MODE ACTIVE";
    const char *nameStr = mNames[mIdx];
    uint16_t bannerCol = mCol;

    if (banner_now) {
        int bX = 410, bY = 170, bW = 365, bH = 108;
        _canvas->fillRect(bX, bY, bW, bH, COLOR_BG);

        // Sleek Floating HUD Mode Card
        int cX = 430, cY = 174, cW = 335, cH = 100;
        _canvas->fillRoundRect(cX, cY, cW, cH, 14, COLOR_BG);
        _canvas->drawRoundRect(cX, cY, cW, cH, 14, bannerCol);
        _canvas->drawRoundRect(cX + 1, cY + 1, cW - 2, cH - 2, 13, bannerCol);

        // Top tag
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
        _canvas->drawString(tagStr, cX + cW / 2, cY + 22);

        // Hero Mode Name
        _canvas->setFont(&fonts::FreeSansBold24pt7b);
        _canvas->setTextSize(1);
        _canvas->setTextColor(bannerCol, COLOR_BG);
        _canvas->drawString(nameStr, cX + cW / 2, cY + 54);

        // Animated smooth duration bar along bottom
        int32_t rem_ms = (int32_t)(telemetry.mode_badge_until - millis());
        float anim_p = (rem_ms > 0 && rem_ms <= 2000) ? ((float)rem_ms / 2000.0f) : 1.0f;
        int pBarW = (int)(anim_p * (cW - 40));
        _canvas->fillRoundRect(cX + 20, cY + cH - 12, cW - 40, 3, 1, 0x18C6);
        if (pBarW > 0) {
            _canvas->fillRoundRect(cX + 20, cY + cH - 12, pBarW, 3, bannerCol);
        }

        _canvas->setFont(&fonts::Font4);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::top_left);
    } else {
        // Battery Percentage & Voltage
        _canvas->fillRect(440, 170, 140, 42, COLOR_BG);
        char batBuf[16];
        snprintf(batBuf, sizeof(batBuf), "%.0f%%", telemetry.battery_pct);
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

        // Battery Bar Fill with leading-edge glow
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

        // Currents
        _canvas->fillRect(410, 246, 368, 34, COLOR_BG);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextSize(1);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->setTextDatum(textdatum_t::top_left);
        char motBuf[32];
        snprintf(motBuf, sizeof(motBuf), "MOTOR %.0f A", telemetry.phase_amps);
        _canvas->drawString(motBuf, 415, 250);

        _canvas->setTextDatum(textdatum_t::top_right);
        char bBuf[32];
        snprintf(bBuf, sizeof(bBuf), "BATTERY %.0f A", telemetry.current_amps);
        _canvas->drawString(bBuf, 768, 250);
    }

    // 9b. Dynamic Range Readout (Open minimalist layout without card container)
    _canvas->fillRect(435, 286, 345, 88, COLOR_BG);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
    _canvas->drawString("RESTREICHWEITE", 445, 298);

    _canvas->setFont(&fonts::FreeSansBold18pt7b);
    _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    char rngBuf[16];
    if (telemetry.est_range_km < 0.0f) {
        snprintf(rngBuf, sizeof(rngBuf), "-- km");
    } else {
        snprintf(rngBuf, sizeof(rngBuf), "%.0f km", telemetry.est_range_km);
    }
    _canvas->drawString(rngBuf, 445, 322);

    // 10. Right Column: Bottom Touch Action Buttons
    _canvas->setTextDatum(textdatum_t::top_left);
    // Button 1: [ BOOST ]
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

    // Button 2: [ ECO ] / [ SPORT ] / [ RACE ] (Always shows active mode)
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

// ==============================================================================
// STYLE A: LEFT-HUG ANALOG DUTY CLUSTER (800x480 Optimized)
// ==============================================================================
// Center: cx = 50, cy = 240 (Screen vertical midpoint). Hub radius: 44.
// Digital duty readout is mounted INSIDE the hub circle.
// Needle radiates outward to r=145, well clear of scale numbers (r=172).
// Full 800x480 resolution utilization: Bottom strip at y=435, Top ribbon at y=14.
// ==============================================================================
void DashboardRenderer::initLeftHugAnalogStyle(const DashTelemetry &telemetry) {
    int cx = 50, cy = 240;

    // 1. Draw Gauge Bezel Arc (Convex arc to the right)
    for (int i = 0; i < 24; i++) {
        float frac1 = (float)i / 24.0f;
        float frac2 = (float)(i + 1) / 24.0f;
        float ang1 = 1.10f - (frac1 * 2.20f);
        float ang2 = 1.10f - (frac2 * 2.20f);

        int ax1 = cx + (int)(cosf(ang1) * 210);
        int ay1 = cy + (int)(sinf(ang1) * 210);
        int ax2 = cx + (int)(cosf(ang2) * 210);
        int ay2 = cy + (int)(sinf(ang2) * 210);

        uint16_t arcCol = (i >= 20) ? COLOR_RED : COLOR_BORDER;
        _canvas->drawLine(ax1, ay1, ax2, ay2, arcCol);
        _canvas->drawLine(ax1 + 1, ay1, ax2 + 1, ay2, arcCol);
    }

    // 2. Draw Ticks & Scale Numbers (Font4)
    for (int i = 0; i <= 24; i++) {
        float frac = (float)i / 24.0f;
        float ang = 1.10f - (frac * 2.20f);
        bool isMajor = (i % 4 == 0); // 0, 20, 40, 60, 80, 100, 120
        int tickLen = isMajor ? 18 : 10;

        int x1 = cx + (int)(cosf(ang) * 210);
        int y1 = cy + (int)(sinf(ang) * 210);
        int x2 = cx + (int)(cosf(ang) * (210 - tickLen));
        int y2 = cy + (int)(sinf(ang) * (210 - tickLen));

        uint16_t tColor = (i >= 20) ? COLOR_RED : ((i >= 16) ? COLOR_AMBER : COLOR_WHITE);
        _canvas->drawLine(x1, y1, x2, y2, tColor);
        if (isMajor) {
            _canvas->drawLine(x1, y1 + 1, x2, y2 + 1, tColor);

            int duty_val = i * 5;
            int numX = cx + (int)(cosf(ang) * 172);
            int numY = cy + (int)(sinf(ang) * 172);

            if (duty_val == 120) {
                _canvas->setTextColor(COLOR_RED, COLOR_BG);
                _canvas->drawCenterString("FW", numX, numY - 8, &fonts::Font4);
            } else {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", duty_val);
                _canvas->setTextColor(tColor, COLOR_BG);
                _canvas->drawCenterString(buf, numX, numY - 8, &fonts::Font4);
            }
        }
    }

    // 3. Central Hub Circle (Contains digital duty readout inside)
    _canvas->fillCircle(cx, cy, 44, COLOR_SURFACE);
    _canvas->drawCircle(cx, cy, 44, COLOR_BORDER);
    _canvas->drawCircle(cx, cy, 43, COLOR_BORDER);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawCenterString("DUTY", cx, cy - 22, &fonts::Font2);

    // 4. Right Cockpit - Top Ribbon
    _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    const char *pName = (telemetry.battery_profile_id == 0) ? "[B1 FRESH]" : "[B2 DAILY]";
    _canvas->drawString(pName, 260, 14, &fonts::Font4);

    _canvas->drawFastHLine(250, 44, 530, COLOR_BORDER);

    // 5. Right Cockpit - Static Section Labels
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
    _canvas->drawString("POWER", 265, 60, &fonts::Font4);
    _canvas->drawString("BATTERY", 645, 60, &fonts::Font4);

    _canvas->drawCenterString("KM/H", 520, 172, &fonts::Font4);

    _canvas->drawString("PHASE", 265, 195, &fonts::Font4);
    _canvas->drawString("ENERGY", 645, 195, &fonts::Font4);

    // 6. Throttle Input Track & Label (x=380, y=330, w=220, h=20)
    _canvas->fillRoundRect(380, 330, 220, 20, 4, COLOR_SURFACE);
    _canvas->drawRoundRect(380, 330, 220, 20, 4, COLOR_BORDER);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
    _canvas->drawString("THR", 335, 332, &fonts::Font4);
    _cache.throttle_fill_w = -1;
    _cache.throttle_pct = -1;

    // 7. Bottom Strip Divider
    _canvas->drawFastHLine(20, 415, 760, COLOR_BORDER);
}

void DashboardRenderer::drawAnalogNeedle(int cx, int cy, int length, float angle_rad, uint16_t color) {
    float cos_a = cosf(angle_rad);
    float sin_a = sinf(angle_rad);
    float perp = angle_rad + (M_PI / 2.0f);
    float cos_p = cosf(perp);
    float sin_p = sinf(perp);

    // Needle starts under the hub cap at r=32 with half-width 3.5px
    int b1X = cx + (int)roundf(cos_a * 32.0f + cos_p * 3.5f);
    int b1Y = cy + (int)roundf(sin_a * 32.0f + sin_p * 3.5f);
    int b2X = cx + (int)roundf(cos_a * 32.0f - cos_p * 3.5f);
    int b2Y = cy + (int)roundf(sin_a * 32.0f - sin_p * 3.5f);
    int tipX = cx + (int)roundf(cos_a * (float)length);
    int tipY = cy + (int)roundf(sin_a * (float)length);

    _canvas->fillTriangle(tipX, tipY, b1X, b1Y, b2X, b2Y, color);

    _last_needle.tipX = tipX;
    _last_needle.tipY = tipY;
    _last_needle.b1X  = b1X;
    _last_needle.b1Y  = b1Y;
    _last_needle.b2X  = b2X;
    _last_needle.b2Y  = b2Y;
    _last_needle.valid = true;
}

void DashboardRenderer::renderLeftHugAnalogStyle(const DashTelemetry &telemetry) {
    int cx = 50, cy = 240;
    float currentDuty = _is_sweeping ? _sweep_duty : telemetry.duty_cycle_pct;
    if (currentDuty < 0.0f) currentDuty = 0.0f;
    if (currentDuty > 120.0f) currentDuty = 120.0f;

    // 1. Redraw needle and hub cap when duty angle changes
    static float last_rendered_duty = -999.0f;
    bool duty_changed = !_last_needle.valid || (fabsf(currentDuty - last_rendered_duty) >= 0.25f);

    if (duty_changed) {
        last_rendered_duty = currentDuty;

        // Erase previous needle
        if (_last_needle.valid) {
            _canvas->fillTriangle(_last_needle.tipX, _last_needle.tipY,
                                  _last_needle.b1X, _last_needle.b1Y,
                                  _last_needle.b2X, _last_needle.b2Y,
                                  COLOR_BG);
            _canvas->drawLine(_last_needle.b1X, _last_needle.b1Y, _last_needle.tipX, _last_needle.tipY, COLOR_BG);
            _canvas->drawLine(_last_needle.b2X, _last_needle.b2Y, _last_needle.tipX, _last_needle.tipY, COLOR_BG);
            _canvas->drawLine(_last_needle.b1X, _last_needle.b1Y, _last_needle.b2X, _last_needle.b2Y, COLOR_BG);
        }

        // Draw needle: Angle +63° (bottom 0%) to -63° (top 120% FW), length=145 safely inside scale numbers (172)
        float frac = currentDuty / 120.0f;
        float needle_angle = 1.10f - (frac * 2.20f);
        drawAnalogNeedle(cx, cy, 145, needle_angle, COLOR_NEEDLE);

        // Solid Hub Cap covers the needle pivot completely
        _canvas->fillCircle(cx, cy, 44, COLOR_SURFACE);
        _canvas->drawCircle(cx, cy, 44, COLOR_BORDER);
        _canvas->drawCircle(cx, cy, 43, COLOR_BORDER);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawCenterString("DUTY", cx, cy - 22, &fonts::Font2);

        int duty_int = (int)roundf(currentDuty);
        char dutyBuf[16];
        snprintf(dutyBuf, sizeof(dutyBuf), "%d%%", duty_int);
        uint16_t dCol = getDutyColor(currentDuty);
        _canvas->setTextColor(dCol, COLOR_SURFACE);
        _canvas->setTextPadding(64);
        _canvas->drawCenterString(dutyBuf, cx, cy - 2, &fonts::Font4);
        _canvas->setTextPadding(0);
        _cache.duty_x10 = duty_int;
    }

    // 2. Top Ribbon
    if (telemetry.battery_profile_id != _cache.batt_prof) {
        _cache.batt_prof = telemetry.battery_profile_id;
        const char *pName = (telemetry.battery_profile_id == 0) ? "[B1 FRESH]" : "[B2 DAILY]";
        _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
        _canvas->setTextPadding(140);
        _canvas->drawString(pName, 260, 14, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curVoltX10 = (int)roundf(telemetry.voltage * 10.0f);
    if (curVoltX10 != _cache.voltage_x10) {
        _cache.voltage_x10 = curVoltX10;
        char vStr[20];
        snprintf(vStr, sizeof(vStr), "%.1f V", telemetry.voltage);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->setTextPadding(130);
        _canvas->drawCenterString(vStr, 520, 14, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    if (telemetry.vesc_connected != _cache.vesc_connected) {
        _cache.vesc_connected = telemetry.vesc_connected;
        _canvas->setTextColor(telemetry.vesc_connected ? COLOR_GREEN : COLOR_RED, COLOR_BG);
        _canvas->setTextPadding(110);
        _canvas->drawRightString(telemetry.vesc_connected ? "VESC" : "NO VESC", 780, 14, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    // 3. Giant Speedometer (Font8, 75px)
    int curSpeed = _is_sweeping ? (int)roundf(_sweep_speed) : (int)roundf(telemetry.speed_kmh);
    if (curSpeed != _cache.speed) {
        _cache.speed = curSpeed;
        char spdStr[8];
        snprintf(spdStr, sizeof(spdStr), "%d", curSpeed);
        _canvas->setTextColor(getSpeedColor((float)curSpeed), COLOR_BG);
        _canvas->setTextPadding(180);
        _canvas->drawCenterString(spdStr, 520, 80, &fonts::Font8);
        _canvas->setTextPadding(0);
    }

    // Range Estimate below KM/H (positioned at y=260)
    int curRange = (telemetry.est_range_km < 0.0f) ? -1 : (int)roundf(telemetry.est_range_km);
    if (curRange != _cache.est_range) {
        _cache.est_range = curRange;
        char rStr[32];
        if (curRange < 0) {
            snprintf(rStr, sizeof(rStr), "EST RANGE: N/A");
        } else {
            snprintf(rStr, sizeof(rStr), "EST RANGE: %d km", curRange);
        }
        _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
        _canvas->setTextPadding(240);
        _canvas->drawCenterString(rStr, 520, 260, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    // 4. Left Flank: Power & Phase Amps
    int curWatts = (int)roundf(telemetry.power_watts);
    if (curWatts != _cache.watts) {
        _cache.watts = curWatts;
        char numStr[16];
        uint16_t pCol = getPowerColor(telemetry.power_watts);
        snprintf(numStr, sizeof(numStr), "%d", curWatts);
        drawValWithUnit(265, 88, 75, 45, numStr, "W", pCol, COLOR_LIGHT_GRAY);
    }

    int curPhaseX10 = (int)roundf(telemetry.phase_amps * 10.0f);
    if (curPhaseX10 != _cache.phase_x10) {
        _cache.phase_x10 = curPhaseX10;
        char numStr[16];
        snprintf(numStr, sizeof(numStr), "%.0f", telemetry.phase_amps);
        uint16_t phCol = getPhaseColor(telemetry.phase_amps);
        drawValWithUnit(265, 223, 75, 45, numStr, "A", phCol, COLOR_LIGHT_GRAY);
    }

    // 5. Right Flank: Battery % and Remaining Wh
    int curBat = (telemetry.battery_pct < 0.0f) ? -1 : (int)roundf(telemetry.battery_pct);
    if (curBat != _cache.battery_pct) {
        _cache.battery_pct = curBat;
        if (curBat < 0) {
            drawValWithUnit(645, 88, 75, 45, "N/A", "", COLOR_MUTED_GRAY, COLOR_MUTED_GRAY);
        } else {
            char numStr[16];
            snprintf(numStr, sizeof(numStr), "%d", curBat);
            uint16_t bCol = getBatteryColor(telemetry.battery_pct);
            drawValWithUnit(645, 88, 75, 45, numStr, "%", bCol, bCol);
        }
    }

    int curRemWh = (telemetry.remaining_wh < 0.0f) ? -1 : (int)roundf(telemetry.remaining_wh);
    if (curRemWh != _cache.remaining_wh) {
        _cache.remaining_wh = curRemWh;
        if (curRemWh < 0) {
            drawValWithUnit(645, 223, 85, 45, "N/A", "", COLOR_MUTED_GRAY, COLOR_MUTED_GRAY);
        } else {
            char numStr[16];
            snprintf(numStr, sizeof(numStr), "%d", curRemWh);
            drawValWithUnit(645, 223, 85, 45, numStr, "Wh", COLOR_WHITE, COLOR_LIGHT_GRAY);
        }
    }

    // 6. Throttle Input Bar & Percentage Readout
    drawThrottleBar(380, 330, 220, 20, telemetry.throttle_pct);
    int curThr = (int)roundf(telemetry.throttle_pct);
    if (curThr != _cache.throttle_pct) {
        _cache.throttle_pct = curThr;
        char thStr[8];
        snprintf(thStr, sizeof(thStr), "%d%%", curThr);
        uint16_t thCol = (curThr > 85) ? COLOR_RED : ((curThr > 60) ? COLOR_AMBER : COLOR_CYAN);
        _canvas->setTextColor(thCol, COLOR_BG);
        _canvas->setTextPadding(50);
        _canvas->drawString(thStr, 612, 332, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    // 7. Bottom Status Strip (Positioned at y=435)
    int curTripX10 = (int)roundf(telemetry.trip_km * 10.0f);
    if (curTripX10 != _cache.trip_x10) {
        _cache.trip_x10 = curTripX10;
        char trStr[24];
        snprintf(trStr, sizeof(trStr), "TRIP  %.1f km", telemetry.trip_km);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->setTextPadding(180);
        _canvas->drawString(trStr, 35, 435, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int mTemp = (int)roundf(telemetry.temp_motor);
    int eTemp = (int)roundf(telemetry.temp_esc);
    if (mTemp != _cache.temp_motor || eTemp != _cache.temp_esc) {
        _cache.temp_motor = mTemp;
        _cache.temp_esc = eTemp;
        char tStr[36];
        snprintf(tStr, sizeof(tStr), "MOT %d C   ESC %d C", mTemp, eTemp);
        _canvas->setTextColor(getTempColor(fmaxf(telemetry.temp_motor, telemetry.temp_esc)), COLOR_BG);
        _canvas->setTextPadding(260);
        _canvas->drawCenterString(tStr, 400, 435, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    char sohStr[20];
    snprintf(sohStr, sizeof(sohStr), "SoH  %.0f%%", telemetry.battery_health_soh);
    _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    _canvas->setTextPadding(140);
    _canvas->drawRightString(sohStr, 765, 435, &fonts::Font4);
    _canvas->setTextPadding(0);
}

// ==============================================================================
// STYLE B: HORIZONTAL DUTY BAR CLUSTER (800x480 Optimized)
// ==============================================================================
void DashboardRenderer::initHorizontalBarStyle(const DashTelemetry &telemetry) {
    // 1. Horizontal Duty Bar Track (x=20, y=14, w=760, h=24)
    int bx = 20, by = 14, bw = 760, bh = 24;
    _canvas->fillRoundRect(bx, by, bw, bh, 4, COLOR_SURFACE);
    _canvas->drawRoundRect(bx, by, bw, bh, 4, COLOR_BORDER);

    // Field Weakening Redline Marker at 100% duty (x=653)
    int fw_mark = bx + (int)(100.0f / 120.0f * (float)bw);
    _canvas->drawFastVLine(fw_mark, by, bh, COLOR_RED);

    // Scale Ticks & Font4 Labels at y=42
    for (int i = 0; i <= 100; i += 20) {
        int tx = bx + (int)((float)i / 120.0f * (float)bw);
        _canvas->drawFastVLine(tx, by + bh, 4, (i == 100) ? COLOR_RED : COLOR_MUTED_GRAY);
        char sbuf[8];
        snprintf(sbuf, sizeof(sbuf), "%d%%", i);
        _canvas->setTextColor((i == 100) ? COLOR_RED : COLOR_MUTED_GRAY, COLOR_BG);
        _canvas->drawCenterString(sbuf, tx, by + bh + 4, &fonts::Font4);
    }
    // FW Label placed at 120% duty mark
    _canvas->drawFastVLine(bx + bw, by + bh, 4, COLOR_RED);
    _canvas->setTextColor(COLOR_RED, COLOR_BG);
    _canvas->drawRightString("FW", bx + bw, by + bh + 4, &fonts::Font4);

    // 2. Mid-Deck Divider & Static Labels
    _canvas->drawFastHLine(20, 74, 760, COLOR_BORDER);

    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
    _canvas->drawString("POWER", 35, 92, &fonts::Font4);
    _canvas->drawString("BATTERY", 645, 92, &fonts::Font4);

    _canvas->drawCenterString("KM/H", 400, 200, &fonts::Font4);

    _canvas->drawString("PHASE", 35, 220, &fonts::Font4);
    _canvas->drawString("ENERGY", 645, 220, &fonts::Font4);

    // 3. Throttle Input Track & Label (x=280, y=320, w=240, h=22)
    _canvas->fillRoundRect(280, 320, 240, 22, 4, COLOR_SURFACE);
    _canvas->drawRoundRect(280, 320, 240, 22, 4, COLOR_BORDER);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
    _canvas->drawString("THR", 235, 322, &fonts::Font4);
    _cache.throttle_fill_w = -1;
    _cache.throttle_pct = -1;
    _cache.duty_fill_w = -1;

    // 4. Bottom Divider
    _canvas->drawFastHLine(20, 415, 760, COLOR_BORDER);
}

void DashboardRenderer::drawPreciseDutyBar(int x, int y, int w, int h, float duty, float max_duty) {
    if (duty < 0.0f) duty = 0.0f;
    if (duty > max_duty) duty = max_duty;
    int target_w = (int)roundf((duty / max_duty) * (w - 4));
    if (target_w < 0) target_w = 0;
    if (target_w > w - 4) target_w = w - 4;

    if (target_w != _cache.duty_fill_w) {
        int fw_px = (int)roundf((100.0f / max_duty) * (w - 4));
        int last_w = (_cache.duty_fill_w >= 0) ? _cache.duty_fill_w : 0;

        if (target_w > last_w) {
            for (int px = last_w; px < target_w; px++) {
                uint16_t col = (px >= fw_px) ? COLOR_RED : COLOR_CYAN;
                _canvas->drawFastVLine(x + 2 + px, y + 2, h - 4, col);
            }
        } else if (target_w < last_w) {
            _canvas->fillRect(x + 2 + target_w, y + 2, last_w - target_w, h - 4, COLOR_SURFACE);
        }
        _cache.duty_fill_w = target_w;
    }
}

void DashboardRenderer::drawThrottleBar(int x, int y, int w, int h, float pct) {
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    int target_w = (int)roundf((pct / 100.0f) * (w - 4));
    if (target_w < 0) target_w = 0;
    if (target_w > w - 4) target_w = w - 4;

    if (target_w != _cache.throttle_fill_w) {
        int last_w = (_cache.throttle_fill_w >= 0) ? _cache.throttle_fill_w : 0;

        if (target_w > last_w) {
            for (int px = last_w; px < target_w; px++) {
                float p = (float)px / (float)(w - 4);
                uint16_t col = (p > 0.85f) ? COLOR_RED : ((p > 0.60f) ? COLOR_AMBER : COLOR_CYAN);
                _canvas->drawFastVLine(x + 2 + px, y + 2, h - 4, col);
            }
        } else if (target_w < last_w) {
            _canvas->fillRect(x + 2 + target_w, y + 2, last_w - target_w, h - 4, COLOR_SURFACE);
        }
        _cache.throttle_fill_w = target_w;
    }
}

void DashboardRenderer::renderHorizontalBarStyle(const DashTelemetry &telemetry) {
    // 1. Top Duty Cycle Bar (x=20, y=14, w=760, h=24)
    float currentDuty = _is_sweeping ? _sweep_duty : telemetry.duty_cycle_pct;
    drawPreciseDutyBar(20, 14, 760, 24, currentDuty, 120.0f);

    // 2. Giant Speedometer (Font8, 75px, at y=110)
    int curSpeed = _is_sweeping ? (int)roundf(_sweep_speed) : (int)roundf(telemetry.speed_kmh);
    if (curSpeed != _cache.speed) {
        _cache.speed = curSpeed;
        char spdStr[8];
        snprintf(spdStr, sizeof(spdStr), "%d", curSpeed);
        _canvas->setTextColor(getSpeedColor((float)curSpeed), COLOR_BG);
        _canvas->setTextPadding(180);
        _canvas->drawCenterString(spdStr, 400, 110, &fonts::Font8);
        _canvas->setTextPadding(0);
    }

    int curRange = (telemetry.est_range_km < 0.0f) ? -1 : (int)roundf(telemetry.est_range_km);
    if (curRange != _cache.est_range) {
        _cache.est_range = curRange;
        char rStr[32];
        if (curRange < 0) {
            snprintf(rStr, sizeof(rStr), "EST RANGE: N/A");
        } else {
            snprintf(rStr, sizeof(rStr), "EST RANGE: %d km", curRange);
        }
        _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
        _canvas->setTextPadding(280);
        _canvas->drawCenterString(rStr, 400, 248, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    // 3. Left Flank: Power & Phase Current
    int curWatts = (int)roundf(telemetry.power_watts);
    if (curWatts != _cache.watts) {
        _cache.watts = curWatts;
        char numStr[16];
        uint16_t pCol = getPowerColor(telemetry.power_watts);
        snprintf(numStr, sizeof(numStr), "%d", curWatts);
        drawValWithUnit(35, 120, 115, 50, numStr, "W", pCol, COLOR_LIGHT_GRAY);
    }

    int curPhaseX10 = (int)roundf(telemetry.phase_amps * 10.0f);
    if (curPhaseX10 != _cache.phase_x10) {
        _cache.phase_x10 = curPhaseX10;
        char numStr[16];
        snprintf(numStr, sizeof(numStr), "%.0f", telemetry.phase_amps);
        uint16_t phCol = getPhaseColor(telemetry.phase_amps);
        drawValWithUnit(35, 248, 115, 50, numStr, "A", phCol, COLOR_LIGHT_GRAY);
    }

    // 4. Right Flank: Battery % & Remaining Wh
    int curBat = (telemetry.battery_pct < 0.0f) ? -1 : (int)roundf(telemetry.battery_pct);
    if (curBat != _cache.battery_pct) {
        _cache.battery_pct = curBat;
        if (curBat < 0) {
            drawValWithUnit(645, 120, 85, 45, "N/A", "", COLOR_MUTED_GRAY, COLOR_MUTED_GRAY);
        } else {
            char numStr[16];
            snprintf(numStr, sizeof(numStr), "%d", curBat);
            uint16_t bCol = getBatteryColor(telemetry.battery_pct);
            drawValWithUnit(645, 120, 85, 45, numStr, "%", bCol, bCol);
        }
    }

    int curRemWh = (telemetry.remaining_wh < 0.0f) ? -1 : (int)roundf(telemetry.remaining_wh);
    if (curRemWh != _cache.remaining_wh) {
        _cache.remaining_wh = curRemWh;
        if (curRemWh < 0) {
            drawValWithUnit(645, 248, 115, 50, "N/A", "", COLOR_MUTED_GRAY, COLOR_MUTED_GRAY);
        } else {
            char numStr[16];
            snprintf(numStr, sizeof(numStr), "%d", curRemWh);
            drawValWithUnit(645, 248, 115, 50, numStr, "Wh", COLOR_WHITE, COLOR_LIGHT_GRAY);
        }
    }

    // 5. Throttle Input Bar & Percentage Readout
    drawThrottleBar(280, 320, 240, 22, telemetry.throttle_pct);
    int curThr = (int)roundf(telemetry.throttle_pct);
    if (curThr != _cache.throttle_pct) {
        _cache.throttle_pct = curThr;
        char thStr[8];
        snprintf(thStr, sizeof(thStr), "%d%%", curThr);
        uint16_t thCol = (curThr > 85) ? COLOR_RED : ((curThr > 60) ? COLOR_AMBER : COLOR_CYAN);
        _canvas->setTextColor(thCol, COLOR_BG);
        _canvas->setTextPadding(50);
        _canvas->drawString(thStr, 535, 322, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    // 6. Bottom Status Strip (Positioned at y=435)
    int curTripX10 = (int)roundf(telemetry.trip_km * 10.0f);
    if (curTripX10 != _cache.trip_x10) {
        _cache.trip_x10 = curTripX10;
        char trStr[24];
        snprintf(trStr, sizeof(trStr), "TRIP %.1f km", telemetry.trip_km);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->setTextPadding(150);
        _canvas->drawString(trStr, 25, 435, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    if (telemetry.battery_profile_id != _cache.batt_prof) {
        _cache.batt_prof = telemetry.battery_profile_id;
        const char *pName = (telemetry.battery_profile_id == 0) ? "[B1 FRESH]" : "[B2 DAILY]";
        _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
        _canvas->setTextPadding(130);
        _canvas->drawString(pName, 185, 435, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curVoltX10 = (int)roundf(telemetry.voltage * 10.0f);
    if (curVoltX10 != _cache.voltage_x10) {
        _cache.voltage_x10 = curVoltX10;
        char vStr[20];
        snprintf(vStr, sizeof(vStr), "%.1f V", telemetry.voltage);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->setTextPadding(100);
        _canvas->drawString(vStr, 345, 435, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int mTemp = (int)roundf(telemetry.temp_motor);
    int eTemp = (int)roundf(telemetry.temp_esc);
    if (mTemp != _cache.temp_motor || eTemp != _cache.temp_esc) {
        _cache.temp_motor = mTemp;
        _cache.temp_esc = eTemp;
        char tStr[36];
        snprintf(tStr, sizeof(tStr), "M %d C  E %d C", mTemp, eTemp);
        _canvas->setTextColor(getTempColor(fmaxf(telemetry.temp_motor, telemetry.temp_esc)), COLOR_BG);
        _canvas->setTextPadding(150);
        _canvas->drawString(tStr, 475, 435, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    char sohStr[20];
    snprintf(sohStr, sizeof(sohStr), "SoH %.0f%%", telemetry.battery_health_soh);
    _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    _canvas->setTextPadding(90);
    _canvas->drawRightString(sohStr, 775, 435, &fonts::Font4);
    _canvas->setTextPadding(0);
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
    _canvas->fillScreen(COLOR_BG);

    // Top Bar: Back Button [< DASH], Center Title, Reset Trip Button
    _canvas->fillRoundRect(20, 12, 110, 36, 18, 0x18C3);
    _canvas->drawRoundRect(20, 12, 110, 36, 18, 0x39C7);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font2);
    _canvas->drawString("< DASH", 75, 30);

    _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    _canvas->setFont(&fonts::Font4);
    _canvas->drawString("FAHRDATEN & STATISTIK", 400, 30);

    _canvas->fillRoundRect(640, 12, 140, 36, 18, 0x18C3);
    _canvas->drawRoundRect(640, 12, 140, 36, 18, 0x39C7);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->setFont(&fonts::Font2);
    _canvas->drawString("RESET TRIP", 710, 30);

    // Top Summary Card: Trip & Movement Overview
    drawCard(20, 56, 760, 92, COLOR_SURFACE, COLOR_BORDER);

    // Col 1: STRECKE
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("STRECKE", 36, 66);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    char distBuf[16]; snprintf(distBuf, sizeof(distBuf), "%.1f km", telemetry.trip_km);
    _canvas->drawString(distBuf, 36, 86);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    char odoBuf[16]; snprintf(odoBuf, sizeof(odoBuf), "ODO: %.1f km", telemetry.odo_km);
    _canvas->drawString(odoBuf, 36, 122);

    // Col 2: FAHRZEIT
    _canvas->drawString("FAHRZEIT", 225, 66);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    uint32_t sec = telemetry.stats.ride_time_sec;
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

    // Col 3: MAX SPEED
    _canvas->drawString("MAX SPEED", 415, 66);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    char maxSpd[16]; snprintf(maxSpd, sizeof(maxSpd), "%.1f km/h", telemetry.stats.max_speed_kmh);
    _canvas->drawString(maxSpd, 415, 86);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    char liveSpd[16]; snprintf(liveSpd, sizeof(liveSpd), "Live: %.1f km/h", telemetry.speed_kmh);
    _canvas->drawString(liveSpd, 415, 122);

    // Col 4: ENERGIE
    _canvas->drawString("ENERGIE (TRIP)", 605, 66);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    char whBuf[16]; snprintf(whBuf, sizeof(whBuf), "%.0f Wh", telemetry.stats.trip_wh);
    _canvas->drawString(whBuf, 605, 86);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    char remWh[16]; snprintf(remWh, sizeof(remWh), "Rest: %.0f Wh", telemetry.remaining_wh);
    _canvas->drawString(remWh, 605, 122);

    // Middle Performance Tile Grid (6 Tiles):
    // Row 1: y = 156, h = 98
    // Tile 1: MAX LEISTUNG (PEAK WATT)
    drawCard(20, 156, 244, 98, COLOR_SURFACE, 0xD400); // Glowing gold border
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
    _canvas->drawString("MAX LEISTUNG (PEAK)", 32, 166);
    _canvas->setFont(&fonts::Font4);
    char pwrBuf[16]; snprintf(pwrBuf, sizeof(pwrBuf), "%.0f W", telemetry.stats.peak_power_watts);
    _canvas->drawString(pwrBuf, 32, 188);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Spitzenleistung ab Akku", 32, 226);

    // Tile 2: PEAK AKKU-STROM
    drawCard(278, 156, 244, 98, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("PEAK AKKU-STROM", 290, 166);
    _canvas->setFont(&fonts::Font4);
    char curBuf[16]; snprintf(curBuf, sizeof(curBuf), "%.1f A", telemetry.stats.peak_current_amps);
    _canvas->drawString(curBuf, 290, 188);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Maximaler Batterie-Strom", 290, 226);

    // Tile 3: PEAK PHASENSTROM
    drawCard(536, 156, 244, 98, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_RACE, COLOR_SURFACE);
    _canvas->drawString("PEAK PHASENSTROM", 548, 166);
    _canvas->setFont(&fonts::Font4);
    char phaBuf[16]; snprintf(phaBuf, sizeof(phaBuf), "%.1f A", telemetry.stats.peak_phase_amps);
    _canvas->drawString(phaBuf, 548, 188);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Max. Motor-Drehmoment", 548, 226);

    // Row 2: y = 262, h = 98
    // Tile 4: VERBRAUCH & EFFIZIENZ
    drawCard(20, 262, 244, 98, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("VERBRAUCH & EFFIZIENZ", 32, 272);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    char effBuf[24]; snprintf(effBuf, sizeof(effBuf), "%.1f Wh/km", telemetry.stats.trip_wh_km);
    _canvas->drawString(effBuf, 32, 294);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    float dynWh = (telemetry.stats.dynamic_wh_km > 0.0f) ? telemetry.stats.dynamic_wh_km : telemetry.dynamic_wh_km;
    char dynBuf[32]; snprintf(dynBuf, sizeof(dynBuf), "Rollend (2km): %.1f Wh/km", dynWh);
    _canvas->drawString(dynBuf, 32, 332);

    // Tile 5: AKKU-SPANNUNG (MIN/MAX)
    drawCard(278, 262, 244, 98, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("AKKU-SPANNUNG (MIN/MAX)", 290, 272);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    char vBuf[32]; snprintf(vBuf, sizeof(vBuf), "%.1fV / %.1fV", telemetry.stats.min_voltage, telemetry.stats.max_voltage);
    _canvas->drawString(vBuf, 290, 294);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    char vSub[32]; snprintf(vSub, sizeof(vSub), "Live: %.1fV (Sag Check)", telemetry.voltage);
    _canvas->drawString(vSub, 290, 332);

    // Tile 6: MAX TEMPERATUR
    drawCard(536, 262, 244, 98, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
    _canvas->drawString("MAX TEMPERATUR", 548, 272);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    char tmpBuf[32]; snprintf(tmpBuf, sizeof(tmpBuf), "%.0f C / %.0f C", telemetry.stats.max_temp_motor, telemetry.stats.max_temp_esc);
    _canvas->drawString(tmpBuf, 548, 294);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Motor / Inverter FETs", 548, 332);

    // Bottom Card: Interactive Display Backlight Control (y = 368, h = 98)
    drawCard(20, 368, 760, 98, COLOR_SURFACE, COLOR_BORDER);

    // Label
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("DISPLAY HELLIGKEIT", 35, 380);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    char brBuf[16]; snprintf(brBuf, sizeof(brBuf), "%d %%", Settings.get().brightness_pct);
    _canvas->drawString(brBuf, 35, 404);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(0x52AA, COLOR_SURFACE);
    _canvas->drawString("Tippen zum Anpassen", 35, 442);

    // Button [-]
    drawCard(210, 388, 56, 56, 0x18C3, 0x39C7);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->drawString("-", 238, 416);

    // Interactive Slider Track (w=270, h=28)
    _canvas->fillRoundRect(280, 402, 270, 28, 8, 0x1084);
    _canvas->drawRoundRect(280, 402, 270, 28, 8, 0x2965);
    int fill_w = (int)((Settings.get().brightness_pct / 100.0f) * 270);
    if (fill_w > 270) fill_w = 270;
    if (fill_w > 8) {
        _canvas->fillRoundRect(280, 402, fill_w, 28, 8, COLOR_CYAN);
    }
    int knob_x = 280 + fill_w;
    if (knob_x < 290) knob_x = 290;
    if (knob_x > 540) knob_x = 540;
    _canvas->fillCircle(knob_x, 416, 12, COLOR_WHITE);
    _canvas->drawCircle(knob_x, 416, 12, 0x39C7);

    // Button [+]
    drawCard(565, 388, 56, 56, 0x18C3, 0x39C7);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->drawString("+", 593, 416);

    // Preset Pills
    drawCard(635, 396, 62, 40, 0x18C3, 0x39C7);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->drawString("25%", 666, 416);

    drawCard(708, 396, 62, 40, 0x18C3, 0x39C7);
    _canvas->drawString("100%", 739, 416);
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
    _canvas->fillScreen(0x0823); // Deep midnight obsidian background

    // Header Badge with Lock Icon
    drawCard(250, 16, 300, 50, 0x1084, COLOR_CYAN);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_CYAN, 0x1084);
    _canvas->drawString("PIN-SPERRE", 400, 41);

    // Prompt Subtitle
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_WHITE, 0x0823);
    _canvas->drawString("SYSTEM GESPERRT - VESC 0A MOTORBLOCKIERT", 400, 80);

    // 4 PIN Circles
    int entered_len = strlen(telemetry.pin_entry);
    bool err = telemetry.pin_error && (millis() < telemetry.pin_error_end_ms);
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
    _canvas->setFont(&fonts::Font2);
    if (err) {
        _canvas->setTextColor(COLOR_RED, 0x0823);
        _canvas->drawString("FALSCHER PIN-CODE! BITTE WIEDERHOLEN", 400, 142);
    } else {
        _canvas->setTextColor(COLOR_MUTED_GRAY, 0x0823);
        _canvas->drawString("4-stelligen Entsperrcode eingeben", 400, 142);
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
                // DEL button
                btn_border = 0xB800; // Red border
                txt_col = 0xFCA5;   // Light coral
            } else if (r == 3 && c == 2) {
                // OK button
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
    _canvas->setTextColor(0x52AA, 0x0823);
    _canvas->drawString("PIN aenderbar im Captive Portal / HyperDash Einstellungen", 400, 455);
}

// ==============================================================================
// SCREEN 3: INTERACTIVE SETTINGS MENU SYSTEM (800x480 Optimized)
// ==============================================================================
static const char *ROOT_CATEGORIES[5] = {
    "1. POWER & DRIVE",
    "2. DISPLAY & UI",
    "3. BATTERY PROFILES",
    "4. BIKE SETUP",
    "5. SYSTEM & DIAG"
};

static int getSubItemCount(uint8_t sub_id) {
    switch (sub_id) {
        case 0: return 6;
        case 1: return 2;
        case 2: return 5;
        case 3: return 3;
        case 4: return 9;
        default: return 0;
    }
}

static void getSubItemDetails2Lines(uint8_t sub_id, uint8_t item_id,
                                    char *title, size_t title_sz,
                                    char *val, size_t val_sz,
                                    char *d1, size_t d1_sz,
                                    char *d2, size_t d2_sz,
                                    const DashTelemetry &telemetry) {
    DashSettings &s = Settings.get();
    title[0] = 0; val[0] = 0; d1[0] = 0; d2[0] = 0;

    switch (sub_id) {
        case 0: // POWER & DRIVE
            switch (item_id) {
                case 0:
                    snprintf(title, title_sz, "Max Battery Current");
                    snprintf(val, val_sz, "%d A", s.max_battery_amps);
                    snprintf(d1, d1_sz, "Pack current limit");
                    snprintf(d2, d2_sz, "Range: 5A - 35A");
                    break;
                case 1:
                    snprintf(title, title_sz, "Max Phase Current");
                    snprintf(val, val_sz, "%d A", s.max_phase_amps);
                    snprintf(d1, d1_sz, "Motor torque limit");
                    snprintf(d2, d2_sz, "Range: 10A - 70A");
                    break;
                case 2:
                    snprintf(title, title_sz, "Field Weakening (FW)");
                    snprintf(val, val_sz, "%d A", s.field_weak_amps);
                    snprintf(d1, d1_sz, "Top speed boost");
                    snprintf(d2, d2_sz, "Redline: 0A - 20A");
                    break;
                case 3:
                    snprintf(title, title_sz, "Speed Limit");
                    if (s.max_speed_kmh == 0) {
                        snprintf(val, val_sz, "OFF (Unlimited)");
                    } else {
                        snprintf(val, val_sz, "%d km/h", s.max_speed_kmh);
                    }
                    snprintf(d1, d1_sz, "Motor cutoff speed");
                    snprintf(d2, d2_sz, "Presets: OFF, 25, 32, 45, 60");
                    break;
                case 4:
                    snprintf(title, title_sz, "Throttle Ramp Time");
                    snprintf(val, val_sz, "%.2f s", s.throttle_ramp_sec);
                    snprintf(d1, d1_sz, "Throttle response filter");
                    snprintf(d2, d2_sz, "Smooth: 0.10s - 1.00s");
                    break;
                case 5:
                    snprintf(title, title_sz, "Throttle ADC Channel");
                    snprintf(val, val_sz, (s.throttle_adc_channel == 0) ? "ADC1" : "ADC2");
                    snprintf(d1, d1_sz, "VESC throttle input port");
                    snprintf(d2, d2_sz, "Select ADC1 or ADC2");
                    break;
            }
            break;

        case 1: // DISPLAY & UI
            switch (item_id) {
                case 0:
                    snprintf(title, title_sz, "Dashboard Style");
                    snprintf(val, val_sz, (s.dash_style == 0) ? "Analog Dial" : "Duty Bar");
                    snprintf(d1, d1_sz, "Instrument cluster view");
                    snprintf(d2, d2_sz, "Analog Dial / Duty Bar");
                    break;
                case 1:
                    snprintf(title, title_sz, "Screen Brightness");
                    snprintf(val, val_sz, "%d %%", s.brightness_pct);
                    snprintf(d1, d1_sz, "Backlight brightness level");
                    snprintf(d2, d2_sz, "Range: 20%% to 100%%");
                    break;
            }
            break;

        case 2: // BATTERY PROFILES
            switch (item_id) {
                case 0:
                    snprintf(title, title_sz, "Active Profile");
                    snprintf(val, val_sz, (s.active_battery_profile == 0) ? "Profile 1 (Fresh)" : "Profile 2 (Daily)");
                    snprintf(d1, d1_sz, "Switch battery pack");
                    snprintf(d2, d2_sz, "Profile 1 or 2");
                    break;
                case 1:
                    snprintf(title, title_sz, "Profile 1 Nominal Wh");
                    snprintf(val, val_sz, "%.0f Wh", Battery.getProfile(0).nominal_wh);
                    snprintf(d1, d1_sz, "Pack 1 full energy");
                    snprintf(d2, d2_sz, "500Wh to 2500Wh");
                    break;
                case 2:
                    snprintf(title, title_sz, "Profile 2 Nominal Wh");
                    snprintf(val, val_sz, "%.0f Wh", Battery.getProfile(1).nominal_wh);
                    snprintf(d1, d1_sz, "Pack 2 full energy");
                    snprintf(d2, d2_sz, "500Wh to 2500Wh");
                    break;
                case 3:
                    snprintf(title, title_sz, "Pack Voltage Trim");
                    snprintf(val, val_sz, "%+.1f V", s.voltage_trim_v);
                    snprintf(d1, d1_sz, "Calibrate pack voltage");
                    snprintf(d2, d2_sz, "-2.0V to +2.0V");
                    break;
                case 4:
                    snprintf(title, title_sz, "Reset SoH Learning");
                    snprintf(val, val_sz, "RESET LEARNING");
                    snprintf(d1, d1_sz, "Reset capacity learning");
                    snprintf(d2, d2_sz, "Tap to reset learned SoH");
                    break;
            }
            break;

        case 3: // BIKE SETUP
            switch (item_id) {
                case 0:
                    snprintf(title, title_sz, "Wheel Diameter");
                    snprintf(val, val_sz, "%.1f in", s.wheel_diameter_in);
                    snprintf(d1, d1_sz, "Tire outer diameter");
                    snprintf(d2, d2_sz, "10.0 to 29.0 inches");
                    break;
                case 1:
                    snprintf(title, title_sz, "Motor Pole Pairs");
                    snprintf(val, val_sz, "%d", s.motor_pole_pairs);
                    snprintf(d1, d1_sz, "Motor pole pairs (ERPM divisor)");
                    snprintf(d2, d2_sz, "Scooter hub motor: 15");
                    break;
                case 2:
                    snprintf(title, title_sz, "Internal Gear Ratio");
                    snprintf(val, val_sz, "%.2f : 1", s.gear_ratio);
                    snprintf(d1, d1_sz, "Motor transmission ratio");
                    snprintf(d2, d2_sz, "Hub motor: 1.00 : 1");
                    break;
            }
            break;

        case 4: // SYSTEM & DIAGNOSTICS
            switch (item_id) {
                case 0:
                    snprintf(title, title_sz, "Sync VESC Settings");
                    if (Vesc.getSyncStatus() == SYNC_REQUESTED) {
                        snprintf(val, val_sz, "PULLING...");
                    } else if (Vesc.getSyncStatus() == SYNC_SUCCESS) {
                        snprintf(val, val_sz, "SYNC OK!");
                    } else if (Vesc.getSyncStatus() == SYNC_FAILED) {
                        snprintf(val, val_sz, "FAILED");
                    } else {
                        snprintf(val, val_sz, "PULL FROM VESC");
                    }
                    snprintf(d1, d1_sz, "Read VESC motor limits");
                    snprintf(d2, d2_sz, "Saves to flash memory");
                    break;
                case 1:
                    snprintf(title, title_sz, "VESC USB Bridge");
                    snprintf(val, val_sz, "START USB BRIDGE");
                    snprintf(d1, d1_sz, "PC VESC Tool Bridge");
                    snprintf(d2, d2_sz, "Tap to start CDC passthrough");
                    break;
                case 2:
                    snprintf(title, title_sz, "VESC Wi-Fi Bridge");
                    snprintf(val, val_sz, "START WI-FI BRIDGE");
                    snprintf(d1, d1_sz, "Wireless TCP Bridge");
                    snprintf(d2, d2_sz, "AP: 192.168.4.1:6510");
                    break;
                case 3:
                    snprintf(title, title_sz, "Reset Trip Statistics");
                    snprintf(val, val_sz, "RESET TRIP STATS");
                    snprintf(d1, d1_sz, "Clear trip distance & peaks");
                    snprintf(d2, d2_sz, "Tap to reset trip to 0.0");
                    break;
                case 4:
                    snprintf(title, title_sz, "Factory Reset");
                    snprintf(val, val_sz, "RESTORE DEFAULTS");
                    snprintf(d1, d1_sz, "Restore default settings");
                    snprintf(d2, d2_sz, "Tap to restore factory config");
                    break;
                case 5:
                    snprintf(title, title_sz, "VESC Hardware Link");
                    snprintf(val, val_sz, telemetry.vesc_connected ? "COMM HEALTHY" : "OFFLINE");
                    snprintf(d1, d1_sz, "Waveshare 4.3\" UART");
                    snprintf(d2, d2_sz, "TX:43 RX:44 (115200 baud)");
                    break;
                case 6:
                    snprintf(title, title_sz, "Boot PIN Lockscreen");
                    snprintf(val, val_sz, s.pin_lock_enabled ? "ENABLED" : "DISABLED");
                    snprintf(d1, d1_sz, "Lock system on startup");
                    snprintf(d2, d2_sz, "Tap to toggle enabled state");
                    break;
                case 7:
                    snprintf(title, title_sz, "PIN Code");
                    snprintf(val, val_sz, "%s", s.pin_code);
                    snprintf(d1, d1_sz, "4-digit unlock code");
                    snprintf(d2, d2_sz, "Tap to cycle PIN presets");
                    break;
                case 8:
                    snprintf(title, title_sz, "VESC Fault Blackbox");
                    snprintf(val, val_sz, "%d FAULTS LOGGED", FaultManager::instance().getFaultCount());
                    snprintf(d1, d1_sz, "Last: %s", FaultManager::instance().getFaultCount() > 0 ? FaultManager::getFaultTitleDe(FaultManager::instance().getFault(0).code) : "Keiner");
                    snprintf(d2, d2_sz, "Tap to clear fault log");
                    break;
            }
            break;
    }
}

void DashboardRenderer::initSettingsScreen(const DashTelemetry &telemetry) {
    // Header Bar
    _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
    _canvas->drawString("SETTINGS & CONFIGURATION", 20, 14, &fonts::Font4);

    _canvas->drawFastHLine(20, 44, 760, COLOR_BORDER);

    // Left Navigation Box (w=310, h=405)
    drawCard(20,  56, 310, 405, COLOR_SURFACE, COLOR_BORDER);

    // Right Content Box (w=435, h=405)
    drawCard(345, 56, 435, 405, COLOR_SURFACE, COLOR_BORDER);
}

void DashboardRenderer::renderRootMenu() {
    for (int i = 0; i < 5; i++) {
        int iy = 62 + i * 76;
        bool isSel = (i == _menu_root_idx);
        if (isSel) {
            _canvas->fillRoundRect(24, iy, 302, 68, 8, COLOR_SURFACE_LIGHT);
            _canvas->fillRoundRect(24, iy, 6, 68, 2, COLOR_CYAN);
            _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE_LIGHT);
        } else {
            _canvas->fillRoundRect(24, iy, 302, 68, 8, COLOR_SURFACE);
            _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
        }
        _canvas->drawString(ROOT_CATEGORIES[i], 38, iy + 22, &fonts::Font4);
    }
}

void DashboardRenderer::renderSubmenu(uint8_t sub_id, const DashTelemetry &telemetry) {
    int count = getSubItemCount(sub_id);
    int start_idx = 0;
    if (count > 5) {
        if (_menu_sub_idx >= 4) {
            start_idx = _menu_sub_idx - 4;
            if (start_idx + 5 > count) start_idx = count - 5;
        }
    }
    int display_count = (count > 5) ? 5 : count;

    for (int k = 0; k < display_count; k++) {
        int j = start_idx + k;
        int iy = 62 + k * 76;
        bool isSel = (j == _menu_sub_idx);

        char title[48], val[32], d1[64], d2[64];
        getSubItemDetails2Lines(sub_id, j, title, sizeof(title), val, sizeof(val), d1, sizeof(d1), d2, sizeof(d2), telemetry);

        if (isSel) {
            uint16_t bgCol = _menu_edit_mode ? 0x3186 : COLOR_SURFACE_LIGHT;
            uint16_t barCol = _menu_edit_mode ? COLOR_AMBER : COLOR_CYAN;
            _canvas->fillRoundRect(24, iy, 302, 68, 8, bgCol);
            _canvas->fillRoundRect(24, iy, 6, 68, 2, barCol);
            _canvas->setTextColor(COLOR_WHITE, bgCol);
        } else {
            _canvas->fillRoundRect(24, iy, 302, 68, 8, COLOR_SURFACE);
            _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
        }
        _canvas->drawString(title, 38, iy + 22, &fonts::Font4);
    }
}

void DashboardRenderer::renderSettingsScreen(const DashTelemetry &telemetry) {
    // 1. Auto-exit edit mode after 3s of inactivity
    if (_menu_edit_mode && (millis() - _menu_last_activity_ms > 3000)) {
        _menu_edit_mode = false;
        Buttons.setEditMode(false);
        Settings.save();
        _menu_dirty = true;
    }

    // Check VESC Sync status transitions
    VescSyncStatus cur_sync = Vesc.getSyncStatus();
    static VescSyncStatus last_sync_status = SYNC_IDLE;
    if (cur_sync != last_sync_status) {
        last_sync_status = cur_sync;
        _menu_dirty = true;
    }
    if (cur_sync != SYNC_IDLE && cur_sync != SYNC_REQUESTED && (millis() - Vesc.getSyncTimeMs() > 4000)) {
        Vesc.resetSyncStatus();
        _menu_dirty = true;
    }

    if (!_menu_dirty) return;
    _menu_dirty = false;

    // 2. Clear inner card contents
    _canvas->fillRect(22, 58, 306, 401, COLOR_SURFACE);
    _canvas->fillRect(347, 58, 431, 401, COLOR_SURFACE);

    // 3. Render Left Pane
    if (!_menu_in_sub) {
        renderRootMenu();
    } else {
        renderSubmenu(_menu_root_idx, telemetry);
    }

    // 4. Render Right Pane
    char curTitle[48] = {0};
    if (!_menu_in_sub) {
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        _canvas->drawString(ROOT_CATEGORIES[_menu_root_idx], 365, 75, &fonts::Font4);

        char d1[64] = {0};
        char d2[64] = {0};
        DashSettings &s = Settings.get();

        switch (_menu_root_idx) {
            case 0: // POWER & DRIVE
                snprintf(d1, sizeof(d1), "Battery: %dA | Phase: %dA", s.max_battery_amps, s.max_phase_amps);
                if (s.max_speed_kmh == 0) {
                    snprintf(d2, sizeof(d2), "FW Boost: %dA | Limit: Off", s.field_weak_amps);
                } else {
                    snprintf(d2, sizeof(d2), "FW Boost: %dA | Limit: %d km/h", s.field_weak_amps, s.max_speed_kmh);
                }
                break;

            case 1: // DISPLAY & UI
                snprintf(d1, sizeof(d1), "Style: %s", (s.dash_style == STYLE_ANALOG_DIAL) ? "Analog Dial" : "Horizontal Bar");
                snprintf(d2, sizeof(d2), "Brightness: %d%%", s.brightness_pct);
                break;

            case 2: // BATTERY PROFILES
                {
                    uint8_t pid = s.active_battery_profile;
                    snprintf(d1, sizeof(d1), "Active: P%d (%s)", pid + 1, Battery.getProfile(pid).name);
                    snprintf(d2, sizeof(d2), "Trim: %+.1fV | Learned: %.0fWh", s.voltage_trim_v, Battery.getProfile(pid).nominal_wh);
                }
                break;

            case 3: // BIKE SETUP
                snprintf(d1, sizeof(d1), "Wheel: %.1f\" | Poles: %d", s.wheel_diameter_in, s.motor_pole_pairs);
                snprintf(d2, sizeof(d2), "Gear Ratio: %.2f : 1", s.gear_ratio);
                break;

            case 4: // SYSTEM & DIAG
                snprintf(d1, sizeof(d1), "VESC: %s", telemetry.vesc_connected ? "Link Online (115200)" : "Link Offline");
                snprintf(d2, sizeof(d2), "Trip: %.1f km | USB / Wi-Fi Bridge", telemetry.trip_km);
                break;

            default:
                break;
        }

        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->drawString(d1, 365, 125, &fonts::Font4);

        _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
        _canvas->drawString(d2, 365, 165, &fonts::Font4);

        // Live VESC Sync Banner if active
        if (cur_sync == SYNC_REQUESTED) {
            _canvas->fillRoundRect(365, 210, 395, 36, 6, 0x2940);
            _canvas->setTextColor(COLOR_AMBER, 0x2940);
            _canvas->drawCenterString("PULLING VESC CONFIG...", 365 + 395/2, 218, &fonts::Font4);
        } else if (cur_sync == SYNC_SUCCESS) {
            _canvas->fillRoundRect(365, 210, 395, 36, 6, 0x0A42);
            _canvas->setTextColor(COLOR_GREEN, 0x0A42);
            char okBuf[48];
            snprintf(okBuf, sizeof(okBuf), "SYNC OK: Bat %dA | Phase %dA", s.max_battery_amps, s.max_phase_amps);
            _canvas->drawCenterString(okBuf, 365 + 395/2, 218, &fonts::Font4);
        } else if (cur_sync == SYNC_FAILED) {
            _canvas->fillRoundRect(365, 210, 395, 36, 6, 0x3000);
            _canvas->setTextColor(COLOR_RED, 0x3000);
            _canvas->drawCenterString("SYNC FAILED: NO VESC LINK", 365 + 395/2, 218, &fonts::Font4);
        }

        // Navigation Tips
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        _canvas->drawString("Tap Category to Open Submenu", 365, 265, &fonts::Font4);
        _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
        _canvas->drawString("Tap [Pull VESC] in System to Sync", 365, 305, &fonts::Font4);
        _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
        _canvas->drawString("Swipe Right to Exit to Dashboard", 365, 345, &fonts::Font4);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("Touch LCD 4.3\" - 800x480 GT911", 365, 385, &fonts::Font4);
    } else {
        char val[32], d1[64], d2[64];
        getSubItemDetails2Lines(_menu_root_idx, _menu_sub_idx, curTitle, sizeof(curTitle), val, sizeof(val), d1, sizeof(d1), d2, sizeof(d2), telemetry);

        // Setting Title
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        _canvas->drawString(curTitle, 365, 75, &fonts::Font4);

        // Big Value Box (w=395, h=80)
        int boxY = 115, boxH = 80;
        uint16_t vBg = _menu_edit_mode ? 0x2124 : COLOR_SURFACE;
        uint16_t vBorder = _menu_edit_mode ? COLOR_AMBER : COLOR_BORDER;
        uint16_t vColor = _menu_edit_mode ? COLOR_AMBER : COLOR_WHITE;

        _canvas->fillRoundRect(365, boxY, 395, boxH, 8, vBg);
        _canvas->drawRoundRect(365, boxY, 395, boxH, 8, vBorder);

        bool hasLetters = false;
        for (int k = 0; val[k] != '\0'; k++) {
            if ((val[k] >= 'a' && val[k] <= 'z') || (val[k] >= 'A' && val[k] <= 'Z')) {
                hasLetters = true;
                break;
            }
        }
        const lgfx::IFont *vFont = hasLetters ? &fonts::Font4 : &fonts::Font6;
        int textY = hasLetters ? (boxY + 26) : (boxY + 16);

        _canvas->setTextColor(vColor, vBg);
        _canvas->drawCenterString(val, 365 + (395 / 2), textY, vFont);

        // Descriptions
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->drawString(d1, 365, 215, &fonts::Font4);

        _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
        _canvas->drawString(d2, 365, 250, &fonts::Font4);

        // Help Hints / Touch Actions
        if (_menu_edit_mode) {
            _canvas->fillRoundRect(365, 300, 190, 44, 6, 0x18E3);
            _canvas->setTextColor(COLOR_AMBER, 0x18E3);
            _canvas->drawCenterString("Tap Left: [ - ]", 365 + 95, 312, &fonts::Font4);

            _canvas->fillRoundRect(570, 300, 190, 44, 6, 0x18E3);
            _canvas->drawCenterString("Tap Right: [ + ]", 570 + 95, 312, &fonts::Font4);

            _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
            _canvas->drawString("Hold or Tap Center to Save", 365, 365, &fonts::Font4);
        } else {
            _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
            if (_menu_root_idx == 4 && _menu_sub_idx == 0) {
                _canvas->drawString("Tap Value Box to Pull From VESC", 365, 310, &fonts::Font4);
            } else if (_menu_root_idx == 4 && (_menu_sub_idx == 1 || _menu_sub_idx == 2)) {
                _canvas->drawString("Tap Value Box to Start Bridge", 365, 310, &fonts::Font4);
            } else if (_menu_root_idx == 4 && (_menu_sub_idx == 3 || _menu_sub_idx == 4)) {
                _canvas->drawString("Tap Value Box to Confirm Reset", 365, 310, &fonts::Font4);
            } else if (_menu_root_idx == 4 && _menu_sub_idx == 5) {
                _canvas->drawString("Hardware Status (Info Only)", 365, 310, &fonts::Font4);
            } else if (_menu_root_idx == 2 && _menu_sub_idx == 4) {
                _canvas->drawString("Tap Value Box to Confirm Reset", 365, 310, &fonts::Font4);
            } else {
                _canvas->drawString("Tap Value Box to Edit Value", 365, 310, &fonts::Font4);
            }
            _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
            _canvas->drawString("Swipe Right or Long Press to Return", 365, 355, &fonts::Font4);
        }
    }

    // 5. Header Breadcrumb
    char topBuf[64];
    if (_menu_edit_mode) {
        snprintf(topBuf, sizeof(topBuf), "EDITING VALUE");
        _canvas->setTextColor(COLOR_AMBER, COLOR_BG);
    } else if (_menu_in_sub) {
        snprintf(topBuf, sizeof(topBuf), "%s", curTitle);
        _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    } else {
        snprintf(topBuf, sizeof(topBuf), "SETTINGS");
        _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_BG);
    }
    _canvas->setTextPadding(300);
    _canvas->drawRightString(topBuf, 770, 14, &fonts::Font4);
    _canvas->setTextPadding(0);
}

void DashboardRenderer::adjustCurrentSetting(int direction, DashTelemetry &telemetry) {
    DashSettings &s = Settings.get();
    _menu_last_activity_ms = millis();
    _menu_dirty = true;

    switch (_menu_root_idx) {
        case 0: // POWER & DRIVE
            if (_menu_sub_idx == 0) { // max battery amps (5..35)
                int v = (int)s.max_battery_amps + direction;
                if (v < 5) v = 5;
                if (v > 35) v = 35;
                s.max_battery_amps = v;
            } else if (_menu_sub_idx == 1) { // max phase amps (10..70)
                int v = (int)s.max_phase_amps + direction;
                if (v < 10) v = 10;
                if (v > 70) v = 70;
                s.max_phase_amps = v;
            } else if (_menu_sub_idx == 2) { // field weakening (0..20)
                int v = (int)s.field_weak_amps + direction;
                if (v < 0) v = 0;
                if (v > 20) v = 20;
                s.field_weak_amps = v;
            } else if (_menu_sub_idx == 3) { // speed limit presets: 0, 25, 32, 45, 60
                static const uint8_t presets[] = { 0, 25, 32, 45, 60 };
                int idx = 0;
                for (int i = 0; i < 5; i++) {
                    if (s.max_speed_kmh == presets[i]) { idx = i; break; }
                }
                idx = (idx + direction + 5) % 5;
                s.max_speed_kmh = presets[idx];
            } else if (_menu_sub_idx == 4) { // ramp time (0.10..1.00s)
                float v = s.throttle_ramp_sec + (direction * 0.05f);
                if (v < 0.10f) v = 0.10f;
                if (v > 1.00f) v = 1.00f;
                s.throttle_ramp_sec = v;
            } else if (_menu_sub_idx == 5) { // throttle adc channel
                s.throttle_adc_channel = (s.throttle_adc_channel == 0) ? 1 : 0;
            }
            break;

        case 1: // DISPLAY & UI
            if (_menu_sub_idx == 0) { // dash style
                s.dash_style = (s.dash_style == 0) ? 1 : 0;
                _active_style = (DashboardStyle)s.dash_style;
            } else if (_menu_sub_idx == 1) { // brightness (20..100)
                int b = (int)s.brightness_pct + (direction * 5);
                if (b < 20) b = 20;
                if (b > 100) b = 100;
                s.brightness_pct = b;
                _display->setBrightness(map(s.brightness_pct, 0, 100, 0, 255));
            }
            break;

        case 2: // BATTERY PROFILES
            if (_menu_sub_idx == 0) { // active profile
                s.active_battery_profile = (s.active_battery_profile == 0) ? 1 : 0;
                Battery.setActiveProfile(s.active_battery_profile);
            } else if (_menu_sub_idx == 1) { // B1 nominal Wh
                float wh = Battery.getProfile(0).nominal_wh + (direction * 50.0f);
                if (wh < 500.0f) wh = 500.0f;
                if (wh > 2500.0f) wh = 2500.0f;
                Battery.getProfile(0).nominal_wh = wh;
                Battery.saveProfiles();
            } else if (_menu_sub_idx == 2) { // B2 nominal Wh
                float wh = Battery.getProfile(1).nominal_wh + (direction * 50.0f);
                if (wh < 500.0f) wh = 500.0f;
                if (wh > 2500.0f) wh = 2500.0f;
                Battery.getProfile(1).nominal_wh = wh;
                Battery.saveProfiles();
            } else if (_menu_sub_idx == 3) { // Voltage Trim (-2.0V .. +2.0V)
                float v = s.voltage_trim_v + (direction * 0.1f);
                if (v < -2.0f) v = -2.0f;
                if (v > 2.0f) v = 2.0f;
                s.voltage_trim_v = v;
            }
            break;

        case 3: // BIKE SETUP
            if (_menu_sub_idx == 0) { // wheel diameter (10..29 in)
                float d = s.wheel_diameter_in + (direction * 0.5f);
                if (d < 10.0f) d = 10.0f;
                if (d > 29.0f) d = 29.0f;
                s.wheel_diameter_in = d;
            } else if (_menu_sub_idx == 1) { // pole pairs (8..24)
                int p = (int)s.motor_pole_pairs + direction;
                if (p < 8) p = 8;
                if (p > 24) p = 24;
                s.motor_pole_pairs = p;
            } else if (_menu_sub_idx == 2) { // gear ratio (1.00..4.00)
                float g = s.gear_ratio + (direction * 0.05f);
                if (g < 1.00f) g = 1.00f;
                if (g > 4.00f) g = 4.00f;
                s.gear_ratio = g;
            }
            break;

        case 4: // SYSTEM & DIAG
            if (_menu_sub_idx == 6) {
                s.pin_lock_enabled = !s.pin_lock_enabled;
                Settings.save();
            } else if (_menu_sub_idx == 7) {
                static const char *PIN_PRESETS[] = { "1234", "0000", "1111", "2580", "4321", "9999", "8888", "1337" };
                const int num_presets = 8;
                int cur_p = 0;
                for (int p = 0; p < num_presets; p++) {
                    if (strcmp(s.pin_code, PIN_PRESETS[p]) == 0) {
                        cur_p = p;
                        break;
                    }
                }
                cur_p = (cur_p + direction + num_presets) % num_presets;
                strncpy(s.pin_code, PIN_PRESETS[cur_p], sizeof(s.pin_code) - 1);
                s.pin_code[sizeof(s.pin_code) - 1] = '\0';
                Settings.save();
            }
            break;

        default:
            break;
    }
}

void DashboardRenderer::handleMenuNav(NavAction action, DashTelemetry &telemetry) {
    if (action == NAV_NONE) return;

    if (_menu_edit_mode) {
        _menu_last_activity_ms = millis();
        if (action == NAV_EDIT_INC || action == NAV_BTN1_SHORT) {
            adjustCurrentSetting(+1, telemetry);
        } else if (action == NAV_EDIT_DEC || action == NAV_BTN2_SHORT) {
            adjustCurrentSetting(-1, telemetry);
        } else if (action == NAV_BOTH_PRESSED || action == NAV_BTN2_LONG || action == NAV_BTN1_LONG) {
            _menu_edit_mode = false;
            Buttons.setEditMode(false);
            Settings.save();
            _menu_dirty = true;
        }
        return;
    }

    if (_menu_in_sub) {
        if (action == NAV_BTN1_SHORT) { // Cursor UP
            int total = getSubItemCount(_menu_root_idx);
            if (total > 0) {
                _menu_sub_idx = (_menu_sub_idx - 1 + total) % total;
                _menu_dirty = true;
            }
        } else if (action == NAV_BTN2_SHORT) { // Cursor DOWN
            int total = getSubItemCount(_menu_root_idx);
            if (total > 0) {
                _menu_sub_idx = (_menu_sub_idx + 1) % total;
                _menu_dirty = true;
            }
        } else if (action == NAV_BOTH_PRESSED || action == NAV_BTN1_LONG) { // SELECT / ENTER
            if (_menu_root_idx == 2 && _menu_sub_idx == 4) {
                Battery.resetProfileLearning(Settings.get().active_battery_profile);
                _menu_dirty = true;
            } else if (_menu_root_idx == 4 && _menu_sub_idx == 0) {
                Vesc.requestMcconf();
                _menu_dirty = true;
            } else if (_menu_root_idx == 4 && _menu_sub_idx == 1) {
                _menu_in_sub = false;
                _menu_edit_mode = false;
                Buttons.setEditMode(false);
                Vesc.enterBridgeMode();
                telemetry.screen = SCREEN_VESC_BRIDGE;
                _screen_dirty = true;
                _cache.invalidate();
            } else if (_menu_root_idx == 4 && _menu_sub_idx == 2) {
                _menu_in_sub = false;
                _menu_edit_mode = false;
                Buttons.setEditMode(false);
                Vesc.enterWifiBridgeMode();
                telemetry.screen = SCREEN_WIFI_BRIDGE;
                _screen_dirty = true;
                _cache.invalidate();
            } else if (_menu_root_idx == 4 && _menu_sub_idx == 3) {
                telemetry.trip_km = 0.0f;
                telemetry.stats.reset();
                _menu_dirty = true;
            } else if (_menu_root_idx == 4 && _menu_sub_idx == 4) {
                Settings.resetToDefaults();
                _display->setBrightness(map(Settings.get().brightness_pct, 0, 100, 0, 255));
                _menu_dirty = true;
            } else if (_menu_root_idx == 4 && _menu_sub_idx == 5) {
                // Info-only
            } else if (_menu_root_idx == 4 && _menu_sub_idx == 6) {
                Settings.get().pin_lock_enabled = !Settings.get().pin_lock_enabled;
                Settings.save();
                _menu_dirty = true;
            } else if (_menu_root_idx == 4 && _menu_sub_idx == 7) {
                _menu_edit_mode = true;
                Buttons.setEditMode(true);
                _menu_last_activity_ms = millis();
                _menu_dirty = true;
            } else if (_menu_root_idx == 4 && _menu_sub_idx == 8) {
                FaultManager::instance().clearFaults();
                _menu_dirty = true;
            } else {
                _menu_edit_mode = true;
                Buttons.setEditMode(true);
                _menu_last_activity_ms = millis();
                _menu_dirty = true;
            }
        } else if (action == NAV_BTN2_LONG) { // BACK
            _menu_in_sub = false;
            _menu_dirty = true;
        }
    } else {
        if (action == NAV_BTN1_SHORT) { // Cursor UP
            _menu_root_idx = (_menu_root_idx - 1 + 5) % 5;
            _menu_dirty = true;
        } else if (action == NAV_BTN2_SHORT) { // Cursor DOWN
            _menu_root_idx = (_menu_root_idx + 1) % 5;
            _menu_dirty = true;
        } else if (action == NAV_BTN1_LONG) {
            Vesc.requestMcconf();
            _menu_dirty = true;
        } else if (action == NAV_BOTH_PRESSED) { // ENTER SUBMENU
            _menu_in_sub = true;
            _menu_sub_idx = 0;
            _menu_dirty = true;
        } else if (action == NAV_BTN2_LONG) { // EXIT SETTINGS
            telemetry.screen = SCREEN_RIDE_DASH;
            _screen_dirty = true;
            _cache.invalidate();
        }
    }
}

// ==============================================================================
// SCREEN 4: VESC TOOL USB PASSTHROUGH BRIDGE (800x480 Optimized)
// ==============================================================================
void DashboardRenderer::initVescBridgeScreen(const DashTelemetry &telemetry) {
    // Header Bar
    _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    _canvas->drawString("VESC TOOL USB PASSTHROUGH BRIDGE", 20, 14, &fonts::Font4);

    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_BG);
    _canvas->setTextPadding(300);
    _canvas->drawRightString("115200 BAUD (CDC <-> UART)", 770, 14, &fonts::Font4);
    _canvas->setTextPadding(0);

    _canvas->drawFastHLine(20, 44, 760, COLOR_BORDER);

    // Left Card: PC / VESC Tool (w=370, h=310)
    drawCard(20, 56, 370, 310, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("PC / VESC TOOL (USB)", 36, 74, &fonts::Font4);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->drawString("Port: USB CDC (/dev/ttyACM0)", 36, 110, &fonts::Font4);
    _canvas->drawString("DATA RECEIVED FROM PC:", 36, 160, &fonts::Font4);

    // Right Card: VESC Controller (w=370, h=310)
    drawCard(410, 56, 370, 310, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setTextColor(COLOR_GREEN, COLOR_SURFACE);
    _canvas->drawString("VESC MOTOR CONTROLLER", 426, 74, &fonts::Font4);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->drawString("Pins: TX=43, RX=44 (Serial1)", 426, 110, &fonts::Font4);
    _canvas->drawString("DATA FORWARDED TO VESC:", 426, 160, &fonts::Font4);

    // Bottom Help Banner (w=760, h=80)
    _canvas->fillRoundRect(20, 380, 760, 80, 8, 0x18E3);
    _canvas->drawRoundRect(20, 380, 760, 80, 8, COLOR_AMBER);
    _canvas->setTextColor(COLOR_AMBER, 0x18E3);
    _canvas->drawCenterString("Tap anywhere or swipe right to Exit Bridge and return to Dash", 400, 408, &fonts::Font4);

    _bridge_last_pc_bytes = 0xFFFFFFFF;
    _bridge_last_vesc_bytes = 0xFFFFFFFF;
}

void DashboardRenderer::renderVescBridgeScreen(const DashTelemetry &telemetry) {
    uint32_t pc_bytes = Vesc.getBridgePcToVescBytes();
    uint32_t vesc_bytes = Vesc.getBridgeVescToPcBytes();

    if (pc_bytes != _bridge_last_pc_bytes) {
        _bridge_last_pc_bytes = pc_bytes;
        char buf[32];
        if (pc_bytes < 1024) {
            snprintf(buf, sizeof(buf), "%u B", (unsigned)pc_bytes);
        } else if (pc_bytes < 1048576) {
            snprintf(buf, sizeof(buf), "%.1f KB", pc_bytes / 1024.0f);
        } else {
            snprintf(buf, sizeof(buf), "%.2f MB", pc_bytes / 1048576.0f);
        }
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setTextPadding(300);
        _canvas->drawString(buf, 36, 205, &fonts::Font6);
        _canvas->setTextPadding(0);
    }

    if (vesc_bytes != _bridge_last_vesc_bytes) {
        _bridge_last_vesc_bytes = vesc_bytes;
        char buf[32];
        if (vesc_bytes < 1024) {
            snprintf(buf, sizeof(buf), "%u B", (unsigned)vesc_bytes);
        } else if (vesc_bytes < 1048576) {
            snprintf(buf, sizeof(buf), "%.1f KB", vesc_bytes / 1024.0f);
        } else {
            snprintf(buf, sizeof(buf), "%.2f MB", vesc_bytes / 1048576.0f);
        }
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setTextPadding(300);
        _canvas->drawString(buf, 426, 205, &fonts::Font6);
        _canvas->setTextPadding(0);
    }
}

// ==============================================================================
// SCREEN 5: VESC TOOL WIRELESS WI-FI BRIDGE (800x480 Optimized)
// ==============================================================================
void DashboardRenderer::initWifiBridgeScreen(const DashTelemetry &telemetry) {
    // Header Bar
    _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
    _canvas->drawString("VESC TOOL WIRELESS WI-FI BRIDGE", 20, 14, &fonts::Font4);

    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_BG);
    _canvas->setTextPadding(300);
    _canvas->drawRightString("PORT 6510 (TCP <-> UART)", 770, 14, &fonts::Font4);
    _canvas->setTextPadding(0);

    _canvas->drawFastHLine(20, 44, 760, COLOR_BORDER);

    // Left Card: Wi-Fi Access Point Details
    drawCard(20, 56, 370, 310, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("WI-FI ACCESS POINT (AP)", 36, 74, &fonts::Font4);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->drawString("SSID:    VESC-DASH-AP", 36, 110, &fonts::Font4);
    _canvas->drawString("IP:      192.168.4.1", 36, 144, &fonts::Font4);
    _canvas->drawString("Port:    6510 (VESC standard)", 36, 178, &fonts::Font4);
    _canvas->drawString("Auth:    Open (No password)", 36, 212, &fonts::Font4);
    _canvas->drawString("Radio:   2.4 GHz SoftAP", 36, 246, &fonts::Font4);

    // Right Card: Client Connection & Traffic Throughput
    drawCard(410, 56, 370, 310, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setTextColor(COLOR_GREEN, COLOR_SURFACE);
    _canvas->drawString("VESC TOOL CLIENT", 426, 74, &fonts::Font4);

    // Bottom Help Banner
    _canvas->fillRoundRect(20, 380, 760, 80, 8, 0x18E3);
    _canvas->drawRoundRect(20, 380, 760, 80, 8, COLOR_AMBER);
    _canvas->setTextColor(COLOR_AMBER, 0x18E3);
    _canvas->drawCenterString("Tap anywhere or swipe right to Exit Bridge & Power Off Wi-Fi", 400, 408, &fonts::Font4);

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
        _canvas->fillRoundRect(414, 100, 362, 255, 6, COLOR_SURFACE);

        if (connected) {
            _canvas->setTextColor(COLOR_GREEN, COLOR_SURFACE);
            _canvas->drawString("STATUS: CONNECTED", 426, 110, &fonts::Font4);

            _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
            char ipBuf[40];
            snprintf(ipBuf, sizeof(ipBuf), "Client IP: %s", Vesc.getWifiClientIp());
            _canvas->drawString(ipBuf, 426, 146, &fonts::Font4);

            _canvas->drawString("APP -> VESC:", 426, 186, &fonts::Font4);
            _canvas->drawString("VESC -> APP:", 426, 226, &fonts::Font4);

            _bridge_last_wifi_rx = 0xFFFFFFFF;
            _bridge_last_wifi_tx = 0xFFFFFFFF;
        } else {
            _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
            _canvas->drawString("STATUS: AWAITING CLIENT...", 426, 110, &fonts::Font4);
            _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
            _canvas->drawString("1. Connect device to SSID:", 426, 146, &fonts::Font4);
            _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
            _canvas->drawString("   \"VESC-DASH-AP\"", 426, 180, &fonts::Font4);
            _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
            _canvas->drawString("2. Open VESC Tool -> TCP 6510", 426, 218, &fonts::Font4);
        }
    }

    if (connected) {
        if (rx_bytes != _bridge_last_wifi_rx) {
            _bridge_last_wifi_rx = rx_bytes;
            char buf[32];
            if (rx_bytes < 1024) snprintf(buf, sizeof(buf), "%u B", (unsigned)rx_bytes);
            else if (rx_bytes < 1048576) snprintf(buf, sizeof(buf), "%.1f KB", rx_bytes / 1024.0f);
            else snprintf(buf, sizeof(buf), "%.2f MB", rx_bytes / 1048576.0f);

            _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
            _canvas->setTextPadding(160);
            _canvas->drawString(buf, 580, 186, &fonts::Font4);
            _canvas->setTextPadding(0);
        }

        if (tx_bytes != _bridge_last_wifi_tx) {
            _bridge_last_wifi_tx = tx_bytes;
            char buf[32];
            if (tx_bytes < 1024) snprintf(buf, sizeof(buf), "%u B", (unsigned)tx_bytes);
            else if (tx_bytes < 1048576) snprintf(buf, sizeof(buf), "%.1f KB", tx_bytes / 1024.0f);
            else snprintf(buf, sizeof(buf), "%.2f MB", tx_bytes / 1048576.0f);

            _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
            _canvas->setTextPadding(160);
            _canvas->drawString(buf, 580, 226, &fonts::Font4);
            _canvas->setTextPadding(0);
        }
    }
}
