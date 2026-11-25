#include "dash_renderer_internal.h"

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

    auto drawKeypadButton = [&](int r, int c, bool pressed) {
        UiRect kr = UiLayout::pinKeyRect(r, c);
        int bx = kr.x;
        int by = kr.y;
        int btn_w = kr.w;
        int btn_h = kr.h;
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
        markFullDirty();

        if (alarm) {
            // Crisp high-visibility crimson border frame
            for (int b = 0; b < 4; b++) {
                _canvas->drawRect(b, b, 800 - 2 * b, 480 - 2 * b, pal.accent_danger);
            }
            drawCard(150, 14, 500, 48, pal.accent_danger, pal.accent_danger, 14);
            drawSmoothText(UIF_HEADING, "DIEBSTAHL-ALARM AKTIV!", 400, 38,
                           textdatum_t::middle_center, onPrimaryFg(pal), pal.accent_danger);

            char moveBuf[80];
            float moved_m = (isnan(telemetry.locked_moved_meters) || isinf(telemetry.locked_moved_meters)) ? 0.0f : telemetry.locked_moved_meters;
            snprintf(moveBuf, sizeof(moveBuf), "BEWEGUNG ERKANNT (%.1f m) — FOC MOTOR-SIRENE AKTIV", moved_m);
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
        bool valid_bat = (!isnan(telemetry.voltage) && !isinf(telemetry.voltage) &&
                          !isnan(telemetry.battery_pct) && !isinf(telemetry.battery_pct) &&
                          telemetry.voltage > 10.0f && telemetry.battery_pct >= 0.0f);
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
            addDirtyRect(px, py, pw, ph);
        }
    }

    // Instant tactile pressed visual feedback
    int8_t active_key = -1;
    if (Buttons.isTouching() && !err) {
        active_key = UiLayout::pinKeypadHit(Buttons.getTouchX(), Buttons.getTouchY());
    }
    if (active_key != _last_pin_active_key) {
        if (_last_pin_active_key >= 0) {
            int r = _last_pin_active_key / 3;
            int c = _last_pin_active_key % 3;
            drawKeypadButton(r, c, false);
            UiRect kr = UiLayout::pinKeyRect(r, c);
            addDirtyRect(kr.x, kr.y, kr.w, kr.h);
        }
        if (active_key >= 0) {
            int r = active_key / 3;
            int c = active_key % 3;
            drawKeypadButton(r, c, true);
            UiRect kr = UiLayout::pinKeyRect(r, c);
            addDirtyRect(kr.x, kr.y, kr.w, kr.h);
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
        addDirtyRect(160, 86, 480, 72);
    }
}


// ==============================================================================
// SCREEN 7: FIRST-TIME SETUP & ONBOARDING WIZARD (Theme-aware + Pill Toggles)
// ==============================================================================
void DashboardRenderer::resetSetupWizard() {
    _wizard_inited = false;
    _last_wizard_step = -1;
    _last_wizard_pin_len = -1;
    _last_wizard_gps = -1;
    _last_wizard_incline = -1;
    _last_wizard_wifi = -1;
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
        _last_wizard_gps = -1;
        _last_wizard_incline = -1;
        _last_wizard_wifi = -1;
        _wizard_inited = true;
    }
}

