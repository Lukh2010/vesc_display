#include "dash_renderer_internal.h"

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

    // Keep PB records consistent when split targets change
    uint8_t cur_sp1 = Settings.get().race_split1_kmh;
    uint8_t cur_sp2 = Settings.get().race_split2_kmh;
    uint8_t cur_sp3 = Settings.get().race_split3_kmh;
    if (_pb_split1_kmh == 0) _pb_split1_kmh = cur_sp1;
    if (_pb_split2_kmh == 0) _pb_split2_kmh = cur_sp2;
    if (_pb_split3_kmh == 0) _pb_split3_kmh = cur_sp3;
    if (cur_sp1 != _pb_split1_kmh || cur_sp2 != _pb_split2_kmh || cur_sp3 != _pb_split3_kmh) {
        if (cur_sp1 != _pb_split1_kmh) { telemetry.race_records.best_0_22_sec = 0.0f; _pb_split1_kmh = cur_sp1; }
        if (cur_sp2 != _pb_split2_kmh) { telemetry.race_records.best_0_50_sec = 0.0f; _pb_split2_kmh = cur_sp2; }
        if (cur_sp3 != _pb_split3_kmh) { telemetry.race_records.best_0_70_sec = 0.0f; _pb_split3_kmh = cur_sp3; }
        Settings.saveRaceRecords(telemetry.race_records);
    }

    auto finishRaceRun = [&]() {
        telemetry.race_state = 3; // FINISHED
        telemetry.boost_active = false;
        ModeManager::instance().setBoostActive(false);
        telemetry.ride_mode = telemetry.saved_ride_mode;
        ModeManager::instance().setRideMode(telemetry.saved_ride_mode);
        Buttons.setRaceState(3);
        markScreenDirty();
    };

    if (telemetry.race_state == 1) { // 1 = ARMED / COUNTDOWN (Auto-Rollout at >= 1.0 km/h)
        telemetry.boost_active = true;
        ModeManager::instance().setBoostActive(true);
        telemetry.boost_end_ms = now + 10000;
        uint32_t elapsed_cd = now - telemetry.race_countdown_start_ms;
        if (elapsed_cd > 5000) elapsed_cd = 5000;
        telemetry.race_elapsed_sec = (float)elapsed_cd / 1000.0f;
        if (telemetry.speed_kmh >= 1.0f) {
            telemetry.race_state = 2; // 2 = MEASURING (Auto-Rollout triggered!)
            telemetry.race_launch_start_ms = now;
            telemetry.race_elapsed_sec = 0.0f;
            telemetry.race_split_0_22 = 0.0f;
            telemetry.race_split_0_50 = 0.0f;
            telemetry.race_split_0_70 = 0.0f;
            _race_plateau_start_ms = 0;
            _race_last_eval_ms = now;
            _race_last_eval_speed_kmh = telemetry.speed_kmh;
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

        uint8_t sp1 = cur_sp1;
        uint8_t sp2 = cur_sp2;
        uint8_t sp3 = cur_sp3;

        if (sp1 > 0 && telemetry.race_split_0_22 <= 0.0f && telemetry.speed_kmh >= (float)sp1) {
            telemetry.race_split_0_22 = (telemetry.race_elapsed_sec > 0.01f) ? telemetry.race_elapsed_sec : 0.01f;
            if (telemetry.race_records.best_0_22_sec <= 0.01f || telemetry.race_split_0_22 < telemetry.race_records.best_0_22_sec) {
                telemetry.race_records.best_0_22_sec = telemetry.race_split_0_22;
                Settings.saveRaceRecords(telemetry.race_records);
            }
        }

        if (sp2 > sp1 && telemetry.race_split_0_50 <= 0.0f && telemetry.speed_kmh >= (float)sp2) {
            telemetry.race_split_0_50 = (telemetry.race_elapsed_sec > 0.01f) ? telemetry.race_elapsed_sec : 0.01f;
            if (telemetry.race_records.best_0_50_sec <= 0.01f || telemetry.race_split_0_50 < telemetry.race_records.best_0_50_sec) {
                telemetry.race_records.best_0_50_sec = telemetry.race_split_0_50;
                Settings.saveRaceRecords(telemetry.race_records);
            }
        }

        if (sp3 > sp2 && telemetry.race_split_0_70 <= 0.0f && telemetry.speed_kmh >= (float)sp3) {
            telemetry.race_split_0_70 = (telemetry.race_elapsed_sec > 0.01f) ? telemetry.race_elapsed_sec : 0.01f;
            if (telemetry.race_records.best_0_70_sec <= 0.01f || telemetry.race_split_0_70 < telemetry.race_records.best_0_70_sec) {
                telemetry.race_records.best_0_70_sec = telemetry.race_split_0_70;
                Settings.saveRaceRecords(telemetry.race_records);
            }
            finishRaceRun();
        }

        // Auto-finish on speed plateau (accel < 0.2 m/s^2 over 2.0 s above Split 1)
        if (telemetry.race_state == 2 && (uint32_t)(now - _race_last_eval_ms) >= 200) {
            float dt_s = (float)(now - _race_last_eval_ms) / 1000.0f;
            if (dt_s < 0.05f) dt_s = 0.05f;
            float accel_ms2 = ((telemetry.speed_kmh - _race_last_eval_speed_kmh) / 3.6f) / dt_s;
            _race_last_eval_ms = now;
            _race_last_eval_speed_kmh = telemetry.speed_kmh;

            if (telemetry.race_split_0_22 > 0.0f && accel_ms2 < 0.2f) {
                if (_race_plateau_start_ms == 0) {
                    _race_plateau_start_ms = now;
                } else if ((uint32_t)(now - _race_plateau_start_ms) >= 2000) {
                    finishRaceRun();
                }
            } else {
                _race_plateau_start_ms = 0;
            }
        }

        if (telemetry.race_state == 2 && telemetry.race_dyno.peak_speed_kmh >= 25.0f && telemetry.race_elapsed_sec > 2.0f && telemetry.speed_kmh < 2.0f) {
            finishRaceRun();
        }

        if (telemetry.race_state == 2 && telemetry.race_elapsed_sec >= 15.0f) {
            finishRaceRun();
        }
    }
}

