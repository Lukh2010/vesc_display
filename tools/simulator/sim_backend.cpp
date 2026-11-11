#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <math.h>

#include "Arduino.h"
#include "board_config.h"
#include "dash_types.h"
#include "display_driver.h"
#include "dash_renderer.h"
#include "settings_manager.h"
#include "battery_manager.h"
#include "mode_manager.h"
#include "buttons.h"
#include "vesc_uart.h"
#include "fault_manager.h"
#include <string>


#include "virtual_vesc/virtual_vesc.h"
#include "virtual_vesc/vesc_tcp_server.h"

// -------------------------------------------------------------
// Virtual VESC & Loopback Transport for Desktop Simulator
// -------------------------------------------------------------
static VescLoopbackTransport g_sim_transport;
static VirtualVesc g_virtual_vesc;
static VescTcpServer g_tcp_server;

MockSerial Serial;
DisplayDriver Display;
extern DashboardRenderer Renderer;
extern ButtonHandler Buttons;

// -------------------------------------------------------------
// State Variables & Simulation Physics
// -------------------------------------------------------------
static DashTelemetry g_telemetry;
static bool g_initialized = false;
static float g_manual_throttle = 0.0f;
static float g_manual_brake = 0.0f;
static uint32_t g_sim_uptime_ms = 0;
static auto g_last_frame_clock = std::chrono::steady_clock::now();

static float s_sim_speed_kmh = 0.0f;
static float s_sim_current_amps = 0.0f;
static float s_sim_phase_amps = 0.0f;
static float s_sim_duty_cycle_pct = 0.0f;
static float s_sim_rpm = 0.0f;
static float s_sim_voltage = 76.7f;
static float s_sim_power_watts = 0.0f;
static float s_sim_temp_fet_rear = 38.0f;
static float s_sim_temp_mot_rear = 42.0f;
static float s_sim_temp_fet_front = 36.0f;
static float s_sim_temp_mot_front = 39.0f;
static float s_sim_trip_km = 0.0f;
static float s_sim_odo_km = 206.6f;
static float s_sim_ah_used = 0.0f;
static float s_sim_ah_charged = 0.0f;
static float s_sim_wh_used = 0.0f;
static float s_sim_wh_charged = 0.0f;
static uint8_t s_sim_fault_code = 0;

#pragma pack(push, 1)
struct SimTelemetryExport {
    float speed_kmh;
    float duty_cycle_pct;
    float throttle_pct;
    float voltage;
    float current_amps;
    float phase_amps;
    float power_watts;
    float battery_pct;
    float remaining_wh;
    float est_range_km;
    float temp_motor;
    float temp_esc;
    float trip_km;
    float odo_km;
    int32_t screen;
    int32_t dash_style;
    int32_t auto_mode;
    int32_t ride_mode;
    int32_t boost_active;
    int32_t hazard_active;
    int32_t light_mode;
    int32_t light_hue;
    float temp_fet_rear;
    float temp_mot_rear;
    float temp_fet_front;
    float temp_mot_front;
    float max_speed_kmh;
    float avg_speed_kmh;
    uint32_t ride_time_sec;
    int32_t tuning_mode;
    int32_t is_locked;
    int32_t race_state;
    float race_elapsed_sec;
    float race_split_0_22;
    float race_split_0_50;
    float race_split_0_70;
    float g_force;
    float incline_deg;
};
#pragma pack(pop)

// -------------------------------------------------------------
// Simulation Logic
// -------------------------------------------------------------
static float s_sim_incline_deg = 0.0f;

static void ensureZeroSpeedIfIdle() {
    bool has_ext = g_virtual_vesc.hasExternalControl() && g_virtual_vesc.getExternalThrottle() > 0.01f;
    if (s_sim_speed_kmh <= 0.5f && g_manual_throttle <= 0.01f && !has_ext) {
        s_sim_speed_kmh = 0.0f;
        s_sim_current_amps = 0.0f;
        s_sim_phase_amps = 0.0f;
        s_sim_power_watts = 0.0f;
        s_sim_duty_cycle_pct = 0.0f;
        s_sim_rpm = 0.0f;
        g_telemetry.speed_kmh = 0.0f;
        g_telemetry.power_watts = 0.0f;
        g_telemetry.current_amps = 0.0f;
        g_telemetry.phase_amps = 0.0f;
        g_telemetry.duty_cycle_pct = 0.0f;
        g_telemetry.rpm = 0.0f;
        g_telemetry.g_force = 0.0f;
    }
}

