#include "dash_renderer.h"
#include "buttons.h"
#include "vesc_uart.h"
#include <math.h>

DashboardRenderer Renderer;

// Modern High-Contrast Color Palette (Optimized for 800x480 LCD)
static const uint16_t COLOR_BG            = 0x0821; // Deep Obsidian Black
static const uint16_t COLOR_SURFACE       = 0x10A2; // Card Background
static const uint16_t COLOR_SURFACE_LIGHT = 0x2124; // Card Highlight / Active Item
static const uint16_t COLOR_BORDER        = 0x39E7; // Crisp Border
static const uint16_t COLOR_WHITE         = 0xFFFF; // Pure White
static const uint16_t COLOR_LIGHT_GRAY    = 0xDEFB; // Crisp Bright Silver
static const uint16_t COLOR_MUTED_GRAY    = 0x9CD3; // Medium Silver-Gray
static const uint16_t COLOR_AMBER         = 0xFD20; // High Power / Warning
static const uint16_t COLOR_RED           = 0xF800; // Redline / Field Weakening / Alarm
static const uint16_t COLOR_GREEN         = 0x07E0; // Connected / Safe / Efficient
static const uint16_t COLOR_CYAN          = 0x07FF; // Electric Accent / Profile
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

DashboardRenderer::DashboardRenderer()
    : _display(nullptr),
      _canvas(nullptr),
      _active_screen(SCREEN_RIDE_DASH),
      _active_style(STYLE_ANALOG_DIAL),
      _screen_dirty(true),
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
      _last_sweep_tick_ms(0) {
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
    _last_sweep_tick_ms = millis();
}