void DashboardRenderer::initRaceTimerScreen(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();
    _canvas->fillScreen(pal.bg);
    markFullDirty();
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
    _canvas->fillRoundRect(UiLayout::RACE_BACK_BTN.x, UiLayout::RACE_BACK_BTN.y, UiLayout::RACE_BACK_BTN.w, UiLayout::RACE_BACK_BTN.h, 17, pal.surface);
    _canvas->drawRoundRect(UiLayout::RACE_BACK_BTN.x, UiLayout::RACE_BACK_BTN.y, UiLayout::RACE_BACK_BTN.w, UiLayout::RACE_BACK_BTN.h, 17, pal.border);
    drawSmoothText(UIF_BODY, "< DASH", UiLayout::RACE_BACK_BTN.cx(), UiLayout::RACE_BACK_BTN.cy(), textdatum_t::middle_center, pal.text, pal.surface);

    // Top [ DIAGRAMM ]
    _canvas->fillRoundRect(UiLayout::RACE_DYNO_BTN.x, UiLayout::RACE_DYNO_BTN.y, UiLayout::RACE_DYNO_BTN.w, UiLayout::RACE_DYNO_BTN.h, 17, pal.surface);
    _canvas->drawRoundRect(UiLayout::RACE_DYNO_BTN.x, UiLayout::RACE_DYNO_BTN.y, UiLayout::RACE_DYNO_BTN.w, UiLayout::RACE_DYNO_BTN.h, 17, pal.primary);
    drawSmoothText(UIF_BODY, "DIAGRAMM", UiLayout::RACE_DYNO_BTN.cx(), UiLayout::RACE_DYNO_BTN.cy(), textdatum_t::middle_center, pal.primary, pal.surface);

    // Top-Right [ BESTZEITEN ]
    _canvas->fillRoundRect(UiLayout::RACE_RECORDS_BTN.x, UiLayout::RACE_RECORDS_BTN.y, UiLayout::RACE_RECORDS_BTN.w, UiLayout::RACE_RECORDS_BTN.h, 17, pal.surface);
    _canvas->drawRoundRect(UiLayout::RACE_RECORDS_BTN.x, UiLayout::RACE_RECORDS_BTN.y, UiLayout::RACE_RECORDS_BTN.w, UiLayout::RACE_RECORDS_BTN.h, 17, pal.border);
    drawSmoothText(UIF_BODY, "BESTZEITEN", UiLayout::RACE_RECORDS_BTN.cx(), UiLayout::RACE_RECORDS_BTN.cy(), textdatum_t::middle_center, pal.text, pal.surface);

    // Speedometer Center Arc Gauge (270° sweep: 135° to 405°, crisp clearance above Launch button)
    constexpr int cx = 310, cy = 224;
    constexpr int r_in = 130, r_out = 158;
    constexpr int s_cap_x = 208, s_cap_y = 326;
    constexpr int e_cap_x = 412, e_cap_y = 326;
    _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, 405.0f, pal.track);
    _canvas->fillCircle(s_cap_x, s_cap_y, 14, pal.track);
    _canvas->fillCircle(e_cap_x, e_cap_y, 14, pal.track);

    drawSmoothText(UIF_HEADING, "km/h", cx, cy + 34, textdatum_t::top_center, pal.muted, pal.bg);

    char activeSplitsHint[32];
    snprintf(activeSplitsHint, sizeof(activeSplitsHint), "Ziele: %d / %d / %d km/h",
             Settings.get().race_split1_kmh, Settings.get().race_split2_kmh, Settings.get().race_split3_kmh);
    drawSmoothText(UIF_SMALL, activeSplitsHint, cx, 430, textdatum_t::top_center, pal.muted, pal.bg);

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
    drawGForceBadge(telemetry.g_force, 50, 416, 27, pal);
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

    constexpr int cx = 310, cy = 224;
    constexpr int r_in = 130, r_out = 158;
    constexpr int s_cap_x = 208, s_cap_y = 326;

    int cur_cd = (telemetry.race_state == 1) ? (5 - (int)telemetry.race_elapsed_sec) : -1;
    if (telemetry.race_state == 1) {
        if (cur_cd < 0) cur_cd = 0;
        if (cur_cd > 5) cur_cd = 5;
    }

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
            char cdTitle[28];
            if (cur_cd == 0) {
                snprintf(cdTitle, sizeof(cdTitle), "BEREIT — LOSFAHREN!");
            } else {
                snprintf(cdTitle, sizeof(cdTitle), "LAUNCH IN %d s", cur_cd);
            }
            drawSmoothText(UIF_HEADING, cdTitle, 304, 30,
                           textdatum_t::middle_center,
                           (cur_cd == 0) ? pal.accent_ok : pal.accent_warn, pal.bg);
        } else {
            drawSmoothText(UIF_HEADING, "LAUNCH CONTROL", 304, 30,
                           textdatum_t::middle_center, pal.muted, pal.bg);
        }
        addDirtyRect(134, 8, 336, 44);
    }

    // 2. Speedometer Arc (Differential Update, 270° sweep: 135° to 405°)
    float spd = telemetry.speed_kmh;
    if (spd < 0.8f) spd = 0.0f;
    float frac = std::min(spd / 100.0f, 1.0f);
    float target_angle = (frac <= 0.005f) ? 135.0f : (135.0f + frac * 270.0f);

    uint16_t arcCol = (telemetry.race_state == 1) ? pal.accent_warn : pal.primary;

    bool race_arc_dirty = false;
    if (target_angle <= 135.0f && _last_race_arc_angle > 135.0f) {
        _canvas->fillArc(cx, cy, r_in, r_out, 135.0f, _last_race_arc_angle + 1.0f, pal.track);
        _canvas->fillCircle(s_cap_x, s_cap_y, 14, pal.track);
        _last_race_arc_angle = 135.0f;
        race_arc_dirty = true;
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
        race_arc_dirty = true;
    }
    if (race_arc_dirty) {
        addDirtyRect(cx - 162, cy - 162, 324, 324);
    }

    // Hero Digital Speed inside Ring
    int cur_spd_int = (int)(spd + 0.5f);
    if (cur_spd_int != _last_race_speed) {
        _last_race_speed = cur_spd_int;
        char spdBuf[16];
        snprintf(spdBuf, sizeof(spdBuf), "%d", cur_spd_int);
        _canvas->fillRect(cx - 88, cy - 48, 176, 68, pal.bg);
        drawSmoothText(UIF_HERO_NUM, spdBuf, cx, cy - 14, textdatum_t::middle_center, pal.text, pal.bg);
        addDirtyRect(cx - 88, cy - 48, 176, 68);
    }

    // 3. Center Action Button cleanly below the horseshoe ring (ZERO arc overlap!)
    bool btn_needs_update = state_changed || (telemetry.race_state == 1 && cur_cd != _last_race_countdown);
    if (btn_needs_update) {
        const UiRect &lb = UiLayout::RACE_LAUNCH_BTN;
        const int btnY = lb.y;
        const int btnH = lb.h;
        _canvas->fillRect(lb.x - 10, btnY - 4, lb.w + 20, btnH + 8, pal.bg);
        if (telemetry.race_state == 0) {
            if (isRaceThermalBlocked(telemetry)) {
                uint16_t warnBg = blendRgb565(pal.surface, pal.accent_warn, pal.is_light ? 25 : 45);
                _canvas->fillRoundRect(lb.x, btnY, lb.w, btnH, 24, warnBg);
                _canvas->drawRoundRect(lb.x, btnY, lb.w, btnH, 24, pal.accent_warn);
                drawSmoothText(UIF_BODY, "TEMP > 65°C", lb.cx(), lb.cy(), textdatum_t::middle_center, pal.accent_warn, warnBg);
            } else {
                _canvas->fillRoundRect(lb.x, btnY, lb.w, btnH, 24, pal.primary);
                drawSmoothText(UIF_BODY, "START LAUNCH", lb.cx(), lb.cy(), textdatum_t::middle_center, onPrimaryFg(pal), pal.primary);
            }
        } else if (telemetry.race_state == 1) { // ARMED / COUNTDOWN (Auto-Rollout)
            char cdBuf[24];
            if (cur_cd == 0) {
                snprintf(cdBuf, sizeof(cdBuf), "ROLLOUT >= 1 km/h");
            } else if (cur_cd == 1) {
                snprintf(cdBuf, sizeof(cdBuf), "1 ... GO!");
            } else {
                snprintf(cdBuf, sizeof(cdBuf), "%d ...", cur_cd);
            }
            uint16_t armCol = (cur_cd == 0) ? pal.accent_ok : pal.accent_warn;
            uint16_t armBg = blendRgb565(pal.surface, armCol, pal.is_light ? 26 : 48);
            _canvas->fillRoundRect(lb.x, btnY, lb.w, btnH, 24, armBg);
            _canvas->drawRoundRect(lb.x, btnY, lb.w, btnH, 24, armCol);
            drawSmoothText(UIF_BODY, cdBuf, lb.cx(), lb.cy(), textdatum_t::middle_center, armCol, armBg);
        } else if (telemetry.race_state == 2) { // MEASURING
            uint16_t runBg = blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 26 : 48);
            _canvas->fillRoundRect(lb.x, btnY, lb.w, btnH, 24, runBg);
            _canvas->drawRoundRect(lb.x, btnY, lb.w, btnH, 24, pal.accent_danger);
            drawSmoothText(UIF_BODY, "STOP / ABORT", lb.cx(), lb.cy(), textdatum_t::middle_center, pal.accent_danger, runBg);
        } else { // FINISHED
            _canvas->fillRoundRect(lb.x, btnY, lb.w, btnH, 24, pal.surface);
            _canvas->drawRoundRect(lb.x, btnY, lb.w, btnH, 24, pal.primary);
            drawSmoothText(UIF_BODY, "NEUER LAUNCH", lb.cx(), lb.cy(), textdatum_t::middle_center, pal.primary, pal.surface);
        }
        addDirtyRect(lb.x - 10, btnY - 4, lb.w + 20, btnH + 8);
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
        addDirtyRect(548, y, 218, 40);
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
        drawGForceBadge(telemetry.g_force, 50, 416, 27, pal);
        addDirtyRect(22, 388, 56, 56);
    }
}