static void runSimulationStep(float dt) {
    if (dt <= 0.0002f) return;

    float sim_t = g_manual_throttle;
    float sim_b = g_manual_brake;

    if (g_virtual_vesc.hasExternalControl()) {
        sim_t = g_virtual_vesc.getExternalThrottle();
        sim_b = g_virtual_vesc.getExternalBrake();
    }

    if (sim_t <= 0.01f) {
        sim_t = 0.0f;
    }
    if (sim_b <= 0.01f) {
        sim_b = 0.0f;
    }

    // Boost multiplier (only when positive throttle applied)
    if (g_telemetry.boost_active) {
        if (sim_t > 0.0f) {
            sim_t *= 1.25f;
            if (sim_t > 100.0f) sim_t = 100.0f;
        }
        if (g_telemetry.screen != SCREEN_RACE_TIMER && g_sim_uptime_ms > g_telemetry.boost_end_ms) {
            g_telemetry.boost_active = false;
            Renderer.markScreenDirty();
        }
    }

    if (g_telemetry.is_locked) {
        sim_t = 0.0f;
        s_sim_speed_kmh = 0.0f;
        s_sim_current_amps = 0.0f;
        s_sim_phase_amps = 0.0f;
        s_sim_power_watts = 0.0f;
        s_sim_duty_cycle_pct = 0.0f;
        s_sim_rpm = 0.0f;

        VirtualVescTelemetry vt;
        vt.temp_mos = s_sim_temp_fet_rear;
        vt.temp_mot = s_sim_temp_mot_rear;
        vt.current_mot = 0.0f;
        vt.current_in = 0.0f;
        vt.duty_cycle_pct = 0.0f;
        vt.rpm = 0.0f;
        vt.pole_pairs = Settings.get().motor_pole_pairs;
        vt.speed_kmh = 0.0f;
        vt.voltage = s_sim_voltage;
        vt.battery_pct = Battery.getFusedSocPct();
        vt.amphours_used = s_sim_ah_used;
        vt.amphours_charged = s_sim_ah_charged;
        vt.watthours_used = s_sim_wh_used;
        vt.watthours_charged = s_sim_wh_charged;
        vt.trip_km = s_sim_trip_km;
        vt.odo_km = s_sim_odo_km;
        vt.fault_code = s_sim_fault_code;
        vt.throttle_pct = 0.0f;
        vt.brake_pct = 0.0f;
        g_virtual_vesc.setPhysicsTelemetry(vt);
        return;
    }

    // 1. Target Speed and Power by Ride Mode & Tuning State from Virtual VESC Controller Limits
    const VirtualVescLimits &lim = g_virtual_vesc.getLimits();

    int pole_pairs = Settings.get().motor_pole_pairs;
    if (pole_pairs < 2) pole_pairs = 15;
    float wheel_diam_in = Settings.get().wheel_diameter_in;
    if (wheel_diam_in < 5.0f) wheel_diam_in = 26.0f;
    float wheel_circ_m = (wheel_diam_in * 0.0254f) * M_PI;
    float gear_ratio = Settings.get().gear_ratio;
    if (gear_ratio < 0.1f) gear_ratio = 1.0f;

    float mech_rpm = (float)lim.max_erpm / (float)pole_pairs;
    float max_possible_speed = (mech_rpm / gear_ratio) * wheel_circ_m * 60.0f / 1000.0f;
    if (max_possible_speed > 120.0f) max_possible_speed = 120.0f;

    float max_bat_a = lim.max_battery_amps;
    float max_pha_a = lim.max_phase_amps;
    float accel_rate = 22.0f;

    if (!g_telemetry.tuning_mode) {
        // Legal Modes: ECO (15 km/h), SPORT (20 km/h), RACE (22 km/h)
        switch (g_telemetry.ride_mode) {
            case MODE_ECO:   accel_rate = 18.0f; break;
            case MODE_SPORT: accel_rate = 24.0f; break;
            case MODE_RACE:  accel_rate = 28.0f; break;
            default: break;
        }
    } else {
        switch (g_telemetry.ride_mode) {
            case MODE_ECO:   accel_rate = 32.0f; break;
            case MODE_SPORT: accel_rate = 46.0f; break;
            case MODE_RACE:  accel_rate = 68.0f; break;
            default: break;
        }
        if (g_telemetry.boost_active) {
            max_possible_speed += 15.0f;
            max_bat_a += 25.0f;
            max_pha_a += 55.0f;
            accel_rate += 18.0f;
        }
    }

    float effective_power_cap = (lim.max_watt > 50.0f) ? lim.max_watt : 40000.0f;
    float max_power_bat_a = (s_sim_voltage > 10.0f) ? (effective_power_cap / s_sim_voltage) : max_bat_a;
    if (max_bat_a > max_power_bat_a) {
        max_bat_a = max_power_bat_a;
    }

    // 2. Realistic Speed & Dynamic Powertrain Physics
    if (sim_b > 1.0f) {
        // Active Braking: strong regenerative decelerating force
        g_manual_throttle = 0.0f; // Safety throttle cutoff upon braking
        float brake_norm = sim_b / 100.0f;
        float decel = (6.0f + 28.0f * brake_norm) * dt;
        s_sim_speed_kmh -= decel;
        if (s_sim_speed_kmh < 0.2f) s_sim_speed_kmh = 0.0f;

        // Regen braking: motor generates phase current
        float target_regen_phase = (s_sim_speed_kmh > 2.0f) ? (max_pha_a * 0.75f * brake_norm) : 0.0f;
        s_sim_phase_amps += (target_regen_phase - s_sim_phase_amps) * (dt * 6.5f);
        if (s_sim_speed_kmh < 0.2f) s_sim_phase_amps = 0.0f;

        // Battery regen current is bounded by duty and max battery regen limit
        float duty_ratio = s_sim_duty_cycle_pct / 100.0f;
        if (duty_ratio > 0.95f) duty_ratio = 0.95f;
        if (duty_ratio < 0.25f) duty_ratio = 0.25f;
        float target_regen_bat = - (s_sim_phase_amps * duty_ratio);
        float max_regen_bat = fabsf(lim.min_battery_amps) > 1.0f ? fabsf(lim.min_battery_amps) : (max_bat_a * 0.6f);
        if (-target_regen_bat > max_regen_bat) target_regen_bat = -max_regen_bat;
        s_sim_current_amps += (target_regen_bat - s_sim_current_amps) * (dt * 6.5f);
        if (s_sim_speed_kmh < 0.2f) s_sim_current_amps = 0.0f;

    } else if (sim_t > 0.0f) {
        // Active Throttle: electric vehicle acceleration
        float target_speed = (sim_t >= 90.0f) ? max_possible_speed : ((sim_t / 100.0f) * max_possible_speed);
        if (s_sim_speed_kmh < target_speed) {
            float speed_diff = target_speed - s_sim_speed_kmh;
            float step = (sim_t / 100.0f) * (accel_rate * dt + speed_diff * (dt * 3.5f));
            s_sim_speed_kmh += step;
            if (s_sim_speed_kmh > target_speed) s_sim_speed_kmh = target_speed;
        } else {
            // Smooth settling onto target speed
            s_sim_speed_kmh += (target_speed - s_sim_speed_kmh) * (dt * 3.5f);
        }

        // Throttle commands target Motor Phase Amps directly (Torque control)
        float target_phase = (sim_t / 100.0f) * max_pha_a;
        if (g_virtual_vesc.hasExternalCurrentControl()) {
            target_phase = fabsf(g_virtual_vesc.getExternalTargetCurrent());
            if (target_phase > max_pha_a) target_phase = max_pha_a;
        }
        s_sim_phase_amps += (target_phase - s_sim_phase_amps) * (dt * 6.5f);
        if (s_sim_phase_amps < 0.0f) s_sim_phase_amps = 0.0f;

        // Battery current = Phase current * effective duty, bounded by max_bat_a
        float duty_ratio = s_sim_duty_cycle_pct / 100.0f;
        if (duty_ratio > 0.95f) duty_ratio = 0.95f;
        if (duty_ratio < 0.20f) duty_ratio = 0.20f;
        float target_bat = s_sim_phase_amps * duty_ratio;
        if (target_bat > max_bat_a) {
            target_bat = max_bat_a;
            float clamped_phase = target_bat / duty_ratio;
            if (s_sim_phase_amps > clamped_phase) {
                s_sim_phase_amps = clamped_phase;
            }
        }
        s_sim_current_amps += (target_bat - s_sim_current_amps) * (dt * 6.5f);
        if (s_sim_current_amps < 0.0f) s_sim_current_amps = 0.0f;

    } else {
        // Coasting / Ausrollen: Throttle released (sim_t == 0.0f)
        if (s_sim_speed_kmh <= 0.2f) {
            s_sim_speed_kmh = 0.0f;
            s_sim_current_amps = 0.0f;
            s_sim_phase_amps = 0.0f;
            s_sim_power_watts = 0.0f;
            s_sim_duty_cycle_pct = 0.0f;
            s_sim_rpm = 0.0f;
            g_telemetry.speed_kmh = 0.0f;
            g_telemetry.power_watts = 0.0f;
            g_telemetry.current_amps = 0.0f;
            g_telemetry.phase_amps = 0.0f;
            g_telemetry.duty_cycle_pct = 0.0f;
            g_telemetry.rpm = 0.0f;
            g_telemetry.g_force = 0.0f;
        } else {
            float v = s_sim_speed_kmh;
            float drag_decel = (0.75f + (v / 100.0f) * (v / 100.0f) * 4.5f + v * 0.028f) * dt;
            s_sim_speed_kmh -= drag_decel;
            if (s_sim_speed_kmh < 0.2f) s_sim_speed_kmh = 0.0f;

            // Passive drag regen (mild back-EMF recuperation)
            float coast_target_a = - (1.2f + (v / 100.0f) * 3.2f);
            s_sim_current_amps += (coast_target_a - s_sim_current_amps) * (dt * 3.8f);
            float coast_phase = fabsf(s_sim_current_amps) * 1.25f;
            s_sim_phase_amps += (coast_phase - s_sim_phase_amps) * (dt * 3.8f);
        }
    }

    // 3. Realistic Duty Cycle (Inverter PWM modulation index)
    // Physical no-load top speed of the motor without field weakening is ~85 km/h (at nominal battery voltage)
    const float physical_motor_top_speed = 85.0f;
    float base_duty = (s_sim_speed_kmh / physical_motor_top_speed) * 95.0f;
    float target_duty = base_duty;

    // Additional duty required to overcome motor phase resistance under heavy acceleration
    if (sim_t > 0.0f && s_sim_phase_amps > 5.0f) {
        float i_r_duty = (s_sim_phase_amps / max_pha_a) * 8.0f; // max ~8% boost for stator IR drop
        target_duty += i_r_duty;
    }

    if (sim_t > 85.0f && s_sim_speed_kmh > 78.0f) {
        // Field Weakening engaged at high speed near physical motor limit: duty pushes beyond 100% (up to 114%)
        float fw_extra = ((s_sim_speed_kmh - 78.0f) / 12.0f) * 18.0f;
        target_duty = 96.0f + fw_extra;
    }
    if (sim_t == 0.0f && sim_b <= 1.0f) {
        // Coasting: duty cycle follows back-EMF of spinning rotor
        target_duty = (s_sim_speed_kmh / physical_motor_top_speed) * 90.0f;
    }
    if (target_duty > 118.0f) target_duty = 118.0f;
    if (target_duty < 0.0f) target_duty = 0.0f;
    s_sim_duty_cycle_pct += (target_duty - s_sim_duty_cycle_pct) * (dt * 8.0f);

    float wheel_rpm = (s_sim_speed_kmh * 1000.0f / 60.0f) / wheel_circ_m;
    s_sim_rpm = wheel_rpm * gear_ratio;

    // 4. Physical Invariant: Phase (motor) current must ALWAYS be >= |Battery current|
    if (s_sim_phase_amps < fabsf(s_sim_current_amps)) {
        s_sim_phase_amps = fabsf(s_sim_current_amps);
    }
    if (s_sim_phase_amps > max_pha_a) {
        s_sim_phase_amps = max_pha_a;
    }

    // 5. Battery Voltage & Power (Nominal 72V pack: sag on load, rise on regen)
    s_sim_voltage = 76.7f - (s_sim_current_amps * 0.038f);
    s_sim_power_watts = s_sim_voltage * s_sim_current_amps;

    // Overvoltage fault detection (72V nominal pack peak is 84.0V)
    if (s_sim_voltage >= 84.5f) {
        s_sim_fault_code = 1; // FAULT_CODE_OVER_VOLTAGE
    } else if (s_sim_fault_code == 1 && s_sim_voltage < 83.5f) {
        s_sim_fault_code = 0;
    }

    // Dynamic Power Limiting governed by Virtual VESC watt limit
    if (s_sim_current_amps > 0.0f && s_sim_power_watts > effective_power_cap) {
        s_sim_power_watts = effective_power_cap;
        if (s_sim_voltage > 10.0f) {
            s_sim_current_amps = s_sim_power_watts / s_sim_voltage;
        }
    }
    if (s_sim_phase_amps < fabsf(s_sim_current_amps)) {
        s_sim_phase_amps = fabsf(s_sim_current_amps);
    }

    // 6. Dual Thermals (Rear & Front)
    if (s_sim_phase_amps > 30.0f) {
        s_sim_temp_fet_rear  += 0.35f * dt;
        s_sim_temp_mot_rear  += 0.45f * dt;
        s_sim_temp_fet_front += 0.28f * dt;
        s_sim_temp_mot_front += 0.36f * dt;
    } else {
        s_sim_temp_fet_rear  -= 0.08f * dt;
        s_sim_temp_mot_rear  -= 0.08f * dt;
        s_sim_temp_fet_front -= 0.08f * dt;
        s_sim_temp_mot_front -= 0.08f * dt;
    }
    if (s_sim_temp_fet_rear < 35.0f)  s_sim_temp_fet_rear = 35.0f;
    if (s_sim_temp_mot_rear < 38.0f)  s_sim_temp_mot_rear = 38.0f;
    if (s_sim_temp_fet_front < 32.0f) s_sim_temp_fet_front = 32.0f;
    if (s_sim_temp_mot_front < 35.0f) s_sim_temp_mot_front = 35.0f;

    // 7. Distance & Odometry
    float distance_delta = (s_sim_speed_kmh / 3600.0f) * dt;
    s_sim_trip_km += distance_delta;
    s_sim_odo_km  += distance_delta;

    // Energy integration
    if (s_sim_current_amps > 0.0f) {
        s_sim_ah_used += (s_sim_current_amps / 3600.0f) * dt;
        s_sim_wh_used += (s_sim_power_watts / 3600.0f) * dt;
    } else {
        s_sim_ah_charged += (-s_sim_current_amps / 3600.0f) * dt;
        s_sim_wh_charged += (-s_sim_power_watts / 3600.0f) * dt;
    }

    // Dynamic Statistics Tracking for the current run
    if (s_sim_power_watts > g_telemetry.stats.peak_power_watts) {
        g_telemetry.stats.peak_power_watts = s_sim_power_watts;
    }
    if (fabsf(s_sim_current_amps) > g_telemetry.stats.peak_current_amps) {
        g_telemetry.stats.peak_current_amps = fabsf(s_sim_current_amps);
    }
    if (s_sim_phase_amps > g_telemetry.stats.peak_phase_amps) {
        g_telemetry.stats.peak_phase_amps = s_sim_phase_amps;
    }
    if (s_sim_speed_kmh > g_telemetry.stats.max_speed_kmh) {
        g_telemetry.stats.max_speed_kmh = s_sim_speed_kmh;
    }
    if (s_sim_temp_mot_rear > g_telemetry.stats.max_temp_motor) {
        g_telemetry.stats.max_temp_motor = s_sim_temp_mot_rear;
    }
    if (s_sim_temp_fet_rear > g_telemetry.stats.max_temp_esc) {
        g_telemetry.stats.max_temp_esc = s_sim_temp_fet_rear;
    }
    if (s_sim_voltage > 10.0f) {
        if (g_telemetry.stats.min_voltage <= 0.0f || s_sim_voltage < g_telemetry.stats.min_voltage) {
            g_telemetry.stats.min_voltage = s_sim_voltage;
        }
        if (s_sim_voltage > g_telemetry.stats.max_voltage) {
            g_telemetry.stats.max_voltage = s_sim_voltage;
        }
    }

    g_telemetry.stats.trip_wh = s_sim_wh_used;
    if (s_sim_trip_km > 0.05f) {
        g_telemetry.stats.trip_wh_km = s_sim_wh_used / s_sim_trip_km;
    }

    // 8. Feed physics state directly into Virtual VESC emulator
    VirtualVescTelemetry vt;
    vt.temp_mos = s_sim_temp_fet_rear;
    vt.temp_mot = s_sim_temp_mot_rear;
    vt.current_mot = s_sim_phase_amps;
    vt.current_in = s_sim_current_amps;
    vt.duty_cycle_pct = s_sim_duty_cycle_pct;
    vt.rpm = s_sim_rpm;
    vt.pole_pairs = pole_pairs;
    vt.speed_kmh = s_sim_speed_kmh;
    vt.voltage = s_sim_voltage;
    vt.battery_pct = Battery.getFusedSocPct();
    vt.amphours_used = s_sim_ah_used;
    vt.amphours_charged = s_sim_ah_charged;
    vt.watthours_used = s_sim_wh_used;
    vt.watthours_charged = s_sim_wh_charged;
    vt.tachometer = (int32_t)(s_sim_trip_km * 1000.0f / wheel_circ_m * pole_pairs * 6);
    vt.tachometer_abs = vt.tachometer;
    vt.trip_km = s_sim_trip_km;
    vt.odo_km = s_sim_odo_km;
    vt.fault_code = s_sim_fault_code;
    vt.throttle_pct = sim_t;
    vt.brake_pct = sim_b;
    g_virtual_vesc.setPhysicsTelemetry(vt);
    g_telemetry.incline_deg = s_sim_incline_deg;
}

