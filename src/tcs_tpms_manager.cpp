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
    _imu_ref_speed_kmh = 0.0f;
    _last_slip_active_ms = 0;
    _sim_slip_override = -1.0f;

    _steady_speed_ref = 0.0f;
    _steady_timer_sec = 0.0f;
    _calibrated_diam_in = Settings.get().wheel_diameter_in;
    if (_calibrated_diam_in < 8.0f || _calibrated_diam_in > 16.0f) {
        _calibrated_diam_in = 11.0f; // Default Segway G3 Max
    }
    _has_calibrated_baseline = Settings.get().wheel_gps_calibrated;
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
    if (!enabled) {
        _tcs_active = false;
        _throttle_scale = 1.0f;
    }
}

void TcsTpmsManager::begin() {
    reset();
    _calibrated_diam_in = Settings.get().wheel_diameter_in;
    if (_calibrated_diam_in < 8.0f || _calibrated_diam_in > 16.0f) {
        _calibrated_diam_in = 11.0f;
    }
    _baseline_diam_in = _calibrated_diam_in;
    _rolling_diam_in = _calibrated_diam_in;
    _has_calibrated_baseline = Settings.get().wheel_gps_calibrated;

#if !defined(NATIVE_SIMULATOR)
    Serial.printf("[TCS_TPMS] Manager initialized. Baseline wheel diam: %.1f in (GPS Calibrated: %s)\n",
                  _baseline_diam_in, _has_calibrated_baseline ? "YES" : "NO");
#else
    printf("[TCS_TPMS] Manager initialized. Baseline wheel diam: %.1f in (GPS Calibrated: %s)\n",
           _baseline_diam_in, _has_calibrated_baseline ? "YES" : "NO");
#endif
}

