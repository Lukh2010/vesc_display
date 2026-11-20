#include "dash_renderer.h"
#include "buttons.h"
#include "vesc_uart.h"
#include "fault_manager.h"
#include "mode_manager.h"
#include "ota_manager.h"
#include "tcs_tpms_manager.h"
#include "boot_logo.h"
#include "ui_fonts.h"
#include <math.h>
#include <algorithm>

DashboardRenderer Renderer;

// Base Neutral Constants (used only for pure-black boot screen & fallback math)
static const uint16_t COLOR_BLACK = 0x0000;
static const uint16_t COLOR_WHITE = 0xFFFF;

// ==============================================================================
// 4 Clean Automotive Color Themes (RGB565)
// Matches ColorThemePalette field order in include/dash_types.h exactly!
// ==============================================================================
static const ColorThemePalette s_theme_palettes[THEME_COUNT] = {
    // 0: OBSIDIAN DARK — Pure #000000 OLED dark, graphite cards, crisp ice-white/sky accent
    {
        THEME_OBSIDIAN_DARK,
        "OBSIDIAN DARK",
        "Pure Black OLED & Ice Sky",
        0x0000, // bg:          #000000
        0x10A3, // surface:     #10151F
        0x1906, // surface_alt: #1B2332
        0x29A8, // border:      #283447
        0xFFFF, // text:        #FFFFFF
        0x8CD6, // muted:       #8C9BB0
        0x3DFF, // primary:     #38BDF8 (Crisp Ice Sky)
        0xDEFB, // secondary:   #DCE7F2 (Crisp Ice White)
        0x1947, // track:       #1E293B
        0x3DFF, // needle:      #38BDF8
        0x262B, // accent_ok:   #22C55E (Emerald)
        0xF4E1, // accent_warn: #F59E0B (Amber)
        0xEA28, // accent_danger:#EF4444 (Crimson)
        false   // is_light
    },
    // 1: ARCTIC LIGHT — Crisp daylight white/silver theme, dark obsidian ink & sky-slate accent
    {
        THEME_ARCTIC_LIGHT,
        "ARCTIC LIGHT",
        "Daylight White & Slate",
        0xEF9F, // bg:          #EFF3F8
        0xFFFF, // surface:     #FFFFFF
        0xE75E, // surface_alt: #E2E8F0
        0xCEBC, // border:      #CBD5E1
        0x08A5, // text:        #0F172A (Deep Obsidian Ink)
        0x63B1, // muted:       #64748B (Cool Slate)
        0x0438, // primary:     #0284C7 (Deep Sky Blue)
        0x42AE, // secondary:   #475569 (Dark Slate)
        0xDF3D, // track:       #DCE4EE
        0x0438, // needle:      #0284C7
        0x1509, // accent_ok:   #16A34A (Deep Emerald)
        0xDBA0, // accent_warn: #D97706 (Deep Amber)
        0xD924, // accent_danger:#DC2626 (Deep Crimson)
        true    // is_light
    },
    // 2: CARBON STEALTH — Matte charcoal cockpit, titanium silver & cool steel accent
    {
        THEME_CARBON_STEALTH,
        "CARBON STEALTH",
        "Matte Charcoal & Titanium",
        0x0862, // bg:          #0A0D12
        0x10C4, // surface:     #151A22
        0x2126, // surface_alt: #202733
        0x31E9, // border:      #323C4D
        0xF7BF, // text:        #F1F5F9
        0x9517, // muted:       #94A3B8
        0xCEBC, // primary:     #CBD5E1 (Brushed Titanium)
        0x649A, // secondary:   #6092D0 (Steel Slate Blue)
        0x1925, // track:       #1E242E
        0xF7BF, // needle:      #F1F5F9
        0x3693, // accent_ok:   #34D399
        0xFDE4, // accent_warn: #FBBF24
        0xFB8E, // accent_danger:#F87171
        false   // is_light
    },
    // 3: NIGHT DIM — Ultra-low-glare software dimmer (pure black bg, dim warm amber & gray)
    {
        THEME_NIGHT_DIM,
        "NIGHT DIM",
        "Low-Glare Software Dimmer",
        0x0000, // bg:          #000000
        0x0862, // surface:     #0A0D12
        0x10C3, // surface_alt: #14181F
        0x2126, // border:      #222730
        0x9C6F, // text:        #9A8F7E (Dim Warm Gray)
        0x5AA9, // muted:       #5A554C (Dim Muted Stone)
        0xBBC5, // primary:     #B87A28 (Dim Warm Amber)
        0x6B4B, // secondary:   #6E695E (Dim Warm Pewter)
        0x18C4, // track:       #181B22
        0xBBC5, // needle:      #B87A28
        0x4C4B, // accent_ok:   #4D8A5A (Dim Sage)
        0xBBC5, // accent_warn: #B87A28 (Dim Amber)
        0xA9E7, // accent_danger:#A83C3C (Dim Crimson)
        false   // is_light
    }
};