void DashboardRenderer::drawRaceRecordsModal(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();
    const UiRect &rm = UiLayout::RACE_REC_MODAL;
    int mx = rm.x, my = rm.y, mw = rm.w, mh = rm.h;
    addDirtyRect(mx, my, mw, mh);
    drawCard(mx, my, mw, mh, pal.surface, pal.accent_warn, 16);
    _canvas->drawRoundRect(mx + 1, my + 1, mw - 2, mh - 2, 15, pal.accent_warn);

    drawSmoothText(UIF_HEADING, "PERSÖNLICHE BESTZEITEN", mx + 24, my + 16, textdatum_t::top_left, pal.accent_warn, pal.surface);
    char goalBuf[40];
    snprintf(goalBuf, sizeof(goalBuf), "Aktive Schwellen: %d / %d / %d km/h",
             Settings.get().race_split1_kmh, Settings.get().race_split2_kmh, Settings.get().race_split3_kmh);
    drawSmoothText(UIF_SMALL, goalBuf, mx + 24, my + 44, textdatum_t::top_left, pal.muted, pal.surface);

    const UiRect &rc = UiLayout::RACE_REC_CLOSE_BTN;
    _canvas->fillRoundRect(rc.x, rc.y, rc.w, rc.h, 8, pal.surface_alt);
    _canvas->drawRoundRect(rc.x, rc.y, rc.w, rc.h, 8, pal.border);
    drawSmoothText(UIF_BODY, "X", rc.cx(), rc.cy(), textdatum_t::middle_center, pal.text, pal.surface_alt);

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

    const UiRect &rb = UiLayout::RACE_REC_RESET_BTN;
    uint16_t resetBg = blendRgb565(pal.surface, pal.accent_danger, pal.is_light ? 22 : 44);
    _canvas->fillRoundRect(rb.x, rb.y, rb.w, rb.h, 12, resetBg);
    _canvas->drawRoundRect(rb.x, rb.y, rb.w, rb.h, 12, pal.accent_danger);
    drawSmoothText(UIF_BODY, "RESET", rb.cx(), rb.cy(), textdatum_t::middle_center, pal.accent_danger, resetBg);

    const UiRect &db = UiLayout::RACE_REC_DONE_BTN;
    _canvas->fillRoundRect(db.x, db.y, db.w, db.h, 12, pal.surface_alt);
    _canvas->drawRoundRect(db.x, db.y, db.w, db.h, 12, pal.border);
    drawSmoothText(UIF_BODY, "SCHLIESSEN", db.cx(), db.cy(), textdatum_t::middle_center, pal.text, pal.surface_alt);
}