void TcsTpmsManager::update(DashTelemetry &telemetry, float dt_sec) {
    if (dt_sec <= 0.0001f) dt_sec = 0.010f;
    if (dt_sec > 0.25f)    dt_sec = 0.25f;

    // =========================================================================
    // 1. Determine Ground Speed References (VESC Wheel Speed vs. Chassis Ref)
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

    float v_ref = 0.0f;
    if (gps_valid) {
        v_ref = telemetry.gps.speed_kmh;
        // Synchronize IMU reference integration with true satellite ground speed
        _imu_ref_speed_kmh = v_ref;
    } else {
        // Fallback: IMU Accelerometer Integration (1 G = 9.80665 m/s² = 35.304 km/h/s)
        float a_kmh_s = telemetry.g_force * 35.304f;
        if (v_vesc < 0.8f && fabsf(telemetry.g_force) < 0.05f) {
            _imu_ref_speed_kmh = 0.0f;
        } else {
            _imu_ref_speed_kmh += a_kmh_s * dt_sec;
            if (_imu_ref_speed_kmh < 0.0f) _imu_ref_speed_kmh = 0.0f;
            // Physical constraint: Under forward acceleration, vehicle chassis speed cannot exceed driven wheel speed
            if (_imu_ref_speed_kmh > v_vesc) _imu_ref_speed_kmh = v_vesc;
        }
        v_ref = _imu_ref_speed_kmh;
    }

    // Simulation / testing injection override
    if (_sim_slip_override >= 0.0f) {
        v_vesc = v_ref + _sim_slip_override;
    }

    // Slip speed: (v_vesc - v_ref) in km/h
    float slip = v_vesc - v_ref;
    if (slip < 0.0f) slip = 0.0f;
    _slip_speed_kmh = slip;
    _slip_ratio = (v_vesc > 1.0f) ? (slip / v_vesc) : 0.0f;

    // =========================================================================
    // 2. Proactive Traction Control (TCS) Fast Intervention & Recovery Hysteresis
    // =========================================================================
    if (!_tcs_enabled) {
        _tcs_active = false;
        _throttle_scale = 1.0f;
    } else {
        // Condition: under acceleration (throttle > 5% or phase amps > 5A)
        bool under_accel = (telemetry.throttle_pct > 5.0f || telemetry.phase_amps > 5.0f);
        const float SLIP_TRIGGER_KMH = 3.0f;
        const float SLIP_RECOVERY_KMH = 1.5f;

        if (under_accel && (slip > SLIP_TRIGGER_KMH)) {
            // Wheel spin detected under acceleration!
            _tcs_active = true;
            _last_slip_active_ms = now;

            float excess_slip = slip - SLIP_TRIGGER_KMH;
            // Aggressive non-linear reduction targeting < 5ms wheel spin suppression
            // Scales down phase current limit smoothly based on excess slip speed
            float target_scale = 1.0f - (excess_slip / (excess_slip + 2.5f)) * 0.85f;
            if (target_scale < 0.15f) target_scale = 0.15f; // Clamp to retain steerability/control

            // Instant drop (< 5ms response time): cut current without rate limiting downward
            if (target_scale < _throttle_scale) {
                _throttle_scale = target_scale;
            } else {
                _throttle_scale += (target_scale - _throttle_scale) * (dt_sec * 6.0f);
            }
        } else if (!under_accel || slip <= SLIP_RECOVERY_KMH) {
            // Wheel has regained traction or throttle has been released
            _tcs_active = false;
            if (_throttle_scale < 1.0f) {
                // Smooth recovery hysteresis: ramps phase current back up smoothly (takes ~0.5s)
                const float RAMP_UP_RATE = 1.8f; // scale units per second
                _throttle_scale += RAMP_UP_RATE * dt_sec;
                if (_throttle_scale > 1.0f) _throttle_scale = 1.0f;
            }
        } else {
            // Hysteresis deadband between 1.5 km/h and 3.0 km/h:
            // If TCS was already triggered, stay active and gently hold/ramp
            if (_tcs_active) {
                _throttle_scale += (0.85f - _throttle_scale) * (dt_sec * 2.5f);
            } else {
                if (_throttle_scale < 1.0f) {
                    _throttle_scale += 1.8f * dt_sec;
                    if (_throttle_scale > 1.0f) _throttle_scale = 1.0f;
                }
            }
        }
    }

    telemetry.tcs_active = _tcs_active;
    telemetry.tcs_slip_kmh = _slip_speed_kmh;
    telemetry.tcs_throttle_scale = _throttle_scale;

    // =========================================================================
    // 3. Autonomous GPS Tire Diameter / Circumference Calibration
    // =========================================================================
    // Requirements: Straight steady cruising >= 15 km/h, |G-Force| < 0.08G,
    // GPS satellites >= 8, steady speed for >= 5s.
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
                // Effective rolling diameter formula: D_eff = (v_gps_m_s * 60) / (RPM * M_PI)
                float d_eff_m = (v_gps_m_s * 60.0f) / (wheel_rpm * (float)M_PI);
                float d_eff_in = d_eff_m / 0.0254f;

                // Sanity range check for nominal 11" Segway G3 Max tire
                if (d_eff_in >= 8.5f && d_eff_in <= 14.5f) {
                    // Low-pass exponential moving average filter
                    const float EMA_ALPHA = 0.04f;
                    _calibrated_diam_in = EMA_ALPHA * d_eff_in + (1.0f - EMA_ALPHA) * _calibrated_diam_in;

                    // Periodic persistence in Settings (rounded to 0.1 inch, with 60s cooldown against NVS wear)
                    float rounded = roundf(_calibrated_diam_in * 10.0f) / 10.0f;
                    if ((fabsf(Settings.get().wheel_diameter_in - rounded) >= 0.1f || !Settings.get().wheel_gps_calibrated) &&
                        (uint32_t)(now - _last_calib_eval_ms) >= 60000) {
                        _last_calib_eval_ms = now;
                        Settings.get().wheel_diameter_in = rounded;
                        Settings.get().wheel_gps_calibrated = true;
                        Settings.save();
                        _baseline_diam_in = rounded;

                        // Synchronize calibrated wheel diameter directly to VESC and recalculate max_erpm
                        VescHandler::instance().syncWheelDiameterToVesc(rounded);
                        ModeManager::instance().applyCurrentMode();
                        telemetry.calibrated_wheel_diam_inch = rounded;
#if !defined(NATIVE_SIMULATOR)
                        Serial.printf("[TCS_TPMS] Autonomous Tire Diameter Calibrated: %.1f in (D_eff=%.2f in) -> Synchronized to VESC\n",
                                      rounded, d_eff_in);
#else
                        printf("[TCS_TPMS] Autonomous Tire Diameter Calibrated: %.1f in (D_eff=%.2f in) -> Synchronized to VESC\n",
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

    // =========================================================================
    // 4. Virtual TPMS (Tire Pressure Monitoring System)
    // =========================================================================
    // Compares effective rolling diameter against calibrated baseline.
    // If diameter drops by 4% to 6% for > 30 seconds: flags tpms_warning = true and records tpms_drift_pct.
    float baseline = (_baseline_diam_in > 5.0f) ? _baseline_diam_in : 
                     (Settings.get().wheel_diameter_in > 5.0f ? Settings.get().wheel_diameter_in : 11.0f);

    float current_eff_in = _calibrated_diam_in;
    if (_sim_drop_pct_override >= 0.0f) {
        // Test override for TPMS deflation verification
        current_eff_in = baseline * (1.0f - (_sim_drop_pct_override / 100.0f));
    } else if (gps_valid && telemetry.gps.satellites >= 6 && telemetry.gps.speed_kmh >= 14.0f &&
               fabsf(telemetry.g_force) < 0.10f && !_tcs_active) {
        float gear = Settings.get().gear_ratio > 0.1f ? Settings.get().gear_ratio : 1.0f;
        float wheel_rpm = telemetry.rpm / gear;
        if (wheel_rpm > 50.0f) {
            float v_m_s = telemetry.gps.speed_kmh / 3.6f;
            float d_m = (v_m_s * 60.0f) / (wheel_rpm * (float)M_PI);
            current_eff_in = d_m / 0.0254f;
        }
    }

    if (current_eff_in >= 7.0f && current_eff_in <= 15.0f) {
        // Fast tracking rolling diameter filter
        _rolling_diam_in = 0.15f * current_eff_in + 0.85f * _rolling_diam_in;
    }

    // Calculate drop percentage: (baseline - rolling) / baseline * 100%
    float drift_pct = ((baseline - _rolling_diam_in) / baseline) * 100.0f;
    if (drift_pct < 0.0f) drift_pct = 0.0f;
    _tpms_drift_pct = drift_pct;

    // Check 4% to 6% drop threshold for > 30 seconds
    const float TPMS_DROP_THRESHOLD_PCT = 4.0f;
    if (drift_pct >= TPMS_DROP_THRESHOLD_PCT) {
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
