#include "tcs_tpms_manager.h"
#include "vesc_uart.h"
#include "mode_manager.h"
#include <math.h>

TcsTpmsManager::TcsTpmsManager() {
    reset();
}

void TcsTpmsManager::reset() {
    _tcs_enabled = false;
    _tcs_active = false;
    _slip_speed_kmh = 0.0f;
    _slip_ratio = 0.0f;
    _throttle_scale = 1.0f;
    _prev_v_vesc = 0.0f;
    _has_prev_v_vesc = false;
    _phys_ref_speed_kmh = 0.0f;
    _filtered_dv_dt = 0.0f;
    _spin_accel_duration_sec = 0.0f;
    _last_slip_active_ms = 0;
    _sim_slip_override = -1.0f;

    _steady_speed_ref = 0.0f;
    _steady_timer_sec = 0.0f;
    _calib_converge_sec = 0.0f;
    _calibrated_diam_in = Settings.get().wheel_diameter_in;
    if (_calibrated_diam_in < 8.0f || _calibrated_diam_in > 16.0f) {
        _calibrated_diam_in = 11.0f; // Default Segway G3 Max
    }
    _has_calibrated_baseline = Settings.get().wheel_gps_calibrated;
    _manual_calib_active = false;
    _last_calib_eval_ms = 0;

    _baseline_diam_in = _calibrated_diam_in;
    _rolling_diam_in = _calibrated_diam_in;
    _tpms_warning = false;
    _tpms_drift_pct = 0.0f;
    _tpms_drop_duration_sec = 0.0f;
    _tpms_recovery_duration_sec = 0.0f;
    _sim_drop_pct_override = -1.0f;
}

void TcsTpmsManager::setTcsEnabled(bool enabled) {
    _tcs_enabled = enabled;
    if (Settings.get().tcs_enabled != enabled) {
        Settings.get().tcs_enabled = enabled;
        Settings.save();
    }
    if (!enabled) {
        _tcs_active = false;
        _throttle_scale = 1.0f;
        _spin_accel_duration_sec = 0.0f;
    }
}

void TcsTpmsManager::startManualCalibration() {
    _manual_calib_active = true;
    _steady_timer_sec = 0.0f;
    _calib_converge_sec = 0.0f;
    _tpms_warning = false;
    _tpms_drop_duration_sec = 0.0f;
    _tpms_recovery_duration_sec = 0.0f;
    _tpms_drift_pct = 0.0f;
#if !defined(NATIVE_SIMULATOR)
    Serial.println("[TCS_TPMS] Manual tire diameter calibration started. Cruise steadily >= 15 km/h with GPS fix.");
#else
    printf("[TCS_TPMS] Manual tire diameter calibration started.\n");
#endif
}

void TcsTpmsManager::begin() {
    reset();
    _tcs_enabled = Settings.get().tcs_enabled;
    _calibrated_diam_in = Settings.get().wheel_diameter_in;
    if (_calibrated_diam_in < 8.0f || _calibrated_diam_in > 16.0f) {
        _calibrated_diam_in = 11.0f;
    }
    _baseline_diam_in = _calibrated_diam_in;
    _rolling_diam_in = _calibrated_diam_in;
    _has_calibrated_baseline = Settings.get().wheel_gps_calibrated;

#if !defined(NATIVE_SIMULATOR)
    Serial.printf("[TCS_TPMS] Manager initialized. TCS=%s | Baseline wheel diam: %.1f in (GPS Calibrated: %s)\n",
                  _tcs_enabled ? "ON" : "OFF",
                  _baseline_diam_in, _has_calibrated_baseline ? "YES" : "NO");
#else
    printf("[TCS_TPMS] Manager initialized. TCS=%s | Baseline wheel diam: %.1f in (GPS Calibrated: %s)\n",
           _tcs_enabled ? "ON" : "OFF",
           _baseline_diam_in, _has_calibrated_baseline ? "YES" : "NO");
#endif
}

