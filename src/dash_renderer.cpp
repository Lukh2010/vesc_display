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
      _last_pin_err(false) {
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

        case SCREEN_RACE_TIMER:
            if (_screen_dirty) {
                _canvas->fillScreen(COLOR_BG);
                initRaceTimerScreen(telemetry);
                _screen_dirty = false;
            }
            renderRaceTimerScreen(telemetry);
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
    canvas->drawString(gBuf, cx, cy - 5);

    canvas->setFont(&fonts::Font0);
    canvas->setTextSize(1);
    canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    canvas->drawString("G-FORCE", cx, cy + 13);
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
    int totalW = valW + 7;
    int startX = cx - (totalW / 2);
    canvas->setTextDatum(textdatum_t::middle_left);
    canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    canvas->drawString(valBuf, startX, cy - 5);
    canvas->drawCircle(startX + valW + 3, cy - 8, 2, COLOR_WHITE);

    canvas->setFont(&fonts::Font0);
    canvas->setTextSize(1);
    canvas->setTextDatum(textdatum_t::middle_center);
    canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    canvas->drawString("NEIGUNG", cx, cy + 13);
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

    // 2. Rider Tag (if set)
    if (telemetry.rider_tag[0] != '\0' && !_hyper_inited) {
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::top_center);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->drawString(telemetry.rider_tag, cx, 110);
    }

    // 3. Hero Digital Speed Readout (Only redrawn when integer speed or sweep changes)
    int cur_spd_int = (int)(spd + 0.5f);
    if (!_hyper_inited || cur_spd_int != _cache.speed || _is_sweeping || telemetry.boost_active != _cache.boost_active) {
        _cache.speed = cur_spd_int;
        _canvas->fillRect(cx - 100, 140, 200, 100, COLOR_BG);
        char spdBuf[16];
        snprintf(spdBuf, sizeof(spdBuf), "%d", cur_spd_int);
        uint16_t spdCol = telemetry.boost_active ? COLOR_CYAN : ((spd >= 85.0f) ? COLOR_RED : COLOR_WHITE);
        drawHeroDigitText(_canvas, spdBuf, cx, 195, textdatum_t::middle_center, spdCol, COLOR_BG);

        _canvas->setFont(&fonts::Font4);
        _canvas->setTextSize(1);
        _canvas->setTextDatum(textdatum_t::top_center);
        _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
        _canvas->drawString("km/h", cx, 245);
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
    int cur_watts = (int)roundf(telemetry.power_watts);
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

    // 9. Bottom Left: G-Force Badge
    int cur_g10 = (int)roundf(telemetry.g_force * 10.0f);
    if (!_hyper_inited || cur_g10 != _cache.g_force_x10) {
        _cache.g_force_x10 = cur_g10;
        drawGForceBadge(_canvas, telemetry.g_force, 52, 416, 26);
    }

    // 9b. Bottom Left: Incline Badge (Only on Standard Dashboard!)
    int cur_inc_x10 = (int)roundf(telemetry.incline_deg * 10.0f);
    if (!_hyper_inited || cur_inc_x10 != _cache.incline_deg) {
        _cache.incline_deg = cur_inc_x10;
        drawInclineBadge(_canvas, telemetry.incline_deg, 112, 416, 26);
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

    // Middle Performance Tile Grid (6 Tiles):
    // Row 1: y = 156, h = 98
    // Tile 1: MAX LEISTUNG (PEAK WATT)
    drawCard(20, 156, 244, 98, COLOR_SURFACE, 0xD400); // Glowing gold border
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
    _canvas->drawString("MAX LEISTUNG (PEAK)", 32, 166);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Spitzenleistung ab Akku", 32, 226);

    // Tile 2: PEAK AKKU-STROM
    drawCard(278, 156, 244, 98, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("PEAK AKKU-STROM", 290, 166);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Maximaler Batterie-Strom", 290, 226);

    // Tile 3: PEAK PHASENSTROM
    drawCard(536, 156, 244, 98, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_RACE, COLOR_SURFACE);
    _canvas->drawString("PEAK PHASENSTROM", 548, 166);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Max. Motor-Drehmoment", 548, 226);

    // Row 2: y = 262, h = 98
    // Tile 4: VERBRAUCH & EFFIZIENZ
    drawCard(20, 262, 244, 98, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("VERBRAUCH & EFFIZIENZ", 32, 272);

    // Tile 5: AKKU-SPANNUNG (MIN/MAX)
    drawCard(278, 262, 244, 98, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("AKKU-SPANNUNG (MIN/MAX)", 290, 272);

    // Tile 6: MAX TEMPERATUR
    drawCard(536, 262, 244, 98, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
    _canvas->drawString("MAX TEMPERATUR", 548, 272);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("Motor / Inverter FETs", 548, 332);

    // Bottom Card: Interactive Display Backlight Control (y = 368, h = 98)
    drawCard(20, 368, 760, 98, COLOR_SURFACE, COLOR_BORDER);
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->drawString("DISPLAY HELLIGKEIT", 35, 380);
    _canvas->setTextColor(0x52AA, COLOR_SURFACE);
    _canvas->drawString("Tippen zum Anpassen", 35, 442);

    // Button [-]
    drawCard(210, 388, 56, 56, 0x18C3, 0x39C7);
    _canvas->setTextDatum(textdatum_t::middle_center);
    _canvas->setFont(&fonts::Font4);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->drawString("-", 238, 416);

    // Interactive Slider Track Outline (w=270, h=28)
    _canvas->fillRoundRect(280, 402, 270, 28, 8, 0x1084);
    _canvas->drawRoundRect(280, 402, 270, 28, 8, 0x2965);

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

    // Tile 1: MAX LEISTUNG (PEAK WATT)
    int cur_pwr = (int)(telemetry.stats.peak_power_watts + 0.5f);
    if (!_stats_inited || cur_pwr != _stats_cache.peak_watts) {
        _stats_cache.peak_watts = cur_pwr;
        _canvas->fillRect(32, 188, 220, 34, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char pwrBuf[16]; snprintf(pwrBuf, sizeof(pwrBuf), "%.0f W", telemetry.stats.peak_power_watts);
        _canvas->drawString(pwrBuf, 32, 188);
    }

    // Tile 2: PEAK AKKU-STROM
    int cur_curr = (int)(telemetry.stats.peak_current_amps * 10.0f + 0.5f);
    if (!_stats_inited || cur_curr != _stats_cache.peak_curr_x10) {
        _stats_cache.peak_curr_x10 = cur_curr;
        _canvas->fillRect(290, 188, 220, 34, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char curBuf[16]; snprintf(curBuf, sizeof(curBuf), "%.1f A", telemetry.stats.peak_current_amps);
        _canvas->drawString(curBuf, 290, 188);
    }

    // Tile 3: PEAK PHASENSTROM
    int cur_pha = (int)(telemetry.stats.peak_phase_amps * 10.0f + 0.5f);
    if (!_stats_inited || cur_pha != _stats_cache.peak_phase_x10) {
        _stats_cache.peak_phase_x10 = cur_pha;
        _canvas->fillRect(548, 188, 220, 34, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char phaBuf[16]; snprintf(phaBuf, sizeof(phaBuf), "%.1f A", telemetry.stats.peak_phase_amps);
        _canvas->drawString(phaBuf, 548, 188);
    }

    // Tile 4: VERBRAUCH & EFFIZIENZ
    int cur_eff = (int)(telemetry.stats.trip_wh_km * 10.0f + 0.5f);
    float dynWh = (telemetry.stats.dynamic_wh_km > 0.0f) ? telemetry.stats.dynamic_wh_km : telemetry.dynamic_wh_km;
    int cur_dyn = (int)(dynWh * 10.0f + 0.5f);
    if (!_stats_inited || cur_eff != _stats_cache.trip_wh_km_x10 || cur_dyn != _stats_cache.dyn_wh_km_x10) {
        _stats_cache.trip_wh_km_x10 = cur_eff;
        _stats_cache.dyn_wh_km_x10 = cur_dyn;
        _canvas->fillRect(32, 294, 220, 60, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char effBuf[24]; snprintf(effBuf, sizeof(effBuf), "%.1f Wh/km", telemetry.stats.trip_wh_km);
        _canvas->drawString(effBuf, 32, 294);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        char dynBuf[32]; snprintf(dynBuf, sizeof(dynBuf), "Rollend (2km): %.1f Wh/km", dynWh);
        _canvas->drawString(dynBuf, 32, 332);
    }

    // Tile 5: AKKU-SPANNUNG (MIN/MAX)
    int cur_min_v = (int)(telemetry.stats.min_voltage * 10.0f + 0.5f);
    int cur_max_v = (int)(telemetry.stats.max_voltage * 10.0f + 0.5f);
    int cur_live_v = (int)(telemetry.voltage * 10.0f + 0.5f);
    if (!_stats_inited || cur_min_v != _stats_cache.min_v_x10 || cur_max_v != _stats_cache.max_v_x10 || cur_live_v != _stats_cache.live_v_x10) {
        _stats_cache.min_v_x10 = cur_min_v;
        _stats_cache.max_v_x10 = cur_max_v;
        _stats_cache.live_v_x10 = cur_live_v;
        _canvas->fillRect(290, 294, 220, 60, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char vBuf[32]; snprintf(vBuf, sizeof(vBuf), "%.1fV / %.1fV", telemetry.stats.min_voltage, telemetry.stats.max_voltage);
        _canvas->drawString(vBuf, 290, 294);
        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        char vSub[32]; snprintf(vSub, sizeof(vSub), "Live: %.1fV (Sag Check)", telemetry.voltage);
        _canvas->drawString(vSub, 290, 332);
    }

    // Tile 6: MAX TEMPERATUR
    int cur_max_mot = (int)(telemetry.stats.max_temp_motor + 0.5f);
    int cur_max_esc = (int)(telemetry.stats.max_temp_esc + 0.5f);
    if (!_stats_inited || cur_max_mot != _stats_cache.max_temp_mot || cur_max_esc != _stats_cache.max_temp_esc) {
        _stats_cache.max_temp_mot = cur_max_mot;
        _stats_cache.max_temp_esc = cur_max_esc;
        _canvas->fillRect(548, 294, 220, 34, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        char tmpBuf[32]; snprintf(tmpBuf, sizeof(tmpBuf), "%.0f C / %.0f C", telemetry.stats.max_temp_motor, telemetry.stats.max_temp_esc);
        _canvas->drawString(tmpBuf, 548, 294);
    }

    // Bottom Card: Interactive Display Backlight Control
    int cur_br = Settings.get().brightness_pct;
    if (!_stats_inited || cur_br != _stats_cache.brightness_pct) {
        _stats_cache.brightness_pct = cur_br;
        _canvas->fillRect(35, 404, 150, 34, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        char brBuf[16]; snprintf(brBuf, sizeof(brBuf), "%d %%", cur_br);
        _canvas->drawString(brBuf, 35, 404);

        // Erase and redraw slider track & knob
        _canvas->fillRoundRect(280, 402, 270, 28, 8, 0x1084);
        _canvas->drawRoundRect(280, 402, 270, 28, 8, 0x2965);
        int fill_w = (int)((cur_br / 100.0f) * 270);
        if (fill_w > 270) fill_w = 270;
        if (fill_w > 8) {
            _canvas->fillRoundRect(280, 402, fill_w, 28, 8, COLOR_CYAN);
        }
        int knob_x = 280 + fill_w;
        if (knob_x < 290) knob_x = 290;
        if (knob_x > 540) knob_x = 540;
        _canvas->fillCircle(knob_x, 416, 12, COLOR_WHITE);
        _canvas->drawCircle(knob_x, 416, 12, 0x39C7);
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

    if (!_pin_screen_drawn) {
        _pin_screen_drawn = true;
        _screen_dirty = false;
        _last_pin_len = -1;
        _last_pin_err = !err;

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
        _canvas->setTextColor(0x52AA, 0x0823);
        _canvas->drawString("PIN aenderbar im Captive Portal / HyperDash Einstellungen", 400, 455);
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
    _canvas->fillScreen(COLOR_BG);
    if (_settings_tab_idx >= 4) {
        _settings_tab_idx = (uint8_t)telemetry.ride_mode;
        if (_settings_tab_idx >= 4) _settings_tab_idx = 0;
    }
    _menu_dirty = true;
}

void DashboardRenderer::renderSettingsScreen(const DashTelemetry &telemetry) {
    if (!_menu_dirty) return;
    _menu_dirty = false;

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
    _canvas->drawString("MODUS KONFIGURATION", 400, 30);

    _canvas->fillRoundRect(670, 12, 110, 36, 18, 0x18C3);
    _canvas->drawRoundRect(670, 12, 110, 36, 18, 0x39C7);
    _canvas->setTextColor(COLOR_WHITE, 0x18C3);
    _canvas->setFont(&fonts::Font2);
    _canvas->drawString("FERTIG", 725, 30);

    // 2. Mode Selection Touch Tabs: [ ECO ] | [ SPORT ] | [ RACE ] | [ VESC TOOL ]
    const char *tabNames[4] = { "ECO", "SPORT", "RACE", "VESC TOOL" };
    const char *tabSub[4]   = { "Sparsam", "Dynamisch", "35kW+ Boost", "Bridge & Tools" };
    uint16_t tabCols[4]     = { COLOR_GREEN, COLOR_CYAN, COLOR_RACE, COLOR_AMBER };
    uint16_t tabBgCols[4]   = { 0x0200, 0x0188, 0x4002, 0x31A0 };

    int tabX[4] = { 20, 210, 400, 590 };
    int tabW[4] = { 178, 178, 178, 190 };
    int tabH = 54;
    int tabY = 62;

    for (int i = 0; i < 4; i++) {
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

    if (_settings_tab_idx == 3) {
        // VESC TOOL CONNECTION CENTER (y: 130..450)
        // Left Card: Wireless Wi-Fi Bridge (AP)
        drawCard(20, 130, 370, 315, COLOR_SURFACE, COLOR_BORDER);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_GREEN, COLOR_SURFACE);
        _canvas->drawString("WI-FI BRIDGE (AP)", 38, 145);
        _canvas->setFont(&fonts::Font0);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("KABELLOS ZU VESC TOOL MOBILE & PC", 38, 175);

        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->drawString("SSID:   VESC-DASH-AP", 38, 205);
        _canvas->drawString("IP:     192.168.4.1", 38, 235);
        _canvas->drawString("Port:   6510 (TCP Standard)", 38, 265);
        _canvas->drawString("Passw:  Kein Passwort (Open)", 38, 295);

        _canvas->setFont(&fonts::Font0);
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        _canvas->drawString("1. Smartphone / PC mit WiFi verbinden", 38, 330);
        _canvas->drawString("2. In VESC Tool: TCP -> 192.168.4.1:6510", 38, 348);

        // Action Button: [ WI-FI BRIDGE STARTEN ]
        _canvas->fillRoundRect(38, 375, 334, 52, 10, 0x0A82);
        _canvas->drawRoundRect(38, 375, 334, 52, 10, COLOR_GREEN);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setTextColor(COLOR_WHITE, 0x0A82);
        _canvas->drawString("WI-FI BRIDGE STARTEN", 38 + 167, 375 + 26);

        // Right Card: USB-C Passthrough Bridge
        drawCard(410, 130, 370, 315, COLOR_SURFACE, COLOR_BORDER);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        _canvas->drawString("USB PASSTHROUGH", 428, 145);
        _canvas->setFont(&fonts::Font0);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
        _canvas->drawString("DIREKTKABEL PC <-> VESC", 428, 175);

        _canvas->setFont(&fonts::Font2);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->drawString("Port:   USB CDC (/dev/ttyACM0)", 428, 205);
        _canvas->drawString("Baud:   115200 (CDC <-> UART)", 428, 235);
        _canvas->drawString("Pins:   TX=43, RX=44 (Serial1)", 428, 265);
        _canvas->drawString("Status: 1:1 Live Hardware Bridge", 428, 295);

        _canvas->setFont(&fonts::Font0);
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        _canvas->drawString("1. PC per USB-C an Display anschliessen", 428, 330);
        _canvas->drawString("2. In VESC Tool: Serial / COM Port waehlen", 428, 348);

        // Action Button: [ USB BRIDGE STARTEN ]
        _canvas->fillRoundRect(428, 375, 334, 52, 10, 0x0188);
        _canvas->drawRoundRect(428, 375, 334, 52, 10, COLOR_CYAN);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setTextColor(COLOR_WHITE, 0x0188);
        _canvas->drawString("USB BRIDGE STARTEN", 428 + 167, 375 + 26);

        // Footer Strip
        _canvas->setFont(&fonts::Font0);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
        _canvas->drawString("Simulator TCP: 127.0.0.1:6510 aktiv  |  Zurueck mit [< DASH]", 400, 462);
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
}

void DashboardRenderer::handleMenuNav(NavAction action, DashTelemetry &telemetry) {
    if (action == NAV_NONE) return;

    if (action == NAV_SCREEN_DASH || action == NAV_MENU_BACK) {
        // Auto-save and apply limits
        VehicleModeConfig &cfg = ModeManager::instance().getConfig();
        uint8_t m = (_settings_tab_idx < 3) ? _settings_tab_idx : 1;
        for (int i = 0; i < 3; i++) {
            if (cfg.tuned_modes[i].max_battery_amps < 95.0f) cfg.tuned_modes[i].max_battery_amps = 95.0f;
            if (cfg.tuned_modes[i].max_phase_amps < 220.0f) cfg.tuned_modes[i].max_phase_amps = 220.0f;
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
        telemetry.screen = SCREEN_RIDE_DASH;
        _screen_dirty = true;
        _cache.invalidate();
        return;
    }

    VehicleModeConfig &cfg = ModeManager::instance().getConfig();
    uint8_t m = (_settings_tab_idx < 3) ? _settings_tab_idx : 1;
    ModeLimits &lim = cfg.tuned_modes[m];

    if (action == NAV_BOTH_PRESSED) {
        // Cycle tab: ECO -> SPORT -> RACE -> VESC TOOL -> ECO
        _settings_tab_idx = (_settings_tab_idx + 1) % 4;
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

    // 1. TOP BAR (y <= 55): Return to Dash & Auto-Save
    if (ty <= 55) {
        if (tx <= 180 || tx >= 620) {
            handleMenuNav(NAV_SCREEN_DASH, telemetry);
            return true;
        }
        return false;
    }

    // 2. TABS (y: 58..125)
    if (ty >= 58 && ty <= 125) {
        if (tx >= 20 && tx < 205) {
            _settings_tab_idx = 0; // ECO
            _menu_dirty = true;
            return true;
        } else if (tx >= 205 && tx < 395) {
            _settings_tab_idx = 1; // SPORT
            _menu_dirty = true;
            return true;
        } else if (tx >= 395 && tx < 585) {
            _settings_tab_idx = 2; // RACE
            _menu_dirty = true;
            return true;
        } else if (tx >= 585 && tx <= 780) {
            _settings_tab_idx = 3; // VESC TOOL
            _menu_dirty = true;
            return true;
        }
    }

    if (_settings_tab_idx == 3) {
        // Left Card Button: Start Wi-Fi Bridge
        if (tx >= 35 && tx <= 375 && ty >= 370 && ty <= 435) {
            Vesc.enterWifiBridgeMode();
            telemetry.screen = SCREEN_WIFI_BRIDGE;
            _screen_dirty = true;
            _cache.invalidate();
            Buttons.setActiveScreen(SCREEN_WIFI_BRIDGE);
            return true;
        }
        // Right Card Button: Start USB Bridge
        if (tx >= 425 && tx <= 765 && ty >= 370 && ty <= 435) {
            Vesc.enterBridgeMode();
            telemetry.screen = SCREEN_VESC_BRIDGE;
            _screen_dirty = true;
            _cache.invalidate();
            Buttons.setActiveScreen(SCREEN_VESC_BRIDGE);
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
        // Slider Track tap / drag
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
        // Slider Track tap / drag
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

// ==============================================================================
// SCREEN 6: "RACE" DRAGY / LAUNCH ACCELERATION TIMER (800x480)
// ==============================================================================
void DashboardRenderer::updateRaceTimer(DashTelemetry &telemetry, uint32_t now) {
    if (telemetry.screen != SCREEN_RACE_TIMER) {
        if (telemetry.race_state != 0) {
            telemetry.race_state = 0;
            telemetry.boost_active = false;
            Buttons.setRaceState(0);
        }
        return;
    }

    // Continuous Thermal Protection: Abort launch if temperatures exceed 65 °C during run
    if (isRaceThermalBlocked(telemetry)) {
        if (telemetry.race_state == 1 || telemetry.race_state == 2) {
            telemetry.race_state = 0; // IDLE / ABORT
            telemetry.boost_active = false;
            telemetry.ride_mode = telemetry.saved_ride_mode;
            ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
            Buttons.setRaceState(0);
            markScreenDirty();
            return;
        }
    }

    if (telemetry.race_state == 1) { // 1 = COUNTDOWN (5s)
        telemetry.boost_active = true;
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
            Buttons.setRaceState(2);
            markScreenDirty();
        }
    } else if (telemetry.race_state == 2) { // 2 = MEASURING
        telemetry.boost_active = true;
        telemetry.boost_end_ms = now + 10000;
        telemetry.race_elapsed_sec = (float)(now - telemetry.race_launch_start_ms) / 1000.0f;

        // Check 0 - 22 km/h split
        if (telemetry.race_split_0_22 <= 0.0f && telemetry.speed_kmh >= 22.0f) {
            telemetry.race_split_0_22 = (telemetry.race_elapsed_sec > 0.01f) ? telemetry.race_elapsed_sec : 0.01f;
            if (telemetry.race_records.best_0_22_sec <= 0.01f || telemetry.race_split_0_22 < telemetry.race_records.best_0_22_sec) {
                telemetry.race_records.best_0_22_sec = telemetry.race_split_0_22;
                Settings.saveRaceRecords(telemetry.race_records);
            }
        }

        // Check 0 - 50 km/h split
        if (telemetry.race_split_0_50 <= 0.0f && telemetry.speed_kmh >= 50.0f) {
            telemetry.race_split_0_50 = (telemetry.race_elapsed_sec > 0.01f) ? telemetry.race_elapsed_sec : 0.01f;
            if (telemetry.race_records.best_0_50_sec <= 0.01f || telemetry.race_split_0_50 < telemetry.race_records.best_0_50_sec) {
                telemetry.race_records.best_0_50_sec = telemetry.race_split_0_50;
                Settings.saveRaceRecords(telemetry.race_records);
            }
        }

        // Check 0 - 70 km/h split
        if (telemetry.race_split_0_70 <= 0.0f && telemetry.speed_kmh >= 70.0f) {
            telemetry.race_split_0_70 = (telemetry.race_elapsed_sec > 0.01f) ? telemetry.race_elapsed_sec : 0.01f;
            if (telemetry.race_records.best_0_70_sec <= 0.01f || telemetry.race_split_0_70 < telemetry.race_records.best_0_70_sec) {
                telemetry.race_records.best_0_70_sec = telemetry.race_split_0_70;
                Settings.saveRaceRecords(telemetry.race_records);
            }
            telemetry.race_state = 3; // FINISHED
            telemetry.boost_active = false;
            telemetry.ride_mode = telemetry.saved_ride_mode;
            ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
            Buttons.setRaceState(3);
            markScreenDirty();
        }

        // 15-second measurement timeout cap (automatic race end)
        if (telemetry.race_elapsed_sec >= 15.0f) {
            telemetry.race_state = 3; // FINISHED
            telemetry.boost_active = false;
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

    // Top-Right [ BESTZEITEN ] Button (x=620, y=12, w=160, h=36, r=18)
    _canvas->fillRoundRect(620, 12, 160, 36, 18, COLOR_SURFACE);
    _canvas->drawRoundRect(620, 12, 160, 36, 18, COLOR_BORDER);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    _canvas->setFont(&fonts::Font2);
    _canvas->drawString("BESTZEITEN", 700, 30);

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
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_SURFACE);
    _canvas->setTextDatum(textdatum_t::top_left);
    _canvas->drawString("0 - 22 km/h", 548, 100);
    _canvas->drawFastHLine(548, 185, 214, COLOR_SURFACE_LIGHT);

    _canvas->drawString("0 - 50 km/h", 548, 205);
    _canvas->drawFastHLine(548, 290, 214, COLOR_SURFACE_LIGHT);

    _canvas->drawString("0 - 70 km/h", 548, 310);
}

void DashboardRenderer::renderRaceTimerScreen(const DashTelemetry &telemetry) {
    int cx = 310, cy = 255;
    int r_in = 130, r_out = 158;
    float mid_r = 144.0f;
    float deg2rad = 3.14159265f / 180.0f;

    // 1. Top-Center Timer Display
    _canvas->fillRect(145, 10, 465, 46, COLOR_BG);
    if (telemetry.race_state == 2 || telemetry.race_state == 3) {
        char timeBuf[16];
        snprintf(timeBuf, sizeof(timeBuf), "%.2f", telemetry.race_elapsed_sec);
        int nw = _canvas->textWidth(timeBuf, &fonts::Font6);
        int sw = _canvas->textWidth("s", &fonts::Font4);
        int totalW = nw + 6 + sw;
        int startX = 375 - totalW / 2;

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
        _canvas->drawString(cdTitle, 375, 30);
    } else {
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_BG);
        _canvas->setTextDatum(textdatum_t::middle_center);
        _canvas->drawString("LAUNCH CONTROL", 375, 30);
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

    // 4. Update Corner Card Split Values (Clean time only during run; *PB* only on finished run)
    auto drawSplit = [&](int y, float split_val, float best_val, bool is_active) {
        _canvas->fillRect(548, y, 215, 36, COLOR_SURFACE);
        _canvas->setFont(&fonts::Font4);
        _canvas->setTextDatum(textdatum_t::top_left);
        char sBuf[32];
        if (split_val > 0.0f) {
            bool is_best = (telemetry.race_state == 3 && best_val > 0.0f && split_val <= best_val + 0.001f);
            snprintf(sBuf, sizeof(sBuf), "%.2f s%s", split_val, is_best ? " *PB*" : "");
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

    drawSplit(130, telemetry.race_split_0_22, telemetry.race_records.best_0_22_sec, active_0_22);
    drawSplit(235, telemetry.race_split_0_50, telemetry.race_records.best_0_50_sec, active_0_50);
    drawSplit(340, telemetry.race_split_0_70, telemetry.race_records.best_0_70_sec, active_0_70);

    // Bottom-Left G-Force Badge
    drawGForceBadge(_canvas, telemetry.g_force, 52, 416, 26);

    // 5. Bestzeiten Popup Modal (if open)
    if (telemetry.race_popup_open) {
        drawRaceRecordsModal(telemetry);
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

    drawRecordRow(my + 70, "0 - 22 km/h", telemetry.race_records.best_0_22_sec);
    drawRecordRow(my + 130, "0 - 50 km/h", telemetry.race_records.best_0_50_sec);
    drawRecordRow(my + 190, "0 - 70 km/h", telemetry.race_records.best_0_70_sec);

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