// ==============================================================================
// Color & Geometry Helpers
// ==============================================================================
static inline uint16_t blendRgb565(uint16_t c1, uint16_t c2, uint8_t alpha) {
    uint32_t r1 = (c1 >> 11) & 0x1F;
    uint32_t g1 = (c1 >> 5)  & 0x3F;
    uint32_t b1 =  c1        & 0x1F;
    uint32_t r2 = (c2 >> 11) & 0x1F;
    uint32_t g2 = (c2 >> 5)  & 0x3F;
    uint32_t b2 =  c2        & 0x1F;
    uint32_t inv = 255u - alpha;
    uint32_t r = (r1 * inv + r2 * alpha) / 255u;
    uint32_t g = (g1 * inv + g2 * alpha) / 255u;
    uint32_t b = (b1 * inv + b2 * alpha) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t onPrimaryFg(const ColorThemePalette &pal) {
    return pal.is_light ? pal.surface : pal.bg;
}

static inline uint16_t getModeColor(int mode_idx, const ColorThemePalette &pal) {
    switch (mode_idx % 3) {
        case 0:  return pal.accent_ok;     // ECO
        case 1:  return pal.primary;       // SPORT
        default: return pal.accent_danger; // RACE
    }
}

static inline uint16_t getTempColorTheme(float temp, const ColorThemePalette &pal) {
    if (temp >= 80.0f) return pal.accent_danger;
    if (temp >= 62.0f) return pal.accent_warn;
    return pal.text;
}

#if ENABLE_LIGHTS_SCREEN
static inline uint16_t hueToRgb565(uint8_t hue) {
    uint8_t r, g, b;
    uint8_t region = hue / 43;
    uint8_t remainder = (hue - (region * 43)) * 6;
    uint8_t p = 0, q = 255 - remainder, t = remainder;
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
#endif

const ColorThemePalette& DashboardRenderer::getThemePalette(uint8_t theme_id) {
    if (theme_id >= THEME_COUNT) theme_id = THEME_OBSIDIAN_DARK;
    return s_theme_palettes[theme_id];
}

const ColorThemePalette& DashboardRenderer::currentTheme() const {
    return getThemePalette(Settings.get().color_theme);
}

// ==============================================================================
// Constructor & Smooth Font Initialization (Zero Per-Frame Heap Allocation)
// ==============================================================================
DashboardRenderer::DashboardRenderer()
    : _display(nullptr),
      _canvas(nullptr),
      _active_screen(SCREEN_RIDE_DASH),
      _active_style(STYLE_HYPER_35KW),
      _active_theme(255),
      _screen_dirty(true),
      _mode_banner_active(false),
      _mode_banner_drawn_mode(-1),
      _settings_tab_idx(255),
      _last_rendered_tab(255),
      _settings_inited(false),
      _menu_dirty(true),
      _settings_last_tuning_mode(-1),
      _is_sweeping(false),
      _sweep_phase(0),
      _sweep_progress(0.0f),
      _sweep_duty(0.0f),
      _sweep_speed(0.0f),
      _last_sweep_tick_ms(0),
      _bridge_last_pc_bytes(0xFFFFFFFF),
      _bridge_last_vesc_bytes(0xFFFFFFFF),
      _bridge_last_wifi_rx(0xFFFFFFFF),
      _bridge_last_wifi_tx(0xFFFFFFFF),
      _bridge_last_wifi_conn(-1),
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
      _last_ble_conn(false),
      _last_ble_bars(255),
      _last_pin_active_key(-1),
      _last_pin_bat_pct(-999),
      _last_pin_volt_x10(-999),
      _last_pin_vesc_conn(-1),
      _pin_modal_open(false),
      _pin_modal_success(false),
      _pin_modal_success_ms(0),
      _wizard_step(0),
      _wizard_inited(false),
      _last_wizard_step(-1),
      _last_wizard_pin_len(-1),
      _wifi_bridge_inited(false),
      _race_records_drawn(false),
      _race_dyno_drawn(false),
      _last_race_arc_angle(130.0f),
      _last_race_speed(-1),
      _last_race_state(255),
      _last_race_countdown(-1),
      _last_race_elapsed_sec(-1.0f),
      _last_race_split_0_22(-999.0f),
      _last_race_split_0_50(-999.0f),
      _last_race_split_0_70(-999.0f),
      _last_race_g_force_x10(-999),
      _boot_anim_active(false),
      _boot_anim_phase(0),
      _boot_anim_start_ms(0),
      _boot_log_lines_drawn(0),
      _vesc_link_skipped(false),
      _vesc_skip_click_ms(0),
      _vesc_connected_at_ms(0) {
    for (int i = 0; i < UIF_COUNT; i++) {
        _vlwLoaded[i] = false;
    }
    _wizard_pin_buf[0] = '\0';
    _pin_modal_buf[0] = '\0';
    _last_needle.valid = false;
    _cache.invalidate();
    _stats_cache.invalidate();
}

void DashboardRenderer::initSmoothFonts() {
    struct FontSpec {
        const uint8_t *data;
        size_t len;
    };
    static const FontSpec specs[UIF_COUNT] = {
        { UI_FONT_HERO_NUM, UI_FONT_HERO_NUM_LEN },
        { UI_FONT_MED_NUM,  UI_FONT_MED_NUM_LEN  },
        { UI_FONT_HEADING,  UI_FONT_HEADING_LEN  },
        { UI_FONT_BODY,     UI_FONT_BODY_LEN     },
        { UI_FONT_SMALL,    UI_FONT_SMALL_LEN    }
    };
    for (int i = 0; i < UIF_COUNT; i++) {
        if (!_vlwLoaded[i]) {
            _vlwData[i].set(specs[i].data, (uint32_t)specs[i].len);
            _vlwLoaded[i] = _vlwFonts[i].loadFont(&_vlwData[i]);
        }
    }
}

const lgfx::IFont* DashboardRenderer::getUiFont(UiFont f) const {
    if ((uint8_t)f < UIF_COUNT && _vlwLoaded[f]) {
        return &_vlwFonts[f];
    }
    switch (f) {
        case UIF_HERO_NUM: return &fonts::Font4;
        case UIF_MED_NUM:  return &fonts::Font4;
        case UIF_HEADING:  return &fonts::Font4;
        case UIF_BODY:     return &fonts::Font2;
        default:           return &fonts::Font0;
    }
}

void DashboardRenderer::useFont(UiFont f) {
    _canvas->setFont(getUiFont(f));
    _canvas->setTextSize(1);
}

void DashboardRenderer::resetFont() {
    _canvas->setFont(&fonts::Font2);
    _canvas->setTextSize(1);
    _canvas->setTextPadding(0);
}

void DashboardRenderer::drawSmoothText(UiFont f, const char *str, int x, int y, textdatum_t datum, uint16_t fg, uint16_t bg, int padW) {
    useFont(f);
    _canvas->setTextDatum(datum);
    _canvas->setTextColor(fg, bg);
    _canvas->setTextPadding(padW);
    _canvas->drawString(str, x, y);
    if (padW > 0) _canvas->setTextPadding(0);
}

void DashboardRenderer::begin(DisplayDriver *display) {
    _display = display;
    _canvas = display->getCanvas();
    initSmoothFonts();
    _cache.invalidate();
    _screen_dirty = true;
    _active_style = (DashboardStyle)Settings.get().dash_style;
    _active_theme = Settings.get().color_theme;
}

// ==============================================================================
// Common Vector Primitives (Cards, Pill Toggles, Icons)
// ==============================================================================
void DashboardRenderer::drawCard(int x, int y, int w, int h, uint16_t bg, uint16_t border, int r) {
    _canvas->fillRoundRect(x, y, w, h, r, bg);
    _canvas->drawRoundRect(x, y, w, h, r, border);
}

void DashboardRenderer::drawPillToggle(int x, int y, bool on, const ColorThemePalette &pal) {
    const int w = 58;
    const int h = 30;
    const int r = 15;
    uint16_t trackCol = on ? pal.primary : pal.surface_alt;
    uint16_t borderCol = on ? pal.primary : pal.border;
    uint16_t knobCol = on ? onPrimaryFg(pal) : pal.muted;

    _canvas->fillRoundRect(x, y, w, h, r, trackCol);
    _canvas->drawRoundRect(x, y, w, h, r, borderCol);

    int knobX = on ? (x + w - 15) : (x + 15);
    int knobY = y + h / 2;
    _canvas->fillCircle(knobX, knobY, 11, knobCol);
}

void DashboardRenderer::drawLockIcon(int cx, int cy, uint16_t col, uint16_t bg) {
    // Clean geometric padlock: curved shackle + rounded body + keyhole
    _canvas->drawArc(cx, cy - 5, 5, 7, 180.0f, 360.0f, col);
    _canvas->drawFastVLine(cx - 6, cy - 5, 5, col);
    _canvas->drawFastVLine(cx - 5, cy - 5, 5, col);
    _canvas->drawFastVLine(cx + 5, cy - 5, 5, col);
    _canvas->drawFastVLine(cx + 6, cy - 5, 5, col);
    _canvas->fillRoundRect(cx - 10, cy - 1, 21, 15, 3, col);
    _canvas->fillCircle(cx, cy + 5, 2, bg);
    _canvas->drawFastVLine(cx, cy + 6, 4, bg);
}

// Crisp geometric vector satellite icon (theme-aware on both dark & light themes!)
static void drawMiniSatellite(LovyanGFX *canvas, int cx, int cy, const ColorThemePalette &pal) {
    // Solar panels (diagonal top-left & bottom-right wings)
    canvas->drawLine(cx - 11, cy - 7, cx - 5, cy - 1, pal.muted);
    canvas->drawLine(cx - 10, cy - 8, cx - 4, cy - 2, pal.muted);
    canvas->drawLine(cx - 9,  cy - 9, cx - 3, cy - 3, pal.muted);

    canvas->drawLine(cx + 3,  cy + 3, cx + 9,  cy + 9, pal.muted);
    canvas->drawLine(cx + 4,  cy + 2, cx + 10, cy + 8, pal.muted);
    canvas->drawLine(cx + 5,  cy + 1, cx + 11, cy + 7, pal.muted);

    // Central satellite body (tilted diamond)
    canvas->fillCircle(cx, cy, 4, pal.text);
    canvas->drawLine(cx - 5, cy + 5, cx + 5, cy - 5, pal.text);

    // Radiating GNSS signal arcs bottom-left in theme primary accent
    canvas->drawArc(cx - 2, cy + 2, 7, 8, 95.0f, 175.0f, pal.primary);
    canvas->drawArc(cx - 2, cy + 2, 11, 12, 100.0f, 170.0f, pal.primary);
}

// ==============================================================================
// Boot Animation Sequence (Compressed PNG Logo + Live Diagnostic Console)
// ==============================================================================
void DashboardRenderer::triggerBootAnimation() {
    _boot_anim_active = true;
    _boot_anim_phase = 0;
    _boot_anim_start_ms = millis();
    _boot_log_lines_drawn = 0;
    _vesc_link_skipped = false;
    _vesc_skip_click_ms = 0;
    _vesc_connected_at_ms = 0;
    _screen_dirty = true;
}

void DashboardRenderer::skipBootAnimation() {
    if (_boot_anim_active) {
        _boot_anim_active = false;
        _boot_anim_phase = 3;
        _wizard_inited = false;
        _last_wizard_step = -1;
        invalidateCache();
        _screen_dirty = true;
        if (_canvas) {
            _canvas->fillScreen(currentTheme().bg);
        }
        if (!Settings.get().pin_lock_enabled) {
            triggerNeedleSweep();
        }
    }
}

void DashboardRenderer::handleBootTouch(bool is_vesc_connected) {
    if (!_boot_anim_active) return;

    uint32_t elapsed_ms = millis() - _boot_anim_start_ms;
    if (!is_vesc_connected && !_vesc_link_skipped && elapsed_ms >= 2250) {
        _boot_anim_phase = 2;
        _vesc_link_skipped = true;
        _vesc_skip_click_ms = millis();
        _screen_dirty = true;
    } else {
        skipBootAnimation();
    }
}

void DashboardRenderer::setBootAnimElapsed(uint32_t elapsed_ms) {
    _boot_anim_active = true;
    _boot_anim_start_ms = millis() - elapsed_ms;
}

void DashboardRenderer::renderBootAnimation(uint32_t elapsed_ms, const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = getThemePalette(THEME_OBSIDIAN_DARK); // Boot always pure black OLED

    if (elapsed_ms < 2250) {
        // Phase 1 (0.0s..2.25s): Lossless PNG Logo centered on pure black OLED
        if (_boot_anim_phase != 1) {
            _boot_anim_phase = 1;
            _boot_log_lines_drawn = 0;
            _canvas->fillScreen(COLOR_BLACK);
            int lx = (800 - BOOT_LOGO_WIDTH) / 2;
            int ly = (480 - BOOT_LOGO_HEIGHT) / 2;
            _canvas->drawPng(BOOT_LOGO_PNG, BOOT_LOGO_PNG_LEN, lx, ly);
            _canvas->releasePngMemory();
        }
        return;
    }

    // Phase 2 (>= 2.25s): Live Diagnostic Console
    uint32_t p2_ms = elapsed_ms - 2250;
    if (_boot_anim_phase != 2) {
        _boot_anim_phase = 2;
        _boot_log_lines_drawn = 0;
        _canvas->fillScreen(COLOR_BLACK);
    }

    const int start_x = 40;
    const int start_y = 54;
    const int line_spacing = 40;
    useFont(UIF_BODY);

    auto drawConsoleLine = [&](int line_idx, const char *bracket_text, uint16_t bracket_col, const char *msg_text, uint16_t msg_col) {
        int y = start_y + (line_idx * line_spacing);
        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextPadding(0);

        _canvas->fillRect(start_x, y - 2, 720, 24, COLOR_BLACK);
        _canvas->setTextColor(pal.muted, COLOR_BLACK);
        _canvas->drawString("[", start_x, y);

        _canvas->setTextDatum(textdatum_t::top_center);
        _canvas->setTextColor(bracket_col, COLOR_BLACK);
        _canvas->drawString(bracket_text, start_x + 36, y);

        _canvas->setTextDatum(textdatum_t::top_left);
        _canvas->setTextColor(pal.muted, COLOR_BLACK);
        _canvas->drawString("]", start_x + 66, y);

        _canvas->setTextColor(msg_col, COLOR_BLACK);
        _canvas->drawString(msg_text, start_x + 86, y);
    };

    if (p2_ms >= 50) {
        drawConsoleLine(0, "OK", pal.accent_ok, "SYSTEM INITIALIZED (PSRAM 8MB)", COLOR_WHITE);
    }
    if (p2_ms >= 180) {
        drawConsoleLine(1, "OK", pal.accent_ok, "CAN-BUS 500k ACTIVE", COLOR_WHITE);
    }

    bool vesc_ready = telemetry.vesc_connected;
    if (p2_ms >= 350) {
        if (vesc_ready) {
            char vesc_msg[64];
            if (telemetry.voltage > 10.0f) {
                snprintf(vesc_msg, sizeof(vesc_msg), "VESC LINK: CONNECTED (%.1f V)", telemetry.voltage);
            } else {
                snprintf(vesc_msg, sizeof(vesc_msg), "VESC LINK: CONNECTED");
            }
            drawConsoleLine(2, "OK", pal.accent_ok, vesc_msg, COLOR_WHITE);
        } else if (_vesc_link_skipped) {
            drawConsoleLine(2, "SKIP", pal.accent_warn, "VESC LINK: OFFLINE (SKIPPED)", pal.muted);
        } else {
            uint8_t dots_idx = (p2_ms / 250) % 4;
            const char *dots_str[4] = { ".", "..", "...", "...." };
            drawConsoleLine(2, dots_str[dots_idx], pal.accent_warn, "VESC LINK: SEARCHING...", pal.muted);

            drawSmoothText(UIF_BODY, "TIPPE ZUM WEITERGEHEN (OFFLINE)", 400, 396,
                           textdatum_t::middle_center, pal.muted, COLOR_BLACK, 600);
            return;
        }
    }

    if ((p2_ms >= 450 && vesc_ready) || _vesc_link_skipped) {
        char tire_val[16];
        float diam = Settings.get().wheel_diameter_in;
        if (Settings.get().wheel_gps_calibrated || fabsf(diam - 11.0f) > 0.05f) {
            if (fabsf(diam - roundf(diam)) < 0.05f) {
                snprintf(tire_val, sizeof(tire_val), "%d in", (int)roundf(diam));
            } else {
                snprintf(tire_val, sizeof(tire_val), "%.1f in", diam);
            }
        } else {
            snprintf(tire_val, sizeof(tire_val), "11 in");
        }
        char tire_msg[64];
        snprintf(tire_msg, sizeof(tire_msg), "TIRE CALIBRATION: %s", tire_val);
        drawConsoleLine(3, "OK", pal.accent_ok, tire_msg, COLOR_WHITE);
    }

    if ((p2_ms >= 550 && vesc_ready) || _vesc_link_skipped) {
        drawConsoleLine(4, "OK", pal.accent_ok, "SECURITY SYSTEM: ARMED (0A CUTOFF)", COLOR_WHITE);

        const char *prompt = Settings.get().pin_lock_enabled ?
                             "TIPPE AUFS DISPLAY ZUM ENTSPERREN" :
                             "TIPPE AUFS DISPLAY ZUM STARTEN";
        uint16_t prompt_col = ((p2_ms / 600) % 2 == 0) ? pal.primary : COLOR_WHITE;
        drawSmoothText(UIF_BODY, prompt, 400, 396,
                       textdatum_t::middle_center, prompt_col, COLOR_BLACK, 600);
    }
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

// ==============================================================================
// Top-Level Screen Dispatcher
// ==============================================================================
void DashboardRenderer::render(const DashTelemetry &telemetry) {
    // 0. Boot Animation Sequence
    if (_boot_anim_active) {
        uint32_t now = millis();
        uint32_t elapsed_ms = now - _boot_anim_start_ms;

        bool should_auto_advance = false;
        if (telemetry.vesc_connected) {
            if (elapsed_ms >= 3250) {
                should_auto_advance = true;
            }
        } else if (_vesc_link_skipped && _vesc_skip_click_ms > 0) {
            if (now - _vesc_skip_click_ms >= 700) {
                should_auto_advance = true;
            }
        }

        if (should_auto_advance) {
            skipBootAnimation();
        } else {
            renderBootAnimation(elapsed_ms, telemetry);
            return;
        }
    }

    // 0a. Live Theme Switch Detection (runs BEFORE Lockscreen/OTA/Screens so every view recolors immediately!)
    uint8_t current_theme = Settings.get().color_theme;
    if (current_theme >= THEME_COUNT) current_theme = THEME_OBSIDIAN_DARK;
    if (current_theme != _active_theme) {
        _active_theme = current_theme;
        _screen_dirty = true;
        invalidateCache();
        _hyper_inited = false;
        _stats_inited = false;
        _pin_screen_drawn = false;
        _settings_inited = false;
        _wifi_bridge_inited = false;
        _wizard_inited = false;
        _last_rendered_tab = 255;
        _menu_dirty = true;
    }

    // 0b. First-Time Setup Wizard / OTA / PIN Lockscreen priority
    if (!telemetry.setup_completed && telemetry.screen == SCREEN_SETUP_WIZARD) {
        // Fall through to screen dispatch
    } else if (OtaManager::instance().isUpdating()) {
        renderOtaScreen(OtaManager::instance().getProgressPct(), OtaManager::instance().getStatusText());
        return;
    } else if (telemetry.is_locked || ModeManager::instance().isLocked()) {
        renderPinLockScreen(telemetry);
        return;
    } else {
        if (_pin_screen_drawn) {
            _pin_screen_drawn = false;
            invalidateCache();
            markScreenDirty();
        }
    }

    // 1. Startup Speedometer Sweep Animation (0 -> 100 km/h -> 0)
    // Safety: real riding speed (>= 2 km/h) always overrides cosmetic sweep immediately
    if (_is_sweeping && telemetry.speed_kmh >= 2.0f) {
        _is_sweeping = false;
        _sweep_speed = 0.0f;
        _sweep_duty = 0.0f;
        _cache.speed = -999;
        _anim_inited = false;
    }
    if (_is_sweeping) {
        uint32_t now = millis();
        float dt = (now - _last_sweep_tick_ms) / 1000.0f;
        _last_sweep_tick_ms = now;

        if (_sweep_phase == 0) {
            _sweep_progress += dt / 0.65f;
            if (_sweep_progress >= 1.0f) {
                _sweep_progress = 1.0f;
                _sweep_phase = 1;
            }
            float ease = _sweep_progress * _sweep_progress * (3.0f - 2.0f * _sweep_progress);
            _sweep_duty = ease * 100.0f;
            _sweep_speed = ease * 100.0f;
        } else if (_sweep_phase == 1) {
            _sweep_progress -= dt / 0.55f;
            if (_sweep_progress <= 0.0f) {
                _sweep_progress = 0.0f;
                _sweep_duty = 0.0f;
                _sweep_speed = 0.0f;
                _is_sweeping = false;
                _cache.speed = -999;
                _anim_speed = 0.0f;
            } else {
                float ease = _sweep_progress * _sweep_progress * (3.0f - 2.0f * _sweep_progress);
                _sweep_duty = ease * 100.0f;
                _sweep_speed = ease * 100.0f;
            }
        }
    }

    // 2. Detect Screen Switch
    if (telemetry.screen != _active_screen) {
        _active_screen = telemetry.screen;
        _screen_dirty = true;
        invalidateCache();
        _last_needle.valid = false;
        _wifi_bridge_inited = false;
        _wizard_inited = false;
        _last_wizard_step = -1;
        _last_wizard_pin_len = -1;
    }

    // 3. Detect Style Switch in Ride Dashboard
    if (_active_screen == SCREEN_RIDE_DASH) {
        DashboardStyle st = (DashboardStyle)Settings.get().dash_style;
        if (st != _active_style) {
            _active_style = st;
            _screen_dirty = true;
            invalidateCache();
            _last_needle.valid = false;
        }
    }

    const ColorThemePalette &pal = currentTheme();

    // 4. Dispatch Screen Rendering
    switch (_active_screen) {
        case SCREEN_RIDE_DASH:
            if (!_hyper_inited) {
                initRideDashboard(telemetry);
                _screen_dirty = false;
            } else if (_screen_dirty) {
                _screen_dirty = false;
                _anim_inited = false;
                _cache.invalidate();
            }
            renderRideDashboard(telemetry);
            break;

        case SCREEN_LIGHTS:
#if ENABLE_LIGHTS_SCREEN
            if (_screen_dirty) {
                initLightsScreen(telemetry);
                _screen_dirty = false;
            }
            renderLightsScreen(telemetry);
#else
            if (!_hyper_inited) {
                initRideDashboard(telemetry);
                _screen_dirty = false;
            } else if (_screen_dirty) {
                _screen_dirty = false;
                _anim_inited = false;
                _cache.invalidate();
            }
            renderRideDashboard(telemetry);
#endif
            break;

        case SCREEN_RT_GRAPHS:
            if (!_stats_inited) {
                _canvas->fillScreen(pal.bg);
                initRtGraphsScreen(telemetry);
                _screen_dirty = false;
            } else if (_screen_dirty) {
                _screen_dirty = false;
                _stats_cache.invalidate();
            }
            renderRtGraphsScreen(telemetry);
            break;

        case SCREEN_SETTINGS_MENU:
            if (_screen_dirty || !_settings_inited) {
                initSettingsScreen(telemetry);
                _screen_dirty = false;
            }
            renderSettingsScreen(telemetry);
            break;

        case SCREEN_VESC_BRIDGE:
            if (_screen_dirty) {
                _canvas->fillScreen(pal.bg);
                initVescBridgeScreen(telemetry);
                _screen_dirty = false;
            }
            renderVescBridgeScreen(telemetry);
            break;

        case SCREEN_WIFI_BRIDGE:
            if (!_wifi_bridge_inited || _screen_dirty) {
                _canvas->fillScreen(pal.bg);
                initWifiBridgeScreen(telemetry);
                _wifi_bridge_inited = true;
                _screen_dirty = false;
            }
            renderWifiBridgeScreen(telemetry);
            break;

        case SCREEN_RACE_TIMER:
            if (_screen_dirty) {
                _canvas->fillScreen(pal.bg);
                initRaceTimerScreen(telemetry);
                _screen_dirty = false;
            }
            renderRaceTimerScreen(telemetry);
            break;

        case SCREEN_SETUP_WIZARD:
            renderSetupWizard(telemetry);
            _screen_dirty = false;
            break;

        default:
            break;
    }

    // Subtle Stealth Indicator Dot in Top-Right Corner (790, 10)
    // Legal mode (!tuning_mode): lit subtle red dot; Tuning mode: completely hidden
    if (telemetry.screen != SCREEN_SETUP_WIZARD) {
        int8_t want_dot = telemetry.tuning_mode ? 0 : 1;
        if (_cache.tuning_mode_dot != want_dot) {
            _cache.tuning_mode_dot = want_dot;
            if (want_dot == 1) {
                _canvas->fillCircle(790, 10, 4, pal.accent_danger);
            } else {
                _canvas->fillCircle(790, 10, 5, pal.bg);
            }
        }
    }
}

// ==============================================================================
// SCREEN 0: MAIN RIDE DASHBOARD — HYPER-VESC 35kW+ PRO CLUSTER (800x480)
// ==============================================================================
void DashboardRenderer::initRideDashboard(const DashTelemetry &telemetry) {
    initHyper35kwStyle(telemetry);
}

void DashboardRenderer::renderRideDashboard(const DashTelemetry &telemetry) {
    if (!_hyper_inited) {
        initHyper35kwStyle(telemetry);
    }
    renderHyper35kwStyle(telemetry);
    if (telemetry.fault_code != 0) {
        drawFaultBanner(telemetry.fault_code);
    }
}

// Design Upgrade 4: Crisp Vector Badges for Bottom-Left Circular Instruments
static void drawGForceBadge(LovyanGFX *canvas, const lgfx::IFont *smallFont, float g_force, int cx, int cy, int r, const ColorThemePalette &pal) {
    uint16_t ringCol = pal.border;
    uint16_t dotCol  = pal.primary;
    uint16_t valCol  = pal.text;
    if (g_force > 0.15f) {
        ringCol = pal.accent_ok;
        dotCol  = pal.accent_ok;
        valCol  = pal.accent_ok;
    } else if (g_force < -0.15f) {
        ringCol = pal.accent_danger;
        dotCol  = pal.accent_danger;
        valCol  = pal.accent_danger;
    }

    canvas->fillRect(cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3, pal.bg);
    canvas->fillCircle(cx, cy, r - 1, pal.surface);
    canvas->drawCircle(cx, cy, r, ringCol);
    canvas->drawCircle(cx, cy, r - 1, ringCol);

    // Mini circular crosshair target in upper half
    const int tcy = cy - 7;
    const int tr = 9;
    canvas->drawCircle(cx, tcy, tr, pal.border);
    canvas->drawFastHLine(cx - tr, tcy, 2 * tr + 1, pal.track);
    canvas->drawFastVLine(cx, tcy - tr, 2 * tr + 1, pal.track);

    // Live moving vector dot displaced vertically by longitudinal G-force
    float clamped_g = g_force;
    if (clamped_g > 1.5f) clamped_g = 1.5f;
    if (clamped_g < -1.5f) clamped_g = -1.5f;
    int dy = (int)roundf((clamped_g / 1.5f) * 7.0f);
    canvas->fillCircle(cx, tcy - dy, 3, dotCol);

    // Crisp readout below target
    char gBuf[12];
    snprintf(gBuf, sizeof(gBuf), "%+.1fG", g_force);
    canvas->setFont(smallFont);
    canvas->setTextSize(1);
    canvas->setTextDatum(textdatum_t::middle_center);
    canvas->setTextColor(valCol, pal.surface);
    canvas->drawString(gBuf, cx, cy + 12);
}

static void drawInclineBadge(LovyanGFX *canvas, const lgfx::IFont *smallFont, float incline_deg, int cx, int cy, int r, const ColorThemePalette &pal) {
    uint16_t ringCol = pal.border;
    uint16_t slopeCol = pal.primary;
    if (incline_deg > 12.0f) {
        ringCol = pal.primary;
        slopeCol = pal.primary;
    } else if (incline_deg < -12.0f) {
        ringCol = pal.accent_warn;
        slopeCol = pal.accent_warn;
    }

    canvas->fillRect(cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3, pal.bg);
    canvas->fillCircle(cx, cy, r - 1, pal.surface);
    canvas->drawCircle(cx, cy, r, ringCol);
    canvas->drawCircle(cx, cy, r - 1, ringCol);

    // Dynamic tilting horizon/slope vector in upper half
    const int hcy = cy - 7;
    // Reference level horizon ticks
    canvas->drawFastHLine(cx - 12, hcy, 25, pal.track);

    float clamped_deg = incline_deg;
    if (clamped_deg > 35.0f) clamped_deg = 35.0f;
    if (clamped_deg < -35.0f) clamped_deg = -35.0f;
    float rad = -clamped_deg * (3.14159265f / 180.0f);
    int dx = (int)roundf(cosf(rad) * 11.0f);
    int dy = (int)roundf(sinf(rad) * 11.0f);

    canvas->drawLine(cx - dx, hcy - dy, cx + dx, hcy + dy, slopeCol);
    canvas->drawLine(cx - dx, hcy - dy - 1, cx + dx, hcy + dy - 1, slopeCol);
    canvas->fillCircle(cx, hcy, 2, pal.text);

    // Crisp degree readout with native UTF-8 degree sign
    int inc = (int)roundf(incline_deg);
    char valBuf[16];
    if (inc > 0) snprintf(valBuf, sizeof(valBuf), "+%d°", inc);
    else snprintf(valBuf, sizeof(valBuf), "%d°", inc);

    canvas->setFont(smallFont);
    canvas->setTextSize(1);
    canvas->setTextDatum(textdatum_t::middle_center);
    canvas->setTextColor(pal.text, pal.surface);
    canvas->drawString(valBuf, cx, cy + 12);
}

static void drawLightBadge(LovyanGFX *canvas, const lgfx::IFont *smallFont, bool lights_on, int cx, int cy, int r, const ColorThemePalette &pal) {
    uint16_t fillBg  = lights_on ? blendRgb565(pal.surface, pal.primary, pal.is_light ? 35 : 48) : pal.surface;
    uint16_t ringCol = lights_on ? pal.primary : pal.border;
    uint16_t iconCol = lights_on ? pal.primary : pal.muted;

    canvas->fillRect(cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3, pal.bg);
    canvas->fillCircle(cx, cy, r - 1, fillBg);
    canvas->drawCircle(cx, cy, r, ringCol);
    canvas->drawCircle(cx, cy, r - 1, ringCol);

    // Geometric headlight dome centered at (cx - 4, cy - 6)
    const int hx = cx - 3;
    const int hy = cy - 6;

    if (lights_on) {
        // Filled headlight dome (left semicircle)
        canvas->fillCircle(hx, hy, 7, iconCol);
        canvas->fillRect(hx + 1, hy - 7, 8, 15, fillBg);
        canvas->drawFastVLine(hx + 1, hy - 6, 13, iconCol);

        // 3 crisp angled beam rays extending rightward
        canvas->drawLine(hx + 4, hy - 5, hx + 13, hy - 8, iconCol);
        canvas->drawLine(hx + 4, hy - 4, hx + 13, hy - 7, iconCol);

        canvas->drawLine(hx + 5, hy,     hx + 14, hy,     iconCol);
        canvas->drawLine(hx + 5, hy + 1, hx + 14, hy + 1, iconCol);

        canvas->drawLine(hx + 4, hy + 5, hx + 13, hy + 8, iconCol);
        canvas->drawLine(hx + 4, hy + 6, hx + 13, hy + 9, iconCol);
    } else {
        // Clean muted headlight dome outline
        canvas->drawCircle(hx, hy, 7, iconCol);
        canvas->fillRect(hx + 1, hy - 7, 8, 15, fillBg);
        canvas->drawFastVLine(hx + 1, hy - 6, 13, iconCol);
        // Subtle short OFF rays
        canvas->drawLine(hx + 5, hy - 3, hx + 9, hy - 4, pal.border);
        canvas->drawLine(hx + 5, hy,     hx + 10, hy,    pal.border);
        canvas->drawLine(hx + 5, hy + 3, hx + 9, hy + 4, pal.border);
    }

    canvas->setFont(smallFont);
    canvas->setTextSize(1);
    canvas->setTextDatum(textdatum_t::middle_center);
    canvas->setTextColor(lights_on ? pal.primary : pal.muted, fillBg);
    canvas->drawString(lights_on ? "AN" : "AUS", cx, cy + 12);
}

static void drawThermalsRow(LovyanGFX *canvas, const lgfx::IFont *bodyFont, float fet, float mot, int cx, int y, const ColorThemePalette &pal) {
    char fetVal[12], motVal[12];
    snprintf(fetVal, sizeof(fetVal), "%.0f°", fet);
    snprintf(motVal, sizeof(motVal), "%.0f°", mot);

    canvas->setFont(bodyFont);
    canvas->setTextSize(1);
    canvas->setTextDatum(textdatum_t::top_left);

    int wFetLbl = canvas->textWidth("FET ");
    int wFetVal = canvas->textWidth(fetVal);
    int wGap    = 14;
    int wMotLbl = canvas->textWidth("MOT ");
    int wMotVal = canvas->textWidth(motVal);
    int totalW  = wFetLbl + wFetVal + wGap + wMotLbl + wMotVal;
    int curX    = cx - totalW / 2;

    canvas->setTextColor(pal.muted, pal.bg);
    canvas->drawString("FET ", curX, y);
    curX += wFetLbl;

    canvas->setTextColor(getTempColorTheme(fet, pal), pal.bg);
    canvas->drawString(fetVal, curX, y);
    curX += wFetVal + wGap;

    canvas->setTextColor(pal.muted, pal.bg);
    canvas->drawString("MOT ", curX, y);
    curX += wMotLbl;

    canvas->setTextColor(getTempColorTheme(mot, pal), pal.bg);
    canvas->drawString(motVal, curX, y);
}

void DashboardRenderer::initHyper35kwStyle(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);
    _mode_banner_active = (millis() < telemetry.mode_badge_until);
    _mode_banner_drawn_mode = -1;
    _hyper_inited = false;
    _anim_inited = false;
    _cache.invalidate();
    _last_arc_frac = -1.0f;
    _last_arc_angle = 135.0f;
    _last_arc_col = 0;

    int cx = 240, cy = 215;
    int r_in = 135, r_out = 162;
    float mid_r = 148.5f;
    float deg2rad = 3.14159265f / 180.0f;

    // 0. Top-Left [ STATS ] Pill Button
    if (telemetry.fault_code == 0) {
        int sX = 20, sY = 16, sW = 96, sH = 36;
        _canvas->fillRoundRect(sX, sY, sW, sH, 18, pal.surface);
        _canvas->drawRoundRect(sX, sY, sW, sH, 18, pal.border);
        drawSmoothText(UIF_BODY, "STATS", sX + sW / 2, sY + sH / 2, textdatum_t::middle_center, pal.text, pal.surface);
    }

    // 1. Speedometer Gauge Background Arc (270 degree sweep: 135° to 405°)
    _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, 405.0f, pal.track);
    int s_cap_x = cx + (int)roundf(cosf(135.0f * deg2rad) * mid_r);
    int s_cap_y = cy + (int)roundf(sinf(135.0f * deg2rad) * mid_r);
    int e_cap_x = cx + (int)roundf(cosf(405.0f * deg2rad) * mid_r);
    int e_cap_y = cy + (int)roundf(sinf(405.0f * deg2rad) * mid_r);
    _canvas->fillCircle(s_cap_x, s_cap_y, 14, pal.track);
    _canvas->fillCircle(e_cap_x, e_cap_y, 14, pal.track);

    // Static label "km/h" under hero speed
    drawSmoothText(UIF_HEADING, "km/h", cx, 242, textdatum_t::top_center, pal.muted, pal.bg);

    // Static label "W" next to hero power
    if (telemetry.fault_code == 0) {
        drawSmoothText(UIF_HEADING, "W", 730, 82, textdatum_t::top_left, pal.muted, pal.bg);
    }

    // Static label "DUTY"
    drawSmoothText(UIF_BODY, "DUTY", 445, 124, textdatum_t::top_left, pal.muted, pal.bg);

    // Static duty track bar
    _canvas->fillRoundRect(445, 150, 320, 10, 5, pal.track);

    // Static battery track bar
    _canvas->fillRoundRect(445, 218, 320, 14, 7, pal.track);

    // Static labels "RESTREICHWEITE" & "TRIP"
    drawSmoothText(UIF_SMALL, "RESTREICHWEITE", 445, 294, textdatum_t::top_left, pal.muted, pal.bg);
    drawSmoothText(UIF_SMALL, "TRIP", 620, 294, textdatum_t::top_left, pal.muted, pal.bg);
}

void DashboardRenderer::renderHyper35kwStyle(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();

    bool banner_now = (millis() < telemetry.mode_badge_until);
    if (banner_now != _mode_banner_active) {
        _mode_banner_active = banner_now;
        _mode_banner_drawn_mode = -1;
        _canvas->fillRect(434, 170, 340, 114, pal.bg);
        _cache.battery_pct = -999;
        _cache.voltage_x10 = -999;
        _cache.phase_x10 = -9999;
        _cache.current_x10 = -9999;
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
    if (spd < 0.8f) spd = 0.0f;
    float spd_anim = (_is_sweeping) ? _sweep_speed : ((_anim_speed > 0.0f) ? _anim_speed : spd);
    if (spd_anim < 0.8f) spd_anim = 0.0f;
    float frac = spd_anim / 100.0f;
    if (frac >= 0.992f) frac = 1.0f;

    uint16_t arcCol = pal.primary;
    if (_is_sweeping) {
        arcCol = pal.primary;
    } else if (spd >= 85.0f) {
        arcCol = pal.accent_danger;
    } else if (spd >= 60.0f) {
        arcCol = pal.accent_warn;
    }

    float target_angle = 135.0f + (frac * 270.0f);
    if (frac <= 0.005f) target_angle = 135.0f;

    // 1. Speedometer Active Arc (Zero-flicker differential update)
    bool reached_full = (target_angle >= 404.5f && _last_arc_angle < 404.5f);
    bool left_full    = (target_angle < 404.5f && _last_arc_angle >= 404.5f);

    if (!_hyper_inited) {
        if (target_angle > 135.0f) {
            _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, target_angle, arcCol);
            _canvas->fillCircle(s_cap_x, s_cap_y, 14, arcCol);
            if (target_angle >= 404.5f) {
                _canvas->fillCircle(e_cap_x, e_cap_y, 14, arcCol);
            }
        } else {
            _canvas->fillCircle(s_cap_x, s_cap_y, 14, pal.track);
        }
        _last_arc_angle = target_angle;
        _last_arc_col = arcCol;
    } else if (target_angle <= 135.0f && _last_arc_angle > 135.0f) {
        _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, _last_arc_angle + 1.0f, pal.track);
        _canvas->fillCircle(s_cap_x, s_cap_y, 14, pal.track);
        _canvas->fillCircle(e_cap_x, e_cap_y, 14, pal.track);
        _last_arc_angle = 135.0f;
        _last_arc_col = arcCol;
    } else if (arcCol != _last_arc_col) {
        if (target_angle < _last_arc_angle) {
            _canvas->fillArc(cx, cy, r_in, r_out, target_angle, _last_arc_angle + 1.0f, pal.track);
            if (_last_arc_angle >= 404.5f && target_angle < 404.5f) {
                _canvas->fillCircle(e_cap_x, e_cap_y, 14, pal.track);
            }
        }
        if (target_angle > 135.0f) {
            _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, target_angle, arcCol);
            _canvas->fillCircle(s_cap_x, s_cap_y, 14, arcCol);
            if (target_angle >= 404.5f) {
                _canvas->fillCircle(e_cap_x, e_cap_y, 14, arcCol);
            }
        } else {
            _canvas->fillCircle(s_cap_x, s_cap_y, 14, pal.track);
        }
        _last_arc_angle = target_angle;
        _last_arc_col = arcCol;
    } else if (reached_full || left_full || fabsf(target_angle - _last_arc_angle) >= 0.8f) {
        if (target_angle > _last_arc_angle) {
            if (_last_arc_angle <= 135.0f) {
                _canvas->fillCircle(s_cap_x, s_cap_y, 14, arcCol);
            }
            _canvas->fillArc(cx, cy, r_in, r_out, _last_arc_angle - 0.5f, target_angle, arcCol);
            if (target_angle >= 404.5f) {
                _canvas->fillCircle(e_cap_x, e_cap_y, 14, arcCol);
            }
        } else {
            if (_last_arc_angle >= 404.5f) {
                _canvas->fillCircle(e_cap_x, e_cap_y, 14, pal.track);
            }
            _canvas->fillArc(cx, cy, r_in, r_out, target_angle, _last_arc_angle + 0.5f, pal.track);
            if (target_angle <= 135.0f) {
                _canvas->fillCircle(s_cap_x, s_cap_y, 14, pal.track);
            }
        }
        _last_arc_angle = target_angle;
    }

    // 1b. Top Status Header: Clock in Center (only shown when GPS has valid fix)
    int8_t cur_gps_en = telemetry.gps_enabled ? 1 : 0;
    bool gps_en_changed = (_cache.gps_enabled != cur_gps_en);
    int8_t gps_clock_valid = (telemetry.gps_enabled && telemetry.gps.valid && (telemetry.gps.satellites > 0)) ? 1 : 0;
    bool clock_vis_changed = (_cache.clock_visible != gps_clock_valid);
    bool clock_changed = strcmp(_cache.clock_str, telemetry.clock_str) != 0;

    if (!_hyper_inited || clock_vis_changed || gps_en_changed || (gps_clock_valid && clock_changed)) {
        _cache.clock_visible = gps_clock_valid;
        _cache.gps_enabled = cur_gps_en;
        snprintf(_cache.clock_str, sizeof(_cache.clock_str), "%s", telemetry.clock_str);

        if (telemetry.fault_code == 0) {
            if (gps_clock_valid) {
                drawSmoothText(UIF_HEADING, telemetry.clock_str[0] ? telemetry.clock_str : "--:--",
                               400, 34, textdatum_t::middle_center, pal.text, pal.bg, 120);
            } else {
                _canvas->fillRect(340, 16, 120, 38, pal.bg);
            }
        }
    }

    // 1c. Compact TCS & TPMS Warning Pills (top bar, right of clock, left of Settings Gear)
    int8_t cur_tcs = telemetry.tcs_active ? 1 : 0;
    int8_t cur_tpms = telemetry.tpms_warning ? 1 : 0;
    if (telemetry.fault_code == 0 && (!_hyper_inited || cur_tcs != _cache.tcs_active || cur_tpms != _cache.tpms_warning)) {
        _cache.tcs_active = cur_tcs;
        _cache.tpms_warning = cur_tpms;
        // Clear pill area (x: 540..730, y: 18..48)
        _canvas->fillRect(540, 18, 190, 30, pal.bg);

        if (cur_tcs) {
            _canvas->fillRoundRect(664, 21, 56, 26, 13, pal.accent_warn);
            drawSmoothText(UIF_SMALL, "TCS", 692, 34, textdatum_t::middle_center, onPrimaryFg(pal), pal.accent_warn);
        }
        if (cur_tpms) {
            int tx = cur_tcs ? 576 : 640;
            _canvas->fillRoundRect(tx, 21, 78, 26, 13, pal.accent_danger);
            drawSmoothText(UIF_SMALL, "REIFEN", tx + 39, 34, textdatum_t::middle_center, onPrimaryFg(pal), pal.accent_danger);
        }
    }

    // 2. Rider Tag (if set)
    if (telemetry.rider_tag[0] != '\0' && !_hyper_inited) {
        drawSmoothText(UIF_BODY, telemetry.rider_tag, cx, 114, textdatum_t::top_center, pal.muted, pal.bg);
    }

    // 3. Hero Digital Speed Readout (Roboto Bold 72px smooth tabular digits)
    int cur_spd_int = (int)(spd + 0.5f);
    int8_t gps_connected = (telemetry.gps_enabled && (telemetry.gps.valid && telemetry.gps.satellites >= 4)) ? 1 : 0;
    if (!_hyper_inited || cur_spd_int != _cache.speed) {
        _cache.speed = cur_spd_int;
        char spdBuf[16];
        snprintf(spdBuf, sizeof(spdBuf), "%d", cur_spd_int);
        uint16_t spdCol = (_is_sweeping) ? pal.text : ((spd >= 85.0f) ? pal.accent_danger : pal.text);
        _canvas->fillRect(cx - 88, 162, 176, 68, pal.bg);
        drawSmoothText(UIF_HERO_NUM, spdBuf, cx, 196, textdatum_t::middle_center, spdCol, pal.bg);
    }

    // Vector Satellite icon under "km/h"
    if (!_hyper_inited || gps_connected != _cache.gps_valid || gps_en_changed) {
        _cache.gps_valid = gps_connected;
        _canvas->fillRect(cx - 18, 272, 36, 30, pal.bg);
        if (gps_connected) {
            drawMiniSatellite(_canvas, cx, 286, pal);
        }
    }

    // 5. Right Column: Hero Power Readout (Roboto Bold 72px smooth tabular digits)
    int cur_watts = (int)roundf(_anim_power);
    if (telemetry.speed_kmh < 0.5f && fabsf(_anim_power) < 15.0f) {
        cur_watts = 0;
    }
    bool watt_changed = !_hyper_inited || (cur_watts != _cache.watts && (abs(cur_watts - _cache.watts) >= 2 || cur_watts == 0));
    if (watt_changed) {
        _cache.watts = cur_watts;
        if (telemetry.fault_code == 0) {
            char pStr[16];
            snprintf(pStr, sizeof(pStr), "%d", cur_watts);
            uint16_t pwrCol = (cur_watts < -10) ? pal.accent_ok : pal.text;

            _canvas->fillRect(442, 54, 282, 60, pal.bg);
            drawSmoothText(UIF_HERO_NUM, pStr, 720, 56, textdatum_t::top_right, pwrCol, pal.bg);

            if (!_hyper_inited) {
                drawSmoothText(UIF_HEADING, "W", 730, 82, textdatum_t::top_left, pal.muted, pal.bg);
            }
        }
    }

    // 6. Right Column: Duty Cycle Section
    float duty = telemetry.duty_cycle_pct;
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 100.0f) duty = 100.0f;
    uint16_t dutyCol = pal.primary;
    int cur_duty = (int)(duty + 0.5f);
    if (cur_duty > 100) cur_duty = 100;
    int cur_d_fill = (int)((duty / 100.0f) * 320.0f);

    if (!_hyper_inited || cur_duty != _cache.duty_x10) {
        _cache.duty_x10 = cur_duty;
        char dBuf[16];
        snprintf(dBuf, sizeof(dBuf), "%d%%", cur_duty);
        _canvas->fillRect(675, 118, 92, 26, pal.bg);
        drawSmoothText(UIF_HEADING, dBuf, 765, 118, textdatum_t::top_right, pal.text, pal.bg);
    }

    if (!_hyper_inited || cur_d_fill != _cache.duty_fill_w || dutyCol != _cache.duty_col) {
        int old_fill = _hyper_inited ? _cache.duty_fill_w : 0;
        bool col_changed = (dutyCol != _cache.duty_col);
        _cache.duty_fill_w = cur_d_fill;
        _cache.duty_col = dutyCol;
        if (!_hyper_inited || cur_d_fill < old_fill || col_changed) {
            _canvas->fillRoundRect(445, 150, 320, 10, 5, pal.track);
        }
        if (cur_d_fill > 0) {
            _canvas->fillRoundRect(445, 150, cur_d_fill, 10, 5, dutyCol);
        }
    }

    // 7. Right Column: Dynamic Mode Banner Overlay OR Battery / Currents Readout
    const char *mNames[] = { "ECO", "SPORT", "RACE" };
    int mIdx = (int)telemetry.ride_mode % 3;
    uint16_t mCol = getModeColor(mIdx, pal);

    if (banner_now) {
        if (_mode_banner_drawn_mode != (int8_t)telemetry.ride_mode) {
            _mode_banner_drawn_mode = (int8_t)telemetry.ride_mode;
            int cX = 438, cY = 172, cW = 330, cH = 104;
            _canvas->fillRect(434, 170, 340, 114, pal.bg);
            _canvas->fillRoundRect(cX, cY, cW, cH, 14, pal.surface);
            _canvas->drawRoundRect(cX, cY, cW, cH, 14, mCol);
            _canvas->drawRoundRect(cX + 1, cY + 1, cW - 2, cH - 2, 13, mCol);

            const char *tagStr = telemetry.tuning_mode ? "RIDE MODE AKTIVIERT" : "FAHRMODUS AKTIV";
            drawSmoothText(UIF_SMALL, tagStr, cX + cW / 2, cY + 22, textdatum_t::middle_center, pal.muted, pal.surface);
            drawSmoothText(UIF_MED_NUM, mNames[mIdx], cX + cW / 2, cY + 58, textdatum_t::middle_center, mCol, pal.surface);

            _canvas->fillRoundRect(cX + 24, cY + cH - 12, cW - 48, 4, 2, mCol);
        }
    } else {
        _mode_banner_drawn_mode = -1;

        bool bat_unknown = (telemetry.battery_pct < 0.0f);
        int cur_bat = bat_unknown ? -1 : (int)(telemetry.battery_pct + 0.5f);
        if (cur_bat > 100) cur_bat = 100;
        int cur_v10 = (telemetry.voltage < 1.0f) ? -1 : (int)(telemetry.voltage * 10.0f + 0.5f);

        float p_amps = telemetry.phase_amps;
        if (fabsf(p_amps) < 0.8f) p_amps = 0.0f;
        float b_amps = telemetry.current_amps;
        if (fabsf(b_amps) < 0.5f) b_amps = 0.0f;
        int cur_pha = (int)roundf(p_amps);
        int cur_a   = (int)roundf(b_amps);

        if (!_hyper_inited || cur_bat != _cache.battery_pct) {
            _cache.battery_pct = cur_bat;
            _canvas->fillRect(442, 172, 155, 38, pal.bg);
            char batBuf[16];
            if (bat_unknown) {
                snprintf(batBuf, sizeof(batBuf), "--%%");
            } else {
                snprintf(batBuf, sizeof(batBuf), "%d%%", cur_bat);
            }
            uint16_t batTxtCol = bat_unknown ? pal.muted :
                                 ((telemetry.battery_pct <= 12.0f) ? pal.accent_danger :
                                 ((telemetry.battery_pct <= 25.0f) ? pal.accent_warn : pal.text));
            drawSmoothText(UIF_MED_NUM, batBuf, 445, 176, textdatum_t::top_left, batTxtCol, pal.bg);

            // Battery Bar (empty if unknown)
            _canvas->fillRoundRect(445, 218, 320, 14, 7, pal.track);
            if (!bat_unknown) {
                float pct_c = telemetry.battery_pct;
                if (pct_c < 0.0f) pct_c = 0.0f;
                if (pct_c > 100.0f) pct_c = 100.0f;
                int bFillW = (int)((pct_c / 100.0f) * 320.0f);
                if (bFillW > 0) {
                    uint16_t bCol = (pct_c > 25.0f) ? pal.primary :
                                    ((pct_c > 12.0f) ? pal.accent_warn : pal.accent_danger);
                    _canvas->fillRoundRect(445, 218, bFillW, 14, 7, bCol);
                }
            }
        }

        if (!_hyper_inited || cur_v10 != _cache.voltage_x10) {
            _cache.voltage_x10 = cur_v10;
            char vBuf[16];
            if (cur_v10 < 0) {
                snprintf(vBuf, sizeof(vBuf), "--.- V");
            } else {
                snprintf(vBuf, sizeof(vBuf), "%.1f V", telemetry.voltage);
            }
            _canvas->fillRect(630, 178, 138, 28, pal.bg);
            drawSmoothText(UIF_HEADING, vBuf, 765, 180, textdatum_t::top_right, pal.text, pal.bg);
        }

        if (!_hyper_inited || cur_pha != _cache.phase_x10) {
            _cache.phase_x10 = cur_pha;
            char motBuf[32];
            snprintf(motBuf, sizeof(motBuf), "MOTOR %d A", cur_pha);
            _canvas->fillRect(442, 246, 165, 24, pal.bg);
            drawSmoothText(UIF_BODY, motBuf, 445, 248, textdatum_t::top_left, pal.text, pal.bg);
        }

        if (!_hyper_inited || cur_a != _cache.current_x10) {
            _cache.current_x10 = cur_a;
            char bBuf[32];
            snprintf(bBuf, sizeof(bBuf), "BATTERY %d A", cur_a);
            _canvas->fillRect(605, 246, 165, 24, pal.bg);
            drawSmoothText(UIF_BODY, bBuf, 765, 248, textdatum_t::top_right, pal.text, pal.bg);
        }
    }

    // 8. Range Readout
    int cur_range = (telemetry.est_range_km < 0.0f) ? -1 : (int)(telemetry.est_range_km + 0.5f);
    if (!_hyper_inited || cur_range != _cache.est_range) {
        _cache.est_range = cur_range;
        char rngBuf[16];
        if (cur_range < 0) snprintf(rngBuf, sizeof(rngBuf), "-- km");
        else snprintf(rngBuf, sizeof(rngBuf), "%d km", cur_range);
        _canvas->fillRect(442, 314, 170, 38, pal.bg);
        drawSmoothText(UIF_MED_NUM, rngBuf, 445, 316, textdatum_t::top_left, pal.text, pal.bg);
    }

    // 8b. Trip Distance Readout
    int cur_trip_x10 = (int)roundf(telemetry.trip_km * 10.0f);
    if (!_hyper_inited || cur_trip_x10 != _cache.trip_x10) {
        _cache.trip_x10 = cur_trip_x10;
        char tripBuf[16];
        snprintf(tripBuf, sizeof(tripBuf), "%.1f km", telemetry.trip_km);
        _canvas->fillRect(616, 314, 158, 38, pal.bg);
        drawSmoothText(UIF_MED_NUM, tripBuf, 620, 316, textdatum_t::top_left, pal.text, pal.bg);
    }

    // 9. Right Column: Bottom Touch Action Buttons
    // ALWAYS render BOTH buttons in bottom-right in all modes: [ BOOST ] (x: 445..597) and [ MODE ] (x: 613..765)
    bool btn_layout_changed = !_hyper_inited;
    int8_t cur_boost = telemetry.boost_active ? 1 : 0;

    if (btn_layout_changed || cur_boost != _cache.boost_active) {
        _cache.boost_active = cur_boost;
        if (telemetry.boost_active) {
            _canvas->fillRoundRect(445, 380, 152, 72, 16, pal.primary);
            _canvas->drawRoundRect(445, 380, 152, 72, 16, pal.primary);
            drawSmoothText(UIF_HEADING, "BOOST", 521, 416, textdatum_t::middle_center, onPrimaryFg(pal), pal.primary);
        } else {
            _canvas->fillRoundRect(445, 380, 152, 72, 16, pal.surface);
            _canvas->drawRoundRect(445, 380, 152, 72, 16, pal.border);
            drawSmoothText(UIF_HEADING, "BOOST", 521, 416, textdatum_t::middle_center, pal.text, pal.surface);
        }
    }

    int8_t cur_banner = banner_now ? 1 : 0;
    if (btn_layout_changed || (int)telemetry.ride_mode != _cache.ride_mode || cur_banner != _cache.banner_active) {
        _cache.ride_mode = (int)telemetry.ride_mode;
        _cache.banner_active = cur_banner;
        if (banner_now) {
            _canvas->fillRoundRect(613, 380, 152, 72, 16, mCol);
            _canvas->drawRoundRect(613, 380, 152, 72, 16, mCol);
            drawSmoothText(UIF_HEADING, mNames[mIdx], 689, 416, textdatum_t::middle_center, onPrimaryFg(pal), mCol);
        } else {
            _canvas->fillRoundRect(613, 380, 152, 72, 16, pal.surface);
            _canvas->drawRoundRect(613, 380, 152, 72, 16, mCol);
            _canvas->drawRoundRect(614, 381, 150, 70, 15, mCol);
            drawSmoothText(UIF_HEADING, mNames[mIdx], 689, 416, textdatum_t::middle_center, mCol, pal.surface);
        }
    }

    // 10. Bottom Left: Dynamic Vector Badges & Centered Thermals Row
    bool gf_on  = telemetry.incline_gforce_enabled && (Settings.get().show_gforce != 0);
    bool inc_on = telemetry.incline_gforce_enabled && (Settings.get().show_incline != 0);
    bool lb_on  = (Settings.get().show_light_btn != 0);

    bool badge_cfg_changed = (_cache.show_gforce != (gf_on ? 1 : 0)) ||
                             (_cache.show_incline != (inc_on ? 1 : 0)) ||
                             (_cache.show_light_btn != (lb_on ? 1 : 0)) ||
                             (_cache.incline_gforce_enabled != (telemetry.incline_gforce_enabled ? 1 : 0));

    int gf_x = -1, gf_y = 416;
    int inc_x = -1, inc_y = 416;
    int light_x = -1, light_y = 416;
    const int temp_x = 240;

    if (lb_on) {
        if (gf_on && inc_on) {
            gf_x = 50;    gf_y = 416;
            inc_x = 110;  inc_y = 416;
            light_x = 50; light_y = 354;
        } else if (gf_on && !inc_on) {
            light_x = 50; light_y = 416;
            gf_x = 110;   gf_y = 416;
        } else if (!gf_on && inc_on) {
            light_x = 50; light_y = 416;
            inc_x = 110;  inc_y = 416;
        } else {
            light_x = 50; light_y = 416;
        }
    } else {
        if (gf_on && inc_on) {
            gf_x = 50;   gf_y = 416;
            inc_x = 110; inc_y = 416;
        } else if (gf_on && !inc_on) {
            gf_x = 50;   gf_y = 416;
        } else if (!gf_on && inc_on) {
            inc_x = 50;  inc_y = 416;
        }
    }

    if (!_hyper_inited || badge_cfg_changed) {
        _cache.show_gforce = gf_on ? 1 : 0;
        _cache.show_incline = inc_on ? 1 : 0;
        _cache.show_light_btn = lb_on ? 1 : 0;
        _cache.incline_gforce_enabled = telemetry.incline_gforce_enabled;
        if (badge_cfg_changed) {
            _canvas->fillRect(20, 324, 62, 62, pal.bg);
            _canvas->fillRect(20, 386, 122, 62, pal.bg);
            _canvas->fillRect(156, 396, 170, 42, pal.bg);
        }
        _cache.temp_esc = -999;
        _cache.temp_motor = -999;
        _cache.g_force_x10 = -999;
        _cache.incline_deg = -999;
        _cache.lights_enabled = -1;
    }

    int cur_fet = (int)(telemetry.temp_esc + 0.5f);
    int cur_mot = (int)(telemetry.temp_motor + 0.5f);
    if (!_hyper_inited || badge_cfg_changed || cur_fet != _cache.temp_esc || cur_mot != _cache.temp_motor) {
        _cache.temp_esc = cur_fet;
        _cache.temp_motor = cur_mot;
        _canvas->fillRect(156, 396, 170, 42, pal.bg);
        drawThermalsRow(_canvas, getUiFont(UIF_BODY), telemetry.temp_esc, telemetry.temp_motor, temp_x, 408, pal);
    }

    const lgfx::IFont *smallFont = getUiFont(UIF_SMALL);
    if (gf_x > 0) {
        int cur_g10 = (int)roundf(telemetry.g_force * 10.0f);
        if (!_hyper_inited || badge_cfg_changed || cur_g10 != _cache.g_force_x10) {
            _cache.g_force_x10 = cur_g10;
            drawGForceBadge(_canvas, smallFont, telemetry.g_force, gf_x, gf_y, 27, pal);
        }
    }

    if (inc_x > 0) {
        int cur_inc_x10 = (int)roundf(telemetry.incline_deg * 10.0f);
        if (!_hyper_inited || badge_cfg_changed || cur_inc_x10 != _cache.incline_deg) {
            _cache.incline_deg = cur_inc_x10;
            drawInclineBadge(_canvas, smallFont, telemetry.incline_deg, inc_x, inc_y, 27, pal);
        }
    }

    if (light_x > 0) {
        int8_t cur_light = telemetry.lights_enabled ? 1 : 0;
        if (!_hyper_inited || badge_cfg_changed || cur_light != _cache.lights_enabled) {
            _cache.lights_enabled = cur_light;
            drawLightBadge(_canvas, smallFont, telemetry.lights_enabled, light_x, light_y, 27, pal);
        }
    }

    _hyper_inited = true;
}