void TcsTpmsManager::update(DashTelemetry &telemetry, float dt_sec) {
    if (dt_sec <= 0.0001f) dt_sec = 0.010f;
    if (dt_sec > 0.25f)    dt_sec = 0.25f;

    // =========================================================================
    // 1. Wheel Speed & Physics-Bounded Reference (No circular g_force loop!)
    // =========================================================================
    float v_vesc = telemetry.vesc_motor_speed;
    if (v_vesc < 0.1f && telemetry.rpm > 30.0f) {
        float gear = Settings.get().gear_ratio > 0.1f ? Settings.get().gear_ratio : 1.0f;
        float d_in = Settings.get().wheel_diameter_in > 5.0f ? Settings.get().wheel_diameter_in : 11.0f;
        v_vesc = (telemetry.rpm / gear) * (float)M_PI * (d_in * 0.0254f) * (60.0f / 1000.0f);
    }

    uint32_t now = millis();
    bool gps_valid = telemetry.gps.valid && (telemetry.gps.satellites >= 4) &&
                     ((uint32_t)(now - telemetry.gps.last_fix_ms) < 2500);

    // Physical maximum tire-grip acceleration on street/performance e-scooter (~1.84 G = 65 km/h/s)
    static constexpr float MAX_GRIP_ACCEL_KMH_S = 65.0f;
    static constexpr float SPIN_DEBOUNCE_SEC    = 0.025f; // 25ms rejects single-frame CAN jitter
    static constexpr float GPS_SLIP_TRIGGER_KMH = 6.0f;   // High-confidence GPS diff threshold
    static constexpr float SLIP_RECOVERY_KMH    = 1.5f;

    float dv_dt = 0.0f;
    if (!_has_prev_v_vesc) {
        _prev_v_vesc = v_vesc;
        _phys_ref_speed_kmh = gps_valid ? telemetry.gps.speed_kmh : v_vesc;
        _filtered_dv_dt = 0.0f;
        _has_prev_v_vesc = true;
    } else {
        dv_dt = (v_vesc - _prev_v_vesc) / dt_sec;
        // Fast low-pass filter on wheel acceleration derivative to reject 1-frame noise
        float alpha = dt_sec / (0.015f + dt_sec);
        _filtered_dv_dt += (dv_dt - _filtered_dv_dt) * alpha;
        _prev_v_vesc = v_vesc;

        // Advance physics-bounded chassis reference speed (independent of circular VESC g_force!)
        if (v_vesc < 0.8f) {
            _phys_ref_speed_kmh = 0.0f;
        } else if (v_vesc <= _phys_ref_speed_kmh) {
            _phys_ref_speed_kmh = v_vesc;
        } else {
            float max_step = MAX_GRIP_ACCEL_KMH_S * dt_sec;
            if ((v_vesc - _phys_ref_speed_kmh) <= max_step) {
                _phys_ref_speed_kmh = v_vesc;
            } else {
                _phys_ref_speed_kmh += max_step;
            }
        }
    }

    float phys_slip = v_vesc - _phys_ref_speed_kmh;
    if (phys_slip < 0.0f) phys_slip = 0.0f;

    float gps_slip = 0.0f;
    if (gps_valid) {
        gps_slip = v_vesc - telemetry.gps.speed_kmh;
        if (gps_slip < 0.0f) gps_slip = 0.0f;
        // Keep physics reference gently anchored when GPS shows normal traction
        if (gps_slip < 1.5f) {
            _phys_ref_speed_kmh = telemetry.gps.speed_kmh;
        }
    }

    // Determine effective reported slip speed
    float slip = gps_valid ? gps_slip : phys_slip;
    if (_sim_slip_override >= 0.0f) {
        slip = _sim_slip_override;
        v_vesc = (gps_valid ? telemetry.gps.speed_kmh : _phys_ref_speed_kmh) + _sim_slip_override;
    }

    _slip_speed_kmh = slip;
    _slip_ratio = (v_vesc > 1.0f) ? (slip / v_vesc) : 0.0f;

    // =========================================================================
    // 2. Proactive Traction Control (TCS) Fast Intervention & Recovery Hysteresis
    // =========================================================================
    // Safety Guard: NEVER engage TCS or manipulate phase current when vehicle is locked!
    if (telemetry.is_locked || ModeManager::instance().isLocked() || !_tcs_enabled) {
        _tcs_active = false;
        _throttle_scale = 1.0f;
        _spin_accel_duration_sec = 0.0f;
    } else {
        bool under_accel = (telemetry.phase_amps > 8.0f || telemetry.throttle_pct > 10.0f);

        // Track sustained wheel-spin acceleration above physical tire grip limit
        if (under_accel && (_filtered_dv_dt > MAX_GRIP_ACCEL_KMH_S || dv_dt > MAX_GRIP_ACCEL_KMH_S)) {
            _spin_accel_duration_sec += dt_sec;
        } else {
            _spin_accel_duration_sec = 0.0f;
        }

        // Primary trigger: dV_wheel/dt > MAX_GRIP_ACCEL_KMH_S sustained > 25ms (or massive abrupt spin spike)
        bool dv_spin_triggered = under_accel && (
            (_spin_accel_duration_sec >= SPIN_DEBOUNCE_SEC && (phys_slip > 1.5f || _filtered_dv_dt > MAX_GRIP_ACCEL_KMH_S)) ||
            (dv_dt > 120.0f && phys_slip >= 3.5f)
        );

        // Secondary trigger: High-confidence fresh GPS slip (> 6.0 km/h) or simulator override (> 3.0 km/h)
        bool gps_spin_triggered = under_accel && gps_valid && (gps_slip >= (GPS_SLIP_TRIGGER_KMH - 0.01f));
        bool sim_spin_triggered = under_accel && (_sim_slip_override > 3.0f);

        bool spin_detected = dv_spin_triggered || gps_spin_triggered || sim_spin_triggered;

        if (spin_detected) {
            _tcs_active = true;
            _last_slip_active_ms = now;

            // Compute excess severity from slip speed or excess wheel acceleration
            float effective_slip = slip;
            if (effective_slip < 3.5f && dv_spin_triggered) {
                float excess_accel_ratio = (_filtered_dv_dt - MAX_GRIP_ACCEL_KMH_S) / 40.0f;
                if (excess_accel_ratio < 0.5f) excess_accel_ratio = 0.5f;
                effective_slip = 3.0f + excess_accel_ratio * 3.0f;
            }
            float excess_slip = effective_slip - 3.0f;
            if (excess_slip < 0.5f) excess_slip = 0.5f;

            // Aggressive non-linear reduction targeting < 5ms wheel spin suppression
            float target_scale = 1.0f - (excess_slip / (excess_slip + 2.5f)) * 0.85f;
            if (target_scale < 0.15f) target_scale = 0.15f;

            // Instant drop (< 5ms response time): cut current without rate limiting downward
            if (target_scale < _throttle_scale) {
                _throttle_scale = target_scale;
            } else {
                _throttle_scale += (target_scale - _throttle_scale) * (dt_sec * 6.0f);
            }
        } else if (!under_accel || (slip <= SLIP_RECOVERY_KMH && _filtered_dv_dt <= MAX_GRIP_ACCEL_KMH_S)) {
            // Wheel has regained traction or throttle released
            _tcs_active = false;
            if (_throttle_scale < 1.0f) {
                // Smooth recovery hysteresis: ramps phase current back up smoothly (~0.55s)
                static constexpr float RAMP_UP_RATE = 1.8f;
                _throttle_scale += RAMP_UP_RATE * dt_sec;
                if (_throttle_scale > 1.0f) _throttle_scale = 1.0f;
            }
        } else {
            // Hysteresis deadband (between 1.5 km/h and trigger threshold):
            // If TCS was already active, hold gently to prevent hunting/chattering
            if (_tcs_active) {
                _throttle_scale += (0.85f - _throttle_scale) * (dt_sec * 2.5f);
            } else if (_throttle_scale < 1.0f) {
                _throttle_scale += 1.8f * dt_sec;
                if (_throttle_scale > 1.0f) _throttle_scale = 1.0f;
            }
        }
    }

    telemetry.tcs_active = _tcs_active;
    telemetry.tcs_slip_kmh = _slip_speed_kmh;
    telemetry.tcs_throttle_scale = _throttle_scale;

    // =========================================================================
    // 3. Tire Diameter Calibration (Manual Trigger OR Initial One-Time Only)
    // =========================================================================
    // Once _has_calibrated_baseline == true, _baseline_diam_in stays FROZEN so
    // Virtual TPMS can genuinely detect gradual tire pressure loss!
    // Re-calibration only runs when explicitly triggered via startManualCalibration().
    bool allow_calibration = _manual_calib_active || (!_has_calibrated_baseline && !Settings.get().wheel_gps_calibrated);

    if (allow_calibration) {
        bool steady_cruising = (gps_valid && telemetry.gps.satellites >= 8 &&
                               telemetry.gps.speed_kmh >= 15.0f &&
                               fabsf(telemetry.g_force) < 0.08f &&
                               fabsf(telemetry.incline_deg) < 4.0f &&
                               telemetry.phase_amps >= 2.0f &&
                               telemetry.phase_amps <= 28.0f &&
                               !_tcs_active && slip < 1.0f);

        if (steady_cruising) {
            if (fabsf(telemetry.gps.speed_kmh - _steady_speed_ref) < 1.2f) {
                _steady_timer_sec += dt_sec;
            } else {
                _steady_speed_ref = telemetry.gps.speed_kmh;
                _steady_timer_sec = 0.0f;
            }

            // When steady cruising holds for >= 5.0 seconds:
            if (_steady_timer_sec >= 5.0f) {
                float gear = Settings.get().gear_ratio > 0.1f ? Settings.get().gear_ratio : 1.0f;
                float wheel_rpm = telemetry.rpm / gear;
                if (wheel_rpm > 50.0f) {
                    float v_gps_m_s = telemetry.gps.speed_kmh / 3.6f;
                    float d_eff_m = (v_gps_m_s * 60.0f) / (wheel_rpm * (float)M_PI);
                    float d_eff_in = d_eff_m / 0.0254f;

                    // Sanity range check for nominal 11" Segway G3 Max tire
                    if (d_eff_in >= 8.5f && d_eff_in <= 14.5f) {
                        static constexpr float EMA_ALPHA = 0.04f;
                        _calibrated_diam_in = EMA_ALPHA * d_eff_in + (1.0f - EMA_ALPHA) * _calibrated_diam_in;
                        _calib_converge_sec += dt_sec;

                        // Finalize & freeze baseline once converged (>= 6.0s of EMA accumulation)
                        if (_calib_converge_sec >= 6.0f) {
                            float rounded = roundf(_calibrated_diam_in * 10.0f) / 10.0f;
                            _last_calib_eval_ms = now;
                            _calibrated_diam_in = rounded;
                            _baseline_diam_in = rounded;
                            _rolling_diam_in = rounded;
                            _has_calibrated_baseline = true;
                            _manual_calib_active = false;
                            _calib_converge_sec = 0.0f;
                            _steady_timer_sec = 0.0f;

                            Settings.get().wheel_diameter_in = rounded;
                            Settings.get().wheel_gps_calibrated = true;
                            Settings.save();

                            VescHandler::instance().syncWheelDiameterToVesc(rounded);
                            ModeManager::instance().applyCurrentMode();
                            telemetry.calibrated_wheel_diam_inch = rounded;
#if !defined(NATIVE_SIMULATOR)
                            Serial.printf("[TCS_TPMS] Tire Diameter Calibrated & Baseline Frozen: %.1f in (D_eff=%.2f in)\n",
                                          rounded, d_eff_in);
#else
                            printf("[TCS_TPMS] Tire Diameter Calibrated & Baseline Frozen: %.1f in (D_eff=%.2f in)\n",
                                   rounded, d_eff_in);
#endif
                        }
                    }
                }
            }
        } else {
            _steady_timer_sec = 0.0f;
            _steady_speed_ref = telemetry.gps.speed_kmh;
        }
    } else {
        _steady_timer_sec = 0.0f;
        _calib_converge_sec = 0.0f;
    }

    // =========================================================================
    // 4. Virtual TPMS (Tire Pressure Monitoring System)
    // =========================================================================
    // Compares effective rolling diameter against the FROZEN calibrated baseline.
    // If diameter drops by >= 4% for > 30 seconds: flags tpms_warning = true.
    float baseline = (_baseline_diam_in > 5.0f) ? _baseline_diam_in :
                     (Settings.get().wheel_diameter_in > 5.0f ? Settings.get().wheel_diameter_in : 11.0f);

    float current_eff_in = _rolling_diam_in;
    bool have_tpms_sample = false;

    if (_sim_drop_pct_override >= 0.0f) {
        current_eff_in = baseline * (1.0f - (_sim_drop_pct_override / 100.0f));
        have_tpms_sample = true;
    } else if (gps_valid && telemetry.gps.satellites >= 6 && telemetry.gps.speed_kmh >= 14.0f &&
               fabsf(telemetry.g_force) < 0.10f && !_tcs_active && !_manual_calib_active) {
        float gear = Settings.get().gear_ratio > 0.1f ? Settings.get().gear_ratio : 1.0f;
        float wheel_rpm = telemetry.rpm / gear;
        if (wheel_rpm > 50.0f) {
            float v_m_s = telemetry.gps.speed_kmh / 3.6f;
            float d_m = (v_m_s * 60.0f) / (wheel_rpm * (float)M_PI);
            float measured_in = d_m / 0.0254f;
            if (measured_in >= 7.0f && measured_in <= 15.0f) {
                current_eff_in = measured_in;
                have_tpms_sample = true;
            }
        }
    }

    if (have_tpms_sample && current_eff_in >= 7.0f && current_eff_in <= 15.0f) {
        _rolling_diam_in = 0.15f * current_eff_in + 0.85f * _rolling_diam_in;
    }

    // Calculate drop percentage: (baseline - rolling) / baseline * 100%
    float drift_pct = ((baseline - _rolling_diam_in) / baseline) * 100.0f;
    if (drift_pct < 0.0f) drift_pct = 0.0f;
    _tpms_drift_pct = drift_pct;

    static constexpr float TPMS_DROP_THRESHOLD_PCT = 4.0f;
    if (!_manual_calib_active && drift_pct >= TPMS_DROP_THRESHOLD_PCT) {
        _tpms_drop_duration_sec += dt_sec;
        _tpms_recovery_duration_sec = 0.0f;
        if (_tpms_drop_duration_sec >= 30.0f) {
            _tpms_warning = true;
        }
    } else if (drift_pct < 2.5f) {
        // Recovery hysteresis: pressure restored for > 5s clears warning
        _tpms_recovery_duration_sec += dt_sec;
        if (_tpms_recovery_duration_sec >= 5.0f) {
            _tpms_warning = false;
            _tpms_drop_duration_sec = 0.0f;
        }
    } else {
        _tpms_recovery_duration_sec = 0.0f;
    }

    telemetry.tpms_warning = _tpms_warning;
    telemetry.tpms_drift_pct = _tpms_drift_pct;
    telemetry.calibrated_wheel_diam_inch = _calibrated_diam_in;
}
