#pragma once

#include <Arduino.h>
#include "dash_types.h"
#include "settings_manager.h"

// ==============================================================================
// Proactive Traction Control System (TCS) & Virtual TPMS Engine
// ==============================================================================
// 1. Proactive Traction Control (TCS):
//    - Fast slip detection comparing VESC ERPM ground speed vs. GPS ground speed
//      (or IMU accelerometer reference when GPS not fixed).
//    - Triggers TCS when slip speed (v_vesc - v_ref) > 3.0 km/h under acceleration
//      (throttle > 5% or phase amps > 5A).
//    - Scales down phase current limit in < 5ms to suppress wheel spin.
//    - Smooth recovery hysteresis: ramps phase current back up smoothly once wheel regains traction.
//
// 2. Autonomous GPS Tire Diameter / Circumference Calibration:
//    - Straight steady cruising >= 15 km/h, |G-Force| < 0.08G, GPS satellites >= 8, steady for >= 5s.
//    - D_eff = (v_gps_m_s * 60) / (RPM * M_PI).
//    - Low-pass exponential moving average filter, persists calibrated value in Settings.
//
// 3. Virtual TPMS (Tire Pressure Monitoring System):
//    - Compares effective rolling diameter against calibrated baseline.
//    - If diameter drops by 4% to 6% for > 30 seconds: flags tpms_warning = true and records tpms_drift_pct.
// ==============================================================================

class TcsTpmsManager {
public:
    static TcsTpmsManager& instance() {
        static TcsTpmsManager inst;
        return inst;
    }

    void begin();
    void update(DashTelemetry &telemetry, float dt_sec);

    // Proactive Traction Control (TCS) state
    bool  isTcsEnabled() const { return _tcs_enabled; }
    void  setTcsEnabled(bool enabled);
    bool  isTcsActive() const { return _tcs_active; }
    float getSlipSpeedKmh() const { return _slip_speed_kmh; }
    float getSlipRatio() const { return _slip_ratio; }
    float getThrottleScale() const { return _throttle_scale; }

    // Virtual TPMS & GPS Calibration state
    bool  isTpmsWarning() const { return _tpms_warning; }
    float getTpmsDriftPct() const { return _tpms_drift_pct; }
    float getCalibratedDiameterInch() const { return _calibrated_diam_in; }
    float getBaselineDiameterInch() const { return _baseline_diam_in; }
    float getRollingDiameterInch() const { return _rolling_diam_in; }
    void  startManualCalibration();
    bool  isCalibrating() const { return _manual_calib_active; }

    // Simulation / testing injection hooks
    void setSimulatedSlipSpeed(float slip_kmh) { _sim_slip_override = slip_kmh; }
    void clearSimulatedSlip() { _sim_slip_override = -1.0f; }
    void setSimulatedDiameterDropPct(float drop_pct) { _sim_drop_pct_override = drop_pct; }
    void clearSimulatedDiameterDrop() { _sim_drop_pct_override = -1.0f; }
    void setBaselineDiameter(float diam_in) {
        _baseline_diam_in = diam_in;
        _rolling_diam_in = diam_in;
        _calibrated_diam_in = diam_in;
        _has_calibrated_baseline = true;
    }

    void reset();

private:
    TcsTpmsManager();

    // TCS Internal State
    bool     _tcs_enabled;
    bool     _tcs_active;
    float    _slip_speed_kmh;
    float    _slip_ratio;
    float    _throttle_scale;
    float    _prev_v_vesc;
    bool     _has_prev_v_vesc;
    float    _phys_ref_speed_kmh;
    float    _filtered_dv_dt;
    float    _spin_accel_duration_sec;
    uint32_t _last_slip_active_ms;
    float    _sim_slip_override;

    // GPS Autonomous / Manual Calibration State
    float    _steady_speed_ref;
    float    _steady_timer_sec;
    float    _calib_converge_sec;
    float    _calibrated_diam_in;
    bool     _has_calibrated_baseline;
    bool     _manual_calib_active;
    uint32_t _last_calib_eval_ms;

    // Virtual TPMS State
    float    _baseline_diam_in;
    float    _rolling_diam_in;
    bool     _tpms_warning;
    float    _tpms_drift_pct;
    float    _tpms_drop_duration_sec;
    float    _tpms_recovery_duration_sec;
    float    _sim_drop_pct_override;
};