// ==============================================================================
// SCREEN 1: LIGHTS SCREEN (Gated cleanly behind ENABLE_LIGHTS_SCREEN to save Flash)
// ==============================================================================
#if ENABLE_LIGHTS_SCREEN
void DashboardRenderer::initLightsScreen(const DashTelemetry &telemetry) {
    _canvas->fillScreen(currentTheme().bg);
}

void DashboardRenderer::drawColorWheel(int cx, int cy, int r_in, int r_out, uint8_t selected_hue) {
    (void)selected_hue;
    for (int deg = 0; deg < 360; deg++) {
        uint8_t h = (uint8_t)(deg * 255 / 360);
        uint16_t col = hueToRgb565(h);
        _canvas->fillArc(cx, cy, r_in, r_out, (float)deg, (float)(deg + 1.2f), col);
    }
}

void DashboardRenderer::renderLightsScreen(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);
    drawSmoothText(UIF_HEADING, "LIGHTS", 35, 22, textdatum_t::top_left, pal.text, pal.bg);
    drawPillToggle(690, 18, telemetry.lights_enabled, pal);

    int cx = 220, cy = 230, r_in = 88, r_out = 125;
    drawColorWheel(cx, cy, r_in, r_out, telemetry.light_hue);
    _canvas->fillCircle(cx, cy, r_in - 1, pal.bg);
    uint16_t curCol = telemetry.lights_enabled ? hueToRgb565(telemetry.light_hue) : pal.surface_alt;
    _canvas->fillCircle(cx, cy, 70, curCol);

    const char *fxNames[] = { "SOLID", "BREATHE", "WAVE", "RAINBOW", "COMET", "SPARKLE", "STROBE", "POLICE", "WHITE" };
    for (int i = 0; i < 9; i++) {
        int col = i % 3, row = i / 3;
        int bx = 415 + col * 120, by = 65 + row * 56, bw = 105, bh = 48;
        bool active = (i == telemetry.light_mode);
        _canvas->fillRoundRect(bx, by, bw, bh, 12, active ? pal.primary : pal.surface);
        _canvas->drawRoundRect(bx, by, bw, bh, 12, active ? pal.primary : pal.border);
        drawSmoothText(UIF_SMALL, fxNames[i], bx + bw / 2, by + bh / 2, textdatum_t::middle_center,
                       active ? onPrimaryFg(pal) : pal.text, active ? pal.primary : pal.surface);
    }
}
#endif

// ==============================================================================
// SCREEN 2: TELEMETRIE & STATISTIK — RESTORED CLASSIC 6-CARD DASH (800x480)
// ==============================================================================
void DashboardRenderer::initRtGraphsScreen(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);

    // Top Bar: Back Button [< DASH], [ RACE ] Button, Center Title, Reset Trip Button
    _canvas->fillRoundRect(20, 12, 110, 36, 18, pal.surface);
    _canvas->drawRoundRect(20, 12, 110, 36, 18, pal.border);
    drawSmoothText(UIF_BODY, "< DASH", 75, 30, textdatum_t::middle_center, pal.text, pal.surface);

    // Styled [ RACE ] touch button (only visible when tuning mode is active)
    if (telemetry.tuning_mode) {
        uint16_t raceBg = blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 28 : 48);
        _canvas->fillRoundRect(140, 12, 100, 36, 18, raceBg);
        _canvas->drawRoundRect(140, 12, 100, 36, 18, pal.accent_danger);
        drawSmoothText(UIF_BODY, "RACE", 190, 30, textdatum_t::middle_center, pal.accent_danger, raceBg);
    }

    int titleX = telemetry.tuning_mode ? 430 : 380;
    drawSmoothText(UIF_HEADING, "FAHRDATEN & STATISTIK", titleX, 30, textdatum_t::middle_center, pal.primary, pal.bg);

    _canvas->fillRoundRect(640, 12, 140, 36, 18, pal.surface);
    _canvas->drawRoundRect(640, 12, 140, 36, 18, pal.border);
    drawSmoothText(UIF_BODY, "RESET TRIP", 710, 30, textdatum_t::middle_center, pal.text, pal.surface);

    // Top Summary Card: Trip & Movement Overview (4 KPI strips)
    drawCard(20, 56, 760, 92, pal.surface, pal.border, 12);
    _canvas->drawFastVLine(210, 66, 72, pal.border);
    _canvas->drawFastVLine(400, 66, 72, pal.border);
    _canvas->drawFastVLine(590, 66, 72, pal.border);

    drawSmoothText(UIF_SMALL, "STRECKE",   36,  66, textdatum_t::top_left, pal.muted, pal.surface);
    drawSmoothText(UIF_SMALL, "FAHRZEIT",  225, 66, textdatum_t::top_left, pal.muted, pal.surface);
    drawSmoothText(UIF_SMALL, "MAX SPEED", 415, 66, textdatum_t::top_left, pal.muted, pal.surface);
    drawSmoothText(UIF_SMALL, "ENERGIE",   605, 66, textdatum_t::top_left, pal.muted, pal.surface);

    // Middle Performance Tile Grid (6 Large Tiles, 2x3 grid):
    // Row 1: y = 160, h = 142
    // Tile 1: MAX LEISTUNG (PEAK WATT)
    drawCard(20, 160, 244, 142, pal.surface, pal.border, 12);
    _canvas->fillRoundRect(32, 160, 220, 3, 1, pal.primary);
    drawSmoothText(UIF_SMALL, "MAX LEISTUNG (PEAK)", 32, 172, textdatum_t::top_left, pal.primary, pal.surface);
    drawSmoothText(UIF_SMALL, "Spitzenleistung ab Akku", 32, 268, textdatum_t::top_left, pal.muted, pal.surface);

    // Tile 2: PEAK AKKU-STROM
    drawCard(278, 160, 244, 142, pal.surface, pal.border, 12);
    _canvas->fillRoundRect(290, 160, 220, 3, 1, pal.primary);
    drawSmoothText(UIF_SMALL, "PEAK AKKU-STROM", 290, 172, textdatum_t::top_left, pal.primary, pal.surface);
    drawSmoothText(UIF_SMALL, "Maximaler Batterie-Strom", 290, 268, textdatum_t::top_left, pal.muted, pal.surface);

    // Tile 3: PEAK PHASENSTROM
    drawCard(536, 160, 244, 142, pal.surface, pal.border, 12);
    _canvas->fillRoundRect(548, 160, 220, 3, 1, pal.primary);
    drawSmoothText(UIF_SMALL, "PEAK PHASENSTROM", 548, 172, textdatum_t::top_left, pal.primary, pal.surface);
    drawSmoothText(UIF_SMALL, "Max. Motor-Drehmoment", 548, 268, textdatum_t::top_left, pal.muted, pal.surface);

    // Row 2: y = 314, h = 142
    // Tile 4: VERBRAUCH & EFFIZIENZ
    drawCard(20, 314, 244, 142, pal.surface, pal.border, 12);
    _canvas->fillRoundRect(32, 314, 220, 3, 1, pal.primary);
    drawSmoothText(UIF_SMALL, "VERBRAUCH & EFFIZIENZ", 32, 326, textdatum_t::top_left, pal.primary, pal.surface);

    // Tile 5: AKKU-SPANNUNG (MIN/MAX)
    drawCard(278, 314, 244, 142, pal.surface, pal.border, 12);
    _canvas->fillRoundRect(290, 314, 220, 3, 1, pal.primary);
    drawSmoothText(UIF_SMALL, "AKKU-SPANNUNG (MIN/MAX)", 290, 326, textdatum_t::top_left, pal.primary, pal.surface);

    // Tile 6: MAX TEMPERATUR
    drawCard(536, 314, 244, 142, pal.surface, pal.border, 12);
    _canvas->fillRoundRect(548, 314, 220, 3, 1, pal.primary);
    drawSmoothText(UIF_SMALL, "MAX TEMPERATUR", 548, 326, textdatum_t::top_left, pal.primary, pal.surface);
    drawSmoothText(UIF_SMALL, "Motor / Inverter FETs", 548, 422, textdatum_t::top_left, pal.muted, pal.surface);

    _stats_cache.invalidate();
    _stats_cache.tuning_mode = telemetry.tuning_mode ? 1 : 0;
    _stats_inited = false;
}

void DashboardRenderer::drawStatsSparkline(const DashTelemetry &telemetry, const ColorThemePalette &pal) {
    // Replaced by classic 6-card telemetry grid
}