// -------------------------------------------------------------
// Extern "C" Interface for Python ctypes
// -------------------------------------------------------------
extern "C" {

void sim_trigger_nav(int nav_action);

void sim_init() {
    if (g_initialized) return;

    g_use_wall_clock_millis = true;

    Settings.begin();
    Battery.begin();
    Buttons.begin();

    // Connect Virtual VESC loopback transport
    Vesc.setTransport(&g_sim_transport.getSideA());
    g_virtual_vesc.setTransport(&g_sim_transport.getSideB());
    Vesc.begin();
    g_virtual_vesc.begin();
    g_tcp_server.start(6510);

    ModeManager::instance().begin();
    g_virtual_vesc.process(); // Process initial limits dispatched by ModeManager
    Display.init();

    Settings.get().dash_style = STYLE_HYPER_35KW;
    Renderer.begin(&Display);

    memset(&g_telemetry, 0, sizeof(g_telemetry));
    g_telemetry.vesc_connected = true;
    g_telemetry.screen = SCREEN_RIDE_DASH;
    g_telemetry.voltage = 76.7f;
    g_telemetry.battery_pct = 68.0f;
    g_telemetry.temp_fet_rear  = 38.0f;
    g_telemetry.temp_mot_rear  = 42.0f;
    g_telemetry.temp_fet_front = 36.0f;
    g_telemetry.temp_mot_front = 39.0f;
    g_telemetry.temp_esc   = 38.0f;
    g_telemetry.temp_motor = 42.0f;
    g_telemetry.trip_km = 14.2f;
    g_telemetry.odo_km = 206.6f;
    g_telemetry.battery_profile_id = 0;
    g_telemetry.battery_health_soh = 98.4f;
    g_telemetry.est_range_km = 68.0f;
    g_telemetry.ride_mode = MODE_SPORT;
    g_telemetry.saved_ride_mode = MODE_SPORT;
    g_telemetry.boost_active = false;
    g_telemetry.light_mode = 2; // RAINBOW
    g_telemetry.light_hue = 180;
    g_telemetry.light_brightness = 200;
    g_telemetry.lights_enabled = true;
    g_telemetry.hazard_active = false;
    g_telemetry.rider_tag[0] = '\0';
    g_telemetry.race_popup_open = false;
    g_telemetry.race_dyno_open = false;
    g_telemetry.race_dyno.reset();
    g_telemetry.race_state = 0;
    g_telemetry.race_countdown_start_ms = 0;
    g_telemetry.race_launch_start_ms = 0;
    g_telemetry.race_elapsed_sec = 0.0f;
    g_telemetry.race_split_0_22 = 0.0f;
    g_telemetry.race_split_0_50 = 0.0f;
    g_telemetry.race_split_0_70 = 0.0f;
    g_telemetry.race_records = Settings.getRaceRecords();

    // Seed realistic sine waveform curve history (from video frame 00:11)
    for (int i = 0; i < WAVE_HISTORY_LEN; i++) {
        float phase = (float)i / (float)WAVE_HISTORY_LEN * 4.0f * 3.14159265f;
        g_telemetry.speed_history[i] = 45.0f + 35.0f * sinf(phase);
        g_telemetry.power_history[i] = 5500.0f + 4200.0f * sinf(phase);
    }
    g_telemetry.history_count = WAVE_HISTORY_LEN;
    g_telemetry.last_history_sample_ms = 1000;

    g_telemetry.stats.reset();
    s_sim_trip_km = 0.0f;
    s_sim_wh_used = 0.0f;
    s_sim_ah_used = 0.0f;
    s_sim_ah_charged = 0.0f;
    s_sim_wh_charged = 0.0f;
    s_sim_speed_kmh = 0.0f;
    s_sim_current_amps = 0.0f;
    s_sim_phase_amps = 0.0f;
    s_sim_power_watts = 0.0f;
    s_sim_duty_cycle_pct = 0.0f;
    s_sim_rpm = 0.0f;
    g_manual_throttle = 0.0f;
    g_manual_brake = 0.0f;
    g_telemetry.speed_kmh = 0.0f;
    g_telemetry.power_watts = 0.0f;
    g_telemetry.current_amps = 0.0f;
    g_telemetry.phase_amps = 0.0f;
    g_telemetry.duty_cycle_pct = 0.0f;
    g_telemetry.rpm = 0.0f;
    g_telemetry.g_force = 0.0f;
    g_telemetry.trip_km = 0.0f;

    g_last_frame_clock = std::chrono::steady_clock::now();
    g_sim_uptime_ms = 1000;
    g_sim_time_ms = 1000;
    ModeManager::instance().setRideMode(g_telemetry.ride_mode);

    if (Settings.get().pin_lock_enabled) {
        g_telemetry.is_locked = true;
        g_telemetry.pin_entry[0] = '\0';
        g_telemetry.pin_error = false;
        g_telemetry.pin_error_end_ms = 0;
        ModeManager::instance().setLocked(true);
        printf("[SIM_BACKEND] Boot PIN-Lockscreen ACTIVE (code: '%s'). VESC locked at 0A.\n", Settings.get().pin_code);
    } else {
        g_telemetry.is_locked = false;
        ModeManager::instance().setLocked(false);
        Renderer.triggerNeedleSweep();
    }

    g_initialized = true;
    printf("[SIM_BACKEND] Initialized Hyper-VESC 35kW+ (800x480) Cluster Simulator Engine.\n");
}

void sim_step(uint32_t step_ms) {
    if (!g_initialized) sim_init();

    if (ModeManager::instance().getRideMode() != g_telemetry.ride_mode) {
        ModeManager::instance().setRideMode(g_telemetry.ride_mode);
    }

    if (step_ms == 0) {
        auto now_clock = std::chrono::steady_clock::now();
        step_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(now_clock - g_last_frame_clock).count();
        g_last_frame_clock = now_clock;
        if (step_ms > 100) step_ms = 100;
        if (step_ms < 5) step_ms = 5;
    }

    g_sim_uptime_ms += step_ms;
    g_sim_time_ms += step_ms;

    // Auto-unlock if PIN lock was disabled via Settings / Captive Portal
    if (!Settings.get().pin_lock_enabled && g_telemetry.is_locked) {
        g_telemetry.is_locked = false;
        ModeManager::instance().setLocked(false);
        Renderer.markScreenDirty();
        Renderer.invalidateCache();
        Renderer.triggerNeedleSweep();
    }

    // 1. Update buttons & touch gesture recognition
    Buttons.setActiveScreen(g_telemetry.screen);
    Buttons.setRacePopupOpen(g_telemetry.race_popup_open);
    Buttons.setRaceDynoOpen(g_telemetry.race_dyno_open);
    Buttons.setRaceState(g_telemetry.race_state);
    Buttons.update();
    Renderer.updateRaceTimer(g_telemetry, g_sim_uptime_ms);

    // PIN Lockscreen Input Processing (when locked)
    if (g_telemetry.is_locked) {
        if (g_telemetry.pin_error) {
            if (g_sim_uptime_ms >= g_telemetry.pin_error_end_ms) {
                g_telemetry.pin_error = false;
                Renderer.markScreenDirty();
            } else {
                int16_t dummy_x, dummy_y;
                Buttons.getLastTap(&dummy_x, &dummy_y);
            }
        }

        int16_t tx = -1, ty = -1;
        if (!g_telemetry.pin_error && Buttons.getLastTap(&tx, &ty)) {
            int c = -1;
            if (tx >= 150 && tx < 330) c = 0;
            else if (tx >= 330 && tx < 470) c = 1;
            else if (tx >= 470 && tx <= 650) c = 2;

            int r = -1;
            if (ty >= 140 && ty < 227) r = 0;
            else if (ty >= 227 && ty < 297) r = 1;
            else if (ty >= 297 && ty < 367) r = 2;
            else if (ty >= 367 && ty <= 445) r = 3;

            if (r >= 0 && c >= 0) {
                static const char KEY_CHARS[4][3] = {
                    { '1', '2', '3' },
                    { '4', '5', '6' },
                    { '7', '8', '9' },
                    { '<', '0', '#' }
                };
                char key = KEY_CHARS[r][c];
                int len = strlen(g_telemetry.pin_entry);

                if (key == '<') {
                    if (len > 0) {
                        g_telemetry.pin_entry[len - 1] = '\0';
                        g_telemetry.pin_error = false;
                        Renderer.markScreenDirty();
                    }
                } else if (key == '#') {
                    if (strcmp(g_telemetry.pin_entry, Settings.get().pin_code) == 0) {
                        g_telemetry.is_locked = false;
                        g_telemetry.pin_error = false;
                        g_telemetry.pin_entry[0] = '\0';
                        g_manual_throttle = 0.0f;
                        g_manual_brake = 0.0f;
                        s_sim_speed_kmh = 0.0f;
                        ModeManager::instance().setLocked(false);
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        Renderer.triggerNeedleSweep();
                        printf("[SIM_PIN] System UNLOCKED with valid PIN code!\n");
                    } else {
                        g_telemetry.pin_error = true;
                        g_telemetry.pin_error_end_ms = g_sim_uptime_ms + 800;
                        g_telemetry.pin_entry[0] = '\0';
                        Renderer.markScreenDirty();
                        printf("[SIM_PIN] Invalid PIN code entered!\n");
                    }
                } else if (key >= '0' && key <= '9') {
                    if (len < 4) {
                        g_telemetry.pin_entry[len] = key;
                        g_telemetry.pin_entry[len + 1] = '\0';
                        g_telemetry.pin_error = false;
                        len++;
                        Renderer.markScreenDirty();

                        if (len == 4) {
                            if (strcmp(g_telemetry.pin_entry, Settings.get().pin_code) == 0) {
                                g_telemetry.is_locked = false;
                                g_telemetry.pin_error = false;
                                g_telemetry.pin_entry[0] = '\0';
                                g_manual_throttle = 0.0f;
                                g_manual_brake = 0.0f;
                                s_sim_speed_kmh = 0.0f;
                                ModeManager::instance().setLocked(false);
                                Renderer.markScreenDirty();
                                Renderer.invalidateCache();
                                Renderer.triggerNeedleSweep();
                                printf("[SIM_PIN] System UNLOCKED automatically upon 4 digits!\n");
                            } else {
                                g_telemetry.pin_error = true;
                                g_telemetry.pin_error_end_ms = g_sim_uptime_ms + 800;
                                g_telemetry.pin_entry[0] = '\0';
                                Renderer.markScreenDirty();
                                printf("[SIM_PIN] Invalid PIN code entered!\n");
                            }
                        }
                    }
                }
            }
        }
    }

    // STATS / RT Graphs Screen: Interactive Backlight adjustment
    if (!g_telemetry.is_locked && g_telemetry.screen == SCREEN_RT_GRAPHS) {
        static bool s_sim_backlight_dragging = false;
        if (Buttons.isTouching()) {
            int16_t tx = Buttons.getTouchX();
            int16_t ty = Buttons.getTouchY();
            if (ty >= 360 && ty <= 465 && tx >= 270 && tx <= 560) {
                s_sim_backlight_dragging = true;
                float pct = (float)(tx - 280) / 270.0f;
                int val = (int)(20.0f + pct * 80.0f + 0.5f);
                if (val < 20) val = 20;
                if (val > 100) val = 100;
                DashSettings &s = Settings.get();
                if (s.brightness_pct != val) {
                    s.brightness_pct = val;
                    Display.setBrightness((uint8_t)((val * 255) / 100));
                    Renderer.markScreenDirty();
                }
            }
        } else {
            if (s_sim_backlight_dragging) {
                s_sim_backlight_dragging = false;
                Settings.save();
            }
        }

        int16_t tx = -1, ty = -1;
        if (Buttons.getLastTap(&tx, &ty)) {
            if (ty >= 360 && ty <= 465) {
                DashSettings &s = Settings.get();
                bool changed = false;
                if (tx >= 195 && tx <= 275) { // Button [-]
                    if (s.brightness_pct > 20) {
                        s.brightness_pct = (s.brightness_pct <= 30) ? 20 : (s.brightness_pct - 10);
                        changed = true;
                    }
                } else if (tx >= 556 && tx <= 628) { // Button [+]
                    if (s.brightness_pct < 100) {
                        s.brightness_pct = (s.brightness_pct >= 90) ? 100 : (s.brightness_pct + 10);
                        changed = true;
                    }
                } else if (tx >= 276 && tx <= 555) { // Tap on slider
                    float pct = (float)(tx - 280) / 270.0f;
                    int val = (int)(20.0f + pct * 80.0f + 0.5f);
                    if (val < 20) val = 20;
                    if (val > 100) val = 100;
                    if (s.brightness_pct != val) {
                        s.brightness_pct = val;
                        changed = true;
                    }
                } else if (tx >= 630 && tx <= 702) { // Button [25%]
                    if (s.brightness_pct != 25) {
                        s.brightness_pct = 25;
                        changed = true;
                    }
                } else if (tx >= 705 && tx <= 778) { // Button [100%]
                    if (s.brightness_pct != 100) {
                        s.brightness_pct = 100;
                        changed = true;
                    }
                }

                if (changed) {
                    Settings.save();
                    Display.setBrightness((uint8_t)((s.brightness_pct * 255) / 100));
                    Renderer.markScreenDirty();
                }
            }
        }
    }

    // SCREEN_SETTINGS_MENU: Interactive Touch & Navigation Handling
    if (!g_telemetry.is_locked && g_telemetry.screen == SCREEN_SETTINGS_MENU) {
        int16_t tx = -1, ty = -1;
        if (Buttons.getLastTap(&tx, &ty)) {
            Renderer.handleSettingsTouch(tx, ty, g_telemetry);
            Buttons.clearNavAction(); // Clear any pending action to prevent double-invocation
        } else if (Buttons.isTouching()) {
            int16_t cx = Buttons.getTouchX();
            int16_t cy = Buttons.getTouchY();
            if (cx >= 120 && cx <= 680 && ((cy >= 135 && cy <= 285) || (cy >= 295 && cy <= 445))) {
                Renderer.handleSettingsTouch(cx, cy, g_telemetry);
            }
        }
    }

    NavAction nav = g_telemetry.is_locked ? NAV_NONE : Buttons.getNavAction();

    if (nav != NAV_NONE) {
        if (g_telemetry.screen == SCREEN_SETTINGS_MENU) {
            Renderer.handleMenuNav(nav, g_telemetry);
        } else {
            switch (nav) {
                case NAV_SCREEN_DASH:
                    if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                        g_telemetry.race_state = 0;
                        g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                        ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                        g_telemetry.boost_active = false;
                        g_telemetry.race_popup_open = false;
                        g_telemetry.race_dyno_open = false;
                        Buttons.setRaceState(0);
                        Buttons.setRacePopupOpen(false);
                        Buttons.setRaceDynoOpen(false);
                    }
                    g_telemetry.screen = SCREEN_RIDE_DASH;
                    Renderer.markScreenDirty();
                    Renderer.invalidateCache();
                    break;
                case NAV_BTN1_SHORT: // Prev Screen
                    {
                        if (g_telemetry.screen == SCREEN_RT_GRAPHS || g_telemetry.screen == SCREEN_RACE_TIMER) {
                            if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                                g_telemetry.race_state = 0;
                                g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                                ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                                g_telemetry.boost_active = false;
                                g_telemetry.race_popup_open = false;
                                g_telemetry.race_dyno_open = false;
                                Buttons.setRaceState(0);
                                Buttons.setRacePopupOpen(false);
                                Buttons.setRaceDynoOpen(false);
                            }
                            g_telemetry.screen = SCREEN_RIDE_DASH;
                            Renderer.markScreenDirty();
                        } else {
#if ENABLE_LIGHTS_SCREEN
                            int cur = (int)g_telemetry.screen;
                            g_telemetry.screen = (DashboardScreen)((cur - 1 + 3) % 3);
#else
                            g_telemetry.screen = (g_telemetry.screen == SCREEN_RIDE_DASH) ? SCREEN_RT_GRAPHS : SCREEN_RIDE_DASH;
#endif
                        }
                        Renderer.markScreenDirty();
                    }
                    break;
                case NAV_BTN2_SHORT: // Next Screen
                    {
                        if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                            g_telemetry.race_state = 0;
                            g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                            ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                            g_telemetry.boost_active = false;
                            g_telemetry.race_popup_open = false;
                            g_telemetry.race_dyno_open = false;
                            Buttons.setRaceState(0);
                            Buttons.setRacePopupOpen(false);
                            Buttons.setRaceDynoOpen(false);
                            g_telemetry.screen = SCREEN_RIDE_DASH;
                        } else if (g_telemetry.screen == SCREEN_RT_GRAPHS) {
                            g_telemetry.screen = SCREEN_RIDE_DASH;
                        } else {
#if ENABLE_LIGHTS_SCREEN
                            int cur = (int)g_telemetry.screen;
                            g_telemetry.screen = (DashboardScreen)((cur + 1) % 3);
#else
                            g_telemetry.screen = (g_telemetry.screen == SCREEN_RIDE_DASH) ? SCREEN_RT_GRAPHS : SCREEN_RIDE_DASH;
#endif
                        }
                        Renderer.markScreenDirty();
                    }
                    break;
                case NAV_SCREEN_STATS:
                    if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                        g_telemetry.race_state = 0;
                        g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                        ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                        g_telemetry.boost_active = false;
                        g_telemetry.race_popup_open = false;
                        g_telemetry.race_dyno_open = false;
                        Buttons.setRaceState(0);
                        Buttons.setRacePopupOpen(false);
                        Buttons.setRaceDynoOpen(false);
                    }
                    g_telemetry.screen = SCREEN_RT_GRAPHS;
                    Renderer.markScreenDirty();
                    break;
                case NAV_SCREEN_RACE:
                    if (!g_telemetry.tuning_mode) {
                        break;
                    }
                    if (g_telemetry.screen != SCREEN_RACE_TIMER) {
                        g_telemetry.saved_ride_mode = g_telemetry.ride_mode;
                    }
                    g_telemetry.screen = SCREEN_RACE_TIMER;
                    if (!isRaceThermalBlocked(g_telemetry)) {
                        g_telemetry.ride_mode = MODE_RACE;
                        ModeManager::instance().setRideMode(MODE_RACE);
                        g_telemetry.boost_active = true;
                    } else {
                        g_telemetry.boost_active = false;
                    }
                    g_telemetry.boost_end_ms = g_sim_uptime_ms + 60000;
                    Renderer.markScreenDirty();
                    break;
                case NAV_RACE_TOGGLE_POPUP:
                    g_telemetry.race_popup_open = !g_telemetry.race_popup_open;
                    if (g_telemetry.race_popup_open) g_telemetry.race_dyno_open = false;
                    Buttons.setRacePopupOpen(g_telemetry.race_popup_open);
                    Buttons.setRaceDynoOpen(g_telemetry.race_dyno_open);
                    Renderer.markScreenDirty();
                    break;
                case NAV_RACE_TOGGLE_DYNO:
                    g_telemetry.race_dyno_open = !g_telemetry.race_dyno_open;
                    if (g_telemetry.race_dyno_open) g_telemetry.race_popup_open = false;
                    Buttons.setRacePopupOpen(g_telemetry.race_popup_open);
                    Buttons.setRaceDynoOpen(g_telemetry.race_dyno_open);
                    Renderer.markScreenDirty();
                    break;
                case NAV_RACE_CLOSE_POPUP:
                    g_telemetry.race_popup_open = false;
                    g_telemetry.race_dyno_open = false;
                    Buttons.setRacePopupOpen(false);
                    Buttons.setRaceDynoOpen(false);
                    Renderer.markScreenDirty();
                    break;
                case NAV_RACE_RESET_RECORDS:
                    g_telemetry.race_records.reset();
                    Settings.saveRaceRecords(g_telemetry.race_records);
                    Renderer.markScreenDirty();
                    break;
                case NAV_RACE_START_LAUNCH:
                    if (isRaceThermalBlocked(g_telemetry)) {
                        Renderer.markScreenDirty();
                        break;
                    }
                    g_telemetry.ride_mode = MODE_RACE;
                    ModeManager::instance().setRideMode(MODE_RACE);
                    g_telemetry.boost_active = true;
                    g_telemetry.boost_end_ms = g_sim_uptime_ms + 60000;
                    g_telemetry.race_state = 1; // COUNTDOWN (5s)
                    g_telemetry.race_countdown_start_ms = g_sim_uptime_ms;
                    g_telemetry.race_elapsed_sec = 0.0f;
                    g_telemetry.race_split_0_22 = 0.0f;
                    g_telemetry.race_split_0_50 = 0.0f;
                    g_telemetry.race_split_0_70 = 0.0f;
                    Buttons.setRaceState(1);
                    Renderer.markScreenDirty();
                    break;
                case NAV_RACE_ABORT:
                    g_telemetry.race_state = 0; // IDLE
                    g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                    ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                    g_telemetry.boost_active = false;
                    Buttons.setRaceState(0);
                    Renderer.markScreenDirty();
                    break;
                case NAV_BTN1_LONG: // Settings Menu
                    if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                        g_telemetry.race_state = 0;
                        g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                        ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                        g_telemetry.boost_active = false;
                        g_telemetry.race_popup_open = false;
                        g_telemetry.race_dyno_open = false;
                        Buttons.setRaceState(0);
                        Buttons.setRacePopupOpen(false);
                        Buttons.setRaceDynoOpen(false);
                    }
                    g_telemetry.screen = SCREEN_SETTINGS_MENU;
                    Renderer.markScreenDirty();
                    break;
                case NAV_BTN2_LONG: // Trip Reset
                    g_telemetry.trip_km = 0.0f;
                    g_telemetry.stats.reset();
                    Battery.resetTrip();
                    Vesc.resetTripOdometer();
                    Renderer.markScreenDirty();
                    break;
                case NAV_BOOST_TOGGLE:
                    g_telemetry.boost_active = !g_telemetry.boost_active;
                    if (g_telemetry.boost_active) {
                        g_telemetry.boost_end_ms = g_sim_uptime_ms + 10000;
                    }
                    ensureZeroSpeedIfIdle();
                    Renderer.markScreenDirty();
                    break;
                case NAV_TUNING_ENABLE:
                    g_telemetry.tuning_mode = true;
                    Buttons.setTuningMode(true);
                    ModeManager::instance().setTuningMode(true);
                    g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
                    ensureZeroSpeedIfIdle();
                    Renderer.markScreenDirty();
                    break;
                case NAV_TUNING_DISABLE:
                    g_telemetry.tuning_mode = false;
                    Buttons.setTuningMode(false);
                    ModeManager::instance().setTuningMode(false);
                    g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
                    if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                        g_telemetry.screen = SCREEN_RIDE_DASH;
                        g_telemetry.race_state = 0;
                        g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                        ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                        g_telemetry.boost_active = false;
                        g_telemetry.race_popup_open = false;
                        g_telemetry.race_dyno_open = false;
                        Buttons.setRaceState(0);
                        Buttons.setRacePopupOpen(false);
                        Buttons.setRaceDynoOpen(false);
                        Renderer.invalidateCache();
                    }
                    ensureZeroSpeedIfIdle();
                    Renderer.markScreenDirty();
                    break;
                case NAV_MODE_CYCLE:
                    g_telemetry.ride_mode = (RideMode)(((int)g_telemetry.ride_mode + 1) % MODE_COUNT);
                    ModeManager::instance().setRideMode(g_telemetry.ride_mode);
                    g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
                    ensureZeroSpeedIfIdle();
                    Renderer.markScreenDirty();
                    break;
                    Renderer.markScreenDirty();
                    break;
                case NAV_HAZARD_TOGGLE:
                    g_telemetry.hazard_active = !g_telemetry.hazard_active;
                    Renderer.markScreenDirty();
                    break;
                case NAV_BOTH_PRESSED:
                    {
                        uint8_t p = (Settings.get().active_battery_profile == 0) ? 1 : 0;
                        Settings.get().active_battery_profile = p;
                        Battery.setActiveProfile(p);
                        Settings.save();
                    }
                    break;
                default:
                    break;
            }
        }
    }

    // 2. Network & Loopback packet dispatch before physics
    g_tcp_server.update(g_virtual_vesc);
    g_virtual_vesc.process();

    // Physics & Motor Telemetry update
    float dt = step_ms / 1000.0f;
    runSimulationStep(dt);

    // End-to-end VESC protocol communication:
    // Process incoming commands on Virtual VESC
    g_virtual_vesc.process();
    g_tcp_server.update(g_virtual_vesc);

    // Dashboard polls Virtual VESC, receives response packets, validates CRC, parses telemetry
    Vesc.update(g_telemetry);
    g_virtual_vesc.process();
    g_tcp_server.update(g_virtual_vesc);

    // Multi-ESC dual thermals sync
    g_telemetry.temp_fet_rear  = g_telemetry.temp_esc;
    g_telemetry.temp_mot_rear  = g_telemetry.temp_motor;
    g_telemetry.temp_fet_front = s_sim_temp_fet_front;
    g_telemetry.temp_mot_front = s_sim_temp_mot_front;

    // Dual controller active limits for RT waveform screens
    const VirtualVescLimits &lim1 = g_virtual_vesc.getLimits();
    const VirtualVescLimits &lim2 = g_virtual_vesc.getCanLimits(1);
    g_telemetry.vesc1_max_power = lim1.max_watt;
    g_telemetry.vesc1_max_phase = lim1.max_phase_amps;
    float sim_wheel_circ_m = (Settings.get().wheel_diameter_in * 0.0254f) * M_PI;
    int sim_pole_pairs = Settings.get().motor_pole_pairs > 1 ? Settings.get().motor_pole_pairs : 15;
    float sim_gear = Settings.get().gear_ratio > 0.1f ? Settings.get().gear_ratio : 1.0f;
    g_telemetry.vesc1_max_speed = (lim1.max_erpm / (float)sim_pole_pairs / sim_gear) * sim_wheel_circ_m * 60.0f / 1000.0f;
    g_telemetry.vesc2_max_power = lim2.max_watt;
    g_telemetry.vesc2_max_phase = lim2.max_phase_amps;
    g_telemetry.vesc2_max_speed = g_telemetry.vesc1_max_speed;

    // Update Waveform History every 1000ms
    if (g_sim_uptime_ms - g_telemetry.last_history_sample_ms >= 1000) {
        g_telemetry.last_history_sample_ms = g_sim_uptime_ms;
        for (int i = 0; i < WAVE_HISTORY_LEN - 1; i++) {
            g_telemetry.speed_history[i] = g_telemetry.speed_history[i + 1];
            g_telemetry.power_history[i] = g_telemetry.power_history[i + 1];
        }
        g_telemetry.speed_history[WAVE_HISTORY_LEN - 1] = g_telemetry.speed_kmh;
        g_telemetry.power_history[WAVE_HISTORY_LEN - 1] = g_telemetry.power_watts;
    }

    // 3. Render frame to sprite
    Renderer.render(g_telemetry);
}

