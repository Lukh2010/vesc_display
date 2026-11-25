#include "dash_renderer_internal.h"
#include "boot_logo.h"
#include "ui_fonts.h"

DashboardRenderer Renderer;

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
      _fault_modal_open(false),
      _last_wled_sig(0xFFFFFFFFUL),
      _wizard_step(0),
      _wizard_inited(false),
      _wizard_pin_warn(false),
      _last_wizard_step(-1),
      _last_wizard_pin_len(-1),
      _last_wizard_gps(-1),
      _last_wizard_incline(-1),
      _last_wizard_wifi(-1),
      _wifi_bridge_inited(false),
      _last_fault_code(0),
      _last_fault_flash(-1),
      _ota_inited(false),
      _last_ota_pct(-1),
      _dirty_rect_count(0),
      _dirty_full(false),
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
      _race_plateau_start_ms(0),
      _race_last_eval_ms(0),
      _race_last_eval_speed_kmh(0.0f),
      _pb_split1_kmh(0),
      _pb_split2_kmh(0),
      _pb_split3_kmh(0),
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
    _last_ota_status[0] = '\0';
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
    return &fonts::Font0;
}

void DashboardRenderer::useFont(UiFont f) {
    if (!_canvas) return;
    _canvas->setFont(getUiFont(f));
    _canvas->setTextSize(1);
}

void DashboardRenderer::resetFont() {
    if (!_canvas) return;
    useFont(UIF_BODY);
    _canvas->setTextPadding(0);
}

void DashboardRenderer::drawSmoothText(UiFont f, const char *str, int x, int y, textdatum_t datum, uint16_t fg, uint16_t bg, int padW) {
    if (!_canvas || !str) return;
    useFont(f);
    _canvas->setTextDatum(datum);
    _canvas->setTextColor(fg, bg);
    _canvas->setTextPadding(padW);
    _canvas->drawString(str, x, y);
    if (padW > 0) _canvas->setTextPadding(0);
}

void DashboardRenderer::begin(DisplayDriver *display) {
    _display = display;
    _canvas = display ? display->getCanvas() : nullptr;
    initSmoothFonts();
    _cache.invalidate();
    _screen_dirty = true;
    markFullDirty();
    _active_style = (DashboardStyle)Settings.get().dash_style;
    _active_theme = Settings.get().color_theme;
}

void DashboardRenderer::addDirtyRect(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (_dirty_full || w <= 0 || h <= 0) return;

    int32_t x0 = (x < 0) ? 0 : x;
    int32_t y0 = (y < 0) ? 0 : y;
    int32_t x1 = (int32_t)x + w;
    int32_t y1 = (int32_t)y + h;
    if (x1 > 800) x1 = 800;
    if (y1 > 480) y1 = 480;
    if (x0 >= x1 || y0 >= y1) return;

    int16_t cx = (int16_t)x0;
    int16_t cy = (int16_t)y0;
    int16_t cw = (int16_t)(x1 - x0);
    int16_t ch = (int16_t)(y1 - y0);

    // Coalesce overlapping or adjacent rects to keep blit count low
    for (uint8_t i = 0; i < _dirty_rect_count; i++) {
        DirtyRect &r = _dirty_rects[i];
        int32_t rx1 = (int32_t)r.x + r.w;
        int32_t ry1 = (int32_t)r.y + r.h;
        if (cx <= rx1 + 4 && x1 >= r.x - 4 && cy <= ry1 + 4 && y1 >= r.y - 4) {
            int32_t nx0 = (cx < r.x) ? cx : r.x;
            int32_t ny0 = (cy < r.y) ? cy : r.y;
            int32_t nx1 = (x1 > rx1) ? x1 : rx1;
            int32_t ny1 = (y1 > ry1) ? y1 : ry1;
            r.x = (int16_t)nx0;
            r.y = (int16_t)ny0;
            r.w = (int16_t)(nx1 - nx0);
            r.h = (int16_t)(ny1 - ny0);
            return;
        }
    }

    if (_dirty_rect_count < MAX_DIRTY_RECTS) {
        _dirty_rects[_dirty_rect_count++] = { cx, cy, cw, ch };
    } else {
        // Fallback: too many fragments -> promote to full screen blit
        markFullDirty();
    }
}