void DashboardRenderer::renderRtGraphsScreen(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();

    int8_t cur_tm = telemetry.tuning_mode ? 1 : 0;
    if (_stats_cache.tuning_mode != cur_tm) {
        _stats_cache.invalidate();
        _stats_cache.tuning_mode = cur_tm;

        // Toggle [ RACE ] button in top bar (x: 140..240, y: 12..48)
        if (cur_tm) {
            uint16_t raceBg = blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 28 : 48);
            _canvas->fillRoundRect(140, 12, 100, 36, 18, raceBg);
            _canvas->drawRoundRect(140, 12, 100, 36, 18, pal.accent_danger);
            drawSmoothText(UIF_BODY, "RACE", 190, 30, textdatum_t::middle_center, pal.accent_danger, raceBg);
        } else {
            _canvas->fillRect(140, 12, 100, 36, pal.bg);
        }
        int titleX = cur_tm ? 430 : 380;
        _canvas->fillRect(250, 12, 380, 36, pal.bg);
        drawSmoothText(UIF_HEADING, "FAHRDATEN & STATISTIK", titleX, 30, textdatum_t::middle_center, pal.primary, pal.bg);
    }

    // Police Mode Masking: When tuning_mode == false, limit high-performance stats
    // to strictly legal-compliant values (Max 22 km/h, 500 W peak, 20 A battery, 45 A phase, 20 km/h avg).
    float disp_max_spd   = telemetry.tuning_mode ? telemetry.stats.max_speed_kmh   : std::min(telemetry.stats.max_speed_kmh, 22.0f);
    float disp_live_spd  = telemetry.tuning_mode ? telemetry.speed_kmh             : std::min(telemetry.speed_kmh, 22.0f);
    float disp_avg_spd   = telemetry.tuning_mode ? telemetry.stats.avg_speed_kmh   : std::min(telemetry.stats.avg_speed_kmh, 20.0f);
    float disp_peak_pwr  = telemetry.tuning_mode ? telemetry.stats.peak_power_watts: std::min(telemetry.stats.peak_power_watts, 500.0f);
    float disp_peak_curr = telemetry.tuning_mode ? telemetry.stats.peak_current_amps: std::min(telemetry.stats.peak_current_amps, 20.0f);
    float disp_peak_pha  = telemetry.tuning_mode ? telemetry.stats.peak_phase_amps : std::min(telemetry.stats.peak_phase_amps, 45.0f);

    // Col 1: STRECKE (Trip km & ODO km)
    int cur_trip = (int)(telemetry.trip_km * 10.0f + 0.5f);
    int cur_odo  = (int)(telemetry.odo_km * 10.0f + 0.5f);
    if (!_stats_inited || cur_trip != _stats_cache.trip_x10 || cur_odo != _stats_cache.odo_x10) {
        _stats_cache.trip_x10 = cur_trip;
        _stats_cache.odo_x10 = cur_odo;
        _canvas->fillRect(36, 86, 168, 56, pal.surface);
        char distBuf[24]; snprintf(distBuf, sizeof(distBuf), "%.1f km", telemetry.trip_km);
        drawSmoothText(UIF_HEADING, distBuf, 36, 86, textdatum_t::top_left, pal.text, pal.surface);
        char odoBuf[24]; snprintf(odoBuf, sizeof(odoBuf), "ODO: %.1f km", telemetry.odo_km);
        drawSmoothText(UIF_SMALL, odoBuf, 36, 122, textdatum_t::top_left, pal.muted, pal.surface);
    }

    // Col 2: FAHRZEIT (Ride time & Avg speed)
    uint32_t sec = telemetry.stats.ride_time_sec;
    int cur_avg = (int)(disp_avg_spd * 10.0f + 0.5f);
    if (!_stats_inited || sec != _stats_cache.ride_sec || cur_avg != _stats_cache.avg_spd_x10) {
        _stats_cache.ride_sec = sec;
        _stats_cache.avg_spd_x10 = cur_avg;
        _canvas->fillRect(225, 86, 168, 56, pal.surface);
        char timeBuf[24];
        if (sec >= 3600) {
            snprintf(timeBuf, sizeof(timeBuf), "%u:%02u:%02u", (unsigned int)(sec / 3600), (unsigned int)((sec % 3600) / 60), (unsigned int)(sec % 60));
        } else {
            snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u", (unsigned int)(sec / 60), (unsigned int)(sec % 60));
        }
        drawSmoothText(UIF_HEADING, timeBuf, 225, 86, textdatum_t::top_left, pal.text, pal.surface);
        char avgSpd[24]; snprintf(avgSpd, sizeof(avgSpd), "Avg: %.1f km/h", disp_avg_spd);
        drawSmoothText(UIF_SMALL, avgSpd, 225, 122, textdatum_t::top_left, pal.muted, pal.surface);
    }

    // Col 3: MAX SPEED (Max speed & Live speed)
    int cur_max_spd  = (int)(disp_max_spd * 10.0f + 0.5f);
    int cur_live_spd = (int)(disp_live_spd * 10.0f + 0.5f);
    if (!_stats_inited || cur_max_spd != _stats_cache.max_spd_x10 || cur_live_spd != _stats_cache.live_spd_x10) {
        _stats_cache.max_spd_x10 = cur_max_spd;
        _stats_cache.live_spd_x10 = cur_live_spd;
        _canvas->fillRect(415, 86, 168, 56, pal.surface);
        char maxSpd[24]; snprintf(maxSpd, sizeof(maxSpd), "%.1f km/h", disp_max_spd);
        drawSmoothText(UIF_HEADING, maxSpd, 415, 86, textdatum_t::top_left, pal.text, pal.surface);
        char liveSpd[24]; snprintf(liveSpd, sizeof(liveSpd), "Live: %.1f km/h", disp_live_spd);
        drawSmoothText(UIF_SMALL, liveSpd, 415, 122, textdatum_t::top_left, pal.muted, pal.surface);
    }

    // Col 4: ENERGIE (Trip Wh & Remaining Wh)
    int cur_wh  = (int)(telemetry.stats.trip_wh + 0.5f);
    int cur_rem = (int)(telemetry.remaining_wh + 0.5f);
    if (!_stats_inited || cur_wh != _stats_cache.trip_wh || cur_rem != _stats_cache.rem_wh) {
        _stats_cache.trip_wh = cur_wh;
        _stats_cache.rem_wh = cur_rem;
        _canvas->fillRect(605, 86, 168, 56, pal.surface);
        char whBuf[16]; snprintf(whBuf, sizeof(whBuf), "%.0f Wh", telemetry.stats.trip_wh);
        drawSmoothText(UIF_HEADING, whBuf, 605, 86, textdatum_t::top_left, pal.text, pal.surface);
        char remWh[16]; snprintf(remWh, sizeof(remWh), "Rest: %.0f Wh", telemetry.remaining_wh);
        drawSmoothText(UIF_SMALL, remWh, 605, 122, textdatum_t::top_left, pal.muted, pal.surface);
    }

    // Tile 1: MAX LEISTUNG (PEAK WATT & PS)
    int cur_pwr = (int)(disp_peak_pwr + 0.5f);
    if (!_stats_inited || cur_pwr != _stats_cache.peak_watts) {
        _stats_cache.peak_watts = cur_pwr;
        _canvas->fillRect(32, 202, 220, 62, pal.surface);
        char pwrBuf[16]; snprintf(pwrBuf, sizeof(pwrBuf), "%.0f W", disp_peak_pwr);
        drawSmoothText(UIF_HEADING, pwrBuf, 32, 202, textdatum_t::top_left, pal.text, pal.surface);

        float ps_stat = disp_peak_pwr * 0.00135962f;
        char psBuf[16]; snprintf(psBuf, sizeof(psBuf), "%.1f PS", ps_stat);
        drawSmoothText(UIF_BODY, psBuf, 32, 238, textdatum_t::top_left, pal.primary, pal.surface);
    }

    // Tile 2: PEAK AKKU-STROM
    int cur_curr = (int)(disp_peak_curr * 10.0f + 0.5f);
    if (!_stats_inited || cur_curr != _stats_cache.peak_curr_x10) {
        _stats_cache.peak_curr_x10 = cur_curr;
        _canvas->fillRect(290, 202, 220, 62, pal.surface);
        char curBuf[16]; snprintf(curBuf, sizeof(curBuf), "%.1f A", disp_peak_curr);
        drawSmoothText(UIF_HEADING, curBuf, 290, 202, textdatum_t::top_left, pal.text, pal.surface);
    }

    // Tile 3: PEAK PHASENSTROM
    int cur_pha = (int)(disp_peak_pha * 10.0f + 0.5f);
    if (!_stats_inited || cur_pha != _stats_cache.peak_phase_x10) {
        _stats_cache.peak_phase_x10 = cur_pha;
        _canvas->fillRect(548, 202, 220, 62, pal.surface);
        char phaBuf[16]; snprintf(phaBuf, sizeof(phaBuf), "%.1f A", disp_peak_pha);
        drawSmoothText(UIF_HEADING, phaBuf, 548, 202, textdatum_t::top_left, pal.text, pal.surface);
    }

    // Tile 4: VERBRAUCH & EFFIZIENZ
    int cur_eff = (int)(telemetry.stats.trip_wh_km * 10.0f + 0.5f);
    float dynWh = (telemetry.stats.dynamic_wh_km > 0.0f) ? telemetry.stats.dynamic_wh_km : telemetry.dynamic_wh_km;
    int cur_dyn = (int)(dynWh * 10.0f + 0.5f);
    if (!_stats_inited || cur_eff != _stats_cache.trip_wh_km_x10 || cur_dyn != _stats_cache.dyn_wh_km_x10) {
        _stats_cache.trip_wh_km_x10 = cur_eff;
        _stats_cache.dyn_wh_km_x10 = cur_dyn;
        _canvas->fillRect(32, 356, 220, 68, pal.surface);
        char effBuf[24]; snprintf(effBuf, sizeof(effBuf), "%.1f Wh/km", telemetry.stats.trip_wh_km);
        drawSmoothText(UIF_HEADING, effBuf, 32, 356, textdatum_t::top_left, pal.text, pal.surface);
        char dynBuf[32]; snprintf(dynBuf, sizeof(dynBuf), "Rollend (2km): %.1f Wh/km", dynWh);
        drawSmoothText(UIF_SMALL, dynBuf, 32, 400, textdatum_t::top_left, pal.muted, pal.surface);
    }

    // Tile 5: AKKU-SPANNUNG (MIN/MAX)
    int cur_min_v = (int)(telemetry.stats.min_voltage * 10.0f + 0.5f);
    int cur_max_v = (int)(telemetry.stats.max_voltage * 10.0f + 0.5f);
    int cur_live_v = (int)(telemetry.voltage * 10.0f + 0.5f);
    if (!_stats_inited || cur_min_v != _stats_cache.min_v_x10 || cur_max_v != _stats_cache.max_v_x10 || cur_live_v != _stats_cache.live_v_x10) {
        _stats_cache.min_v_x10 = cur_min_v;
        _stats_cache.max_v_x10 = cur_max_v;
        _stats_cache.live_v_x10 = cur_live_v;
        _canvas->fillRect(290, 356, 220, 68, pal.surface);
        char vBuf[32]; snprintf(vBuf, sizeof(vBuf), "%.1fV / %.1fV", telemetry.stats.min_voltage, telemetry.stats.max_voltage);
        drawSmoothText(UIF_HEADING, vBuf, 290, 356, textdatum_t::top_left, pal.text, pal.surface);
        char vSub[32]; snprintf(vSub, sizeof(vSub), "Live: %.1fV (Sag Check)", telemetry.voltage);
        drawSmoothText(UIF_SMALL, vSub, 290, 400, textdatum_t::top_left, pal.muted, pal.surface);
    }

    // Tile 6: MAX TEMPERATUR
    int cur_max_mot = (int)(telemetry.stats.max_temp_motor + 0.5f);
    int cur_max_esc = (int)(telemetry.stats.max_temp_esc + 0.5f);
    if (!_stats_inited || cur_max_mot != _stats_cache.max_temp_mot || cur_max_esc != _stats_cache.max_temp_esc) {
        _stats_cache.max_temp_mot = cur_max_mot;
        _stats_cache.max_temp_esc = cur_max_esc;
        _canvas->fillRect(548, 356, 220, 60, pal.surface);
        char tmpBuf[32]; snprintf(tmpBuf, sizeof(tmpBuf), "%.0f °C / %.0f °C", telemetry.stats.max_temp_motor, telemetry.stats.max_temp_esc);
        drawSmoothText(UIF_HEADING, tmpBuf, 548, 356, textdatum_t::top_left, pal.text, pal.surface);
    }

    _stats_inited = true;
}

// ==============================================================================
// VESC FAULT ALARM BANNER (Theme-aware High-Visibility Banner)
// ==============================================================================
void DashboardRenderer::drawFaultBanner(uint8_t fault_code) {
    const ColorThemePalette &pal = currentTheme();
    uint32_t now = millis();
    bool flash = ((now / 350) % 2) == 0;
    uint16_t bg_col = flash ? pal.accent_danger : blendRgb565(pal.surface, pal.accent_danger, 140);
    uint16_t border_col = flash ? onPrimaryFg(pal) : pal.accent_danger;
    uint16_t fg_col = onPrimaryFg(pal);

    drawCard(20, 8, 760, 56, bg_col, border_col, 12);

    _canvas->fillRoundRect(28, 14, 44, 44, 8, pal.accent_danger);
    _canvas->drawRoundRect(28, 14, 44, 44, 8, onPrimaryFg(pal));
    drawSmoothText(UIF_HEADING, "!", 50, 36, textdatum_t::middle_center, fg_col, pal.accent_danger);

    char titleBuf[64];
    snprintf(titleBuf, sizeof(titleBuf), "VESC ALARM: %s", FaultManager::getFaultTitleDe(fault_code));
    drawSmoothText(UIF_HEADING, titleBuf, 84, 13, textdatum_t::top_left, fg_col, bg_col);

    char descBuf[96];
    snprintf(descBuf, sizeof(descBuf), "%s (MOTOR ABGESCHALTET)", FaultManager::getFaultDescriptionDe(fault_code));
    drawSmoothText(UIF_BODY, descBuf, 84, 39, textdatum_t::top_left, fg_col, bg_col);
}

// ==============================================================================
// DESIGN UPGRADE 5: COMPLETE PIN LOCKSCREEN REDESIGN (Theme-aware Glass UI)
// ==============================================================================
void DashboardRenderer::renderPinLockScreen(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();

    int entered_len = (int)strlen(telemetry.pin_entry);
    bool err = telemetry.pin_error && (millis() < telemetry.pin_error_end_ms);
    bool alarm = telemetry.theft_alarm_active;

    if (alarm != _last_theft_alarm) {
        _last_theft_alarm = alarm;
        _pin_screen_drawn = false;
    }

    static const char *KEYS[4][3] = {
        { "1", "2", "3" },
        { "4", "5", "6" },
        { "7", "8", "9" },
        { "DEL", "0", "OK" }
    };
    static const int col_x[3] = { 200, 340, 480 };
    static const int row_y[4] = { 164, 234, 304, 374 };
    const int btn_w = 120;
    const int btn_h = 56;

    auto drawKeypadButton = [&](int r, int c, bool pressed) {
        int bx = col_x[c];
        int by = row_y[r];
        const char *label = KEYS[r][c];
        bool isDel = (r == 3 && c == 0);
        bool isOk  = (r == 3 && c == 2);

        uint16_t btn_bg, btn_border, txt_col;
        if (isDel) {
            btn_bg     = pressed ? pal.accent_danger : blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 22 : 34);
            btn_border = pal.accent_danger;
            txt_col    = pressed ? onPrimaryFg(pal) : pal.accent_danger;
        } else if (isOk) {
            btn_bg     = pressed ? pal.accent_ok : blendRgb565(pal.surface, pal.accent_ok, pal.is_light ? 22 : 34);
            btn_border = pal.accent_ok;
            txt_col    = pressed ? onPrimaryFg(pal) : pal.accent_ok;
        } else {
            btn_bg     = pressed ? pal.primary : pal.surface;
            btn_border = pressed ? pal.primary : pal.border;
            txt_col    = pressed ? onPrimaryFg(pal) : pal.text;
        }

        drawCard(bx, by, btn_w, btn_h, btn_bg, btn_border, 14);
        drawSmoothText((isDel || isOk) ? UIF_HEADING : UIF_MED_NUM,
                       label, bx + btn_w / 2, by + btn_h / 2,
                       textdatum_t::middle_center, txt_col, btn_bg);
    };

    if (!_pin_screen_drawn) {
        _pin_screen_drawn = true;
        _screen_dirty = false;
        _last_pin_len = -1;
        _last_pin_err = !err;
        _last_pin_active_key = -1;
        _last_pin_bat_pct = -999;
        _last_pin_volt_x10 = -999;
        _last_pin_vesc_conn = -1;

        _canvas->fillScreen(pal.bg);

        if (alarm) {
            // Crisp high-visibility crimson border frame
            for (int b = 0; b < 4; b++) {
                _canvas->drawRect(b, b, 800 - 2 * b, 480 - 2 * b, pal.accent_danger);
            }
            drawCard(150, 14, 500, 48, pal.accent_danger, pal.accent_danger, 14);
            drawSmoothText(UIF_HEADING, "DIEBSTAHL-ALARM AKTIV!", 400, 38,
                           textdatum_t::middle_center, onPrimaryFg(pal), pal.accent_danger);

            char moveBuf[80];
            snprintf(moveBuf, sizeof(moveBuf), "BEWEGUNG ERKANNT (%.1f m) — FOC MOTOR-SIRENE AKTIV", telemetry.locked_moved_meters);
            drawSmoothText(UIF_BODY, moveBuf, 400, 76, textdatum_t::middle_center, pal.accent_danger, pal.bg);
        } else {
            // Sleek Automotive Header: Lock Icon + "PIN-SPERRE" centered
            drawLockIcon(316, 38, pal.primary, pal.bg);
            drawSmoothText(UIF_HEADING, "PIN-SPERRE", 412, 38, textdatum_t::middle_center, pal.text, pal.bg);
            // Subtle bottom hairline under top header
            _canvas->drawFastHLine(24, 68, 752, pal.border);
        }

        // Sleek 3x4 Glass Keypad Grid
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 3; c++) {
                drawKeypadButton(r, c, false);
            }
        }
    }

    // Compact Battery Status Pill in top-right header (visible before unlocking!)
    if (!alarm) {
        bool valid_bat = (telemetry.voltage > 10.0f && telemetry.battery_pct >= 0.0f);
        int cur_bp = valid_bat ? (int)(telemetry.battery_pct + 0.5f) : -1;
        int cur_vx10 = valid_bat ? (int)(telemetry.voltage * 10.0f + 0.5f) : -1;
        int8_t cur_vc = valid_bat ? 1 : 0;

        if (cur_bp != _last_pin_bat_pct || cur_vx10 != _last_pin_volt_x10 || cur_vc != _last_pin_vesc_conn) {
            _last_pin_bat_pct = cur_bp;
            _last_pin_volt_x10 = cur_vx10;
            _last_pin_vesc_conn = cur_vc;

            const int px = 582, py = 18, pw = 194, ph = 38;
            _canvas->fillRoundRect(px, py, pw, ph, 19, pal.surface);
            _canvas->drawRoundRect(px, py, pw, ph, 19, pal.border);

            // Mini vector battery glyph
            uint16_t bCol = !valid_bat ? pal.muted :
                            ((telemetry.battery_pct <= 12.0f) ? pal.accent_danger :
                            ((telemetry.battery_pct <= 25.0f) ? pal.accent_warn : pal.accent_ok));
            _canvas->drawRoundRect(px + 14, py + 12, 20, 14, 3, bCol);
            _canvas->fillRect(px + 34, py + 16, 3, 6, bCol);
            if (valid_bat) {
                int fw = (int)((std::min(std::max(telemetry.battery_pct, 0.0f), 100.0f) / 100.0f) * 14.0f);
                if (fw > 0) _canvas->fillRect(px + 17, py + 15, fw, 8, bCol);
            }

            char pillBuf[32];
            if (valid_bat) {
                snprintf(pillBuf, sizeof(pillBuf), "%d%%  |  %.1f V", cur_bp, telemetry.voltage);
            } else {
                snprintf(pillBuf, sizeof(pillBuf), "--%%  |  --.- V");
            }
            drawSmoothText(UIF_BODY, pillBuf, px + 46, py + ph / 2, textdatum_t::middle_left,
                           valid_bat ? pal.text : pal.muted, pal.surface);
        }
    }

    // Instant tactile pressed visual feedback
    int8_t active_key = -1;
    if (Buttons.isTouching() && !err) {
        int16_t tx = Buttons.getTouchX();
        int16_t ty = Buttons.getTouchY();
        if (tx >= 100 && tx <= 700 && ty >= 120 && ty <= 460) {
            int c = (tx < 330) ? 0 : ((tx < 470) ? 1 : 2);
            int r = (ty < 227) ? 0 : ((ty < 297) ? 1 : ((ty < 367) ? 2 : 3));
            active_key = (int8_t)(r * 3 + c);
        }
    }
    if (active_key != _last_pin_active_key) {
        if (_last_pin_active_key >= 0) {
            drawKeypadButton(_last_pin_active_key / 3, _last_pin_active_key % 3, false);
        }
        if (active_key >= 0) {
            drawKeypadButton(active_key / 3, active_key % 3, true);
        }
        _last_pin_active_key = active_key;
    }

    // PIN Indicator Dots & Status Message (Zero-Flicker Differential Update)
    if (entered_len != _last_pin_len || err != _last_pin_err) {
        _last_pin_len = entered_len;
        _last_pin_err = err;

        _canvas->fillRect(160, 86, 480, 72, pal.bg);

        int dot_xs[4] = { 334, 378, 422, 466 };
        int dot_y = 106;

        for (int i = 0; i < 4; i++) {
            if (i < entered_len) {
                uint16_t dot_col = err ? pal.accent_danger : pal.primary;
                _canvas->fillCircle(dot_xs[i], dot_y, 11, dot_col);
                if (err) {
                    _canvas->drawCircle(dot_xs[i], dot_y, 14, pal.accent_danger);
                }
            } else {
                _canvas->fillCircle(dot_xs[i], dot_y, 11, pal.surface);
                _canvas->drawCircle(dot_xs[i], dot_y, 11, err ? pal.accent_danger : pal.border);
            }
        }

        if (err) {
            uint16_t errBg = blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 26 : 45);
            _canvas->fillRoundRect(205, 128, 390, 26, 13, errBg);
            _canvas->drawRoundRect(205, 128, 390, 26, 13, pal.accent_danger);
            drawSmoothText(UIF_BODY, "FALSCHER PIN — BITTE WIEDERHOLEN", 400, 141,
                           textdatum_t::middle_center, pal.accent_danger, errBg);
        } else if (alarm) {
            drawSmoothText(UIF_BODY, "PIN eingeben zum Entsperren & Alarm aus", 400, 140,
                           textdatum_t::middle_center, pal.accent_danger, pal.bg);
        } else {
            drawSmoothText(UIF_BODY, "4-stelligen Entsperrcode eingeben", 400, 140,
                           textdatum_t::middle_center, pal.muted, pal.bg);
        }
    }
}

// ==============================================================================
// SCREEN 3: MODERN AUTOMOTIVE SETTINGS (5 Tabs Tuning / 3 Legal Stealth Tabs)
// ==============================================================================
void DashboardRenderer::initSettingsScreen(const DashTelemetry &telemetry) {
    // Stealth enforcement: in legal mode, only TAB_DASH, TAB_SYSTEM, TAB_PIN are accessible!
    if (!telemetry.tuning_mode) {
        if (_settings_tab_idx < TAB_DASH || _settings_tab_idx >= TAB_COUNT) {
            _settings_tab_idx = TAB_DASH;
        }
    } else {
        if (_settings_tab_idx >= TAB_COUNT) {
            _settings_tab_idx = TAB_SPEED;
        }
    }
    _pin_modal_open = false;
    _settings_inited = false;
    _last_rendered_tab = 255;
    _settings_last_tuning_mode = telemetry.tuning_mode ? 1 : 0;
    _menu_dirty = true;
}