static bool g_last_sim_touch_down = false;

void sim_touch_event(int is_down, int x, int y) {
    if (x < 0) x = 0;
    if (x >= LCD_WIDTH) x = LCD_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= LCD_HEIGHT) y = LCD_HEIGHT - 1;

    bool is_new_down = (is_down && !g_last_sim_touch_down);
    g_last_sim_touch_down = (is_down != 0);

    // 0. PIN Lockscreen Input Processing (Interactive Keypad when locked)
    if (g_telemetry.is_locked) {
        if (is_new_down) {
            if (g_telemetry.pin_error) {
                if (g_sim_uptime_ms >= g_telemetry.pin_error_end_ms) {
                    g_telemetry.pin_error = false;
                    Renderer.markScreenDirty();
                } else {
                    return; // Ignore taps during error cooldown
                }
            }

            int c = -1;
            if (x >= 150 && x < 330) c = 0;
            else if (x >= 330 && x < 470) c = 1;
            else if (x >= 470 && x <= 650) c = 2;

            int r = -1;
            if (y >= 140 && y < 227) r = 0;
            else if (y >= 227 && y < 297) r = 1;
            else if (y >= 297 && y < 367) r = 2;
            else if (y >= 367 && y <= 445) r = 3;

            if (r >= 0 && c >= 0) {
                static const char KEY_CHARS[4][3] = {
                    { '1', '2', '3' },
                    { '4', '5', '6' },
                    { '7', '8', '9' },
                    { '<', '0', '#' }
                };
                char key = KEY_CHARS[r][c];
                int len = strlen(g_telemetry.pin_entry);

                if (key == '<') {
                    if (len > 0) {
                        g_telemetry.pin_entry[len - 1] = '\0';
                        g_telemetry.pin_error = false;
                        Renderer.markScreenDirty();
                    }
                } else if (key == '#') {
                    if (strcmp(g_telemetry.pin_entry, Settings.get().pin_code) == 0) {
                        g_telemetry.is_locked = false;
                        g_telemetry.pin_error = false;
                        g_telemetry.pin_entry[0] = '\0';
                        g_manual_throttle = 0.0f;
                        g_manual_brake = 0.0f;
                        s_sim_speed_kmh = 0.0f;
                        ModeManager::instance().setLocked(false);
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        Renderer.triggerNeedleSweep();
                        printf("[SIM_PIN] System UNLOCKED with valid PIN code!\n");
                    } else {
                        g_telemetry.pin_error = true;
                        g_telemetry.pin_error_end_ms = g_sim_uptime_ms + 800;
                        g_telemetry.pin_entry[0] = '\0';
                        Renderer.markScreenDirty();
                        printf("[SIM_PIN] Invalid PIN code entered!\n");
                    }
                } else if (key >= '0' && key <= '9') {
                    if (len < 4) {
                        g_telemetry.pin_entry[len] = key;
                        g_telemetry.pin_entry[len + 1] = '\0';
                        g_telemetry.pin_error = false;
                        len++;
                        Renderer.markScreenDirty();

                        if (len == 4) {
                            if (strcmp(g_telemetry.pin_entry, Settings.get().pin_code) == 0) {
                                g_telemetry.is_locked = false;
                                g_telemetry.pin_error = false;
                                g_telemetry.pin_entry[0] = '\0';
                                g_manual_throttle = 0.0f;
                                g_manual_brake = 0.0f;
                                s_sim_speed_kmh = 0.0f;
                                ModeManager::instance().setLocked(false);
                                Renderer.markScreenDirty();
                                Renderer.invalidateCache();
                                Renderer.triggerNeedleSweep();
                                printf("[SIM_PIN] System UNLOCKED automatically upon 4 digits!\n");
                            } else {
                                g_telemetry.pin_error = true;
                                g_telemetry.pin_error_end_ms = g_sim_uptime_ms + 800;
                                g_telemetry.pin_entry[0] = '\0';
                                Renderer.markScreenDirty();
                                printf("[SIM_PIN] Invalid PIN code entered!\n");
                            }
                        }
                    }
                }
            }
        }
        return;
    }

    Buttons.setActiveScreen(g_telemetry.screen);
    Buttons.setRacePopupOpen(g_telemetry.race_popup_open);
    Buttons.setRaceDynoOpen(g_telemetry.race_dyno_open);
    Buttons.setRaceState(g_telemetry.race_state);
    Buttons.setTuningMode(g_telemetry.tuning_mode);
    Display.setSimTouch(is_down != 0, (int16_t)x, (int16_t)y);
    Buttons.update();
    Renderer.updateRaceTimer(g_telemetry, g_sim_uptime_ms);

    NavAction act = g_telemetry.is_locked ? NAV_NONE : Buttons.getNavAction();
    if (act != NAV_NONE) {
        sim_trigger_nav((int)act);
    }
}