void DashboardRenderer::flushDirtyToDisplay() {
#if !defined(NATIVE_SIMULATOR)
    if (_display && _display->hasBackbuffer()) {
        if (_dirty_full) {
            _display->pushCanvas();
        } else {
            for (uint8_t i = 0; i < _dirty_rect_count; i++) {
                const DirtyRect &r = _dirty_rects[i];
                if (r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 &&
                    (int32_t)r.x + r.w <= 800 && (int32_t)r.y + r.h <= 480) {
                    _display->pushRect(r.x, r.y, r.w, r.h);
                }
            }
        }
    }
#endif
    clearDirtyTracking();
}

// ==============================================================================
// Common Vector Primitives (Cards, Pill Toggles, Icons)
// ==============================================================================
void DashboardRenderer::drawCard(int x, int y, int w, int h, uint16_t bg, uint16_t border, int r) {
    if (!_canvas) return;
    _canvas->fillRoundRect(x, y, w, h, r, bg);
    _canvas->drawRoundRect(x, y, w, h, r, border);
}

void DashboardRenderer::drawPillToggle(int x, int y, bool on, const ColorThemePalette &pal) {
    if (!_canvas) return;
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
            markFullDirty();
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
            markFullDirty();
        }
        return;
    }

    // Phase 2 (>= 2.25s): Live Diagnostic Console
    uint32_t p2_ms = elapsed_ms - 2250;
    if (_boot_anim_phase != 2) {
        _boot_anim_phase = 2;
        _boot_log_lines_drawn = 0;
        _canvas->fillScreen(COLOR_BLACK);
        markFullDirty();
    } else {
        addDirtyRect(40, 50, 720, 366);
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
    if (!_canvas) return;
    struct RenderScopeGuard {
        DashboardRenderer *r;
        LovyanGFX *c;
        RenderScopeGuard(DashboardRenderer *renderer, LovyanGFX *canvas) : r(renderer), c(canvas) {
            if (c) c->startWrite();
        }
        ~RenderScopeGuard() {
            if (c) c->endWrite();
            if (r) r->flushDirtyToDisplay();
        }
    };
    RenderScopeGuard render_guard(this, _canvas);

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
        _ota_inited = false;
        _last_fault_flash = -1;
        _last_rendered_tab = 255;
        _menu_dirty = true;
    }

    // 0b. First-Time Setup Wizard / OTA / PIN Lockscreen priority
    if (!OtaManager::instance().isUpdating()) {
        if (_ota_inited) {
            _ota_inited = false;
            invalidateCache();
            markScreenDirty();
        }
    }
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
        }
    }

    // If fault just cleared, invalidate caches BEFORE rendering so the top area redraws immediately
    if (telemetry.screen != SCREEN_SETUP_WIZARD && telemetry.fault_code == 0 && _last_fault_code != 0) {
        _last_fault_code = 0;
        _last_fault_flash = -1;
        invalidateCache();
        _screen_dirty = true;
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
                markFullDirty();
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
                markFullDirty();
                _screen_dirty = false;
            }
            renderSettingsScreen(telemetry);
            break;

        case SCREEN_VESC_BRIDGE:
            if (_screen_dirty) {
                _canvas->fillScreen(pal.bg);
                markFullDirty();
                initVescBridgeScreen(telemetry);
                _screen_dirty = false;
            }
            renderVescBridgeScreen(telemetry);
            break;

        case SCREEN_WIFI_BRIDGE:
            if (!_wifi_bridge_inited || _screen_dirty) {
                _canvas->fillScreen(pal.bg);
                markFullDirty();
                initWifiBridgeScreen(telemetry);
                _wifi_bridge_inited = true;
                _screen_dirty = false;
            }
            renderWifiBridgeScreen(telemetry);
            break;

        case SCREEN_RACE_TIMER:
            if (_screen_dirty) {
                _canvas->fillScreen(pal.bg);
                markFullDirty();
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

    // Global VESC Fault Alarm Banner (cached against fault_code & 350ms flash state, visible on all active screens)
    if (telemetry.screen != SCREEN_SETUP_WIZARD && telemetry.fault_code != 0) {
        int8_t flash = (int8_t)(((millis() / 350) % 2) == 0 ? 1 : 0);
        if (telemetry.fault_code != _last_fault_code || flash != _last_fault_flash) {
            _last_fault_code = telemetry.fault_code;
            _last_fault_flash = flash;
            drawFaultBanner(telemetry.fault_code);
            addDirtyRect(20, 8, 760, 56);
        }
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
            addDirtyRect(784, 4, 12, 12);
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
}

// Design Upgrade 4: Crisp Vector Badges for Bottom-Left Circular Instruments
void DashboardRenderer::drawGForceBadge(float g_force, int cx, int cy, int r, const ColorThemePalette &pal) {
    LovyanGFX *canvas = _canvas;
    const lgfx::IFont *smallFont = getUiFont(UIF_SMALL);
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

    uint16_t fetCol = getTempColorTheme(fet, pal);
    uint16_t motCol = getTempColorTheme(mot, pal);

    canvas->setTextColor((fet >= 75.0f) ? fetCol : pal.muted, pal.bg);
    canvas->drawString("FET ", curX, y);
    curX += wFetLbl;

    canvas->setTextColor(fetCol, pal.bg);
    canvas->drawString(fetVal, curX, y);
    curX += wFetVal + wGap;

    canvas->setTextColor((mot >= 75.0f) ? motCol : pal.muted, pal.bg);
    canvas->drawString("MOT ", curX, y);
    curX += wMotLbl;

    canvas->setTextColor(motCol, pal.bg);
    canvas->drawString(motVal, curX, y);
}

void DashboardRenderer::initHyper35kwStyle(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);
    markFullDirty();
    _mode_banner_active = (millis() < telemetry.mode_badge_until);
    _mode_banner_drawn_mode = -1;
    _hyper_inited = false;
    _anim_inited = false;
    _cache.invalidate();
    _last_arc_frac = -1.0f;
    _last_arc_angle = 135.0f;
    _last_arc_col = 0;

    constexpr int cx = UiLayout::DASH_SPEED_CX, cy = 215;
    constexpr int r_in = 135, r_out = 162;
    // Compile-time arc end-cap centers for 135° / 405° at mid_r = 148.5
    constexpr int s_cap_x = 135, s_cap_y = 320;
    constexpr int e_cap_x = 345, e_cap_y = 320;

    // 0. Top-Left [ STATS ] Pill Button
    if (telemetry.fault_code == 0) {
        const UiRect &sb = UiLayout::DASH_STATS_BTN;
        _canvas->fillRoundRect(sb.x, sb.y, sb.w, sb.h, 18, pal.surface);
        _canvas->drawRoundRect(sb.x, sb.y, sb.w, sb.h, 18, pal.border);
        drawSmoothText(UIF_BODY, "STATS", sb.cx(), sb.cy(), textdatum_t::middle_center, pal.text, pal.surface);
    }

    // 1. Speedometer Gauge Background Arc (270 degree sweep: 135° to 405°)
    _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, 405.0f, pal.track);
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
        addDirtyRect(434, 170, 340, 114);
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

    constexpr int cx = UiLayout::DASH_SPEED_CX, cy = 215;
    constexpr int r_in = 135, r_out = 162;
    constexpr int s_cap_x = 135, s_cap_y = 320;
    constexpr int e_cap_x = 345, e_cap_y = 320;

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

    bool arc_dirty = false;
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
        arc_dirty = true;
    } else if (target_angle <= 135.0f && _last_arc_angle > 135.0f) {
        _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, _last_arc_angle + 1.0f, pal.track);
        _canvas->fillCircle(s_cap_x, s_cap_y, 14, pal.track);
        _canvas->fillCircle(e_cap_x, e_cap_y, 14, pal.track);
        _last_arc_angle = 135.0f;
        _last_arc_col = arcCol;
        arc_dirty = true;
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
        arc_dirty = true;
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
        arc_dirty = true;
    }
    if (arc_dirty) {
        addDirtyRect(cx - 165, cy - 165, 330, 330);
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
            addDirtyRect(340, 14, 120, 42);
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
        addDirtyRect(540, 18, 190, 30);
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
        addDirtyRect(cx - 88, 162, 176, 68);
    }

    // Vector Satellite icon under "km/h"
    if (!_hyper_inited || gps_connected != _cache.gps_valid || gps_en_changed) {
        _cache.gps_valid = gps_connected;
        _canvas->fillRect(cx - 18, 272, 36, 30, pal.bg);
        if (gps_connected) {
            drawMiniSatellite(_canvas, cx, 286, pal);
        }
        addDirtyRect(cx - 18, 272, 36, 30);
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
            addDirtyRect(442, 54, 282, 60);
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
        addDirtyRect(675, 118, 92, 26);
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
        addDirtyRect(445, 150, 320, 10);
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
            addDirtyRect(434, 170, 340, 114);
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
            addDirtyRect(442, 172, 324, 62);
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
            addDirtyRect(630, 178, 138, 28);
        }

        if (!_hyper_inited || cur_pha != _cache.phase_x10) {
            _cache.phase_x10 = cur_pha;
            char motBuf[32];
            snprintf(motBuf, sizeof(motBuf), "MOTOR %d A", cur_pha);
            _canvas->fillRect(442, 246, 165, 24, pal.bg);
            drawSmoothText(UIF_BODY, motBuf, 445, 248, textdatum_t::top_left, pal.text, pal.bg);
            addDirtyRect(442, 246, 165, 24);
        }

        if (!_hyper_inited || cur_a != _cache.current_x10) {
            _cache.current_x10 = cur_a;
            char bBuf[32];
            snprintf(bBuf, sizeof(bBuf), "BATTERY %d A", cur_a);
            _canvas->fillRect(605, 246, 165, 24, pal.bg);
            drawSmoothText(UIF_BODY, bBuf, 765, 248, textdatum_t::top_right, pal.text, pal.bg);
            addDirtyRect(605, 246, 165, 24);
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
        addDirtyRect(442, 314, 170, 38);
    }

    // 8b. Trip Distance Readout
    int cur_trip_x10 = (int)roundf(telemetry.trip_km * 10.0f);
    if (!_hyper_inited || cur_trip_x10 != _cache.trip_x10) {
        _cache.trip_x10 = cur_trip_x10;
        char tripBuf[16];
        snprintf(tripBuf, sizeof(tripBuf), "%.1f km", telemetry.trip_km);
        _canvas->fillRect(616, 314, 158, 38, pal.bg);
        drawSmoothText(UIF_MED_NUM, tripBuf, 620, 316, textdatum_t::top_left, pal.text, pal.bg);
        addDirtyRect(616, 314, 158, 38);
    }

    // 9. Right Column: Bottom Touch Action Buttons
    // ALWAYS render BOTH buttons in bottom-right in all modes: [ BOOST ] (x: 445..597) and [ MODE ] (x: 613..765)
    bool btn_layout_changed = !_hyper_inited;
    bool boost_vis = telemetry.tuning_mode && telemetry.boost_active;
    int8_t cur_boost = boost_vis ? 1 : 0;

    const UiRect &bb = UiLayout::DASH_BOOST_BTN;
    if (btn_layout_changed || cur_boost != _cache.boost_active) {
        _cache.boost_active = cur_boost;
        if (boost_vis) {
            _canvas->fillRoundRect(bb.x, bb.y, bb.w, bb.h, 16, pal.primary);
            _canvas->drawRoundRect(bb.x, bb.y, bb.w, bb.h, 16, pal.primary);
            drawSmoothText(UIF_HEADING, "BOOST", bb.cx(), bb.cy(), textdatum_t::middle_center, onPrimaryFg(pal), pal.primary);
        } else {
            _canvas->fillRoundRect(bb.x, bb.y, bb.w, bb.h, 16, pal.surface);
            _canvas->drawRoundRect(bb.x, bb.y, bb.w, bb.h, 16, pal.border);
            drawSmoothText(UIF_HEADING, "BOOST", bb.cx(), bb.cy(), textdatum_t::middle_center, pal.text, pal.surface);
        }
        addDirtyRect(bb.x, bb.y, bb.w, bb.h);
    }

    const UiRect &mb = UiLayout::DASH_MODE_BTN;
    int8_t cur_banner = banner_now ? 1 : 0;
    if (btn_layout_changed || (int)telemetry.ride_mode != _cache.ride_mode || cur_banner != _cache.banner_active) {
        _cache.ride_mode = (int)telemetry.ride_mode;
        _cache.banner_active = cur_banner;
        if (banner_now) {
            _canvas->fillRoundRect(mb.x, mb.y, mb.w, mb.h, 16, mCol);
            _canvas->drawRoundRect(mb.x, mb.y, mb.w, mb.h, 16, mCol);
            drawSmoothText(UIF_HEADING, mNames[mIdx], mb.cx(), mb.cy(), textdatum_t::middle_center, onPrimaryFg(pal), mCol);
        } else {
            _canvas->fillRoundRect(mb.x, mb.y, mb.w, mb.h, 16, pal.surface);
            _canvas->drawRoundRect(mb.x, mb.y, mb.w, mb.h, 16, mCol);
            _canvas->drawRoundRect(mb.x + 1, mb.y + 1, mb.w - 2, mb.h - 2, 15, mCol);
            drawSmoothText(UIF_HEADING, mNames[mIdx], mb.cx(), mb.cy(), textdatum_t::middle_center, mCol, pal.surface);
        }
        addDirtyRect(mb.x, mb.y, mb.w, mb.h);
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
            gf_x = UiLayout::DASH_LIGHT_CX;    gf_y = UiLayout::DASH_LIGHT_Y_BOT;
            inc_x = 110;                       inc_y = UiLayout::DASH_LIGHT_Y_BOT;
            light_x = UiLayout::DASH_LIGHT_CX; light_y = UiLayout::DASH_LIGHT_Y_TOP;
        } else if (gf_on && !inc_on) {
            light_x = UiLayout::DASH_LIGHT_CX; light_y = UiLayout::DASH_LIGHT_Y_BOT;
            gf_x = 110;                        gf_y = UiLayout::DASH_LIGHT_Y_BOT;
        } else if (!gf_on && inc_on) {
            light_x = UiLayout::DASH_LIGHT_CX; light_y = UiLayout::DASH_LIGHT_Y_BOT;
            inc_x = 110;                       inc_y = UiLayout::DASH_LIGHT_Y_BOT;
        } else {
            light_x = UiLayout::DASH_LIGHT_CX; light_y = UiLayout::DASH_LIGHT_Y_BOT;
        }
    } else {
        if (gf_on && inc_on) {
            gf_x = UiLayout::DASH_LIGHT_CX;   gf_y = UiLayout::DASH_LIGHT_Y_BOT;
            inc_x = 110;                      inc_y = UiLayout::DASH_LIGHT_Y_BOT;
        } else if (gf_on && !inc_on) {
            gf_x = UiLayout::DASH_LIGHT_CX;   gf_y = UiLayout::DASH_LIGHT_Y_BOT;
        } else if (!gf_on && inc_on) {
            inc_x = UiLayout::DASH_LIGHT_CX;  inc_y = UiLayout::DASH_LIGHT_Y_BOT;
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
            addDirtyRect(20, 324, 308, 124);
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
        addDirtyRect(156, 396, 170, 42);
    }

    const lgfx::IFont *smallFont = getUiFont(UIF_SMALL);
    if (gf_x > 0) {
        int cur_g10 = (int)roundf(telemetry.g_force * 10.0f);
        if (!_hyper_inited || badge_cfg_changed || cur_g10 != _cache.g_force_x10) {
            _cache.g_force_x10 = cur_g10;
            drawGForceBadge(telemetry.g_force, gf_x, gf_y, 27, pal);
            addDirtyRect(gf_x - 28, gf_y - 28, 56, 56);
        }
    }

    if (inc_x > 0) {
        int cur_inc_x10 = (int)roundf(telemetry.incline_deg * 10.0f);
        if (!_hyper_inited || badge_cfg_changed || cur_inc_x10 != _cache.incline_deg) {
            _cache.incline_deg = cur_inc_x10;
            drawInclineBadge(_canvas, smallFont, telemetry.incline_deg, inc_x, inc_y, 27, pal);
            addDirtyRect(inc_x - 28, inc_y - 28, 56, 56);
        }
    }

    if (light_x > 0) {
        int8_t cur_light = telemetry.lights_enabled ? 1 : 0;
        if (!_hyper_inited || badge_cfg_changed || cur_light != _cache.lights_enabled) {
            _cache.lights_enabled = cur_light;
            drawLightBadge(_canvas, smallFont, telemetry.lights_enabled, light_x, light_y, 27, pal);
            addDirtyRect(light_x - 28, light_y - 28, 56, 56);
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
    markFullDirty();
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
    markFullDirty();
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
    markFullDirty();

    // Top Bar: Back Button [< DASH], [ RACE ] Button, Center Title, Reset Trip Button
    _canvas->fillRoundRect(UiLayout::STATS_BACK_BTN.x, UiLayout::STATS_BACK_BTN.y, UiLayout::STATS_BACK_BTN.w, UiLayout::STATS_BACK_BTN.h, 18, pal.surface);
    _canvas->drawRoundRect(UiLayout::STATS_BACK_BTN.x, UiLayout::STATS_BACK_BTN.y, UiLayout::STATS_BACK_BTN.w, UiLayout::STATS_BACK_BTN.h, 18, pal.border);
    drawSmoothText(UIF_BODY, "< DASH", UiLayout::STATS_BACK_BTN.cx(), UiLayout::STATS_BACK_BTN.cy(), textdatum_t::middle_center, pal.text, pal.surface);

    // Styled [ RACE ] touch button (only visible when tuning mode is active)
    if (telemetry.tuning_mode) {
        uint16_t raceBg = blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 28 : 48);
        _canvas->fillRoundRect(UiLayout::STATS_RACE_BTN.x, UiLayout::STATS_RACE_BTN.y, UiLayout::STATS_RACE_BTN.w, UiLayout::STATS_RACE_BTN.h, 18, raceBg);
        _canvas->drawRoundRect(UiLayout::STATS_RACE_BTN.x, UiLayout::STATS_RACE_BTN.y, UiLayout::STATS_RACE_BTN.w, UiLayout::STATS_RACE_BTN.h, 18, pal.accent_danger);
        drawSmoothText(UIF_BODY, "RACE", UiLayout::STATS_RACE_BTN.cx(), UiLayout::STATS_RACE_BTN.cy(), textdatum_t::middle_center, pal.accent_danger, raceBg);
    }

    int titleX = telemetry.tuning_mode ? 430 : 380;
    drawSmoothText(UIF_HEADING, "FAHRDATEN & STATISTIK", titleX, 30, textdatum_t::middle_center, pal.primary, pal.bg);

    _canvas->fillRoundRect(UiLayout::STATS_RESET_BTN.x, UiLayout::STATS_RESET_BTN.y, UiLayout::STATS_RESET_BTN.w, UiLayout::STATS_RESET_BTN.h, 18, pal.surface);
    _canvas->drawRoundRect(UiLayout::STATS_RESET_BTN.x, UiLayout::STATS_RESET_BTN.y, UiLayout::STATS_RESET_BTN.w, UiLayout::STATS_RESET_BTN.h, 18, pal.border);
    drawSmoothText(UIF_BODY, "RESET TRIP", UiLayout::STATS_RESET_BTN.cx(), UiLayout::STATS_RESET_BTN.cy(), textdatum_t::middle_center, pal.text, pal.surface);

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
        addDirtyRect(140, 12, 490, 38);
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
        addDirtyRect(36, 86, 168, 56);
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
        addDirtyRect(225, 86, 168, 56);
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
        addDirtyRect(415, 86, 168, 56);
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
        addDirtyRect(605, 86, 168, 56);
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
        addDirtyRect(32, 202, 220, 62);
    }

    // Tile 2: PEAK AKKU-STROM
    int cur_curr = (int)(disp_peak_curr * 10.0f + 0.5f);
    if (!_stats_inited || cur_curr != _stats_cache.peak_curr_x10) {
        _stats_cache.peak_curr_x10 = cur_curr;
        _canvas->fillRect(290, 202, 220, 62, pal.surface);
        char curBuf[16]; snprintf(curBuf, sizeof(curBuf), "%.1f A", disp_peak_curr);
        drawSmoothText(UIF_HEADING, curBuf, 290, 202, textdatum_t::top_left, pal.text, pal.surface);
        addDirtyRect(290, 202, 220, 62);
    }

    // Tile 3: PEAK PHASENSTROM
    int cur_pha = (int)(disp_peak_pha * 10.0f + 0.5f);
    if (!_stats_inited || cur_pha != _stats_cache.peak_phase_x10) {
        _stats_cache.peak_phase_x10 = cur_pha;
        _canvas->fillRect(548, 202, 220, 62, pal.surface);
        char phaBuf[16]; snprintf(phaBuf, sizeof(phaBuf), "%.1f A", disp_peak_pha);
        drawSmoothText(UIF_HEADING, phaBuf, 548, 202, textdatum_t::top_left, pal.text, pal.surface);
        addDirtyRect(548, 202, 220, 62);
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
        addDirtyRect(32, 356, 220, 68);
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
        addDirtyRect(290, 356, 220, 68);
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
        addDirtyRect(548, 356, 220, 60);
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
// SCREEN 4: VESC TOOL USB PASSTHROUGH BRIDGE (Theme-aware)
// ==============================================================================
void DashboardRenderer::initVescBridgeScreen(const DashTelemetry &telemetry) {
    (void)telemetry;
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);
    markFullDirty();

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
        addDirtyRect(38, 224, 320, 44);
    }

    if (vesc_bytes != _bridge_last_vesc_bytes) {
        _bridge_last_vesc_bytes = vesc_bytes;
        char buf[32]; fmtBytes(vesc_bytes, buf, sizeof(buf));
        _canvas->fillRect(428, 224, 320, 44, pal.surface);
        drawSmoothText(UIF_MED_NUM, buf, 428, 226, textdatum_t::top_left, pal.text, pal.surface);
        addDirtyRect(428, 224, 320, 44);
    }
}