void DashboardRenderer::renderSettingsScreen(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();

    // If tuning_mode changed live while in Settings, re-initialize tab bar cleanly!
    int8_t cur_tm = telemetry.tuning_mode ? 1 : 0;
    if (_settings_last_tuning_mode != cur_tm) {
        _settings_last_tuning_mode = cur_tm;
        if (!telemetry.tuning_mode && _settings_tab_idx < TAB_DASH) {
            _settings_tab_idx = TAB_DASH;
        }
        _settings_inited = false;
        _last_rendered_tab = 255;
        _menu_dirty = true;
    }

    // Clamp legal mode tab safety
    if (!telemetry.tuning_mode && _settings_tab_idx < TAB_DASH) {
        _settings_tab_idx = TAB_DASH;
        _last_rendered_tab = 255;
        _menu_dirty = true;
    }

    if (_pin_modal_open && _pin_modal_success) {
        if (millis() >= _pin_modal_success_ms) {
            _pin_modal_open = false;
            _pin_modal_success = false;
            _pin_modal_buf[0] = '\0';
            _settings_inited = false;
            _last_rendered_tab = 255;
            _menu_dirty = true;
        }
    }

    if (!_menu_dirty) return;
    _menu_dirty = false;

    bool tabChanged = (_last_rendered_tab != _settings_tab_idx) || !_settings_inited;

    if (!_settings_inited) {
        _canvas->fillScreen(pal.bg);

        // 1. Sleek Header Bar: [< DASH], Title, [FERTIG]
        _canvas->fillRoundRect(20, 12, 108, 34, 17, pal.surface);
        _canvas->drawRoundRect(20, 12, 108, 34, 17, pal.border);
        drawSmoothText(UIF_BODY, "< DASH", 74, 29, textdatum_t::middle_center, pal.text, pal.surface);

        drawSmoothText(UIF_HEADING, "EINSTELLUNGEN", 400, 29, textdatum_t::middle_center, pal.text, pal.bg);

        _canvas->fillRoundRect(672, 12, 108, 34, 17, pal.primary);
        drawSmoothText(UIF_BODY, "FERTIG", 726, 29, textdatum_t::middle_center, onPrimaryFg(pal), pal.primary);

        _settings_inited = true;
    }

    // 2. Segmented Tab Bar (Stealth-safe: Legal = 3 tabs, Tuning = 5 tabs)
    static const char *ALL_TAB_NAMES[TAB_COUNT] = { "SPEED", "DRAGY", "DASH", "SYSTEM", "PIN" };
    uint16_t tabAccents[TAB_COUNT] = {
        pal.primary, pal.accent_warn, pal.primary, pal.primary, pal.primary
    };

    if (tabChanged) {
        _last_rendered_tab = _settings_tab_idx;
        _cache.invalidate();

        _canvas->fillRect(0, 54, 800, 58, pal.bg);

        const int tabY = 58;
        const int tabH = 46;

        if (telemetry.tuning_mode) {
            // 5 clean tabs across x=20..780 (w=144, gap=10, pitch=154)
            for (int i = 0; i < TAB_COUNT; i++) {
                int tx = 20 + i * 154;
                int tw = 144;
                bool isSel = (i == _settings_tab_idx);
                uint16_t accent = tabAccents[i];
                uint16_t bgCol = isSel ? blendRgb565(pal.surface, accent, pal.is_light ? 30 : 48) : pal.surface;
                uint16_t bdCol = isSel ? accent : pal.border;
                uint16_t fgCol = isSel ? accent : pal.muted;

                _canvas->fillRoundRect(tx, tabY, tw, tabH, 10, bgCol);
                _canvas->drawRoundRect(tx, tabY, tw, tabH, 10, bdCol);
                if (isSel) {
                    _canvas->fillRoundRect(tx + 24, tabY + tabH - 4, tw - 48, 3, 1, accent);
                }
                drawSmoothText(UIF_BODY, ALL_TAB_NAMES[i], tx + tw / 2, tabY + tabH / 2 - 1,
                               textdatum_t::middle_center, fgCol, bgCol);
            }
        } else {
            // Legal Mode: ONLY 3 clean tabs across x=20..780 (DASH, SYSTEM, PIN)
            static const uint8_t LEGAL_TABS[3] = { TAB_DASH, TAB_SYSTEM, TAB_PIN };
            for (int idx = 0; idx < 3; idx++) {
                uint8_t tabId = LEGAL_TABS[idx];
                int tx = 20 + idx * 258;
                int tw = 244;
                bool isSel = (tabId == _settings_tab_idx);
                uint16_t accent = pal.primary;
                uint16_t bgCol = isSel ? blendRgb565(pal.surface, accent, pal.is_light ? 30 : 48) : pal.surface;
                uint16_t bdCol = isSel ? accent : pal.border;
                uint16_t fgCol = isSel ? accent : pal.muted;

                _canvas->fillRoundRect(tx, tabY, tw, tabH, 10, bgCol);
                _canvas->drawRoundRect(tx, tabY, tw, tabH, 10, bdCol);
                if (isSel) {
                    _canvas->fillRoundRect(tx + 36, tabY + tabH - 4, tw - 72, 3, 1, accent);
                }
                drawSmoothText(UIF_HEADING, ALL_TAB_NAMES[tabId], tx + tw / 2, tabY + tabH / 2 - 1,
                               textdatum_t::middle_center, fgCol, bgCol);
            }
        }

        // Clear content canvas cleanly below tabs
        _canvas->fillRect(0, 112, 800, 368, pal.bg);
    }

    // ------------------------------------------------------------------
    // TAB 3: SPEED — TUNED TOPSPEED PER MODE (3 STACKED ROWS)
    // ------------------------------------------------------------------
    if (_settings_tab_idx == TAB_SPEED) {
        VehicleModeConfig &cfg = ModeManager::instance().getConfig();
        static const char *MODE_TITLES[3] = { "ECO TOPSPEED", "SPORT TOPSPEED", "RACE TOPSPEED" };
        uint16_t modeCols[3] = { pal.accent_ok, pal.primary, pal.accent_danger };

        for (int m = 0; m < 3; m++) {
            int ry = 118 + m * 112;
            int rh = 104;
            ModeLimits &lim = cfg.tuned_modes[m];
            if (lim.max_speed_kmh < 15.0f) lim.max_speed_kmh = 15.0f;
            if (lim.max_speed_kmh > 100.0f) lim.max_speed_kmh = 100.0f;
            uint16_t col = modeCols[m];

            drawCard(20, ry, 760, rh, pal.surface, pal.border, 12);
            _canvas->fillRoundRect(20, ry + 14, 4, rh - 28, 2, col);

            drawSmoothText(UIF_HEADING, MODE_TITLES[m], 40, ry + 12, textdatum_t::top_left, col, pal.surface);

            char spdBuf[24];
            snprintf(spdBuf, sizeof(spdBuf), "%.0f km/h", lim.max_speed_kmh);
            drawSmoothText(UIF_MED_NUM, spdBuf, 760, ry + 6, textdatum_t::top_right, pal.text, pal.surface);

            // [-] Button (x: 40..100, y: ry+48..ry+94)
            _canvas->fillRoundRect(40, ry + 48, 60, 44, 10, pal.surface_alt);
            _canvas->drawRoundRect(40, ry + 48, 60, 44, 10, pal.border);
            drawSmoothText(UIF_HEADING, "-", 70, ry + 70, textdatum_t::middle_center, pal.text, pal.surface_alt);

            // [+] Button (x: 700..760, y: ry+48..ry+94)
            _canvas->fillRoundRect(700, ry + 48, 60, 44, 10, pal.surface_alt);
            _canvas->drawRoundRect(700, ry + 48, 60, 44, 10, pal.border);
            drawSmoothText(UIF_HEADING, "+", 730, ry + 70, textdatum_t::middle_center, pal.text, pal.surface_alt);

            // Speed Slider Track (x: 120..680, w: 560)
            _canvas->fillRoundRect(120, ry + 62, 560, 16, 8, pal.track);
            float frac = (lim.max_speed_kmh - 15.0f) / (100.0f - 15.0f);
            frac = std::min(std::max(frac, 0.0f), 1.0f);
            int fillW = (int)(frac * 560.0f);
            if (fillW > 6) {
                _canvas->fillRoundRect(120, ry + 62, fillW, 16, 8, col);
            }
            int knobX = 120 + fillW;
            _canvas->fillCircle(knobX, ry + 70, 12, pal.surface);
            _canvas->drawCircle(knobX, ry + 70, 12, col);
            _canvas->drawCircle(knobX, ry + 70, 11, col);
            _canvas->fillCircle(knobX, ry + 70, 4, col);
        }

        drawSmoothText(UIF_SMALL, "Tuned Topspeed pro Modus (15–100 km/h)  •  Speichert automatisch bei FERTIG",
                       400, 462, textdatum_t::middle_center, pal.muted, pal.bg);
        return;
    }

    // ------------------------------------------------------------------
    // TAB 4: DRAGY SPLITS (Tuning Mode Only)
    // ------------------------------------------------------------------
    if (_settings_tab_idx == TAB_DRAGY) {
        auto drawSplitCol = [&](int col_idx, const char *title, uint8_t split_spd, uint16_t col) {
            int cx = 20 + col_idx * 258;
            int cw = 244, ch = 326;
            drawCard(cx, 120, cw, ch, pal.surface, pal.border, 12);
            _canvas->fillRoundRect(cx + 16, 120, cw - 32, 3, 1, col);

            drawSmoothText(UIF_HEADING, title, cx + 18, 134, textdatum_t::top_left, col, pal.surface);

            char subBuf[32];
            snprintf(subBuf, sizeof(subBuf), "Ziel: 0 – %d km/h", split_spd);
            drawSmoothText(UIF_SMALL, subBuf, cx + 18, 166, textdatum_t::top_left, pal.muted, pal.surface);

            char numBuf[16];
            snprintf(numBuf, sizeof(numBuf), "%d", split_spd);
            drawSmoothText(UIF_HERO_NUM, numBuf, cx + cw / 2, 232, textdatum_t::middle_center, pal.text, pal.surface);
            drawSmoothText(UIF_BODY, "km/h", cx + cw / 2, 276, textdatum_t::top_center, pal.muted, pal.surface);

            // [-] Button
            _canvas->fillRoundRect(cx + 18, 364, 96, 56, 12, pal.surface_alt);
            _canvas->drawRoundRect(cx + 18, 364, 96, 56, 12, pal.border);
            drawSmoothText(UIF_HEADING, "-", cx + 18 + 48, 364 + 28, textdatum_t::middle_center, pal.text, pal.surface_alt);

            // [+] Button
            _canvas->fillRoundRect(cx + cw - 114, 364, 96, 56, 12, pal.surface_alt);
            _canvas->drawRoundRect(cx + cw - 114, 364, 96, 56, 12, pal.border);
            drawSmoothText(UIF_HEADING, "+", cx + cw - 114 + 48, 364 + 28, textdatum_t::middle_center, pal.text, pal.surface_alt);
        };

        drawSplitCol(0, "SPLIT 1", Settings.get().race_split1_kmh, pal.accent_ok);
        drawSplitCol(1, "SPLIT 2", Settings.get().race_split2_kmh, pal.primary);
        drawSplitCol(2, "SPLIT 3", Settings.get().race_split3_kmh, pal.accent_danger);

        drawSmoothText(UIF_SMALL, "Launch-Control Split-Ziele  •  Speichert automatisch bei FERTIG",
                       400, 462, textdatum_t::middle_center, pal.muted, pal.bg);
        return;
    }

    // ------------------------------------------------------------------
    // TAB 4: DASH (Theme, Cockpit Badges, Brake Mode, Turbine Sound)
    // ------------------------------------------------------------------
    if (_settings_tab_idx == TAB_DASH) {
        // Card 1: COLOR THEME SELECTOR (y: 118..194, h: 76)
        drawCard(20, 118, 760, 76, pal.surface, pal.border, 12);
        drawSmoothText(UIF_HEADING, "FARBSCHEMA", 38, 130, textdatum_t::top_left, pal.text, pal.surface);
        drawSmoothText(UIF_BODY, pal.subtitle, 38, 160, textdatum_t::top_left, pal.muted, pal.surface);

        // [<] button (x: 472..520)
        _canvas->fillRoundRect(472, 132, 48, 48, 12, pal.surface_alt);
        _canvas->drawRoundRect(472, 132, 48, 48, 12, pal.border);
        drawSmoothText(UIF_HEADING, "<", 496, 156, textdatum_t::middle_center, pal.text, pal.surface_alt);

        // Theme preview pill (x: 528..708, w=180)
        _canvas->fillRoundRect(528, 132, 180, 48, 12, pal.surface_alt);
        _canvas->drawRoundRect(528, 132, 180, 48, 12, pal.primary);
        _canvas->fillCircle(546, 156, 6, pal.primary);
        drawSmoothText(UIF_BODY, pal.name, 625, 156, textdatum_t::middle_center, pal.text, pal.surface_alt);

        // [>] button (x: 716..764)
        _canvas->fillRoundRect(716, 132, 48, 48, 12, pal.surface_alt);
        _canvas->drawRoundRect(716, 132, 48, 48, 12, pal.border);
        drawSmoothText(UIF_HEADING, ">", 740, 156, textdatum_t::middle_center, pal.text, pal.surface_alt);

        // Card 2: COCKPIT BADGES (3 sleek inline pill toggles, y: 202..278, h: 76)
        drawCard(20, 202, 760, 76, pal.surface, pal.border, 12);
        _canvas->drawFastVLine(273, 216, 48, pal.border);
        _canvas->drawFastVLine(526, 216, 48, pal.border);

        // G-Force toggle
        drawSmoothText(UIF_BODY, "G-Force", 38, 224, textdatum_t::top_left, pal.text, pal.surface);
        drawSmoothText(UIF_SMALL, "Badge", 38, 246, textdatum_t::top_left, pal.muted, pal.surface);
        drawPillToggle(198, 225, Settings.get().show_gforce != 0, pal);

        // Incline toggle
        drawSmoothText(UIF_BODY, "Neigung", 292, 224, textdatum_t::top_left, pal.text, pal.surface);
        drawSmoothText(UIF_SMALL, "Horizont", 292, 246, textdatum_t::top_left, pal.muted, pal.surface);
        drawPillToggle(452, 225, Settings.get().show_incline != 0, pal);

        // Light toggle
        drawSmoothText(UIF_BODY, "Licht", 546, 224, textdatum_t::top_left, pal.text, pal.surface);
        drawSmoothText(UIF_SMALL, "Schalter", 546, 246, textdatum_t::top_left, pal.muted, pal.surface);
        drawPillToggle(706, 225, Settings.get().show_light_btn != 0, pal);

        // Card 3: MOTORBREMSE (3-way segmented control, y: 286..362, h: 76)
        drawCard(20, 286, 760, 76, pal.surface, pal.border, 12);
        drawSmoothText(UIF_HEADING, "MOTORBREMSE", 38, 298, textdatum_t::top_left, pal.text, pal.surface);
        drawSmoothText(UIF_SMALL, "Elektronische Rekuperation beim Bremsen", 38, 328, textdatum_t::top_left, pal.muted, pal.surface);

        uint8_t bm = Settings.get().brake_mode;
        const char *bmLabels[3] = { "AUS", "MILD", "MAX" };
        for (int b = 0; b < 3; b++) {
            int bx = 466 + b * 102;
            bool sel = (bm == b);
            uint16_t bBg = sel ? pal.primary : pal.surface_alt;
            uint16_t bBd = sel ? pal.primary : pal.border;
            uint16_t bFg = sel ? onPrimaryFg(pal) : pal.muted;
            _canvas->fillRoundRect(bx, 300, 94, 48, 12, bBg);
            _canvas->drawRoundRect(bx, 300, 94, 48, 12, bBd);
            drawSmoothText(UIF_BODY, bmLabels[b], bx + 47, 324, textdatum_t::middle_center, bFg, bBg);
        }

        // Card 4: FOC TURBINE SOUND (y: 370..446, h: 76)
        drawCard(20, 370, 760, 76, pal.surface, pal.border, 12);
        drawSmoothText(UIF_HEADING, "FOC TURBINE SOUND", 38, 382, textdatum_t::top_left, pal.text, pal.surface);
        drawSmoothText(UIF_SMALL, "Synthetischer E-Antriebs-Sound über Motorphasen beim Fahren", 38, 412, textdatum_t::top_left, pal.muted, pal.surface);
        drawPillToggle(706, 393, Settings.get().turbine_sound_enabled, pal);

        drawSmoothText(UIF_SMALL, "Design & Cockpit-Anzeigen  •  Nachtmodus dimmt Display per Software",
                       400, 462, textdatum_t::middle_center, pal.muted, pal.bg);
        return;
    }

    // ------------------------------------------------------------------
    // TAB 5: SYSTEM (TCS Toggle, Battery Cells 10S-24S, Tire Calib, Tools)
    // ------------------------------------------------------------------
    if (_settings_tab_idx == TAB_SYSTEM) {
        // Card 1: TCS (Traktionskontrolle) (y: 118..194, h: 76)
        drawCard(20, 118, 760, 76, pal.surface, pal.border, 12);
        drawSmoothText(UIF_HEADING, "TRAKTIONSKONTROLLE (TCS)", 38, 130, textdatum_t::top_left, pal.text, pal.surface);
        drawSmoothText(UIF_SMALL, "Regelt Phasenstrom blitzschnell bei Radschlupf (Nässe / Schotter)", 38, 160, textdatum_t::top_left, pal.muted, pal.surface);
        drawPillToggle(706, 141, Settings.get().tcs_enabled, pal);

        // Card 2: AKKU-ZELLEN (10S - 24S) (y: 202..278, h: 76)
        drawCard(20, 202, 760, 76, pal.surface, pal.border, 12);
        uint8_t cells = Settings.get().battery_cells;
        if (cells < 10) cells = 10;
        if (cells > 24) cells = 24;
        drawSmoothText(UIF_HEADING, "AKKU-ZELLEN (SERIE)", 38, 214, textdatum_t::top_left, pal.text, pal.surface);
        char cellSub[64];
        snprintf(cellSub, sizeof(cellSub), "Cutoff Start %.1f V  •  End %.1f V  •  Voll %.1f V",
                 cells * 3.3f, cells * 3.0f, cells * 4.2f);
        drawSmoothText(UIF_SMALL, cellSub, 38, 244, textdatum_t::top_left, pal.muted, pal.surface);

        // [-] button (x: 546..598)
        _canvas->fillRoundRect(546, 216, 52, 48, 12, pal.surface_alt);
        _canvas->drawRoundRect(546, 216, 52, 48, 12, pal.border);
        drawSmoothText(UIF_HEADING, "-", 572, 240, textdatum_t::middle_center, pal.text, pal.surface_alt);

        // Value chip "13S" (x: 606..704)
        char cellStr[12];
        snprintf(cellStr, sizeof(cellStr), "%uS", (unsigned)cells);
        _canvas->fillRoundRect(606, 216, 98, 48, 12, pal.surface_alt);
        _canvas->drawRoundRect(606, 216, 98, 48, 12, pal.primary);
        drawSmoothText(UIF_HEADING, cellStr, 655, 240, textdatum_t::middle_center, pal.primary, pal.surface_alt);

        // [+] button (x: 712..764)
        _canvas->fillRoundRect(712, 216, 52, 48, 12, pal.surface_alt);
        _canvas->drawRoundRect(712, 216, 52, 48, 12, pal.border);
        drawSmoothText(UIF_HEADING, "+", 738, 240, textdatum_t::middle_center, pal.text, pal.surface_alt);

        // Card 3: REIFEN-KALIBRIERUNG & VIRTUAL TPMS (y: 286..362, h: 76)
        drawCard(20, 286, 760, 76, pal.surface, pal.border, 12);
        bool calib_active = TcsTpmsManager::instance().isCalibrating();
        float cal_diam = Settings.get().wheel_diameter_in;
        drawSmoothText(UIF_HEADING, "REIFEN-KALIBRIERUNG", 38, 298, textdatum_t::top_left, pal.text, pal.surface);
        char calSub[80];
        if (calib_active) {
            snprintf(calSub, sizeof(calSub), "Kalibrierung aktiv — bitte geradeaus >15 km/h mit GPS fahren...");
        } else {
            snprintf(calSub, sizeof(calSub), "Durchmesser: %.1f Zoll  •  %s",
                     cal_diam, Settings.get().wheel_gps_calibrated ? "GPS kalibriert" : "Werkswert");
        }
        drawSmoothText(UIF_SMALL, calSub, 38, 328, textdatum_t::top_left,
                       calib_active ? pal.accent_warn : pal.muted, pal.surface);

        uint16_t calBg = calib_active ? pal.accent_warn : pal.surface_alt;
        uint16_t calBd = calib_active ? pal.accent_warn : pal.primary;
        uint16_t calFg = calib_active ? onPrimaryFg(pal) : pal.primary;
        _canvas->fillRoundRect(546, 300, 218, 48, 12, calBg);
        _canvas->drawRoundRect(546, 300, 218, 48, 12, calBd);
        drawSmoothText(UIF_BODY, calib_active ? "AKTIV..." : "KALIBRIEREN", 655, 324,
                       textdatum_t::middle_center, calFg, calBg);

        // Card 4: WERKZEUGE (WiFi Bridge & Setup Wizard) (y: 370..446, h: 76)
        drawCard(20, 370, 760, 76, pal.surface, pal.border, 12);
        _canvas->fillRoundRect(36, 384, 352, 48, 12, pal.surface_alt);
        _canvas->drawRoundRect(36, 384, 352, 48, 12, pal.primary);
        drawSmoothText(UIF_BODY, "WIFI VESC-TOOL BRIDGE", 212, 408, textdatum_t::middle_center, pal.primary, pal.surface_alt);

        _canvas->fillRoundRect(412, 384, 352, 48, 12, pal.surface_alt);
        _canvas->drawRoundRect(412, 384, 352, 48, 12, pal.border);
        drawSmoothText(UIF_BODY, "SETUP-ASSISTENT STARTEN", 588, 408, textdatum_t::middle_center, pal.text, pal.surface_alt);

        drawSmoothText(UIF_SMALL, "System, Traktion & Akku-Schutz  •  Speichert im NVS Flash",
                       400, 462, textdatum_t::middle_center, pal.muted, pal.bg);
        return;
    }

    // ------------------------------------------------------------------
    // TAB 6: PIN & SICHERHEIT (with [ JETZT SPERREN ] button!)
    // ------------------------------------------------------------------
    if (_settings_tab_idx == TAB_PIN) {
        // Card 1: PIN-WEGFAHRSPERRE & INSTANT LOCK (y: 120..268, h: 148)
        drawCard(20, 120, 760, 148, pal.surface, pal.border, 12);
        drawSmoothText(UIF_HEADING, "PIN-WEGFAHRSPERRE", 38, 134, textdatum_t::top_left, pal.text, pal.surface);
        drawSmoothText(UIF_SMALL, "Elektronische 0A Motor-Sperre beim Einschalten", 38, 164, textdatum_t::top_left, pal.muted, pal.surface);
        drawPillToggle(706, 142, Settings.get().pin_lock_enabled, pal);

        // Bottom row buttons inside Card 1 (y: 200..252, h: 52)
        _canvas->fillRoundRect(38, 200, 344, 52, 12, pal.surface_alt);
        _canvas->drawRoundRect(38, 200, 344, 52, 12, pal.primary);
        drawSmoothText(UIF_HEADING, "PIN ÄNDERN", 210, 226, textdatum_t::middle_center, pal.primary, pal.surface_alt);

        bool can_lock = (telemetry.speed_kmh < 0.8f && telemetry.vesc_motor_speed < 0.8f);
        uint16_t lockBg = can_lock ? blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 24 : 44) : pal.surface_alt;
        uint16_t lockBd = can_lock ? pal.accent_danger : pal.border;
        uint16_t lockFg = can_lock ? pal.accent_danger : pal.muted;
        _canvas->fillRoundRect(418, 200, 344, 52, 12, lockBg);
        _canvas->drawRoundRect(418, 200, 344, 52, 12, lockBd);
        drawSmoothText(UIF_HEADING, "JETZT SPERREN", 590, 226, textdatum_t::middle_center, lockFg, lockBg);

        // Card 2: SICHERHEITSHINWEISE (y: 280..444, h: 164)
        drawCard(20, 280, 760, 164, pal.surface, pal.border, 12);
        drawSmoothText(UIF_HEADING, "STATUS & NOTFALL-RESET", 38, 294, textdatum_t::top_left, pal.primary, pal.surface);
        drawSmoothText(UIF_BODY, "• Wegfahrsperre blockiert den VESC-Motorstrom zuverlässig auf 0 A.", 38, 328, textdatum_t::top_left, pal.text, pal.surface);
        drawSmoothText(UIF_BODY, "• Bei Bewegung > 2 m im gesperrten Zustand ertönt die FOC-Sirene.", 38, 356, textdatum_t::top_left, pal.text, pal.surface);
        drawSmoothText(UIF_BODY, "• Entsperrung per 4-stelligem PIN oder aktiver VESC-Tool Verbindung.", 38, 384, textdatum_t::top_left, pal.muted, pal.surface);
        drawSmoothText(UIF_BODY, "• Notfall-Reset auf 1234: 5s auf Sperrbildschirm halten oder USB 'r'.", 38, 412, textdatum_t::top_left, pal.muted, pal.surface);

        drawSmoothText(UIF_SMALL, "100% autark im NVS Flash gesichert  •  Zurück mit < DASH",
                       400, 462, textdatum_t::middle_center, pal.muted, pal.bg);

        if (_pin_modal_open) {
            drawPinChangeModal();
        }
        return;
    }
}

void DashboardRenderer::drawPinChangeModal() {
    const ColorThemePalette &pal = currentTheme();
    int mx = 160, my = 32, mw = 480, mh = 416;
    drawCard(mx, my, mw, mh, pal.surface, pal.primary, 16);
    _canvas->drawRoundRect(mx + 1, my + 1, mw - 2, mh - 2, 15, pal.primary);

    drawSmoothText(UIF_HEADING, "NEUEN PIN FESTLEGEN", mx + 22, my + 18, textdatum_t::top_left, pal.text, pal.surface);

    // Top-Right [ X ] Close Button
    _canvas->fillRoundRect(mx + mw - 48, my + 14, 34, 32, 8, pal.surface_alt);
    _canvas->drawRoundRect(mx + mw - 48, my + 14, 34, 32, 8, pal.border);
    drawSmoothText(UIF_BODY, "X", mx + mw - 31, my + 30, textdatum_t::middle_center, pal.text, pal.surface_alt);

    if (_pin_modal_success) {
        drawSmoothText(UIF_HEADING, "PIN GESPEICHERT!", mx + mw / 2, my + 140,
                       textdatum_t::middle_center, pal.accent_ok, pal.surface);
        char savedBuf[32];
        snprintf(savedBuf, sizeof(savedBuf), "PIN: %s", Settings.get().pin_code);
        drawSmoothText(UIF_MED_NUM, savedBuf, mx + mw / 2, my + 205,
                       textdatum_t::middle_center, pal.text, pal.surface);
        drawSmoothText(UIF_BODY, "Sicher im NVS Flash abgelegt", mx + mw / 2, my + 266,
                       textdatum_t::middle_center, pal.muted, pal.surface);
        return;
    }

    drawSmoothText(UIF_BODY, "Gib 4 neue Ziffern ein:", mx + mw / 2, my + 56,
                   textdatum_t::top_center, pal.muted, pal.surface);

    int entered_len = (int)strlen(_pin_modal_buf);
    int dot_xs[4] = { mx + mw / 2 - 60, mx + mw / 2 - 20, mx + mw / 2 + 20, mx + mw / 2 + 60 };
    int dot_y = my + 94;
    for (int i = 0; i < 4; i++) {
        if (i < entered_len) {
            _canvas->fillCircle(dot_xs[i], dot_y, 10, pal.primary);
        } else {
            _canvas->fillCircle(dot_xs[i], dot_y, 10, pal.surface_alt);
            _canvas->drawCircle(dot_xs[i], dot_y, 10, pal.border);
        }
    }

    static const char *KEYS[4][3] = {
        { "1", "2", "3" },
        { "4", "5", "6" },
        { "7", "8", "9" },
        { "DEL", "0", "OK" }
    };
    int kw = 100, kh = 48;
    int kx0 = mx + (mw - (3 * kw + 2 * 14)) / 2;
    int ky0 = my + 126;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            int bx = kx0 + c * (kw + 14);
            int by = ky0 + r * (kh + 10);
            bool isDel = (r == 3 && c == 0);
            bool isOk  = (r == 3 && c == 2);
            uint16_t bg = pal.surface_alt, bd = pal.border, txt = pal.text;
            if (isDel) { bg = blendRgb565(pal.surface_alt, pal.accent_danger, 28); bd = pal.accent_danger; txt = pal.accent_danger; }
            else if (isOk) { bg = blendRgb565(pal.surface_alt, pal.accent_ok, 28); bd = pal.accent_ok; txt = pal.accent_ok; }
            drawCard(bx, by, kw, kh, bg, bd, 10);
            drawSmoothText((isDel || isOk) ? UIF_BODY : UIF_HEADING, KEYS[r][c], bx + kw / 2, by + kh / 2,
                           textdatum_t::middle_center, txt, bg);
        }
    }
}

