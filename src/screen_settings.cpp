#include "dash_renderer_internal.h"

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
        _cache.invalidate();
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
        markFullDirty();

        // 1. Sleek Header Bar: [< DASH], Title, [FERTIG]
        _canvas->fillRoundRect(UiLayout::SETTINGS_BACK_BTN.x, UiLayout::SETTINGS_BACK_BTN.y, UiLayout::SETTINGS_BACK_BTN.w, UiLayout::SETTINGS_BACK_BTN.h, 17, pal.surface);
        _canvas->drawRoundRect(UiLayout::SETTINGS_BACK_BTN.x, UiLayout::SETTINGS_BACK_BTN.y, UiLayout::SETTINGS_BACK_BTN.w, UiLayout::SETTINGS_BACK_BTN.h, 17, pal.border);
        drawSmoothText(UIF_BODY, "< DASH", UiLayout::SETTINGS_BACK_BTN.cx(), UiLayout::SETTINGS_BACK_BTN.cy(), textdatum_t::middle_center, pal.text, pal.surface);

        drawSmoothText(UIF_HEADING, "EINSTELLUNGEN", 400, 29, textdatum_t::middle_center, pal.text, pal.bg);

        _canvas->fillRoundRect(UiLayout::SETTINGS_DONE_BTN.x, UiLayout::SETTINGS_DONE_BTN.y, UiLayout::SETTINGS_DONE_BTN.w, UiLayout::SETTINGS_DONE_BTN.h, 17, pal.primary);
        drawSmoothText(UIF_BODY, "FERTIG", UiLayout::SETTINGS_DONE_BTN.cx(), UiLayout::SETTINGS_DONE_BTN.cy(), textdatum_t::middle_center, onPrimaryFg(pal), pal.primary);

        _settings_inited = true;
    } else {
        addDirtyRect(0, _pin_modal_open ? 26 : 54, 800, _pin_modal_open ? 454 : 426);
    }

    // 2. Segmented Tab Bar (Stealth-safe: Legal = 3 tabs, Tuning = 5 tabs)
    static const char *ALL_TAB_NAMES[TAB_COUNT] = { "SPEED", "DRAGY", "DASH", "SYSTEM", "PIN" };
    uint16_t tabAccents[TAB_COUNT] = {
        pal.primary, pal.accent_warn, pal.primary, pal.primary, pal.primary
    };

    if (tabChanged) {
        _last_rendered_tab = _settings_tab_idx;
        _cache.invalidate();
        _last_fault_flash = -1;

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
    int mx = 160, my = 26, mw = 480, mh = 426;
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
    int kw = 104, kh = 50;
    int kx0 = mx + (mw - (3 * kw + 2 * 14)) / 2;
    int ky0 = my + 124;

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
        int mx = 160, my = 26, mw = 480;
        if ((tx >= mx + mw - 55 && tx <= mx + mw && ty >= my && ty <= my + 52) ||
            _pin_modal_success ||
            (tx < mx || tx > mx + mw || ty < my || ty > 475)) {
            _pin_modal_open = false;
            _settings_inited = false;
            _last_rendered_tab = 255;
            _cache.invalidate();
            _menu_dirty = true;
            return true;
        }

        int kw = 104, kh = 50;
        int kx0 = mx + (mw - (3 * kw + 2 * 14)) / 2;
        int ky0 = my + 124;

        // Continuous keypad hitbox mapping without dead zones (bottom row tolerant up to Y=475)
        int c = -1;
        if (tx >= kx0 - 10 && tx < kx0 + kw + 7) c = 0;
        else if (tx >= kx0 + kw + 7 && tx < kx0 + 2 * kw + 21) c = 1;
        else if (tx >= kx0 + 2 * kw + 21 && tx <= kx0 + 3 * kw + 38) c = 2;

        int r = -1;
        if (ty >= ky0 - 6 && ty < ky0 + kh + 5) r = 0;
        else if (ty >= ky0 + kh + 5 && ty < ky0 + 2 * kh + 15) r = 1;
        else if (ty >= ky0 + 2 * kh + 15 && ty < ky0 + 3 * kh + 25) r = 2;
        else if (ty >= ky0 + 3 * kh + 25 && ty <= 475) r = 3;

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
        _cache.invalidate();
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
            bool s1_mod = false, s2_mod = false, s3_mod = false;
            // Split 1 (cx=20, cw=244): [-] 38..134, [+] 150..246
            if (tx >= 25 && tx <= 140) {
                if (Settings.get().race_split1_kmh > 10) {
                    Settings.get().race_split1_kmh--;
                    s1_mod = true;
                }
            } else if (tx >= 142 && tx <= 260) {
                if (Settings.get().race_split1_kmh < Settings.get().race_split2_kmh - 1) {
                    Settings.get().race_split1_kmh++;
                    s1_mod = true;
                }
            }
            // Split 2 (cx=278, cw=244): [-] 296..392, [+] 408..504
            else if (tx >= 282 && tx <= 398) {
                if (Settings.get().race_split2_kmh > Settings.get().race_split1_kmh + 1) {
                    Settings.get().race_split2_kmh--;
                    s2_mod = true;
                }
            } else if (tx >= 400 && tx <= 518) {
                if (Settings.get().race_split2_kmh < Settings.get().race_split3_kmh - 1) {
                    Settings.get().race_split2_kmh++;
                    s2_mod = true;
                }
            }
            // Split 3 (cx=536, cw=244): [-] 554..650, [+] 666..762
            else if (tx >= 540 && tx <= 656) {
                if (Settings.get().race_split3_kmh > Settings.get().race_split2_kmh + 1) {
                    Settings.get().race_split3_kmh--;
                    s3_mod = true;
                }
            } else if (tx >= 658 && tx <= 776) {
                if (Settings.get().race_split3_kmh < 130) {
                    Settings.get().race_split3_kmh++;
                    s3_mod = true;
                }
            }
            if (s1_mod || s2_mod || s3_mod) {
                if (s1_mod) { telemetry.race_records.best_0_22_sec = 0.0f; _pb_split1_kmh = Settings.get().race_split1_kmh; }
                if (s2_mod) { telemetry.race_records.best_0_50_sec = 0.0f; _pb_split2_kmh = Settings.get().race_split2_kmh; }
                if (s3_mod) { telemetry.race_records.best_0_70_sec = 0.0f; _pb_split3_kmh = Settings.get().race_split3_kmh; }
                Settings.saveRaceRecords(telemetry.race_records);
                _menu_dirty = true;
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