void DashboardRenderer::renderSetupWizard(const DashTelemetry &telemetry) {
    (void)telemetry;
    const ColorThemePalette &pal = currentTheme();
    bool step_changed = (_wizard_step != _last_wizard_step) || !_wizard_inited;
    if (step_changed) {
        _canvas->fillScreen(pal.bg);
        markFullDirty();
        _last_wizard_step = _wizard_step;
        _wizard_inited = true;
        _last_wizard_pin_len = -1;
        _last_wizard_gps = -1;
        _last_wizard_incline = -1;
        _last_wizard_wifi = -1;
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
            drawSmoothText(UIF_BODY, "OK", bx + kw / 2, by + kh / 2,
                           textdatum_t::middle_center, okFg, okBg);

            if (pinLen == 4) {
                _canvas->fillRoundRect(mx + 20, 396, mw - 40, 52, 12, pal.accent_ok);
                drawSmoothText(UIF_HEADING, "PIN ÜBERNEHMEN & WEITER ->", 400, 422,
                               textdatum_t::middle_center, onPrimaryFg(pal), pal.accent_ok);
            } else if (_wizard_pin_warn) {
                uint16_t warnBg = blendRgb565(pal.surface_alt, pal.accent_danger, 32);
                _canvas->fillRoundRect(mx + 20, 396, mw - 40, 52, 12, warnBg);
                _canvas->drawRoundRect(mx + 20, 396, mw - 40, 52, 12, pal.accent_danger);
                drawSmoothText(UIF_BODY, "BITTE GENAU 4 ZIFFERN EINGEBEN!", 400, 422,
                               textdatum_t::middle_center, pal.accent_danger, warnBg);
            } else {
                _canvas->fillRoundRect(mx + 20, 396, mw - 40, 52, 12, pal.surface_alt);
                _canvas->drawRoundRect(mx + 20, 396, mw - 40, 52, 12, pal.border);
                drawSmoothText(UIF_BODY, "4 ZIFFERN EINGEBEN (ODER OBEN SKIP)", 400, 422,
                               textdatum_t::middle_center, pal.muted, pal.surface_alt);
            }
            addDirtyRect(mx, my, mw, mh);
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

        int8_t cur_gps = Settings.get().gps_enabled ? 1 : 0;
        if (step_changed || cur_gps != _last_wizard_gps) {
            _last_wizard_gps = cur_gps;
            drawPillToggle(700, 106, cur_gps != 0, pal);
            addDirtyRect(700, 106, 60, 32);
        }
        int8_t cur_inc = Settings.get().incline_gforce_enabled ? 1 : 0;
        if (step_changed || cur_inc != _last_wizard_incline) {
            _last_wizard_incline = cur_inc;
            drawPillToggle(700, 200, cur_inc != 0, pal);
            addDirtyRect(700, 200, 60, 32);
        }
        int8_t cur_wifi = Settings.get().wifi_bridge_enabled ? 1 : 0;
        if (step_changed || cur_wifi != _last_wizard_wifi) {
            _last_wizard_wifi = cur_wifi;
            drawPillToggle(700, 294, cur_wifi != 0, pal);
            addDirtyRect(700, 294, 60, 32);
        }
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
                _wizard_pin_warn = false;
                if (len > 0) {
                    _wizard_pin_buf[len - 1] = '\0';
                    strncpy(telemetry.wizard_pin_buf, _wizard_pin_buf, sizeof(telemetry.wizard_pin_buf) - 1);
                    telemetry.wizard_pin_buf[sizeof(telemetry.wizard_pin_buf) - 1] = '\0';
                    _screen_dirty = true;
                }
            } else if (key == '#') {
                if (len == 4) {
                    _wizard_pin_warn = false;
                    strncpy(Settings.get().pin_code, _wizard_pin_buf, sizeof(Settings.get().pin_code) - 1);
                    Settings.get().pin_code[sizeof(Settings.get().pin_code) - 1] = '\0';
                    Settings.get().pin_lock_enabled = true;
                    Settings.save();
                    _wizard_step = 2;
                    telemetry.wizard_step = 2;
                    _screen_dirty = true;
                } else {
                    _wizard_pin_warn = true;
                    _last_wizard_pin_len = -1;
                    _screen_dirty = true;
                }
            } else if (key >= '0' && key <= '9') {
                _wizard_pin_warn = false;
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
                _wizard_pin_warn = false;
                strncpy(Settings.get().pin_code, _wizard_pin_buf, sizeof(Settings.get().pin_code) - 1);
                Settings.get().pin_code[sizeof(Settings.get().pin_code) - 1] = '\0';
                Settings.get().pin_lock_enabled = true;
                Settings.save();
                _wizard_step = 2;
                telemetry.wizard_step = 2;
                _screen_dirty = true;
            } else {
                _wizard_pin_warn = true;
                _last_wizard_pin_len = -1;
                _screen_dirty = true;
            }
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