void DashboardRenderer::handleMenuNav(NavAction action, DashTelemetry &telemetry) {
    if (action == NAV_NONE) return;

    if (action == NAV_SCREEN_DASH || action == NAV_MENU_BACK) {
        // Auto-save cleanly on exit (FIXED: NO longer forces ECO max_battery_amps >= 60 or max_phase_amps >= 120!)
        VehicleModeConfig &cfg = ModeManager::instance().getConfig();
        for (int i = 0; i < 3; i++) {
            if (cfg.tuned_modes[i].max_pull_watts < cfg.tuned_modes[i].max_power_watts) {
                cfg.tuned_modes[i].max_pull_watts = cfg.tuned_modes[i].max_power_watts * 1.25f;
            }
        }
        ModeManager::instance().saveToNvs();
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

    if (action == NAV_BOTH_PRESSED) {
        if (telemetry.tuning_mode) {
            _settings_tab_idx = (_settings_tab_idx + 1) % TAB_COUNT;
        } else {
            if (_settings_tab_idx == TAB_DASH) _settings_tab_idx = TAB_SYSTEM;
            else if (_settings_tab_idx == TAB_SYSTEM) _settings_tab_idx = TAB_PIN;
            else _settings_tab_idx = TAB_DASH;
        }
        _last_rendered_tab = 255;
        _cache.invalidate();
        _menu_dirty = true;
        return;
    }

    if (!telemetry.tuning_mode || _settings_tab_idx != TAB_SPEED) {
        return;
    }

    uint8_t mIdx = (uint8_t)telemetry.ride_mode;
    if (mIdx >= 3) mIdx = 1;
    VehicleModeConfig &cfg = ModeManager::instance().getConfig();
    ModeLimits &lim = cfg.tuned_modes[mIdx];

    if (action == NAV_EDIT_INC || action == NAV_BTN2_SHORT) {
        lim.max_speed_kmh += 1.0f;
        if (lim.max_speed_kmh > 100.0f) lim.max_speed_kmh = 100.0f;
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
        int mx = 160, my = 32, mw = 480, mh = 416;
        if ((tx >= mx + mw - 55 && tx <= mx + mw && ty >= my && ty <= my + 52) ||
            _pin_modal_success ||
            (tx < mx || tx > mx + mw || ty < my || ty > my + mh)) {
            _pin_modal_open = false;
            _settings_inited = false;
            _last_rendered_tab = 255;
            _cache.invalidate();
            _menu_dirty = true;
            return true;
        }

        int kw = 100, kh = 48;
        int kx0 = mx + (mw - (3 * kw + 2 * 14)) / 2;
        int ky0 = my + 126;

        int c = -1;
        for (int col = 0; col < 3; col++) {
            int bx = kx0 + col * (kw + 14);
            if (tx >= bx && tx <= bx + kw) { c = col; break; }
        }
        int r = -1;
        for (int row = 0; row < 4; row++) {
            int by = ky0 + row * (kh + 10);
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
            int len = (int)strlen(_pin_modal_buf);
            if (key == '<') {
                if (len > 0) {
                    _pin_modal_buf[len - 1] = '\0';
                    _menu_dirty = true;
                }
            } else if (key == '#') {
                if (len == 4) {
                    strncpy(Settings.get().pin_code, _pin_modal_buf, sizeof(Settings.get().pin_code) - 1);
                    Settings.get().pin_code[sizeof(Settings.get().pin_code) - 1] = '\0';
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
                        strncpy(Settings.get().pin_code, _pin_modal_buf, sizeof(Settings.get().pin_code) - 1);
                        Settings.get().pin_code[sizeof(Settings.get().pin_code) - 1] = '\0';
                        Settings.save();
                        _pin_modal_success = true;
                        _pin_modal_success_ms = millis() + 1400;
                    }
                }
            }
        }
        return true;
    }

    // 1. TOP BAR (y <= 52): Return to Dash & Auto-Save
    if (ty <= 52) {
        if (tx <= 170 || tx >= 630) {
            handleMenuNav(NAV_SCREEN_DASH, telemetry);
            return true;
        }
        return false;
    }

    // 2. TAB BAR (y: 54..110)
    if (ty >= 54 && ty <= 110) {
        uint8_t target_tab = 255;
        if (telemetry.tuning_mode) {
            // 5 tabs across x=20..780 (pitch 154)
            if (tx >= 12 && tx <= 788) {
                int idx = (tx - 15) / 154;
                if (idx < 0) idx = 0;
                if (idx >= TAB_COUNT) idx = TAB_COUNT - 1;
                target_tab = (uint8_t)idx;
            }
        } else {
            // 3 legal tabs: DASH (20..264), SYSTEM (278..522), PIN (536..780)
            if (tx >= 12 && tx < 271)       target_tab = TAB_DASH;
            else if (tx >= 271 && tx < 529) target_tab = TAB_SYSTEM;
            else if (tx >= 529 && tx <= 788) target_tab = TAB_PIN;
        }

        if (target_tab < TAB_COUNT) {
            if (_settings_tab_idx != target_tab) {
                _settings_tab_idx = target_tab;
                _last_rendered_tab = 255;
                _cache.invalidate();
                _menu_dirty = true;
            }
            return true;
        }
    }

    // Enforce Stealth Guard
    if (!telemetry.tuning_mode && _settings_tab_idx < TAB_DASH) {
        _settings_tab_idx = TAB_DASH;
        _last_rendered_tab = 255;
        _menu_dirty = true;
        return true;
    }

    // ------------------------------------------------------------------
    // TAB 3: SPEED — TUNED TOPSPEED PER MODE (3 STACKED ROWS)
    // ------------------------------------------------------------------
    if (_settings_tab_idx == TAB_SPEED) {
        VehicleModeConfig &cfg = ModeManager::instance().getConfig();
        for (int m = 0; m < 3; m++) {
            int ry = 118 + m * 112;
            if (ty >= ry && ty <= ry + 104) {
                ModeLimits &lim = cfg.tuned_modes[m];
                if (tx >= 30 && tx <= 110) {
                    lim.max_speed_kmh -= 1.0f;
                    if (lim.max_speed_kmh < 15.0f) lim.max_speed_kmh = 15.0f;
                    _menu_dirty = true;
                    return true;
                }
                if (tx >= 690 && tx <= 770) {
                    lim.max_speed_kmh += 1.0f;
                    if (lim.max_speed_kmh > 100.0f) lim.max_speed_kmh = 100.0f;
                    _menu_dirty = true;
                    return true;
                }
                if (tx >= 114 && tx <= 686) {
                    float frac = (float)(tx - 120) / 560.0f;
                    frac = std::min(std::max(frac, 0.0f), 1.0f);
                    lim.max_speed_kmh = roundf(15.0f + frac * (100.0f - 15.0f));
                    _menu_dirty = true;
                    return true;
                }
            }
        }
        return false;
    }

    // ------------------------------------------------------------------
    // TAB 4: DRAGY SPLITS
    // ------------------------------------------------------------------
    if (_settings_tab_idx == TAB_DRAGY) {
        if (ty >= 350 && ty <= 435) {
            // Split 1 (cx=20, cw=244): [-] 38..134, [+] 150..246
            if (tx >= 25 && tx <= 140) {
                if (Settings.get().race_split1_kmh > 10) {
                    Settings.get().race_split1_kmh--;
                    _menu_dirty = true;
                }
                return true;
            } else if (tx >= 142 && tx <= 260) {
                if (Settings.get().race_split1_kmh < Settings.get().race_split2_kmh - 1) {
                    Settings.get().race_split1_kmh++;
                    _menu_dirty = true;
                }
                return true;
            }
            // Split 2 (cx=278, cw=244): [-] 296..392, [+] 408..504
            else if (tx >= 282 && tx <= 398) {
                if (Settings.get().race_split2_kmh > Settings.get().race_split1_kmh + 1) {
                    Settings.get().race_split2_kmh--;
                    _menu_dirty = true;
                }
                return true;
            } else if (tx >= 400 && tx <= 518) {
                if (Settings.get().race_split2_kmh < Settings.get().race_split3_kmh - 1) {
                    Settings.get().race_split2_kmh++;
                    _menu_dirty = true;
                }
                return true;
            }
            // Split 3 (cx=536, cw=244): [-] 554..650, [+] 666..762
            else if (tx >= 540 && tx <= 656) {
                if (Settings.get().race_split3_kmh > Settings.get().race_split2_kmh + 1) {
                    Settings.get().race_split3_kmh--;
                    _menu_dirty = true;
                }
                return true;
            } else if (tx >= 658 && tx <= 776) {
                if (Settings.get().race_split3_kmh < 130) {
                    Settings.get().race_split3_kmh++;
                    _menu_dirty = true;
                }
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------
    // TAB 4: DASH (Theme, Cockpit Badges, Brake Mode, Turbine Sound)
    // ------------------------------------------------------------------
    if (_settings_tab_idx == TAB_DASH) {
        // Card 1: Theme Selector (y: 118..194)
        if (ty >= 118 && ty <= 194) {
            if (tx >= 464 && tx <= 524) {
                uint8_t th = Settings.get().color_theme;
                Settings.get().color_theme = (th == 0) ? (THEME_COUNT - 1) : (th - 1);
                telemetry.color_theme = Settings.get().color_theme;
                Settings.save();
                invalidateCache();
                _settings_inited = false;
                _last_rendered_tab = 255;
                _menu_dirty = true;
                return true;
            }
            if (tx >= 525 && tx <= 776) {
                Settings.get().color_theme = (Settings.get().color_theme + 1) % THEME_COUNT;
                telemetry.color_theme = Settings.get().color_theme;
                Settings.save();
                invalidateCache();
                _settings_inited = false;
                _last_rendered_tab = 255;
                _menu_dirty = true;
                return true;
            }
        }

        // Card 2: Cockpit Badges (y: 202..278)
        if (ty >= 202 && ty <= 278) {
            if (tx >= 20 && tx < 273) {
                Settings.get().show_gforce = Settings.get().show_gforce ? 0 : 1;
                Settings.save();
                _menu_dirty = true;
                return true;
            }
            if (tx >= 273 && tx < 526) {
                Settings.get().show_incline = Settings.get().show_incline ? 0 : 1;
                Settings.save();
                _menu_dirty = true;
                return true;
            }
            if (tx >= 526 && tx <= 780) {
                Settings.get().show_light_btn = Settings.get().show_light_btn ? 0 : 1;
                Settings.save();
                _menu_dirty = true;
                return true;
            }
        }

        // Card 3: Motorbremse 3-way segmented control (y: 286..362)
        if (ty >= 286 && ty <= 362) {
            if (tx >= 460 && tx < 564) {
                Settings.get().brake_mode = 0;
                Settings.save();
                _menu_dirty = true;
                return true;
            }
            if (tx >= 564 && tx < 666) {
                Settings.get().brake_mode = 1;
                Settings.save();
                _menu_dirty = true;
                return true;
            }
            if (tx >= 666 && tx <= 780) {
                Settings.get().brake_mode = 2;
                Settings.save();
                _menu_dirty = true;
                return true;
            }
        }

        // Card 4: FOC Turbine Sound Pill Toggle (y: 370..446)
        if (ty >= 370 && ty <= 446 && tx >= 580 && tx <= 780) {
            Settings.get().turbine_sound_enabled = !Settings.get().turbine_sound_enabled;
            Settings.save();
            _menu_dirty = true;
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------
    // TAB 5: SYSTEM (TCS, Battery Cells, Tire Calib, Bridge & Wizard)
    // ------------------------------------------------------------------
    if (_settings_tab_idx == TAB_SYSTEM) {
        // Card 1: TCS Pill Toggle (y: 118..194)
        if (ty >= 118 && ty <= 194 && tx >= 580 && tx <= 780) {
            Settings.get().tcs_enabled = !Settings.get().tcs_enabled;
            TcsTpmsManager::instance().setTcsEnabled(Settings.get().tcs_enabled);
            Settings.save();
            if (!Settings.get().tcs_enabled) {
                ModeManager::instance().applyCurrentMode();
            }
            _menu_dirty = true;
            return true;
        }

        // Card 2: Battery Cells 10S..24S (y: 202..278)
        if (ty >= 202 && ty <= 278) {
            if (tx >= 538 && tx <= 604) { // [-]
                uint8_t c = Settings.get().battery_cells;
                if (c > 10) {
                    c--;
                    Settings.get().battery_cells = c;
                    ModeManager::instance().setBatteryCells(c);
                    Settings.save();
                    _menu_dirty = true;
                }
                return true;
            }
            if (tx >= 706 && tx <= 776) { // [+]
                uint8_t c = Settings.get().battery_cells;
                if (c < 24) {
                    c++;
                    Settings.get().battery_cells = c;
                    ModeManager::instance().setBatteryCells(c);
                    Settings.save();
                    _menu_dirty = true;
                }
                return true;
            }
        }

        // Card 3: Tire Calibration Button (y: 286..362, x: 540..776)
        if (ty >= 286 && ty <= 362 && tx >= 540 && tx <= 776) {
            TcsTpmsManager::instance().startManualCalibration();
            _menu_dirty = true;
            return true;
        }

        // Card 4: WiFi Bridge & Setup Wizard (y: 370..446)
        if (ty >= 370 && ty <= 446) {
            if (tx >= 28 && tx <= 394) {
                Vesc.enterWifiBridgeMode();
                telemetry.screen = SCREEN_WIFI_BRIDGE;
                _settings_inited = false;
                _screen_dirty = true;
                return true;
            }
            if (tx >= 406 && tx <= 772) {
                resetSetupWizard();
                telemetry.wizard_step = 0;
                telemetry.wizard_pin_buf[0] = '\0';
                telemetry.screen = SCREEN_SETUP_WIZARD;
                _settings_inited = false;
                _screen_dirty = true;
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------
    // TAB 6: PIN & SICHERHEIT
    // ------------------------------------------------------------------
    if (_settings_tab_idx == TAB_PIN) {
        // Card 1 Top: PIN Lock Pill Toggle (y: 120..192, x: 580..780)
        if (ty >= 120 && ty <= 192 && tx >= 580 && tx <= 780) {
            Settings.get().pin_lock_enabled = !Settings.get().pin_lock_enabled;
            Settings.save();
            _menu_dirty = true;
            return true;
        }
        // Card 1 Bottom Left: [ PIN ÄNDERN ] (x: 34..386, y: 196..258)
        if (ty >= 196 && ty <= 258 && tx >= 34 && tx <= 386) {
            _pin_modal_open = true;
            _pin_modal_buf[0] = '\0';
            _pin_modal_success = false;
            _menu_dirty = true;
            return true;
        }
        // Card 1 Bottom Right: [ JETZT SPERREN ] (x: 414..766, y: 196..258)
        if (ty >= 196 && ty <= 258 && tx >= 414 && tx <= 766) {
            if (telemetry.speed_kmh < 0.8f && telemetry.vesc_motor_speed < 0.8f) {
                Settings.save();
                telemetry.is_locked = true;
                telemetry.pin_entry[0] = '\0';
                telemetry.pin_error = false;
                telemetry.theft_alarm_active = false;
                telemetry.locked_moved_meters = 0.0f;
                ModeManager::instance().setLocked(true);
                Buttons.setLocked(true);
                _settings_tab_idx = 255;
                _settings_inited = false;
                _last_rendered_tab = 255;
                telemetry.screen = SCREEN_RIDE_DASH;
                _pin_screen_drawn = false;
                _screen_dirty = true;
                invalidateCache();
            }
            return true;
        }
        return false;
    }

    return false;
}

bool DashboardRenderer::handleSettingsSliderDrag(int16_t cx, int16_t cy, DashTelemetry &telemetry) {
    if (_pin_modal_open) return false;
    if (!telemetry.tuning_mode) return false;

    if (_settings_tab_idx == TAB_SPEED) {
        VehicleModeConfig &cfg = ModeManager::instance().getConfig();
        for (int m = 0; m < 3; m++) {
            int ry = 118 + m * 112;
            if (cy >= ry + 38 && cy <= ry + 100 && cx >= 114 && cx <= 686) {
                float frac = (float)(cx - 120) / 560.0f;
                frac = std::min(std::max(frac, 0.0f), 1.0f);
                float new_spd = roundf(15.0f + frac * (100.0f - 15.0f));
                if (new_spd != cfg.tuned_modes[m].max_speed_kmh) {
                    cfg.tuned_modes[m].max_speed_kmh = new_spd;
                    _menu_dirty = true;
                    return true;
                }
                return false;
            }
        }
        return false;
    }

    return false;
}

// ==============================================================================
// SCREEN 4: VESC TOOL USB PASSTHROUGH BRIDGE (Theme-aware)
// ==============================================================================
void DashboardRenderer::initVescBridgeScreen(const DashTelemetry &telemetry) {
    (void)telemetry;
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);

    _canvas->fillRoundRect(20, 12, 108, 34, 17, pal.surface);
    _canvas->drawRoundRect(20, 12, 108, 34, 17, pal.border);
    drawSmoothText(UIF_BODY, "< DASH", 74, 29, textdatum_t::middle_center, pal.text, pal.surface);

    drawSmoothText(UIF_HEADING, "VESC TOOL USB BRIDGE", 400, 29, textdatum_t::middle_center, pal.primary, pal.bg);

    _canvas->fillRoundRect(640, 12, 140, 34, 17, pal.surface);
    _canvas->drawRoundRect(640, 12, 140, 34, 17, pal.accent_ok);
    drawSmoothText(UIF_SMALL, "115200 BAUD", 710, 29, textdatum_t::middle_center, pal.accent_ok, pal.surface);

    drawCard(20, 62, 370, 320, pal.surface, pal.border, 12);
    drawSmoothText(UIF_HEADING, "PC / VESC TOOL (USB)", 38, 76, textdatum_t::top_left, pal.primary, pal.surface);
    _canvas->drawFastHLine(36, 108, 338, pal.border);
    drawSmoothText(UIF_BODY, "Port: USB CDC (/dev/ttyACM0)", 38, 122, textdatum_t::top_left, pal.text, pal.surface);
    drawSmoothText(UIF_BODY, "Status: Virtueller COM-Port aktiv", 38, 150, textdatum_t::top_left, pal.muted, pal.surface);
    drawSmoothText(UIF_SMALL, "EMPFANGEN VOM PC", 38, 198, textdatum_t::top_left, pal.muted, pal.surface);

    drawCard(410, 62, 370, 320, pal.surface, pal.border, 12);
    drawSmoothText(UIF_HEADING, "VESC CONTROLLER", 428, 76, textdatum_t::top_left, pal.accent_ok, pal.surface);
    _canvas->drawFastHLine(426, 108, 338, pal.border);
    drawSmoothText(UIF_BODY, "Interface: CAN / UART Bridge", 428, 122, textdatum_t::top_left, pal.text, pal.surface);
    drawSmoothText(UIF_BODY, "Modus: Transparenter Tunnel", 428, 150, textdatum_t::top_left, pal.muted, pal.surface);
    drawSmoothText(UIF_SMALL, "GESENDET AN VESC", 428, 198, textdatum_t::top_left, pal.muted, pal.surface);

    drawCard(20, 396, 760, 68, pal.surface, pal.accent_warn, 12);
    drawSmoothText(UIF_HEADING, "USB-BRIDGE BEENDEN & ZURÜCK", 400, 420, textdatum_t::middle_center, pal.accent_warn, pal.surface);
    drawSmoothText(UIF_SMALL, "Tippen kehrt direkt zum Fahr-Dashboard zurück", 400, 446, textdatum_t::middle_center, pal.muted, pal.surface);

    _bridge_last_pc_bytes = 0xFFFFFFFF;
    _bridge_last_vesc_bytes = 0xFFFFFFFF;
}

void DashboardRenderer::renderVescBridgeScreen(const DashTelemetry &telemetry) {
    (void)telemetry;
    const ColorThemePalette &pal = currentTheme();
    uint32_t pc_bytes = Vesc.getBridgePcToVescBytes();
    uint32_t vesc_bytes = Vesc.getBridgeVescToPcBytes();

    auto fmtBytes = [](uint32_t b, char *out, size_t n) {
        if (b < 1024) snprintf(out, n, "%u B", (unsigned)b);
        else if (b < 1048576) snprintf(out, n, "%.1f KB", b / 1024.0f);
        else snprintf(out, n, "%.2f MB", b / 1048576.0f);
    };

    if (pc_bytes != _bridge_last_pc_bytes) {
        _bridge_last_pc_bytes = pc_bytes;
        char buf[32]; fmtBytes(pc_bytes, buf, sizeof(buf));
        _canvas->fillRect(38, 224, 320, 44, pal.surface);
        drawSmoothText(UIF_MED_NUM, buf, 38, 226, textdatum_t::top_left, pal.text, pal.surface);
    }

    if (vesc_bytes != _bridge_last_vesc_bytes) {
        _bridge_last_vesc_bytes = vesc_bytes;
        char buf[32]; fmtBytes(vesc_bytes, buf, sizeof(buf));
        _canvas->fillRect(428, 224, 320, 44, pal.surface);
        drawSmoothText(UIF_MED_NUM, buf, 428, 226, textdatum_t::top_left, pal.text, pal.surface);
    }
}

// ==============================================================================
// SCREEN 5: VESC TOOL WIRELESS WI-FI BRIDGE (Theme-aware)
// ==============================================================================
void DashboardRenderer::initWifiBridgeScreen(const DashTelemetry &telemetry) {
    (void)telemetry;
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);

    _canvas->fillRoundRect(20, 12, 108, 34, 17, pal.surface);
    _canvas->drawRoundRect(20, 12, 108, 34, 17, pal.border);
    drawSmoothText(UIF_BODY, "< DASH", 74, 29, textdatum_t::middle_center, pal.text, pal.surface);

    drawSmoothText(UIF_HEADING, "VESC TOOL WI-FI BRIDGE", 400, 29, textdatum_t::middle_center, pal.primary, pal.bg);

    _canvas->fillRoundRect(660, 12, 120, 34, 17, pal.surface);
    _canvas->drawRoundRect(660, 12, 120, 34, 17, pal.accent_ok);
    drawSmoothText(UIF_SMALL, "TCP 6510", 720, 29, textdatum_t::middle_center, pal.accent_ok, pal.surface);

    // Left Card: WLAN Access Point Details
    drawCard(20, 62, 370, 320, pal.surface, pal.border, 12);
    _canvas->fillCircle(38, 86, 5, pal.accent_ok);
    drawSmoothText(UIF_HEADING, "WLAN ACCESS POINT", 52, 74, textdatum_t::top_left, pal.primary, pal.surface);
    _canvas->drawFastHLine(36, 106, 338, pal.border);

    int rowY = 118;
    auto drawRow = [&](const char *label, const char *val, uint16_t valCol) {
        drawSmoothText(UIF_BODY, label, 36, rowY, textdatum_t::top_left, pal.muted, pal.surface);
        drawSmoothText(UIF_BODY, val,   374, rowY, textdatum_t::top_right, valCol, pal.surface);
        rowY += 32;
    };

    drawRow("Netzwerk (SSID):", "VESC-DASH-AP", pal.text);
    drawRow("IP-Adresse:", "192.168.4.1", pal.text);
    drawRow("TCP-Port:", "6510 (VESC Tool)", pal.text);
    drawRow("Sicherheit:", "Offen (Kein PIN)", pal.accent_ok);
    drawRow("Frequenz:", "2.4 GHz SoftAP", pal.text);

    drawCard(34, 294, 342, 72, pal.surface_alt, pal.border, 10);
    drawSmoothText(UIF_SMALL, "KABELLOSES OTA & FLASHEN", 46, 304, textdatum_t::top_left, pal.primary, pal.surface_alt);
    drawSmoothText(UIF_BODY,  "http://192.168.4.1/update", 46, 322, textdatum_t::top_left, pal.text, pal.surface_alt);
    drawSmoothText(UIF_SMALL, "VESC-Tool TCP: 6510  •  OTA CLI: 3232", 46, 346, textdatum_t::top_left, pal.muted, pal.surface_alt);

    // Right Card: VESC Tool Client
    drawCard(410, 62, 370, 320, pal.surface, pal.border, 12);
    drawSmoothText(UIF_HEADING, "VESC TOOL CLIENT", 428, 74, textdatum_t::top_left, pal.accent_ok, pal.surface);
    _canvas->drawFastHLine(426, 106, 338, pal.border);

    // Bottom Exit Banner
    drawCard(20, 396, 760, 68, pal.surface, pal.accent_warn, 12);
    drawSmoothText(UIF_HEADING, "WI-FI BEENDEN & ZURÜCK", 400, 420, textdatum_t::middle_center, pal.accent_warn, pal.surface);
    drawSmoothText(UIF_SMALL, "Tippen schaltet WLAN ab und kehrt zum Dashboard zurück", 400, 446, textdatum_t::middle_center, pal.muted, pal.surface);

    _bridge_last_wifi_rx = 0xFFFFFFFF;
    _bridge_last_wifi_tx = 0xFFFFFFFF;
    _bridge_last_wifi_conn = -1;
}

void DashboardRenderer::renderWifiBridgeScreen(const DashTelemetry &telemetry) {
    (void)telemetry;
    const ColorThemePalette &pal = currentTheme();
    bool connected = Vesc.isWifiClientConnected();
    int8_t conn_state = connected ? 1 : 0;
    uint32_t rx_bytes = Vesc.getBridgeWifiToVescBytes();
    uint32_t tx_bytes = Vesc.getBridgeVescToWifiBytes();

    if (conn_state != _bridge_last_wifi_conn) {
        _bridge_last_wifi_conn = conn_state;
        _canvas->fillRect(416, 112, 358, 260, pal.surface);

        if (connected) {
            uint16_t okBg = blendRgb565(pal.surface, pal.accent_ok, pal.is_light ? 26 : 46);
            drawCard(426, 120, 338, 38, okBg, pal.accent_ok, 10);
            drawSmoothText(UIF_BODY, "STATUS: VERBUNDEN", 595, 139, textdatum_t::middle_center, pal.accent_ok, okBg);

            char ipBuf[44];
            snprintf(ipBuf, sizeof(ipBuf), "Client-IP: %s", Vesc.getWifiClientIp());
            drawSmoothText(UIF_BODY, ipBuf, 426, 174, textdatum_t::top_left, pal.text, pal.surface);
            _canvas->drawFastHLine(426, 204, 338, pal.border);

            drawSmoothText(UIF_BODY, "APP -> VESC:", 426, 224, textdatum_t::top_left, pal.muted, pal.surface);
            drawSmoothText(UIF_BODY, "VESC -> APP:", 426, 264, textdatum_t::top_left, pal.muted, pal.surface);

            _bridge_last_wifi_rx = 0xFFFFFFFF;
            _bridge_last_wifi_tx = 0xFFFFFFFF;
        } else {
            uint16_t warnBg = blendRgb565(pal.surface, pal.accent_warn, pal.is_light ? 24 : 42);
            drawCard(426, 120, 338, 38, warnBg, pal.accent_warn, 10);
            drawSmoothText(UIF_BODY, "WARTET AUF VESC TOOL...", 595, 139, textdatum_t::middle_center, pal.accent_warn, warnBg);

            drawSmoothText(UIF_BODY, "1. Mit WLAN 'VESC-DASH-AP' verbinden", 426, 176, textdatum_t::top_left, pal.text, pal.surface);
            drawSmoothText(UIF_BODY, "2. VESC Tool öffnen (PC / Handy)",     426, 208, textdatum_t::top_left, pal.text, pal.surface);
            drawSmoothText(UIF_BODY, "3. Verbindungsart 'TCP' wählen",       426, 240, textdatum_t::top_left, pal.text, pal.surface);
            drawSmoothText(UIF_BODY, "4. Host: 192.168.4.1  •  Port: 6510",  426, 272, textdatum_t::top_left, pal.text, pal.surface);
            drawSmoothText(UIF_BODY, "5. Auf 'Connect' tippen!",             426, 304, textdatum_t::top_left, pal.primary, pal.surface);
        }
    }

    if (connected) {
        auto fmtBytes = [](uint32_t b, char *out, size_t n) {
            if (b < 1024) snprintf(out, n, "%u B", (unsigned)b);
            else if (b < 1048576) snprintf(out, n, "%.1f KB", b / 1024.0f);
            else snprintf(out, n, "%.2f MB", b / 1048576.0f);
        };
        if (rx_bytes != _bridge_last_wifi_rx) {
            _bridge_last_wifi_rx = rx_bytes;
            char buf[32]; fmtBytes(rx_bytes, buf, sizeof(buf));
            _canvas->fillRect(570, 220, 194, 30, pal.surface);
            drawSmoothText(UIF_HEADING, buf, 764, 222, textdatum_t::top_right, pal.text, pal.surface);
        }
        if (tx_bytes != _bridge_last_wifi_tx) {
            _bridge_last_wifi_tx = tx_bytes;
            char buf[32]; fmtBytes(tx_bytes, buf, sizeof(buf));
            _canvas->fillRect(570, 260, 194, 30, pal.surface);
            drawSmoothText(UIF_HEADING, buf, 764, 262, textdatum_t::top_right, pal.text, pal.surface);
        }
    }
}

// ==============================================================================
// SCREEN 6: "RACE" DRAGY / AUTO-ROLLOUT LAUNCH ACCELERATION TIMER (800x480)
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

    // Continuous Thermal Protection (> 65 °C aborts launch cleanly)
    if (isRaceThermalBlocked(telemetry)) {
        if (telemetry.race_state == 1 || telemetry.race_state == 2) {
            telemetry.race_state = 0;
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
        }
    } else if (telemetry.race_state == 2) { // 2 = MEASURING
        telemetry.boost_active = true;
        ModeManager::instance().setBoostActive(true);
        telemetry.boost_end_ms = now + 10000;
        telemetry.race_elapsed_sec = (float)(now - telemetry.race_launch_start_ms) / 1000.0f;

        // Sample Dyno Telemetry every 100ms
        if (now - telemetry.race_dyno.last_sample_ms >= 100) {
            telemetry.race_dyno.last_sample_ms = now;
            if (telemetry.race_dyno.sample_count < RACE_DYNO_SAMPLES) {
                int idx = telemetry.race_dyno.sample_count++;
                telemetry.race_dyno.samples_power[idx] = telemetry.power_watts;
                telemetry.race_dyno.samples_speed[idx] = telemetry.speed_kmh;
                telemetry.race_dyno.samples_g[idx]     = telemetry.g_force;
            }
        }
        if (telemetry.power_watts > telemetry.race_dyno.peak_watts)     telemetry.race_dyno.peak_watts = telemetry.power_watts;
        if (telemetry.g_force     > telemetry.race_dyno.peak_g_force)   telemetry.race_dyno.peak_g_force = telemetry.g_force;
        if (telemetry.speed_kmh   > telemetry.race_dyno.peak_speed_kmh) telemetry.race_dyno.peak_speed_kmh = telemetry.speed_kmh;
        if (telemetry.phase_amps  > telemetry.race_dyno.peak_phase_amps)telemetry.race_dyno.peak_phase_amps = telemetry.phase_amps;

        uint8_t sp1 = Settings.get().race_split1_kmh;
        uint8_t sp2 = Settings.get().race_split2_kmh;
        uint8_t sp3 = Settings.get().race_split3_kmh;

        if (telemetry.race_split_0_22 <= 0.0f && telemetry.speed_kmh >= (float)sp1) {
            telemetry.race_split_0_22 = (telemetry.race_elapsed_sec > 0.01f) ? telemetry.race_elapsed_sec : 0.01f;
            if (telemetry.race_records.best_0_22_sec <= 0.01f || telemetry.race_split_0_22 < telemetry.race_records.best_0_22_sec) {
                telemetry.race_records.best_0_22_sec = telemetry.race_split_0_22;
                Settings.saveRaceRecords(telemetry.race_records);
            }
        }

        if (telemetry.race_split_0_50 <= 0.0f && telemetry.speed_kmh >= (float)sp2) {
            telemetry.race_split_0_50 = (telemetry.race_elapsed_sec > 0.01f) ? telemetry.race_elapsed_sec : 0.01f;
            if (telemetry.race_records.best_0_50_sec <= 0.01f || telemetry.race_split_0_50 < telemetry.race_records.best_0_50_sec) {
                telemetry.race_records.best_0_50_sec = telemetry.race_split_0_50;
                Settings.saveRaceRecords(telemetry.race_records);
            }
        }

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

        if (telemetry.race_dyno.peak_speed_kmh >= 25.0f && telemetry.race_elapsed_sec > 2.0f && telemetry.speed_kmh < 2.0f) {
            telemetry.race_state = 3;
            telemetry.boost_active = false;
            ModeManager::instance().setBoostActive(false);
            telemetry.ride_mode = telemetry.saved_ride_mode;
            ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
            Buttons.setRaceState(3);
            markScreenDirty();
        }

        if (telemetry.race_elapsed_sec >= 15.0f) {
            telemetry.race_state = 3;
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
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);
    _cache.invalidate();
    _race_records_drawn = false;
    _race_dyno_drawn = false;
    _last_race_arc_angle = 135.0f;
    _last_race_speed = -1;
    _last_race_state = 255;
    _last_race_countdown = -1;
    _last_race_elapsed_sec = -1.0f;
    _last_race_split_0_22 = -999.0f;
    _last_race_split_0_50 = -999.0f;
    _last_race_split_0_70 = -999.0f;

    // Top-Left [< DASH]
    _canvas->fillRoundRect(20, 12, 108, 34, 17, pal.surface);
    _canvas->drawRoundRect(20, 12, 108, 34, 17, pal.border);
    drawSmoothText(UIF_BODY, "< DASH", 74, 29, textdatum_t::middle_center, pal.text, pal.surface);

    // Top [ DIAGRAMM ]
    _canvas->fillRoundRect(475, 12, 140, 34, 17, pal.surface);
    _canvas->drawRoundRect(475, 12, 140, 34, 17, pal.primary);
    drawSmoothText(UIF_BODY, "DIAGRAMM", 545, 29, textdatum_t::middle_center, pal.primary, pal.surface);

    // Top-Right [ BESTZEITEN ]
    _canvas->fillRoundRect(630, 12, 150, 34, 17, pal.surface);
    _canvas->drawRoundRect(630, 12, 150, 34, 17, pal.border);
    drawSmoothText(UIF_BODY, "BESTZEITEN", 705, 29, textdatum_t::middle_center, pal.text, pal.surface);

    // Speedometer Center Arc Gauge (270° sweep: 135° to 405°, crisp clearance above Launch button)
    int cx = 310, cy = 224;
    int r_in = 130, r_out = 158;
    float mid_r = 144.0f;
    float deg2rad = 3.14159265f / 180.0f;
    _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, 405.0f, pal.track);
    int s_cap_x = cx + (int)roundf(cosf(135.0f * deg2rad) * mid_r);
    int s_cap_y = cy + (int)roundf(sinf(135.0f * deg2rad) * mid_r);
    int e_cap_x = cx + (int)roundf(cosf(405.0f * deg2rad) * mid_r);
    int e_cap_y = cy + (int)roundf(sinf(405.0f * deg2rad) * mid_r);
    _canvas->fillCircle(s_cap_x, s_cap_y, 14, pal.track);
    _canvas->fillCircle(e_cap_x, e_cap_y, 14, pal.track);

    drawSmoothText(UIF_HEADING, "km/h", cx, cy + 34, textdatum_t::top_center, pal.muted, pal.bg);

    // Right Splits Card
    drawCard(530, 68, 250, 392, pal.surface, pal.border, 14);
    char label1[24], label2[24], label3[24];
    snprintf(label1, sizeof(label1), "0 – %d km/h", Settings.get().race_split1_kmh);
    snprintf(label2, sizeof(label2), "0 – %d km/h", Settings.get().race_split2_kmh);
    snprintf(label3, sizeof(label3), "0 – %d km/h", Settings.get().race_split3_kmh);

    drawSmoothText(UIF_BODY, label1, 550, 96, textdatum_t::top_left, pal.muted, pal.surface);
    _canvas->drawFastHLine(550, 186, 210, pal.border);

    drawSmoothText(UIF_BODY, label2, 550, 206, textdatum_t::top_left, pal.muted, pal.surface);
    _canvas->drawFastHLine(550, 296, 210, pal.border);

    drawSmoothText(UIF_BODY, label3, 550, 316, textdatum_t::top_left, pal.muted, pal.surface);

    _last_race_g_force_x10 = (int)roundf(telemetry.g_force * 10.0f);
    drawGForceBadge(_canvas, getUiFont(UIF_SMALL), telemetry.g_force, 50, 416, 27, pal);
}

void DashboardRenderer::renderRaceTimerScreen(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();

    // 0. Modal Popup Isolation
    if (telemetry.race_popup_open) {
        if (!_race_records_drawn) {
            drawRaceRecordsModal(telemetry);
            _race_records_drawn = true;
        }
        return;
    } else {
        _race_records_drawn = false;
    }

    if (telemetry.race_dyno_open) {
        if (!_race_dyno_drawn) {
            drawRaceDynoModal(telemetry);
            _race_dyno_drawn = true;
        }
        return;
    } else {
        _race_dyno_drawn = false;
    }

    int cx = 310, cy = 224;
    int r_in = 130, r_out = 158;
    float mid_r = 144.0f;
    float deg2rad = 3.14159265f / 180.0f;

    int cur_cd = (telemetry.race_state == 1) ? (5 - (int)telemetry.race_elapsed_sec) : -1;
    if (cur_cd < 1) cur_cd = 1;
    if (cur_cd > 5) cur_cd = 5;

    // Evaluate state changes ONCE BEFORE overwriting caches
    bool state_changed = (telemetry.race_state != _last_race_state);
    bool timer_needs_update = state_changed ||
                              (telemetry.race_state == 1 && cur_cd != _last_race_countdown) ||
                              (telemetry.race_state == 2 && fabsf(telemetry.race_elapsed_sec - _last_race_elapsed_sec) >= 0.04f);

    // 1. Top-Center Timer Display
    if (timer_needs_update) {
        _last_race_elapsed_sec = telemetry.race_elapsed_sec;
        _canvas->fillRect(134, 8, 336, 44, pal.bg);

        if (telemetry.race_state == 2 || telemetry.race_state == 3) {
            char timeBuf[20];
            snprintf(timeBuf, sizeof(timeBuf), "%.2f s", telemetry.race_elapsed_sec);
            drawSmoothText(UIF_MED_NUM, timeBuf, 304, 30, textdatum_t::middle_center,
                           (telemetry.race_state == 3) ? pal.accent_ok : pal.text, pal.bg);
        } else if (telemetry.race_state == 1) {
            char cdTitle[24];
            snprintf(cdTitle, sizeof(cdTitle), "LAUNCH IN %d s", cur_cd);
            drawSmoothText(UIF_HEADING, cdTitle, 304, 30,
                           textdatum_t::middle_center, pal.accent_warn, pal.bg);
        } else {
            drawSmoothText(UIF_HEADING, "LAUNCH CONTROL", 304, 30,
                           textdatum_t::middle_center, pal.muted, pal.bg);
        }
    }

    // 2. Speedometer Arc (Differential Update, 270° sweep: 135° to 405°)
    float spd = telemetry.speed_kmh;
    if (spd < 0.8f) spd = 0.0f;
    float frac = std::min(spd / 100.0f, 1.0f);
    float target_angle = (frac <= 0.005f) ? 135.0f : (135.0f + frac * 270.0f);

    uint16_t arcCol = (telemetry.race_state == 1) ? pal.accent_warn : pal.primary;
    int s_cap_x = cx + (int)roundf(cosf(135.0f * deg2rad) * mid_r);
    int s_cap_y = cy + (int)roundf(sinf(135.0f * deg2rad) * mid_r);

    if (target_angle <= 135.0f && _last_race_arc_angle > 135.0f) {
        _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, _last_race_arc_angle + 1.0f, pal.track);
        _canvas->fillCircle(s_cap_x, s_cap_y, 14, pal.track);
        _last_race_arc_angle = 135.0f;
    } else if (fabsf(target_angle - _last_race_arc_angle) >= 0.8f) {
        if (target_angle > _last_race_arc_angle) {
            if (_last_race_arc_angle <= 135.0f) {
                _canvas->fillCircle(s_cap_x, s_cap_y, 14, arcCol);
            }
            _canvas->fillArc(cx, cy, r_in, r_out, _last_race_arc_angle - 0.5f, target_angle, arcCol);
        } else {
            _canvas->fillArc(cx, cy, r_in, r_out, target_angle, _last_race_arc_angle + 0.5f, pal.track);
            if (target_angle <= 135.0f) {
                _canvas->fillCircle(s_cap_x, s_cap_y, 14, pal.track);
            }
        }
        _last_race_arc_angle = target_angle;
    }

    // Hero Digital Speed inside Ring
    int cur_spd_int = (int)(spd + 0.5f);
    if (cur_spd_int != _last_race_speed) {
        _last_race_speed = cur_spd_int;
        char spdBuf[16];
        snprintf(spdBuf, sizeof(spdBuf), "%d", cur_spd_int);
        _canvas->fillRect(cx - 88, cy - 48, 176, 68, pal.bg);
        drawSmoothText(UIF_HERO_NUM, spdBuf, cx, cy - 14, textdatum_t::middle_center, pal.text, pal.bg);
    }

    // 3. Center Action Button cleanly below the horseshoe ring (ZERO arc overlap!)
    bool btn_needs_update = state_changed || (telemetry.race_state == 1 && cur_cd != _last_race_countdown);
    if (btn_needs_update) {
        const int btnY = 368;
        const int btnH = 48;
        _canvas->fillRect(cx - 116, btnY - 4, 232, btnH + 8, pal.bg);
        if (telemetry.race_state == 0) {
            if (isRaceThermalBlocked(telemetry)) {
                uint16_t warnBg = blendRgb565(pal.surface, pal.accent_warn, pal.is_light ? 25 : 45);
                _canvas->fillRoundRect(cx - 106, btnY, 212, btnH, 24, warnBg);
                _canvas->drawRoundRect(cx - 106, btnY, 212, btnH, 24, pal.accent_warn);
                drawSmoothText(UIF_BODY, "TEMP > 65°C", cx, btnY + btnH / 2, textdatum_t::middle_center, pal.accent_warn, warnBg);
            } else {
                _canvas->fillRoundRect(cx - 106, btnY, 212, btnH, 24, pal.primary);
                drawSmoothText(UIF_BODY, "START LAUNCH", cx, btnY + btnH / 2, textdatum_t::middle_center, onPrimaryFg(pal), pal.primary);
            }
        } else if (telemetry.race_state == 1) { // COUNTDOWN (5s)
            char cdBuf[16];
            if (cur_cd == 1) {
                snprintf(cdBuf, sizeof(cdBuf), "1 ... GO!");
            } else {
                snprintf(cdBuf, sizeof(cdBuf), "%d ...", cur_cd);
            }
            uint16_t armBg = blendRgb565(pal.surface, pal.accent_warn, pal.is_light ? 26 : 48);
            _canvas->fillRoundRect(cx - 106, btnY, 212, btnH, 24, armBg);
            _canvas->drawRoundRect(cx - 106, btnY, 212, btnH, 24, pal.accent_warn);
            drawSmoothText(UIF_BODY, cdBuf, cx, btnY + btnH / 2, textdatum_t::middle_center, pal.accent_warn, armBg);
        } else if (telemetry.race_state == 2) { // MEASURING
            uint16_t runBg = blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 26 : 48);
            _canvas->fillRoundRect(cx - 106, btnY, 212, btnH, 24, runBg);
            _canvas->drawRoundRect(cx - 106, btnY, 212, btnH, 24, pal.accent_danger);
            drawSmoothText(UIF_BODY, "STOP / ABORT", cx, btnY + btnH / 2, textdatum_t::middle_center, pal.accent_danger, runBg);
        } else { // FINISHED
            _canvas->fillRoundRect(cx - 106, btnY, 212, btnH, 24, pal.surface);
            _canvas->drawRoundRect(cx - 106, btnY, 212, btnH, 24, pal.primary);
            drawSmoothText(UIF_BODY, "NEUER LAUNCH", cx, btnY + btnH / 2, textdatum_t::middle_center, pal.primary, pal.surface);
        }
    }

    // Commit state cache AFTER both header and button updated
    _last_race_state = telemetry.race_state;
    _last_race_countdown = (telemetry.race_state == 1) ? cur_cd : -1;

    // 4. Right Splits Values
    bool s1_changed = (telemetry.race_split_0_22 != _last_race_split_0_22);
    bool s2_changed = (telemetry.race_split_0_50 != _last_race_split_0_50);
    bool s3_changed = (telemetry.race_split_0_70 != _last_race_split_0_70);

    bool active_0_22 = (telemetry.race_split_0_22 <= 0.0f);
    bool active_0_50 = (telemetry.race_split_0_22 > 0.0f && telemetry.race_split_0_50 <= 0.0f);
    bool active_0_70 = (telemetry.race_split_0_50 > 0.0f && telemetry.race_split_0_70 <= 0.0f);

    auto drawSplit = [&](int y, float split_val, bool is_active) {
        _canvas->fillRect(548, y, 218, 40, pal.surface);
        char sBuf[24];
        if (split_val > 0.0f) {
            snprintf(sBuf, sizeof(sBuf), "%.2f s", split_val);
            drawSmoothText(UIF_MED_NUM, sBuf, 550, y, textdatum_t::top_left, pal.text, pal.surface);
        } else if (telemetry.race_state == 2 && is_active) {
            snprintf(sBuf, sizeof(sBuf), "%.2f s", telemetry.race_elapsed_sec);
            drawSmoothText(UIF_MED_NUM, sBuf, 550, y, textdatum_t::top_left, pal.accent_warn, pal.surface);
        } else {
            drawSmoothText(UIF_MED_NUM, "--.-- s", 550, y, textdatum_t::top_left, pal.muted, pal.surface);
        }
    };

    if (s1_changed || (telemetry.race_state == 2 && active_0_22 && timer_needs_update)) {
        _last_race_split_0_22 = telemetry.race_split_0_22;
        drawSplit(128, telemetry.race_split_0_22, active_0_22);
    }
    if (s2_changed || (telemetry.race_state == 2 && active_0_50 && timer_needs_update)) {
        _last_race_split_0_50 = telemetry.race_split_0_50;
        drawSplit(238, telemetry.race_split_0_50, active_0_50);
    }
    if (s3_changed || (telemetry.race_state == 2 && active_0_70 && timer_needs_update)) {
        _last_race_split_0_70 = telemetry.race_split_0_70;
        drawSplit(348, telemetry.race_split_0_70, active_0_70);
    }

    int cur_g10 = (int)roundf(telemetry.g_force * 10.0f);
    if (cur_g10 != _last_race_g_force_x10) {
        _last_race_g_force_x10 = cur_g10;
        drawGForceBadge(_canvas, getUiFont(UIF_SMALL), telemetry.g_force, 50, 416, 27, pal);
    }
}

void DashboardRenderer::drawRaceRecordsModal(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();
    int mx = 140, my = 70, mw = 520, mh = 340;
    drawCard(mx, my, mw, mh, pal.surface, pal.accent_warn, 16);
    _canvas->drawRoundRect(mx + 1, my + 1, mw - 2, mh - 2, 15, pal.accent_warn);

    drawSmoothText(UIF_HEADING, "PERSÖNLICHE BESTZEITEN", mx + 24, my + 20, textdatum_t::top_left, pal.accent_warn, pal.surface);

    _canvas->fillRoundRect(mx + mw - 52, my + 14, 38, 34, 8, pal.surface_alt);
    _canvas->drawRoundRect(mx + mw - 52, my + 14, 38, 34, 8, pal.border);
    drawSmoothText(UIF_BODY, "X", mx + mw - 33, my + 31, textdatum_t::middle_center, pal.text, pal.surface_alt);

    auto drawRecordRow = [&](int y, const char *label, float best_sec) {
        drawSmoothText(UIF_HEADING, label, mx + 28, y, textdatum_t::top_left, pal.text, pal.surface);
        char valBuf[32];
        if (best_sec > 0.01f) {
            snprintf(valBuf, sizeof(valBuf), "%.2f s", best_sec);
            drawSmoothText(UIF_HEADING, valBuf, mx + mw - 28, y, textdatum_t::top_right, pal.accent_ok, pal.surface);
        } else {
            drawSmoothText(UIF_BODY, "Keine Daten", mx + mw - 28, y + 4, textdatum_t::top_right, pal.muted, pal.surface);
        }
        _canvas->drawFastHLine(mx + 28, y + 38, mw - 56, pal.border);
    };

    char rec1[24], rec2[24], rec3[24];
    snprintf(rec1, sizeof(rec1), "0 – %d km/h", Settings.get().race_split1_kmh);
    snprintf(rec2, sizeof(rec2), "0 – %d km/h", Settings.get().race_split2_kmh);
    snprintf(rec3, sizeof(rec3), "0 – %d km/h", Settings.get().race_split3_kmh);

    drawRecordRow(my + 72,  rec1, telemetry.race_records.best_0_22_sec);
    drawRecordRow(my + 132, rec2, telemetry.race_records.best_0_50_sec);
    drawRecordRow(my + 192, rec3, telemetry.race_records.best_0_70_sec);

    int btnY = my + 266;
    uint16_t resetBg = blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 22 : 44);
    _canvas->fillRoundRect(mx + 30, btnY, 150, 48, 12, resetBg);
    _canvas->drawRoundRect(mx + 30, btnY, 150, 48, 12, pal.accent_danger);
    drawSmoothText(UIF_BODY, "RESET", mx + 105, btnY + 24, textdatum_t::middle_center, pal.accent_danger, resetBg);

    _canvas->fillRoundRect(mx + mw - 210, btnY, 180, 48, 12, pal.surface_alt);
    _canvas->drawRoundRect(mx + mw - 210, btnY, 180, 48, 12, pal.border);
    drawSmoothText(UIF_BODY, "SCHLIESSEN", mx + mw - 120, btnY + 24, textdatum_t::middle_center, pal.text, pal.surface_alt);
}

void DashboardRenderer::drawRaceDynoModal(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();
    int mx = 100, my = 45, mw = 600, mh = 390;

    // Dark modal surface card with primary border
    drawCard(mx, my, mw, mh, pal.surface, pal.primary, 16);
    _canvas->drawRoundRect(mx + 1, my + 1, mw - 2, mh - 2, 15, pal.primary);

    // Modal Header
    drawSmoothText(UIF_HEADING, "LEISTUNGSDIAGRAMM & TELEMETRIE", mx + 22, my + 16, textdatum_t::top_left, pal.primary, pal.surface);

    // Top-Right Close Button [ X ]
    _canvas->fillRoundRect(mx + mw - 50, my + 12, 36, 32, 8, pal.surface_alt);
    _canvas->drawRoundRect(mx + mw - 50, my + 12, 36, 32, 8, pal.border);
    drawSmoothText(UIF_BODY, "X", mx + mw - 32, my + 28, textdatum_t::middle_center, pal.text, pal.surface_alt);

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
    int tile_y = my + 52;
    int tile_w = 132;
    int tile_h = 54;
    int tile_gap = 10;
    int tile_x0 = mx + 18;

    auto drawStatTile = [&](int idx, const char *label, const char *val, const char *sub_val, uint16_t valColor) {
        int tx = tile_x0 + idx * (tile_w + tile_gap);
        drawCard(tx, tile_y, tile_w, tile_h, pal.surface_alt, pal.border, 8);
        drawSmoothText(UIF_SMALL, label, tx + tile_w / 2, tile_y + 4, textdatum_t::top_center, pal.muted, pal.surface_alt);
        if (sub_val && sub_val[0] != 0) {
            drawSmoothText(UIF_BODY, val, tx + tile_w / 2, tile_y + 20, textdatum_t::top_center, valColor, pal.surface_alt);
            drawSmoothText(UIF_SMALL, sub_val, tx + tile_w / 2, tile_y + 38, textdatum_t::top_center, pal.accent_warn, pal.surface_alt);
        } else {
            drawSmoothText(UIF_HEADING, val, tx + tile_w / 2, tile_y + 34, textdatum_t::middle_center, valColor, pal.surface_alt);
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

    drawStatTile(0, "MAX WATT", b1, sub_b1, pal.primary);
    drawStatTile(1, "MAX G-FORCE", b2, nullptr, pal.accent_warn);
    char dynoSplitLabel[24];
    snprintf(dynoSplitLabel, sizeof(dynoSplitLabel), "0 - %d KM/H", Settings.get().race_split3_kmh);
    drawStatTile(2, dynoSplitLabel, b3, nullptr, pal.accent_ok);
    drawStatTile(3, "MAX PHASEN-I", b4, nullptr, pal.text);

    // Dyno Chart Area
    int gx = mx + 55;
    int gy = my + 115;
    int gw = mw - 110; // 490px
    int gh = 185;

    // Background of chart
    uint16_t chartBg = pal.is_light ? pal.bg : blendRgb565(pal.bg, pal.surface, 120);
    _canvas->fillRect(gx, gy, gw, gh, chartBg);
    _canvas->drawRect(gx, gy, gw, gh, pal.border);

    // Grid lines (horizontal: 25%, 50%, 75%)
    for (int i = 1; i <= 3; i++) {
        int y_line = gy + (gh * i) / 4;
        _canvas->drawFastHLine(gx, y_line, gw, pal.surface_alt);
    }
    // Vertical grid lines (25%, 50%, 75%)
    for (int i = 1; i <= 3; i++) {
        int x_line = gx + (gw * i) / 4;
        _canvas->drawFastVLine(x_line, gy, gh, pal.surface_alt);
    }

    // Y-Axis Left (Power in kW, pal.primary)
    float max_kw_scale = 6.0f;
    if (peak_kw > 10.0f) max_kw_scale = 15.0f;
    else if (peak_kw > 7.5f) max_kw_scale = 10.0f;
    else if (peak_kw > 5.5f) max_kw_scale = 8.0f;
    char kwBuf[16];
    snprintf(kwBuf, sizeof(kwBuf), "%.1fkW", max_kw_scale);
    drawSmoothText(UIF_SMALL, kwBuf, gx - 5, gy + 4, textdatum_t::middle_right, pal.primary, pal.surface);
    snprintf(kwBuf, sizeof(kwBuf), "%.1fkW", max_kw_scale * 0.5f);
    drawSmoothText(UIF_SMALL, kwBuf, gx - 5, gy + gh / 2, textdatum_t::middle_right, pal.primary, pal.surface);
    drawSmoothText(UIF_SMALL, "0 kW", gx - 5, gy + gh - 4, textdatum_t::middle_right, pal.primary, pal.surface);

    // Y-Axis Right (G-Force, pal.accent_warn)
    float max_g_scale = 1.5f;
    if (peak_g > 1.8f) max_g_scale = 2.5f;
    else if (peak_g > 1.35f) max_g_scale = 2.0f;
    char gBuf[16];
    snprintf(gBuf, sizeof(gBuf), "%.1f G", max_g_scale);
    drawSmoothText(UIF_SMALL, gBuf, gx + gw + 5, gy + 4, textdatum_t::middle_left, pal.accent_warn, pal.surface);
    snprintf(gBuf, sizeof(gBuf), "%.2f G", max_g_scale * 0.5f);
    drawSmoothText(UIF_SMALL, gBuf, gx + gw + 5, gy + gh / 2, textdatum_t::middle_left, pal.accent_warn, pal.surface);
    drawSmoothText(UIF_SMALL, "0.0 G", gx + gw + 5, gy + gh - 4, textdatum_t::middle_left, pal.accent_warn, pal.surface);

    // X-Axis Speed Labels under chart
    drawSmoothText(UIF_SMALL, "0 km/h", gx, gy + gh + 10, textdatum_t::top_center, pal.muted, pal.surface);
    drawSmoothText(UIF_SMALL, "25 km/h", gx + gw / 4, gy + gh + 10, textdatum_t::top_center, pal.muted, pal.surface);
    drawSmoothText(UIF_SMALL, "50 km/h", gx + gw / 2, gy + gh + 10, textdatum_t::top_center, pal.muted, pal.surface);
    drawSmoothText(UIF_SMALL, "75 km/h", gx + (gw * 3) / 4, gy + gh + 10, textdatum_t::top_center, pal.muted, pal.surface);
    drawSmoothText(UIF_SMALL, "100 km/h", gx + gw, gy + gh + 10, textdatum_t::top_center, pal.muted, pal.surface);

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
            // Realistic preview curve
            float t_norm = (num_points > 1) ? ((float)i / (float)(num_points - 1)) : 0.0f;
            spd_val = 80.0f * (1.0f - expf(-2.2f * t_norm));
            float p_frac = (t_norm < 0.25f) ? (t_norm / 0.25f) : (1.0f - 0.12f * (t_norm - 0.25f));
            pwr_val = peak_kw * 1000.0f * p_frac;
            g_val = peak_g * expf(-1.1f * t_norm);
        }

        float x_ratio = spd_val / 100.0f;
        if (x_ratio < 0.0f) x_ratio = 0.0f;
        if (x_ratio > 1.0f) x_ratio = 1.0f;
        int px = gx + (int)(x_ratio * (gw - 2));

        float p_kw = pwr_val / 1000.0f;
        float p_ratio = p_kw / max_kw_scale;
        if (p_ratio < 0.0f) p_ratio = 0.0f;
        if (p_ratio > 1.0f) p_ratio = 1.0f;
        int py_pwr = gy + gh - 2 - (int)(p_ratio * (gh - 4));

        float g_ratio = g_val / max_g_scale;
        if (g_ratio < 0.0f) g_ratio = 0.0f;
        if (g_ratio > 1.0f) g_ratio = 1.0f;
        int py_g = gy + gh - 2 - (int)(g_ratio * (gh - 4));

        if (i > 0 && prev_px >= 0) {
            _canvas->drawLine(prev_px, prev_py_power, px, py_pwr, pal.primary);
            _canvas->drawLine(prev_px, prev_py_power + 1, px, py_pwr + 1, pal.primary);
            _canvas->drawLine(prev_px, prev_py_g, px, py_g, pal.accent_warn);
            _canvas->drawLine(prev_px, prev_py_g + 1, px, py_g + 1, pal.accent_warn);
        }

        prev_px = px;
        prev_py_power = py_pwr;
        prev_py_g = py_g;
    }

    // Legend & Mode Tag under Chart
    int leg_y = gy + gh + 22;
    _canvas->fillRoundRect(gx + 10, leg_y + 4, 18, 4, 2, pal.primary);
    drawSmoothText(UIF_SMALL, "Leistung (kW)", gx + 34, leg_y + 6, textdatum_t::middle_left, pal.primary, pal.surface);

    _canvas->fillRoundRect(gx + 160, leg_y + 4, 18, 4, 2, pal.accent_warn);
    drawSmoothText(UIF_SMALL, "Beschleunigung (+G)", gx + 184, leg_y + 6, textdatum_t::middle_left, pal.accent_warn, pal.surface);

    drawSmoothText(UIF_SMALL, has_real_data ? "[Live Dyno Telemetrie]" : "[Vorschau / Letzter Lauf]",
                   gx + gw, leg_y + 6, textdatum_t::middle_right, pal.muted, pal.surface);

    // Bottom Action Button: [ SCHLIESSEN ]
    int btnW = 200, btnH = 40;
    int btnX = mx + (mw - btnW) / 2;
    int btnY = my + mh - 50;
    _canvas->fillRoundRect(btnX, btnY, btnW, btnH, 12, pal.surface_alt);
    _canvas->drawRoundRect(btnX, btnY, btnW, btnH, 12, pal.border);
    drawSmoothText(UIF_BODY, "SCHLIESSEN", btnX + btnW / 2, btnY + btnH / 2,
                   textdatum_t::middle_center, pal.text, pal.surface_alt);
}

// ==============================================================================
// SCREEN 7: FIRST-TIME SETUP & ONBOARDING WIZARD (Theme-aware + Pill Toggles)
// ==============================================================================
void DashboardRenderer::resetSetupWizard() {
    _wizard_inited = false;
    _last_wizard_step = -1;
    _last_wizard_pin_len = -1;
    _wizard_step = 0;
    _wizard_pin_buf[0] = '\0';
    _screen_dirty = true;
}

void DashboardRenderer::initSetupWizard(const DashTelemetry &telemetry) {
    if (!_wizard_inited) {
        _wizard_step = telemetry.wizard_step;
        _wizard_pin_buf[0] = '\0';
        _last_wizard_step = -1;
        _last_wizard_pin_len = -1;
        _wizard_inited = true;
    }
}

void DashboardRenderer::renderSetupWizard(const DashTelemetry &telemetry) {
    (void)telemetry;
    const ColorThemePalette &pal = currentTheme();
    bool step_changed = (_wizard_step != _last_wizard_step) || !_wizard_inited;
    if (step_changed) {
        _canvas->fillScreen(pal.bg);
        _last_wizard_step = _wizard_step;
        _wizard_inited = true;
        _last_wizard_pin_len = -1;
    }

    if (_wizard_step == 0) {
        if (step_changed) {
            uint16_t hdrBg = blendRgb565(pal.surface, pal.primary, pal.is_light ? 22 : 36);
            drawCard(20, 14, 760, 56, hdrBg, pal.primary, 12);
            drawSmoothText(UIF_HEADING, "WILLKOMMEN ZUM VESC DISPLAY", 400, 32, textdatum_t::middle_center, pal.text, hdrBg);
            drawSmoothText(UIF_SMALL, "Ersteinrichtung  •  Schritt 1 von 3: Schnellanleitung", 400, 54, textdatum_t::middle_center, pal.primary, hdrBg);

            drawCard(20, 82, 760, 128, pal.surface, pal.border, 12);
            drawSmoothText(UIF_HEADING, "WIE ÖFFNE ICH DIE EINSTELLUNGEN?", 38, 96, textdatum_t::top_left, pal.primary, pal.surface);
            drawSmoothText(UIF_BODY, "Im Fahr-Dashboard oben rechts kurz tippen oder halten, um Einstellungen zu öffnen.", 38, 130, textdatum_t::top_left, pal.text, pal.surface);
            drawSmoothText(UIF_BODY, "Dort kannst du Theme, TCS, Akku-Zellen, Bremse & Sicherheit anpassen.", 38, 158, textdatum_t::top_left, pal.muted, pal.surface);
            drawSmoothText(UIF_SMALL, "Zurück zum Cockpit jederzeit über [< DASH] oder [FERTIG].", 38, 186, textdatum_t::top_left, pal.muted, pal.surface);

            drawCard(20, 222, 760, 142, pal.surface, pal.accent_danger, 12);
            drawSmoothText(UIF_HEADING, "TUNING- & RACE-MODUS AKTIVIEREN", 38, 236, textdatum_t::top_left, pal.accent_danger, pal.surface);
            drawSmoothText(UIF_BODY, "Halte die große km/h-Geschwindigkeitszahl für 4 Sekunden gedrückt!", 38, 270, textdatum_t::top_left, pal.text, pal.surface);
            drawSmoothText(UIF_BODY, "Der dezente rote Punkt oben rechts erlischt — Tuning & Launch Control aktiv.", 38, 298, textdatum_t::top_left, pal.muted, pal.surface);
            drawSmoothText(UIF_BODY, "Sofort-Drosselung: Ein einzelner Tipp auf km/h sperrt sofort auf 20 km/h!", 38, 326, textdatum_t::top_left, pal.accent_ok, pal.surface);

            _canvas->fillRoundRect(200, 384, 400, 64, 16, pal.primary);
            drawSmoothText(UIF_HEADING, "WEITER: PIN FESTLEGEN ->", 400, 416, textdatum_t::middle_center, onPrimaryFg(pal), pal.primary);
        }
    } else if (_wizard_step == 1) {
        int pinLen = (int)strlen(_wizard_pin_buf);
        int mx = 180, my = 76, mw = 440, mh = 388;
        int kw = 84, kh = 44;
        int kx0 = mx + (mw - (3 * kw + 2 * 14)) / 2;
        int ky0 = 176;

        if (step_changed) {
            drawCard(20, 14, 760, 52, pal.surface, pal.border, 12);
            drawSmoothText(UIF_HEADING, "SICHERHEITS-PIN WÄHLEN", 360, 30, textdatum_t::middle_center, pal.text, pal.surface);
            drawSmoothText(UIF_SMALL, "Ersteinrichtung  •  Schritt 2 von 3: Wegfahrsperre", 360, 52, textdatum_t::middle_center, pal.muted, pal.surface);

            _canvas->fillRoundRect(615, 20, 155, 40, 10, pal.surface_alt);
            _canvas->drawRoundRect(615, 20, 155, 40, 10, pal.primary);
            drawSmoothText(UIF_SMALL, "ÜBERSPRINGEN >", 692, 40, textdatum_t::middle_center, pal.primary, pal.surface_alt);

            drawCard(mx, my, mw, mh, pal.surface, pal.border, 14);
            drawSmoothText(UIF_BODY, "Gib deinen 4-stelligen PIN ein:", 400, 96, textdatum_t::middle_center, pal.muted, pal.surface);

            static const char KEY_LBL[4][3] = {
                { '1', '2', '3' },
                { '4', '5', '6' },
                { '7', '8', '9' },
                { '<', '0', '#' }
            };
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 3; c++) {
                    if (r == 3 && c == 2) continue;
                    int bx = kx0 + c * (kw + 14);
                    int by = ky0 + r * (kh + 8);
                    char key = KEY_LBL[r][c];
                    bool isDel = (key == '<');
                    uint16_t bg = isDel ? blendRgb565(pal.surface_alt, pal.accent_danger, 28) : pal.surface_alt;
                    uint16_t bd = isDel ? pal.accent_danger : pal.border;
                    uint16_t fg = isDel ? pal.accent_danger : pal.text;
                    drawCard(bx, by, kw, kh, bg, bd, 10);
                    char str[8] = { key, '\0' };
                    if (isDel) strcpy(str, "DEL");
                    drawSmoothText(isDel ? UIF_BODY : UIF_HEADING, str, bx + kw / 2, by + kh / 2,
                                   textdatum_t::middle_center, fg, bg);
                }
            }
        }

        if (step_changed || pinLen != _last_wizard_pin_len) {
            _last_wizard_pin_len = pinLen;

            for (int i = 0; i < 4; i++) {
                int bx = 265 + i * 70, by = 118;
                drawCard(bx, by, 58, 48, pal.surface_alt, (i < pinLen) ? pal.primary : pal.border, 10);
                char chStr[2] = { (i < pinLen) ? _wizard_pin_buf[i] : '-', '\0' };
                drawSmoothText(UIF_HEADING, chStr, bx + 29, by + 24, textdatum_t::middle_center,
                               (i < pinLen) ? pal.primary : pal.muted, pal.surface_alt);
            }

            int bx = kx0 + 2 * (kw + 14);
            int by = ky0 + 3 * (kh + 8);
            uint16_t okBg = (pinLen == 4) ? pal.accent_ok : pal.surface_alt;
            uint16_t okBd = (pinLen == 4) ? pal.accent_ok : pal.border;
            uint16_t okFg = (pinLen == 4) ? onPrimaryFg(pal) : pal.muted;
            drawCard(bx, by, kw, kh, okBg, okBd, 10);
            drawSmoothText(UIF_BODY, (pinLen == 4) ? "OK" : "SKIP", bx + kw / 2, by + kh / 2,
                           textdatum_t::middle_center, okFg, okBg);

            if (pinLen == 4) {
                _canvas->fillRoundRect(mx + 20, 396, mw - 40, 52, 12, pal.accent_ok);
                drawSmoothText(UIF_HEADING, "PIN ÜBERNEHMEN & WEITER ->", 400, 422,
                               textdatum_t::middle_center, onPrimaryFg(pal), pal.accent_ok);
            } else {
                _canvas->fillRoundRect(mx + 20, 396, mw - 40, 52, 12, pal.surface_alt);
                _canvas->drawRoundRect(mx + 20, 396, mw - 40, 52, 12, pal.border);
                drawSmoothText(UIF_BODY, "ÜBERSPRINGEN (OHNE PIN) ->", 400, 422,
                               textdatum_t::middle_center, pal.primary, pal.surface_alt);
            }
        }
    } else if (_wizard_step == 2) {
        if (step_changed) {
            uint16_t hdrBg = blendRgb565(pal.surface, pal.primary, pal.is_light ? 22 : 36);
            drawCard(20, 14, 760, 54, hdrBg, pal.primary, 12);
            drawSmoothText(UIF_HEADING, "HARDWARE & FEATURES", 400, 31, textdatum_t::middle_center, pal.text, hdrBg);
            drawSmoothText(UIF_SMALL, "Ersteinrichtung  •  Schritt 3 von 3: Ausstattung wählen", 400, 53, textdatum_t::middle_center, pal.primary, hdrBg);

            drawCard(20, 80, 760, 82, pal.surface, pal.border, 12);
            drawSmoothText(UIF_HEADING, "GPS & LIVE-UHRZEIT", 38, 94, textdatum_t::top_left, pal.text, pal.surface);
            drawSmoothText(UIF_BODY, "Zeigt Uhrzeit oben zentriert & Satelliten-Icon bei 3D-Fix.", 38, 124, textdatum_t::top_left, pal.muted, pal.surface);

            drawCard(20, 174, 760, 82, pal.surface, pal.border, 12);
            drawSmoothText(UIF_HEADING, "NEIGUNG & G-FORCE", 38, 188, textdatum_t::top_left, pal.text, pal.surface);
            drawSmoothText(UIF_BODY, "Zeigt Horizont-Neigung & Beschleunigung unten links.", 38, 218, textdatum_t::top_left, pal.muted, pal.surface);

            drawCard(20, 268, 760, 82, pal.surface, pal.border, 12);
            drawSmoothText(UIF_HEADING, "VESC TOOL WI-FI BRIDGE", 38, 282, textdatum_t::top_left, pal.accent_warn, pal.surface);
            drawSmoothText(UIF_BODY, "Aktiviert den SoftAP Access Point für kabelloses VESC Tool.", 38, 312, textdatum_t::top_left, pal.muted, pal.surface);

            _canvas->fillRoundRect(160, 376, 480, 66, 16, pal.accent_ok);
            drawSmoothText(UIF_HEADING, "SETUP ABSCHLIESSEN & STARTEN", 400, 409, textdatum_t::middle_center, onPrimaryFg(pal), pal.accent_ok);
        }

        drawPillToggle(700, 106, Settings.get().gps_enabled, pal);
        drawPillToggle(700, 200, Settings.get().incline_gforce_enabled, pal);
        drawPillToggle(700, 294, Settings.get().wifi_bridge_enabled, pal);
    }
}