void sim_trigger_nav(int nav_action) {
    if (g_telemetry.screen == SCREEN_VESC_BRIDGE || g_telemetry.screen == SCREEN_WIFI_BRIDGE) {
        Vesc.exitBridgeMode();
        Vesc.exitWifiBridgeMode();
        g_telemetry.screen = SCREEN_SETTINGS_MENU;
        Renderer.markScreenDirty();
        Renderer.invalidateCache();
        Buttons.setActiveScreen(SCREEN_SETTINGS_MENU);
        return;
    }
    if (g_telemetry.screen == SCREEN_SETTINGS_MENU) {
        Renderer.handleMenuNav((NavAction)nav_action, g_telemetry);
    } else {
        switch ((NavAction)nav_action) {
            case NAV_BTN1_SHORT: {
                if (g_telemetry.screen == SCREEN_RT_GRAPHS || g_telemetry.screen == SCREEN_RACE_TIMER) {
                    if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                        g_telemetry.race_state = 0;
                        g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                        ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                        g_telemetry.boost_active = false;
                        g_telemetry.race_popup_open = false;
                        g_telemetry.race_dyno_open = false;
                        Buttons.setRaceState(0);
                        Buttons.setRacePopupOpen(false);
                        Buttons.setRaceDynoOpen(false);
                    }
                    g_telemetry.screen = SCREEN_RIDE_DASH;
                } else {
#if ENABLE_LIGHTS_SCREEN
                    int cur = (int)g_telemetry.screen;
                    g_telemetry.screen = (DashboardScreen)((cur - 1 + 3) % 3);
#else
                    g_telemetry.screen = (g_telemetry.screen == SCREEN_RIDE_DASH) ? SCREEN_RT_GRAPHS : SCREEN_RIDE_DASH;
#endif
                }
                Renderer.markScreenDirty();
                break;
            }
            case NAV_BTN2_SHORT: {
                if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                    g_telemetry.race_state = 0;
                    g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                    ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                    g_telemetry.boost_active = false;
                    g_telemetry.race_popup_open = false;
                    g_telemetry.race_dyno_open = false;
                    Buttons.setRaceState(0);
                    Buttons.setRacePopupOpen(false);
                    Buttons.setRaceDynoOpen(false);
                    g_telemetry.screen = SCREEN_RIDE_DASH;
                } else if (g_telemetry.screen == SCREEN_RT_GRAPHS) {
                    g_telemetry.screen = SCREEN_RIDE_DASH;
                } else {
#if ENABLE_LIGHTS_SCREEN
                    int cur = (int)g_telemetry.screen;
                    g_telemetry.screen = (DashboardScreen)((cur + 1) % 3);
#else
                    g_telemetry.screen = (g_telemetry.screen == SCREEN_RIDE_DASH) ? SCREEN_RT_GRAPHS : SCREEN_RIDE_DASH;
#endif
                }
                Renderer.markScreenDirty();
                break;
            }
            case NAV_SCREEN_STATS:
                if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                    g_telemetry.race_state = 0;
                    g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                    ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                    g_telemetry.boost_active = false;
                    g_telemetry.race_popup_open = false;
                    g_telemetry.race_dyno_open = false;
                    Buttons.setRaceState(0);
                    Buttons.setRacePopupOpen(false);
                    Buttons.setRaceDynoOpen(false);
                }
                g_telemetry.screen = SCREEN_RT_GRAPHS;
                Renderer.markScreenDirty();
                break;
            case NAV_SCREEN_RACE:
                if (!g_telemetry.tuning_mode) {
                    break;
                }
                if (g_telemetry.screen != SCREEN_RACE_TIMER) {
                    g_telemetry.saved_ride_mode = g_telemetry.ride_mode;
                }
                g_telemetry.screen = SCREEN_RACE_TIMER;
                if (!isRaceThermalBlocked(g_telemetry)) {
                    g_telemetry.ride_mode = MODE_RACE;
                    ModeManager::instance().setRideMode(MODE_RACE);
                    g_telemetry.boost_active = true;
                } else {
                    g_telemetry.boost_active = false;
                }
                g_telemetry.boost_end_ms = g_sim_uptime_ms + 60000;
                Renderer.markScreenDirty();
                break;
            case NAV_RACE_TOGGLE_POPUP:
                g_telemetry.race_popup_open = !g_telemetry.race_popup_open;
                if (g_telemetry.race_popup_open) g_telemetry.race_dyno_open = false;
                Buttons.setRacePopupOpen(g_telemetry.race_popup_open);
                Buttons.setRaceDynoOpen(g_telemetry.race_dyno_open);
                Renderer.markScreenDirty();
                break;
            case NAV_RACE_TOGGLE_DYNO:
                g_telemetry.race_dyno_open = !g_telemetry.race_dyno_open;
                if (g_telemetry.race_dyno_open) g_telemetry.race_popup_open = false;
                Buttons.setRacePopupOpen(g_telemetry.race_popup_open);
                Buttons.setRaceDynoOpen(g_telemetry.race_dyno_open);
                Renderer.markScreenDirty();
                break;
            case NAV_RACE_CLOSE_POPUP:
                g_telemetry.race_popup_open = false;
                g_telemetry.race_dyno_open = false;
                Buttons.setRacePopupOpen(false);
                Buttons.setRaceDynoOpen(false);
                Renderer.markScreenDirty();
                break;
            case NAV_RACE_RESET_RECORDS:
                g_telemetry.race_records.reset();
                Settings.saveRaceRecords(g_telemetry.race_records);
                Renderer.markScreenDirty();
                break;
            case NAV_RACE_START_LAUNCH:
                if (isRaceThermalBlocked(g_telemetry)) {
                    Renderer.markScreenDirty();
                    break;
                }
                g_telemetry.ride_mode = MODE_RACE;
                ModeManager::instance().setRideMode(MODE_RACE);
                g_telemetry.boost_active = true;
                g_telemetry.boost_end_ms = g_sim_uptime_ms + 60000;
                g_telemetry.race_state = 1;
                g_telemetry.race_countdown_start_ms = g_sim_uptime_ms;
                g_telemetry.race_elapsed_sec = 0.0f;
                g_telemetry.race_split_0_22 = 0.0f;
                g_telemetry.race_split_0_50 = 0.0f;
                g_telemetry.race_split_0_70 = 0.0f;
                Buttons.setRaceState(1);
                Renderer.markScreenDirty();
                break;
            case NAV_RACE_ABORT:
                g_telemetry.race_state = 0;
                g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                g_telemetry.boost_active = false;
                Buttons.setRaceState(0);
                Renderer.markScreenDirty();
                break;
            case NAV_SCREEN_DASH:
                if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                    g_telemetry.race_state = 0;
                    g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                    ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                    g_telemetry.boost_active = false;
                    g_telemetry.race_popup_open = false;
                    g_telemetry.race_dyno_open = false;
                    Buttons.setRaceState(0);
                    Buttons.setRacePopupOpen(false);
                    Buttons.setRaceDynoOpen(false);
                }
                g_telemetry.screen = SCREEN_RIDE_DASH;
                Renderer.markScreenDirty();
                Renderer.invalidateCache();
                break;
            case NAV_BTN1_LONG:
                if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                    g_telemetry.race_state = 0;
                    g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                    ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                    g_telemetry.boost_active = false;
                    g_telemetry.race_popup_open = false;
                    g_telemetry.race_dyno_open = false;
                    Buttons.setRaceState(0);
                    Buttons.setRacePopupOpen(false);
                    Buttons.setRaceDynoOpen(false);
                }
                g_telemetry.screen = SCREEN_SETTINGS_MENU;
                break;
            case NAV_BTN2_LONG:
                s_sim_trip_km = 0.0f;
                s_sim_wh_used = 0.0f;
                s_sim_ah_used = 0.0f;
                s_sim_ah_charged = 0.0f;
                s_sim_wh_charged = 0.0f;
                g_telemetry.trip_km = 0.0f;
                g_telemetry.stats.reset();
                Battery.resetTrip();
                Vesc.resetTripOdometer();
                Renderer.markScreenDirty();
                break;
            case NAV_BOOST_TOGGLE:
                g_telemetry.boost_active = !g_telemetry.boost_active;
                if (g_telemetry.boost_active) {
                    g_telemetry.boost_end_ms = g_sim_uptime_ms + 10000;
                }
                ensureZeroSpeedIfIdle();
                break;
            case NAV_TUNING_ENABLE:
                g_telemetry.tuning_mode = true;
                Buttons.setTuningMode(true);
                ModeManager::instance().setTuningMode(true);
                g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
                ensureZeroSpeedIfIdle();
                break;
            case NAV_TUNING_DISABLE:
                g_telemetry.tuning_mode = false;
                Buttons.setTuningMode(false);
                ModeManager::instance().setTuningMode(false);
                g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
                if (g_telemetry.screen == SCREEN_RACE_TIMER) {
                    g_telemetry.screen = SCREEN_RIDE_DASH;
                    g_telemetry.race_state = 0;
                    g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
                    ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
                    g_telemetry.boost_active = false;
                    g_telemetry.race_popup_open = false;
                    g_telemetry.race_dyno_open = false;
                    Buttons.setRaceState(0);
                    Buttons.setRacePopupOpen(false);
                    Buttons.setRaceDynoOpen(false);
                    Renderer.invalidateCache();
                }
                ensureZeroSpeedIfIdle();
                break;
            case NAV_MODE_CYCLE:
                g_telemetry.ride_mode = (RideMode)(((int)g_telemetry.ride_mode + 1) % MODE_COUNT);
                ModeManager::instance().setRideMode(g_telemetry.ride_mode);
                g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
                ensureZeroSpeedIfIdle();
                break;
            case NAV_HAZARD_TOGGLE:
                g_telemetry.hazard_active = !g_telemetry.hazard_active;
                break;
            case NAV_LIGHT_TOGGLE:
                g_telemetry.lights_enabled = !g_telemetry.lights_enabled;
                break;
            default:
                break;
        }
    }
    Renderer.markScreenDirty();
}