// ==============================================================================
// SCREEN 5: VESC TOOL WIRELESS WI-FI BRIDGE (Theme-aware)
// ==============================================================================
void DashboardRenderer::initWifiBridgeScreen(const DashTelemetry &telemetry) {
    (void)telemetry;
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);
    markFullDirty();

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
        addDirtyRect(416, 112, 358, 260);
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
            addDirtyRect(570, 220, 194, 30);
        }
        if (tx_bytes != _bridge_last_wifi_tx) {
            _bridge_last_wifi_tx = tx_bytes;
            char buf[32]; fmtBytes(tx_bytes, buf, sizeof(buf));
            _canvas->fillRect(570, 260, 194, 30, pal.surface);
            drawSmoothText(UIF_HEADING, buf, 764, 262, textdatum_t::top_right, pal.text, pal.surface);
            addDirtyRect(570, 260, 194, 30);
        }
    }
}

// ==============================================================================
// SCREEN 9: WIRELESS OTA FIRMWARE UPDATE PROGRESS HUD (Theme-aware, Differential)
// ==============================================================================
void DashboardRenderer::initOtaScreen() {
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);
    markFullDirty();

    uint16_t hdrBg = blendRgb565(pal.surface, pal.primary, pal.is_light ? 22 : 36);
    drawCard(40, 24, 720, 56, hdrBg, pal.primary, 14);
    drawSmoothText(UIF_HEADING, "FIRMWARE UPDATE VIA WLAN (OTA)", 400, 52, textdatum_t::middle_center, pal.text, hdrBg);

    drawCard(40, 96, 720, 344, pal.surface, pal.border, 14);

    uint16_t warnBg = blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 22 : 42);
    drawCard(150, 116, 500, 40, warnBg, pal.accent_danger, 10);
    drawSmoothText(UIF_BODY, "BITTE FAHRZEUG NICHT AUSSCHALTEN!", 400, 136,
                   textdatum_t::middle_center, pal.accent_danger, warnBg);

    _canvas->fillRoundRect(100, 272, 600, 24, 12, pal.track);
    drawSmoothText(UIF_BODY, "Dual-Bank Safe Flash: Altes System bleibt bei Abbruch erhalten", 400, 392,
                   textdatum_t::middle_center, pal.muted, pal.surface);

    _ota_inited = true;
    _last_ota_pct = -1;
    _last_ota_status[0] = '\0';
}

void DashboardRenderer::renderOtaScreen(int pct, const char *status) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const char *safe_status = status ? status : "Übertrage Firmware...";

    if (!_ota_inited) {
        initOtaScreen();
    }

    const ColorThemePalette &pal = currentTheme();

    if (pct != _last_ota_pct) {
        _last_ota_pct = pct;
        _canvas->fillRect(260, 174, 280, 80, pal.surface);
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
        addDirtyRect(260, 174, 280, 80);
        addDirtyRect(barX, barY, barW, barH);
    }

    if (strncmp(_last_ota_status, safe_status, sizeof(_last_ota_status) - 1) != 0) {
        strncpy(_last_ota_status, safe_status, sizeof(_last_ota_status) - 1);
        _last_ota_status[sizeof(_last_ota_status) - 1] = '\0';
        _canvas->fillRect(60, 314, 680, 44, pal.surface);
        drawSmoothText(UIF_HEADING, _last_ota_status, 400, 336,
                       textdatum_t::middle_center, pal.text, pal.surface);
        addDirtyRect(60, 314, 680, 44);
    }
}