bool DashboardRenderer::handleWizardTouch(int16_t tx, int16_t ty, DashTelemetry &telemetry) {
    if (tx < 0 || ty < 0) return false;

    if (_wizard_step == 0) {
        if (tx >= 180 && tx <= 620 && ty >= 375 && ty <= 460) {
            _wizard_step = 1;
            telemetry.wizard_step = 1;
            _screen_dirty = true;
            return true;
        }
    } else if (_wizard_step == 1) {
        int mx = 180, mw = 440;
        if (tx >= 600 && tx <= 780 && ty >= 14 && ty <= 68) {
            Settings.get().pin_lock_enabled = false;
            Settings.save();
            _wizard_step = 2;
            telemetry.wizard_step = 2;
            _screen_dirty = true;
            return true;
        }

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
            int len = (int)strlen(_wizard_pin_buf);
            if (key == '<') {
                if (len > 0) {
                    _wizard_pin_buf[len - 1] = '\0';
                    strncpy(telemetry.wizard_pin_buf, _wizard_pin_buf, sizeof(telemetry.wizard_pin_buf) - 1);
                    telemetry.wizard_pin_buf[sizeof(telemetry.wizard_pin_buf) - 1] = '\0';
                    _screen_dirty = true;
                }
            } else if (key == '#') {
                if (len == 4) {
                    strncpy(Settings.get().pin_code, _wizard_pin_buf, sizeof(Settings.get().pin_code) - 1);
                    Settings.get().pin_code[sizeof(Settings.get().pin_code) - 1] = '\0';
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
                    strncpy(telemetry.wizard_pin_buf, _wizard_pin_buf, sizeof(telemetry.wizard_pin_buf) - 1);
                    telemetry.wizard_pin_buf[sizeof(telemetry.wizard_pin_buf) - 1] = '\0';
                    _screen_dirty = true;
                }
            }
            return true;
        }

        if (tx >= mx + 10 && tx <= mx + mw - 10 && ty >= 390 && ty <= 455) {
            if (strlen(_wizard_pin_buf) == 4) {
                strncpy(Settings.get().pin_code, _wizard_pin_buf, sizeof(Settings.get().pin_code) - 1);
                Settings.get().pin_code[sizeof(Settings.get().pin_code) - 1] = '\0';
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
    } else if (_wizard_step == 2) {
        if (tx >= 530 && tx <= 775 && ty >= 80 && ty <= 162) {
            Settings.get().gps_enabled = !Settings.get().gps_enabled;
            Settings.save();
            telemetry.gps_enabled = Settings.get().gps_enabled;
            _screen_dirty = true;
            return true;
        }
        if (tx >= 530 && tx <= 775 && ty >= 174 && ty <= 256) {
            Settings.get().incline_gforce_enabled = !Settings.get().incline_gforce_enabled;
            Settings.save();
            telemetry.incline_gforce_enabled = Settings.get().incline_gforce_enabled;
            _screen_dirty = true;
            return true;
        }
        if (tx >= 530 && tx <= 775 && ty >= 268 && ty <= 350) {
            Settings.get().wifi_bridge_enabled = !Settings.get().wifi_bridge_enabled;
            Settings.save();
            telemetry.wifi_bridge_enabled = Settings.get().wifi_bridge_enabled;
            _screen_dirty = true;
            return true;
        }
        if (tx >= 140 && tx <= 660 && ty >= 365 && ty <= 455) {
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
// SCREEN 9: WIRELESS OTA FIRMWARE UPDATE PROGRESS HUD (Theme-aware)
// ==============================================================================
void DashboardRenderer::renderOtaScreen(int pct, const char *status) {
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);

    uint16_t hdrBg = blendRgb565(pal.surface, pal.primary, pal.is_light ? 22 : 36);
    drawCard(40, 24, 720, 56, hdrBg, pal.primary, 14);
    drawSmoothText(UIF_HEADING, "FIRMWARE UPDATE VIA WLAN (OTA)", 400, 52, textdatum_t::middle_center, pal.text, hdrBg);

    drawCard(40, 96, 720, 344, pal.surface, pal.border, 14);

    uint16_t warnBg = blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 22 : 42);
    drawCard(150, 116, 500, 40, warnBg, pal.accent_danger, 10);
    drawSmoothText(UIF_BODY, "BITTE FAHRZEUG NICHT AUSSCHALTEN!", 400, 136,
                   textdatum_t::middle_center, pal.accent_danger, warnBg);

    char pctStr[16];
    snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
    drawSmoothText(UIF_HERO_NUM, pctStr, 400, 214, textdatum_t::middle_center,
                   (pct >= 100) ? pal.accent_ok : pal.primary, pal.surface);

    int barX = 100, barY = 272, barW = 600, barH = 24;
    _canvas->fillRoundRect(barX, barY, barW, barH, 12, pal.track);
    int fillW = (pct * barW) / 100;
    if (fillW > 8) {
        _canvas->fillRoundRect(barX, barY, fillW, barH, 12, (pct >= 100) ? pal.accent_ok : pal.primary);
    }

    drawSmoothText(UIF_HEADING, status ? status : "Übertrage Firmware...", 400, 336,
                   textdatum_t::middle_center, pal.text, pal.surface);
    drawSmoothText(UIF_BODY, "Dual-Bank Safe Flash: Altes System bleibt bei Abbruch erhalten", 400, 392,
                   textdatum_t::middle_center, pal.muted, pal.surface);

    Display.pushCanvas();
}