void DashboardRenderer::drawRaceDynoModal(const DashTelemetry &telemetry) {
    const ColorThemePalette &pal = currentTheme();
    const UiRect &dm = UiLayout::RACE_DYNO_MODAL;
    int mx = dm.x, my = dm.y, mw = dm.w, mh = dm.h;
    addDirtyRect(mx, my, mw, mh);

    // Dark modal surface card with primary border
    drawCard(mx, my, mw, mh, pal.surface, pal.primary, 16);
    _canvas->drawRoundRect(mx + 1, my + 1, mw - 2, mh - 2, 15, pal.primary);

    // Modal Header
    drawSmoothText(UIF_HEADING, "LEISTUNGSDIAGRAMM & TELEMETRIE", mx + 22, my + 16, textdatum_t::top_left, pal.primary, pal.surface);

    // Top-Right Close Button [ X ]
    const UiRect &dc = UiLayout::RACE_DYNO_CLOSE_BTN;
    _canvas->fillRoundRect(dc.x, dc.y, dc.w, dc.h, 8, pal.surface_alt);
    _canvas->drawRoundRect(dc.x, dc.y, dc.w, dc.h, 8, pal.border);
    drawSmoothText(UIF_BODY, "X", dc.cx(), dc.cy(), textdatum_t::middle_center, pal.text, pal.surface_alt);

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
    const UiRect &dd = UiLayout::RACE_DYNO_DONE_BTN;
    _canvas->fillRoundRect(dd.x, dd.y, dd.w, dd.h, 12, pal.surface_alt);
    _canvas->drawRoundRect(dd.x, dd.y, dd.w, dd.h, 12, pal.border);
    drawSmoothText(UIF_BODY, "SCHLIESSEN", dd.cx(), dd.cy(),
                   textdatum_t::middle_center, pal.text, pal.surface_alt);
}