void sim_toggle_lights() {
    g_telemetry.lights_enabled = !g_telemetry.lights_enabled;
    Renderer.markScreenDirty();
}

void sim_toggle_boost() {
    g_telemetry.boost_active = !g_telemetry.boost_active;
    if (g_telemetry.boost_active) {
        g_telemetry.boost_end_ms = g_sim_uptime_ms + 10000;
    }
    ensureZeroSpeedIfIdle();
    Renderer.markScreenDirty();
}

void sim_cycle_mode() {
    g_telemetry.ride_mode = (RideMode)(((int)g_telemetry.ride_mode + 1) % MODE_COUNT);
    ModeManager::instance().setRideMode(g_telemetry.ride_mode);
    g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
    ensureZeroSpeedIfIdle();
    Renderer.markScreenDirty();
}

void sim_set_ride_mode(int mode) {
    if (mode >= 0 && mode < MODE_COUNT) {
        g_telemetry.ride_mode = (RideMode)mode;
        ModeManager::instance().setRideMode(g_telemetry.ride_mode);
        g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
        ensureZeroSpeedIfIdle();
        Renderer.markScreenDirty();
    }
}

void sim_toggle_hazard() {
    g_telemetry.hazard_active = !g_telemetry.hazard_active;
    Renderer.markScreenDirty();
}

void sim_set_light_preset(int preset_id) {
    if (preset_id >= 0 && preset_id < 9) {
        g_telemetry.light_mode = preset_id;
        Renderer.markScreenDirty();
    }
}

void sim_set_light_hue(int hue) {
    g_telemetry.light_hue = (uint8_t)(hue & 0xFF);
    Renderer.markScreenDirty();
}

void sim_set_tuning_mode(int enable) {
    g_telemetry.tuning_mode = (enable != 0);
    Buttons.setTuningMode(g_telemetry.tuning_mode);
    ModeManager::instance().setTuningMode(g_telemetry.tuning_mode);
    if (!g_telemetry.tuning_mode && g_telemetry.screen == SCREEN_RACE_TIMER) {
        g_telemetry.screen = SCREEN_RIDE_DASH;
        g_telemetry.race_state = 0;
        g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
        ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
        g_telemetry.boost_active = false;
        g_telemetry.race_popup_open = false;
        g_telemetry.race_dyno_open = false;
        Buttons.setRaceState(0);
        Buttons.setRacePopupOpen(false);
        Buttons.setRaceDynoOpen(false);
        Renderer.invalidateCache();
    }
    ensureZeroSpeedIfIdle();
    Renderer.markScreenDirty();
}