// Top-Level Screen Dispatcher
void DashboardRenderer::render(const DashTelemetry &telemetry) {
    // 1. Needle sweep animation physics (120% duty cycle sweep)
    if (_is_sweeping) {
        uint32_t now = millis();
        float dt = (now - _last_sweep_tick_ms) / 1000.0f;
        _last_sweep_tick_ms = now;

        if (_sweep_phase == 0) { // Sweep up: 0 -> 120% in 0.6s
            _sweep_progress += dt / 0.6f;
            if (_sweep_progress >= 1.0f) {
                _sweep_progress = 1.0f;
                _sweep_phase = 1;
            }
            _sweep_duty = _sweep_progress * 120.0f;
        } else if (_sweep_phase == 1) { // Sweep down: 120 -> 0% in 0.5s
            _sweep_progress -= dt / 0.5f;
            if (_sweep_progress <= 0.0f) {
                _sweep_progress = 0.0f;
                _sweep_duty = 0.0f;
                _is_sweeping = false;
            } else {
                _sweep_duty = _sweep_progress * 120.0f;
            }
        }
    }

    // 2. Detect Screen Switch
    if (telemetry.screen != _active_screen) {
        _active_screen = telemetry.screen;
        _screen_dirty = true;
        _cache.invalidate();
        _last_needle.valid = false;
        if (_active_screen == SCREEN_RIDE_DASH && Settings.get().dash_style == STYLE_ANALOG_DIAL) {
            triggerNeedleSweep();
        }
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

        case SCREEN_ENERGY_STATS:
            if (_screen_dirty) {
                _canvas->fillScreen(COLOR_BG);
                initEnergyStatsScreen(telemetry);
                _screen_dirty = false;
            }
            renderEnergyStatsScreen(telemetry);
            break;

        case SCREEN_PERF_STATS:
            if (_screen_dirty) {
                _canvas->fillScreen(COLOR_BG);
                initPerfStatsScreen(telemetry);
                _screen_dirty = false;
            }
            renderPerfStatsScreen(telemetry);
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
}

// ==============================================================================
// SCREEN 0: MAIN RIDE DASHBOARD
// ==============================================================================
void DashboardRenderer::initRideDashboard(const DashTelemetry &telemetry) {
    if (_active_style == STYLE_ANALOG_DIAL) {
        initLeftHugAnalogStyle(telemetry);
    } else {
        initHorizontalBarStyle(telemetry);
    }
}

void DashboardRenderer::renderRideDashboard(const DashTelemetry &telemetry) {
    if (_active_style == STYLE_ANALOG_DIAL) {
        renderLeftHugAnalogStyle(telemetry);
    } else {
        renderHorizontalBarStyle(telemetry);
    }
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
    int curSpeed = (int)roundf(telemetry.speed_kmh);
    if (curSpeed != _cache.speed) {
        _cache.speed = curSpeed;
        char spdStr[8];
        snprintf(spdStr, sizeof(spdStr), "%d", curSpeed);
        _canvas->setTextColor(getSpeedColor(telemetry.speed_kmh), COLOR_BG);
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
    int curSpeed = (int)roundf(telemetry.speed_kmh);
    if (curSpeed != _cache.speed) {
        _cache.speed = curSpeed;
        char spdStr[8];
        snprintf(spdStr, sizeof(spdStr), "%d", curSpeed);
        _canvas->setTextColor(getSpeedColor(telemetry.speed_kmh), COLOR_BG);
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
// SCREEN 1: ENERGY & BATTERY ANALYTICS DASHBOARD (800x480 Optimized)
// ==============================================================================
void DashboardRenderer::initEnergyStatsScreen(const DashTelemetry &telemetry) {
    // Header Bar
    _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
    _canvas->drawString("1. ENERGY & BATTERY", 25, 14, &fonts::Font4);

    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
    _canvas->drawString("[1/2]", 310, 14, &fonts::Font4);

    _canvas->drawFastHLine(20, 44, 760, COLOR_BORDER);

    // Two Large Cards (w=370, h=405)
    drawCard(20,  56, 370, 405, COLOR_SURFACE, COLOR_BORDER);
    drawCard(410, 56, 370, 405, COLOR_SURFACE, COLOR_BORDER);

    // Card 1 Static Labels
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("CONSUMPTION & EFFICIENCY", 35, 70, &fonts::Font4);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->drawString("AVERAGE EFFICIENCY", 35, 100, &fonts::Font4);
    _canvas->drawFastHLine(35, 205, 340, COLOR_BORDER);

    _canvas->drawString("Trip Energy:", 35, 228, &fonts::Font4);
    _canvas->drawString("Charge Used:", 35, 272, &fonts::Font4);
    _canvas->drawString("Pack Remaining:", 35, 316, &fonts::Font4);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("Projected Range:", 35, 360, &fonts::Font4);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->drawString("Pack Voltage:", 35, 404, &fonts::Font4);

    // Card 2 Static Labels
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("BATTERY HEALTH & PACK", 425, 70, &fonts::Font4);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->drawString("BATTERY HEALTH (SoH)", 425, 100, &fonts::Font4);
    _canvas->drawFastHLine(425, 205, 340, COLOR_BORDER);

    _canvas->drawString("Active Profile:", 425, 228, &fonts::Font4);
    _canvas->drawString("Learned Capacity:", 425, 272, &fonts::Font4);
    _canvas->drawString("Nominal Capacity:", 425, 316, &fonts::Font4);
    _canvas->drawString("Moving Time:", 425, 360, &fonts::Font4);
    _canvas->drawString("VESC Link:", 425, 404, &fonts::Font4);
}

void DashboardRenderer::renderEnergyStatsScreen(const DashTelemetry &telemetry) {
    // Top Bar: Moving Ride Time
    uint32_t rTime = telemetry.stats.ride_time_sec;
    if (rTime != _cache.ride_sec) {
        _cache.ride_sec = rTime;
        int hrs = rTime / 3600;
        int mins = (rTime % 3600) / 60;
        int secs = rTime % 60;
        char timeStr[32];
        snprintf(timeStr, sizeof(timeStr), "MOVING: %02d:%02d:%02d", hrs, mins, secs);
        _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
        _canvas->setTextPadding(220);
        _canvas->drawRightString(timeStr, 770, 14, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    // Card 1: Hero Metric (Avg Efficiency Wh/km)
    int curEffX10 = (int)roundf(telemetry.stats.trip_wh_km * 10.0f);
    if (curEffX10 != _cache.eff_x10) {
        _cache.eff_x10 = curEffX10;
        char effStr[16];
        if (telemetry.stats.trip_wh_km > 0.0f) {
            snprintf(effStr, sizeof(effStr), "%.1f", telemetry.stats.trip_wh_km);
        } else {
            snprintf(effStr, sizeof(effStr), "--.-");
        }
        drawValWithUnit(35, 130, 135, 100, effStr, "Wh/km", COLOR_WHITE, COLOR_CYAN, COLOR_SURFACE);
    }

    // Card 1: Secondary Rows
    char buf[32];
    int curTripWh = (int)roundf(telemetry.stats.trip_wh);
    if (curTripWh != _cache.watts) {
        _cache.watts = curTripWh;
        snprintf(buf, sizeof(buf), "%.0f Wh", telemetry.stats.trip_wh);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setTextPadding(140);
        _canvas->drawRightString(buf, 370, 228, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curAhX100 = (int)roundf(telemetry.amphours_used * 100.0f);
    if (curAhX100 != _cache.current_x10) {
        _cache.current_x10 = curAhX100;
        snprintf(buf, sizeof(buf), "%.2f Ah", telemetry.amphours_used);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setTextPadding(140);
        _canvas->drawRightString(buf, 370, 272, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curRemWh = (int)roundf(telemetry.remaining_wh);
    if (curRemWh != _cache.remaining_wh) {
        _cache.remaining_wh = curRemWh;
        if (telemetry.remaining_wh < 0.0f) {
            snprintf(buf, sizeof(buf), "N/A");
        } else {
            snprintf(buf, sizeof(buf), "%.0f Wh", telemetry.remaining_wh);
        }
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setTextPadding(140);
        _canvas->drawRightString(buf, 370, 316, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curRange = (int)roundf(telemetry.est_range_km);
    if (curRange != _cache.est_range) {
        _cache.est_range = curRange;
        if (telemetry.est_range_km < 0.0f) {
            snprintf(buf, sizeof(buf), "N/A");
        } else {
            snprintf(buf, sizeof(buf), "%.1f km", telemetry.est_range_km);
        }
        _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
        _canvas->setTextPadding(140);
        _canvas->drawRightString(buf, 370, 360, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curVoltX10 = (int)roundf(telemetry.voltage * 10.0f);
    if (curVoltX10 != _cache.voltage_x10) {
        _cache.voltage_x10 = curVoltX10;
        snprintf(buf, sizeof(buf), "%.1f V (%.0f%%)", telemetry.voltage, telemetry.battery_pct);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setTextPadding(140);
        _canvas->drawRightString(buf, 370, 404, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    // Card 2: Hero Metric (SoH %)
    int curSoh = (int)roundf(telemetry.battery_health_soh);
    if (curSoh != _cache.battery_pct) {
        _cache.battery_pct = curSoh;
        char sohNumStr[16];
        snprintf(sohNumStr, sizeof(sohNumStr), "%.0f", telemetry.battery_health_soh);
        drawValWithUnit(425, 130, 135, 70, sohNumStr, "%", COLOR_CYAN, COLOR_CYAN, COLOR_SURFACE);
    }

    // Card 2: Secondary Rows
    uint8_t pId = telemetry.battery_profile_id;
    snprintf(buf, sizeof(buf), "P%d (%s)", pId + 1, (pId == 0) ? "Fresh" : "Daily");
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->setTextPadding(140);
    _canvas->drawRightString(buf, 760, 228, &fonts::Font4);
    _canvas->setTextPadding(0);

    snprintf(buf, sizeof(buf), "%.0f Wh", Battery.getProfile(pId).learned_wh);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    _canvas->setTextPadding(140);
    _canvas->drawRightString(buf, 760, 272, &fonts::Font4);
    _canvas->setTextPadding(0);

    snprintf(buf, sizeof(buf), "%.0f Wh", Battery.getProfile(pId).nominal_wh);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->setTextPadding(140);
    _canvas->drawRightString(buf, 760, 316, &fonts::Font4);
    _canvas->setTextPadding(0);

    int hrs = rTime / 3600;
    int mins = (rTime % 3600) / 60;
    snprintf(buf, sizeof(buf), "%02dh %02dm", hrs, mins);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    _canvas->setTextPadding(140);
    _canvas->drawRightString(buf, 760, 360, &fonts::Font4);
    _canvas->setTextPadding(0);

    snprintf(buf, sizeof(buf), telemetry.vesc_connected ? "ONLINE" : "OFFLINE");
    _canvas->setTextColor(telemetry.vesc_connected ? COLOR_GREEN : COLOR_RED, COLOR_SURFACE);
    _canvas->setTextPadding(100);
    _canvas->drawRightString(buf, 760, 404, &fonts::Font4);
    _canvas->setTextPadding(0);
}

// ==============================================================================
// SCREEN 2: POWER & PERFORMANCE ANALYTICS DASHBOARD (800x480 Optimized)
// ==============================================================================
void DashboardRenderer::initPerfStatsScreen(const DashTelemetry &telemetry) {
    // Header Bar
    _canvas->setTextColor(COLOR_WHITE, COLOR_BG);
    _canvas->drawString("2. POWER & DYNAMICS", 25, 14, &fonts::Font4);

    _canvas->setTextColor(COLOR_MUTED_GRAY, COLOR_BG);
    _canvas->drawString("[2/2]", 310, 14, &fonts::Font4);

    _canvas->drawFastHLine(20, 44, 760, COLOR_BORDER);

    // Two Large Cards (w=370, h=405)
    drawCard(20,  56, 370, 405, COLOR_SURFACE, COLOR_BORDER);
    drawCard(410, 56, 370, 405, COLOR_SURFACE, COLOR_BORDER);

    // Card 1 Static Labels
    _canvas->setTextColor(COLOR_AMBER, COLOR_SURFACE);
    _canvas->drawString("ELECTRICAL POWER PEAKS", 35, 70, &fonts::Font4);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->drawString("PEAK ELECTRICAL POWER", 35, 100, &fonts::Font4);
    _canvas->drawFastHLine(35, 205, 340, COLOR_BORDER);

    _canvas->drawString("Peak Battery Curr:", 35, 228, &fonts::Font4);
    _canvas->drawString("Peak Phase Curr:", 35, 272, &fonts::Font4);
    _canvas->drawString("Peak Motor Temp:", 35, 316, &fonts::Font4);
    _canvas->drawString("Peak ESC Temp:", 35, 360, &fonts::Font4);
    _canvas->drawString("Current Power:", 35, 404, &fonts::Font4);

    // Card 2 Static Labels
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("SPEED & TRIP DYNAMICS", 425, 70, &fonts::Font4);
    _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
    _canvas->drawString("MAX VEHICLE SPEED", 425, 100, &fonts::Font4);
    _canvas->drawFastHLine(425, 205, 340, COLOR_BORDER);

    _canvas->drawString("Average Speed:", 425, 228, &fonts::Font4);
    _canvas->drawString("Trip Distance:", 425, 272, &fonts::Font4);
    _canvas->drawString("Lifetime ODO:", 425, 316, &fonts::Font4);
    _canvas->drawString("Ride Duration:", 425, 360, &fonts::Font4);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawString("Trip Statistics:", 425, 404, &fonts::Font4);
}

void DashboardRenderer::renderPerfStatsScreen(const DashTelemetry &telemetry) {
    // Top Bar: Moving Ride Time
    uint32_t rTime = telemetry.stats.ride_time_sec;
    if (rTime != _cache.ride_sec) {
        _cache.ride_sec = rTime;
        int hrs = rTime / 3600;
        int mins = (rTime % 3600) / 60;
        int secs = rTime % 60;
        char timeStr[32];
        snprintf(timeStr, sizeof(timeStr), "MOVING: %02d:%02d:%02d", hrs, mins, secs);
        _canvas->setTextColor(COLOR_CYAN, COLOR_BG);
        _canvas->setTextPadding(220);
        _canvas->drawRightString(timeStr, 770, 14, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    // Card 1: Hero Metric (Peak Power)
    int curPeakWatts = (int)roundf(telemetry.stats.peak_power_watts);
    if (curPeakWatts != _cache.peak_watts) {
        _cache.peak_watts = curPeakWatts;
        char pNumStr[16];
        snprintf(pNumStr, sizeof(pNumStr), "%d", curPeakWatts);
        uint16_t pCol = getPowerColor(telemetry.stats.peak_power_watts);
        drawValWithUnit(35, 130, 135, 60, pNumStr, "W", pCol, COLOR_AMBER, COLOR_SURFACE);
    }

    // Card 1: Secondary Rows
    char buf[32];
    int curPeakCurrX10 = (int)roundf(telemetry.stats.peak_current_amps * 10.0f);
    if (curPeakCurrX10 != _cache.peak_curr_x10) {
        _cache.peak_curr_x10 = curPeakCurrX10;
        snprintf(buf, sizeof(buf), "%.1f A", telemetry.stats.peak_current_amps);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setTextPadding(100);
        _canvas->drawRightString(buf, 370, 228, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curPeakPhaseX10 = (int)roundf(telemetry.stats.peak_phase_amps * 10.0f);
    if (curPeakPhaseX10 != _cache.peak_phase_x10) {
        _cache.peak_phase_x10 = curPeakPhaseX10;
        snprintf(buf, sizeof(buf), "%.1f A", telemetry.stats.peak_phase_amps);
        _canvas->setTextColor(getPhaseColor(telemetry.stats.peak_phase_amps), COLOR_SURFACE);
        _canvas->setTextPadding(100);
        _canvas->drawRightString(buf, 370, 272, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curMaxMot = (int)roundf(telemetry.stats.max_temp_motor);
    if (curMaxMot != _cache.temp_motor) {
        _cache.temp_motor = curMaxMot;
        snprintf(buf, sizeof(buf), "%.1f C", telemetry.stats.max_temp_motor);
        _canvas->setTextColor(getTempColor(telemetry.stats.max_temp_motor), COLOR_SURFACE);
        _canvas->setTextPadding(100);
        _canvas->drawRightString(buf, 370, 316, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curMaxEsc = (int)roundf(telemetry.stats.max_temp_esc);
    if (curMaxEsc != _cache.temp_esc) {
        _cache.temp_esc = curMaxEsc;
        snprintf(buf, sizeof(buf), "%.1f C", telemetry.stats.max_temp_esc);
        _canvas->setTextColor(getTempColor(telemetry.stats.max_temp_esc), COLOR_SURFACE);
        _canvas->setTextPadding(100);
        _canvas->drawRightString(buf, 370, 360, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curWatts = (int)roundf(telemetry.power_watts);
    snprintf(buf, sizeof(buf), "%d W", curWatts);
    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->setTextPadding(100);
    _canvas->drawRightString(buf, 370, 404, &fonts::Font4);
    _canvas->setTextPadding(0);

    // Card 2: Hero Metric (Max Speed)
    int curMaxSpdX10 = (int)roundf(telemetry.stats.max_speed_kmh * 10.0f);
    if (curMaxSpdX10 != _cache.max_spd_x10) {
        _cache.max_spd_x10 = curMaxSpdX10;
        char spdNumStr[16];
        snprintf(spdNumStr, sizeof(spdNumStr), "%.1f", telemetry.stats.max_speed_kmh);
        drawValWithUnit(425, 130, 135, 80, spdNumStr, "km/h", getSpeedColor(telemetry.stats.max_speed_kmh), COLOR_CYAN, COLOR_SURFACE);
    }

    // Card 2: Secondary Rows
    int curAvgSpdX10 = (int)roundf(telemetry.stats.avg_speed_kmh * 10.0f);
    if (curAvgSpdX10 != _cache.avg_spd_x10) {
        _cache.avg_spd_x10 = curAvgSpdX10;
        snprintf(buf, sizeof(buf), "%.1f km/h", telemetry.stats.avg_speed_kmh);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setTextPadding(140);
        _canvas->drawRightString(buf, 760, 228, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curTripX10 = (int)roundf(telemetry.trip_km * 10.0f);
    if (curTripX10 != _cache.trip_x10) {
        _cache.trip_x10 = curTripX10;
        snprintf(buf, sizeof(buf), "%.1f km", telemetry.trip_km);
        _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
        _canvas->setTextPadding(140);
        _canvas->drawRightString(buf, 760, 272, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int curOdo = (int)roundf(telemetry.odo_km);
    if (curOdo != _cache.odo) {
        _cache.odo = curOdo;
        snprintf(buf, sizeof(buf), "%.1f km", telemetry.odo_km);
        _canvas->setTextColor(COLOR_LIGHT_GRAY, COLOR_SURFACE);
        _canvas->setTextPadding(140);
        _canvas->drawRightString(buf, 760, 316, &fonts::Font4);
        _canvas->setTextPadding(0);
    }

    int hrs = rTime / 3600;
    int mins = (rTime % 3600) / 60;
    snprintf(buf, sizeof(buf), "%02dh %02dm", hrs, mins);
    _canvas->setTextColor(COLOR_WHITE, COLOR_SURFACE);
    _canvas->setTextPadding(140);
    _canvas->drawRightString(buf, 760, 360, &fonts::Font4);
    _canvas->setTextPadding(0);

    _canvas->setTextColor(COLOR_CYAN, COLOR_SURFACE);
    _canvas->drawRightString("[Hold to Reset]", 760, 404, &fonts::Font4);
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
        case 4: return 6;
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