void sim_toggle_tuning_mode() {
    g_telemetry.tuning_mode = !g_telemetry.tuning_mode;
    Buttons.setTuningMode(g_telemetry.tuning_mode);
    ModeManager::instance().setTuningMode(g_telemetry.tuning_mode);
    if (!g_telemetry.tuning_mode && g_telemetry.screen == SCREEN_RACE_TIMER) {
        g_telemetry.screen = SCREEN_RIDE_DASH;
        g_telemetry.race_state = 0;
        g_telemetry.ride_mode = g_telemetry.saved_ride_mode;
        ModeManager::instance().setRideMode(g_telemetry.saved_ride_mode);
        g_telemetry.boost_active = false;
        g_telemetry.race_popup_open = false;
        g_telemetry.race_dyno_open = false;
        Buttons.setRaceState(0);
        Buttons.setRacePopupOpen(false);
        Buttons.setRaceDynoOpen(false);
        Renderer.invalidateCache();
    }
    ensureZeroSpeedIfIdle();
    Renderer.markScreenDirty();
}

void sim_set_incline(float deg) {
    s_sim_incline_deg = deg;
    g_telemetry.incline_deg = deg;
    Renderer.markScreenDirty();
}

void sim_set_auto_mode(int enable) {
    (void)enable;
}

void sim_toggle_auto_mode() {
}

void sim_set_manual_throttle(float throttle_pct) {
    if (g_telemetry.is_locked) {
        g_manual_throttle = 0.0f;
        return;
    }
    if (throttle_pct <= 0.01f) throttle_pct = 0.0f;
    if (throttle_pct > 100.0f) throttle_pct = 100.0f;
    g_manual_throttle = throttle_pct;
    g_virtual_vesc.resetExternalControl();
    ensureZeroSpeedIfIdle();
}

void sim_set_manual_brake(float brake_pct) {
    if (brake_pct < 0.0f) brake_pct = 0.0f;
    if (brake_pct > 100.0f) brake_pct = 100.0f;
    g_manual_brake = brake_pct;
    g_virtual_vesc.resetExternalControl();
}

void sim_set_dash_style(int style) {
    Settings.get().dash_style = (uint8_t)style;
    Settings.save();
    Renderer.markScreenDirty();
}

void sim_reset_trip() {
    g_telemetry.trip_km = 0.0f;
    g_telemetry.stats.reset();
}

void sim_get_frame_rgb24(uint8_t *rgb_out) {
    if (!rgb_out) return;
    uint16_t *buf = (uint16_t*)Display.getSprite().getBuffer();
    int w = LCD_WIDTH;
    int h = LCD_HEIGHT;
    size_t idx = 0;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint16_t raw = buf[y * w + x];
            uint16_t c = (raw >> 8) | (raw << 8);
            uint8_t r = ((c >> 11) & 0x1F) * 255 / 31;
            uint8_t g = ((c >> 5) & 0x3F) * 255 / 63;
            uint8_t b = (c & 0x1F) * 255 / 31;
            rgb_out[idx++] = r;
            rgb_out[idx++] = g;
            rgb_out[idx++] = b;
        }
    }
}

void sim_get_telemetry(SimTelemetryExport *out) {
    if (!out) return;
    out->speed_kmh = g_telemetry.speed_kmh;
    out->duty_cycle_pct = g_telemetry.duty_cycle_pct;
    out->throttle_pct = g_telemetry.throttle_pct;
    out->voltage = g_telemetry.voltage;
    out->current_amps = g_telemetry.current_amps;
    out->phase_amps = g_telemetry.phase_amps;
    out->power_watts = g_telemetry.power_watts;
    out->battery_pct = g_telemetry.battery_pct;
    out->remaining_wh = g_telemetry.remaining_wh;
    out->est_range_km = g_telemetry.est_range_km;
    out->temp_motor = g_telemetry.temp_motor;
    out->temp_esc = g_telemetry.temp_esc;
    out->trip_km = g_telemetry.trip_km;
    out->odo_km = g_telemetry.odo_km;
    out->screen = (int32_t)g_telemetry.screen;
    out->dash_style = (int32_t)Settings.get().dash_style;
    out->auto_mode = 0;
    out->ride_mode = (int32_t)g_telemetry.ride_mode;
    out->boost_active = g_telemetry.boost_active ? 1 : 0;
    out->hazard_active = g_telemetry.hazard_active ? 1 : 0;
    out->light_mode = (int32_t)g_telemetry.light_mode;
    out->light_hue = (int32_t)g_telemetry.light_hue;
    out->temp_fet_rear = g_telemetry.temp_fet_rear;
    out->temp_mot_rear = g_telemetry.temp_mot_rear;
    out->temp_fet_front = g_telemetry.temp_fet_front;
    out->temp_mot_front = g_telemetry.temp_mot_front;
    out->max_speed_kmh = g_telemetry.stats.max_speed_kmh;
    out->avg_speed_kmh = g_telemetry.stats.avg_speed_kmh;
    out->ride_time_sec = g_telemetry.stats.ride_time_sec;
    out->tuning_mode = g_telemetry.tuning_mode ? 1 : 0;
    out->is_locked = g_telemetry.is_locked ? 1 : 0;
    out->race_state = (int32_t)g_telemetry.race_state;
    out->race_elapsed_sec = g_telemetry.race_elapsed_sec;
    out->race_split_0_22 = g_telemetry.race_split_0_22;
    out->race_split_0_50 = g_telemetry.race_split_0_50;
    out->race_split_0_70 = g_telemetry.race_split_0_70;
    out->g_force = g_telemetry.g_force;
    out->incline_deg = g_telemetry.incline_deg;
}

void sim_get_config_json(char *out_buf, int max_len) {
    if (!out_buf || max_len < 10) return;
    const VehicleModeConfig &cfg = ModeManager::instance().getConfig();
    snprintf(out_buf, max_len,
        "{\"tuning_mode\":%s,\"active_mode\":%d,"
        "\"legal_eco\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"legal_sport\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"legal_race\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"tuned_eco\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"tuned_sport\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"tuned_race\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"stealth\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"eco\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"sport\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"race\":{\"speed\":%.1f,\"power\":%.0f,\"pull\":%.0f,\"bat_a\":%.1f,\"pha_a\":%.1f},"
        "\"throttle_curve\":%d,\"battery_cells\":%d,\"cutoff_start\":%.1f,\"cutoff_end\":%.1f,"
        "\"pin_lock_enabled\":%s,\"pin_code\":\"%s\"}",
        g_telemetry.tuning_mode ? "true" : "false",
        (int)g_telemetry.ride_mode,
        cfg.legal_modes[PROFILE_MODE_ECO].max_speed_kmh, cfg.legal_modes[PROFILE_MODE_ECO].max_power_watts,
        cfg.legal_modes[PROFILE_MODE_ECO].max_pull_watts, cfg.legal_modes[PROFILE_MODE_ECO].max_battery_amps,
        cfg.legal_modes[PROFILE_MODE_ECO].max_phase_amps,
        cfg.legal_modes[PROFILE_MODE_SPORT].max_speed_kmh, cfg.legal_modes[PROFILE_MODE_SPORT].max_power_watts,
        cfg.legal_modes[PROFILE_MODE_SPORT].max_pull_watts, cfg.legal_modes[PROFILE_MODE_SPORT].max_battery_amps,
        cfg.legal_modes[PROFILE_MODE_SPORT].max_phase_amps,
        cfg.legal_modes[PROFILE_MODE_RACE].max_speed_kmh, cfg.legal_modes[PROFILE_MODE_RACE].max_power_watts,
        cfg.legal_modes[PROFILE_MODE_RACE].max_pull_watts, cfg.legal_modes[PROFILE_MODE_RACE].max_battery_amps,
        cfg.legal_modes[PROFILE_MODE_RACE].max_phase_amps,
        cfg.tuned_modes[PROFILE_MODE_ECO].max_speed_kmh, cfg.tuned_modes[PROFILE_MODE_ECO].max_power_watts,
        cfg.tuned_modes[PROFILE_MODE_ECO].max_pull_watts, cfg.tuned_modes[PROFILE_MODE_ECO].max_battery_amps,
        cfg.tuned_modes[PROFILE_MODE_ECO].max_phase_amps,
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_speed_kmh, cfg.tuned_modes[PROFILE_MODE_SPORT].max_power_watts,
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_pull_watts, cfg.tuned_modes[PROFILE_MODE_SPORT].max_battery_amps,
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_phase_amps,
        cfg.tuned_modes[PROFILE_MODE_RACE].max_speed_kmh, cfg.tuned_modes[PROFILE_MODE_RACE].max_power_watts,
        cfg.tuned_modes[PROFILE_MODE_RACE].max_pull_watts, cfg.tuned_modes[PROFILE_MODE_RACE].max_battery_amps,
        cfg.tuned_modes[PROFILE_MODE_RACE].max_phase_amps,
        cfg.legal_modes[PROFILE_MODE_SPORT].max_speed_kmh, cfg.legal_modes[PROFILE_MODE_SPORT].max_power_watts,
        cfg.legal_modes[PROFILE_MODE_SPORT].max_pull_watts, cfg.legal_modes[PROFILE_MODE_SPORT].max_battery_amps,
        cfg.legal_modes[PROFILE_MODE_SPORT].max_phase_amps,
        cfg.tuned_modes[PROFILE_MODE_ECO].max_speed_kmh, cfg.tuned_modes[PROFILE_MODE_ECO].max_power_watts,
        cfg.tuned_modes[PROFILE_MODE_ECO].max_pull_watts, cfg.tuned_modes[PROFILE_MODE_ECO].max_battery_amps,
        cfg.tuned_modes[PROFILE_MODE_ECO].max_phase_amps,
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_speed_kmh, cfg.tuned_modes[PROFILE_MODE_SPORT].max_power_watts,
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_pull_watts, cfg.tuned_modes[PROFILE_MODE_SPORT].max_battery_amps,
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_phase_amps,
        cfg.tuned_modes[PROFILE_MODE_RACE].max_speed_kmh, cfg.tuned_modes[PROFILE_MODE_RACE].max_power_watts,
        cfg.tuned_modes[PROFILE_MODE_RACE].max_pull_watts, cfg.tuned_modes[PROFILE_MODE_RACE].max_battery_amps,
        cfg.tuned_modes[PROFILE_MODE_RACE].max_phase_amps,
        (int)cfg.throttle_curve, (int)cfg.battery_cells,
        cfg.battery_cutoff_start, cfg.battery_cutoff_end,
        Settings.get().pin_lock_enabled ? "true" : "false",
        Settings.get().pin_code
    );
}

static float parseSimJsonFloat(const std::string &src, const char *key, float fallback) {
    size_t idx = src.find(key);
    if (idx == std::string::npos) return fallback;
    idx = src.find(':', idx);
    if (idx == std::string::npos) return fallback;
    idx++;
    while (idx < src.length() && (src[idx] == ' ' || src[idx] == '\"')) idx++;
    if (idx >= src.length()) return fallback;
    try {
        return std::stof(src.substr(idx));
    } catch (...) {
        return fallback;
    }
}

static int parseSimJsonInt(const std::string &src, const char *key, int fallback) {
    size_t idx = src.find(key);
    if (idx == std::string::npos) return fallback;
    idx = src.find(':', idx);
    if (idx == std::string::npos) return fallback;
    idx++;
    while (idx < src.length() && (src[idx] == ' ' || src[idx] == '\"')) idx++;
    if (idx >= src.length()) return fallback;
    try {
        return std::stoi(src.substr(idx));
    } catch (...) {
        return fallback;
    }
}

static std::string extractSimJsonObject(const std::string &src, const char *key) {
    size_t idx = src.find(key);
    if (idx == std::string::npos) return "";
    size_t b_start = src.find('{', idx);
    if (b_start == std::string::npos) return "";
    size_t b_end = src.find('}', b_start);
    if (b_end == std::string::npos) return "";
    return src.substr(b_start, b_end - b_start + 1);
}

int sim_set_config_json(const char *json_in) {
    if (!json_in) return 0;
    std::string body(json_in);
    if (body.length() < 10) return 0;

    VehicleModeConfig &cfg = ModeManager::instance().getConfig();

    // Legal ECO
    std::string le_part = extractSimJsonObject(body, "\"legal_eco\"");
    if (le_part.length() > 0) {
        cfg.legal_modes[PROFILE_MODE_ECO].max_speed_kmh    = parseSimJsonFloat(le_part, "\"speed\"", cfg.legal_modes[PROFILE_MODE_ECO].max_speed_kmh);
        cfg.legal_modes[PROFILE_MODE_ECO].max_power_watts  = parseSimJsonFloat(le_part, "\"power\"", cfg.legal_modes[PROFILE_MODE_ECO].max_power_watts);
        cfg.legal_modes[PROFILE_MODE_ECO].max_pull_watts   = parseSimJsonFloat(le_part, "\"pull\"", cfg.legal_modes[PROFILE_MODE_ECO].max_pull_watts);
        cfg.legal_modes[PROFILE_MODE_ECO].max_battery_amps = parseSimJsonFloat(le_part, "\"bat_a\"", cfg.legal_modes[PROFILE_MODE_ECO].max_battery_amps);
        cfg.legal_modes[PROFILE_MODE_ECO].max_phase_amps   = parseSimJsonFloat(le_part, "\"pha_a\"", cfg.legal_modes[PROFILE_MODE_ECO].max_phase_amps);
    }

    // Legal SPORT
    std::string ls_part = extractSimJsonObject(body, "\"legal_sport\"");
    if (ls_part.length() == 0) ls_part = extractSimJsonObject(body, "\"stealth\"");
    if (ls_part.length() > 0) {
        cfg.legal_modes[PROFILE_MODE_SPORT].max_speed_kmh    = parseSimJsonFloat(ls_part, "\"speed\"", cfg.legal_modes[PROFILE_MODE_SPORT].max_speed_kmh);
        cfg.legal_modes[PROFILE_MODE_SPORT].max_power_watts  = parseSimJsonFloat(ls_part, "\"power\"", cfg.legal_modes[PROFILE_MODE_SPORT].max_power_watts);
        cfg.legal_modes[PROFILE_MODE_SPORT].max_pull_watts   = parseSimJsonFloat(ls_part, "\"pull\"", cfg.legal_modes[PROFILE_MODE_SPORT].max_pull_watts);
        cfg.legal_modes[PROFILE_MODE_SPORT].max_battery_amps = parseSimJsonFloat(ls_part, "\"bat_a\"", cfg.legal_modes[PROFILE_MODE_SPORT].max_battery_amps);
        cfg.legal_modes[PROFILE_MODE_SPORT].max_phase_amps   = parseSimJsonFloat(ls_part, "\"pha_a\"", cfg.legal_modes[PROFILE_MODE_SPORT].max_phase_amps);
    }

    // Legal RACE
    std::string lr_part = extractSimJsonObject(body, "\"legal_race\"");
    if (lr_part.length() > 0) {
        cfg.legal_modes[PROFILE_MODE_RACE].max_speed_kmh    = parseSimJsonFloat(lr_part, "\"speed\"", cfg.legal_modes[PROFILE_MODE_RACE].max_speed_kmh);
        cfg.legal_modes[PROFILE_MODE_RACE].max_power_watts  = parseSimJsonFloat(lr_part, "\"power\"", cfg.legal_modes[PROFILE_MODE_RACE].max_power_watts);
        cfg.legal_modes[PROFILE_MODE_RACE].max_pull_watts   = parseSimJsonFloat(lr_part, "\"pull\"", cfg.legal_modes[PROFILE_MODE_RACE].max_pull_watts);
        cfg.legal_modes[PROFILE_MODE_RACE].max_battery_amps = parseSimJsonFloat(lr_part, "\"bat_a\"", cfg.legal_modes[PROFILE_MODE_RACE].max_battery_amps);
        cfg.legal_modes[PROFILE_MODE_RACE].max_phase_amps   = parseSimJsonFloat(lr_part, "\"pha_a\"", cfg.legal_modes[PROFILE_MODE_RACE].max_phase_amps);
    }

    // Tuned ECO
    std::string te_part = extractSimJsonObject(body, "\"tuned_eco\"");
    if (te_part.length() == 0) te_part = extractSimJsonObject(body, "\"eco\"");
    if (te_part.length() > 0) {
        cfg.tuned_modes[PROFILE_MODE_ECO].max_speed_kmh    = parseSimJsonFloat(te_part, "\"speed\"", cfg.tuned_modes[PROFILE_MODE_ECO].max_speed_kmh);
        cfg.tuned_modes[PROFILE_MODE_ECO].max_power_watts  = parseSimJsonFloat(te_part, "\"power\"", cfg.tuned_modes[PROFILE_MODE_ECO].max_power_watts);
        cfg.tuned_modes[PROFILE_MODE_ECO].max_pull_watts   = parseSimJsonFloat(te_part, "\"pull\"", cfg.tuned_modes[PROFILE_MODE_ECO].max_pull_watts);
        cfg.tuned_modes[PROFILE_MODE_ECO].max_battery_amps = parseSimJsonFloat(te_part, "\"bat_a\"", cfg.tuned_modes[PROFILE_MODE_ECO].max_battery_amps);
        cfg.tuned_modes[PROFILE_MODE_ECO].max_phase_amps   = parseSimJsonFloat(te_part, "\"pha_a\"", cfg.tuned_modes[PROFILE_MODE_ECO].max_phase_amps);
    }

    // Tuned SPORT
    std::string ts_part = extractSimJsonObject(body, "\"tuned_sport\"");
    if (ts_part.length() == 0) ts_part = extractSimJsonObject(body, "\"sport\"");
    if (ts_part.length() > 0) {
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_speed_kmh    = parseSimJsonFloat(ts_part, "\"speed\"", cfg.tuned_modes[PROFILE_MODE_SPORT].max_speed_kmh);
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_power_watts  = parseSimJsonFloat(ts_part, "\"power\"", cfg.tuned_modes[PROFILE_MODE_SPORT].max_power_watts);
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_pull_watts   = parseSimJsonFloat(ts_part, "\"pull\"", cfg.tuned_modes[PROFILE_MODE_SPORT].max_pull_watts);
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_battery_amps = parseSimJsonFloat(ts_part, "\"bat_a\"", cfg.tuned_modes[PROFILE_MODE_SPORT].max_battery_amps);
        cfg.tuned_modes[PROFILE_MODE_SPORT].max_phase_amps   = parseSimJsonFloat(ts_part, "\"pha_a\"", cfg.tuned_modes[PROFILE_MODE_SPORT].max_phase_amps);
    }

    // Tuned RACE
    std::string tr_part = extractSimJsonObject(body, "\"tuned_race\"");
    if (tr_part.length() == 0) tr_part = extractSimJsonObject(body, "\"race\"");
    if (tr_part.length() > 0) {
        cfg.tuned_modes[PROFILE_MODE_RACE].max_speed_kmh    = parseSimJsonFloat(tr_part, "\"speed\"", cfg.tuned_modes[PROFILE_MODE_RACE].max_speed_kmh);
        cfg.tuned_modes[PROFILE_MODE_RACE].max_power_watts  = parseSimJsonFloat(tr_part, "\"power\"", cfg.tuned_modes[PROFILE_MODE_RACE].max_power_watts);
        cfg.tuned_modes[PROFILE_MODE_RACE].max_pull_watts   = parseSimJsonFloat(tr_part, "\"pull\"", cfg.tuned_modes[PROFILE_MODE_RACE].max_pull_watts);
        cfg.tuned_modes[PROFILE_MODE_RACE].max_battery_amps = parseSimJsonFloat(tr_part, "\"bat_a\"", cfg.tuned_modes[PROFILE_MODE_RACE].max_battery_amps);
        cfg.tuned_modes[PROFILE_MODE_RACE].max_phase_amps   = parseSimJsonFloat(tr_part, "\"pha_a\"", cfg.tuned_modes[PROFILE_MODE_RACE].max_phase_amps);
    }

    cfg.throttle_curve       = (int8_t)parseSimJsonInt(body, "\"throttle_curve\"", cfg.throttle_curve);
    cfg.battery_cells        = (uint8_t)parseSimJsonInt(body, "\"battery_cells\"", cfg.battery_cells);
    cfg.battery_cutoff_start = parseSimJsonFloat(body, "\"cutoff_start\"", cfg.battery_cutoff_start);
    cfg.battery_cutoff_end   = parseSimJsonFloat(body, "\"cutoff_end\"", cfg.battery_cutoff_end);

    // PIN lock settings
    size_t pin_en_idx = body.find("\"pin_lock_enabled\"");
    if (pin_en_idx != std::string::npos) {
        size_t c_idx = body.find(':', pin_en_idx);
        if (c_idx != std::string::npos) {
            c_idx++;
            while (c_idx < body.length() && (body[c_idx] == ' ' || body[c_idx] == '\"')) c_idx++;
            bool en = (body[c_idx] == 't' || body[c_idx] == 'T' || body[c_idx] == '1');
            Settings.get().pin_lock_enabled = en;
            if (!en && g_telemetry.is_locked) {
                g_telemetry.is_locked = false;
                ModeManager::instance().setLocked(false);
                Renderer.markScreenDirty();
                Renderer.invalidateCache();
                Renderer.triggerNeedleSweep();
            }
        }
    }
    size_t pin_code_idx = body.find("\"pin_code\"");
    if (pin_code_idx != std::string::npos) {
        size_t q1 = body.find('\"', pin_code_idx + 10);
        if (q1 != std::string::npos) {
            size_t q2 = body.find('\"', q1 + 1);
            if (q2 != std::string::npos && q2 > q1) {
                std::string code = body.substr(q1 + 1, q2 - q1 - 1);
                if (code.length() == 4) {
                    bool all_digits = true;
                    for (char c : code) {
                        if (c < '0' || c > '9') { all_digits = false; break; }
                    }
                    if (all_digits) {
                        strncpy(Settings.get().pin_code, code.c_str(), sizeof(Settings.get().pin_code) - 1);
                        Settings.get().pin_code[sizeof(Settings.get().pin_code) - 1] = '\0';
                    }
                }
            }
        }
    }

    ModeManager::instance().saveToNvs();
    ModeManager::instance().applyLimitsToVesc();
    Renderer.markScreenDirty();
    return 1;
}

} // extern "C"
