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


// -------------------------------------------------------------
// VescHandler Implementation for Simulator
// -------------------------------------------------------------
VescHandler::VescHandler()
    : _bridge_active(false),
      _bridge_pc_to_vesc(0),
      _bridge_vesc_to_pc(0),
      _wifi_bridge_active(false),
      _wifi_client_connected(false),
      _bridge_wifi_to_vesc(0),
      _bridge_vesc_to_wifi(0),
      _sync_status(SYNC_IDLE),
      _sync_ms(0),
      _mcconf_req_ms(0) {
    _wifi_client_ip[0] = '\0';
}
void VescHandler::begin() {}
void VescHandler::update(DashTelemetry &) {}
void VescHandler::setSimThrottle(float) {}
void VescHandler::enterBridgeMode() { _bridge_active = true; }
void VescHandler::exitBridgeMode() { _bridge_active = false; }
void VescHandler::enterWifiBridgeMode() {
    _wifi_bridge_active = true;
    _wifi_client_connected = true;
    strncpy(_wifi_client_ip, "192.168.4.2", sizeof(_wifi_client_ip));
    _bridge_wifi_to_vesc = 1420;
    _bridge_vesc_to_wifi = 3680;
}
void VescHandler::exitWifiBridgeMode() { _wifi_bridge_active = false; _wifi_client_connected = false; }
void VescHandler::updateBridge() {}
void VescHandler::updateUsbBridge() {}
void VescHandler::updateWifiBridge() {}
void VescHandler::requestMcconf() { _sync_status = SYNC_SUCCESS; }
void VescHandler::sendModeLimits(float max_speed_kmh, float max_power_watts, float max_battery_amps, float max_phase_amps, float cutoff_start, float cutoff_end) {
    Settings.get().max_speed_kmh    = (uint8_t)roundf(max_speed_kmh);
    Settings.get().max_battery_amps = (uint16_t)roundf(max_battery_amps);
    Settings.get().max_phase_amps   = (uint16_t)roundf(max_phase_amps);
}
void VescHandler::sendTerminalCommand(const char *) {}
VescHandler Vesc;

MockSerial Serial;
DisplayDriver Display;
extern DashboardRenderer Renderer;
extern ButtonHandler Buttons;

// -------------------------------------------------------------
// State Variables
// -------------------------------------------------------------
static DashTelemetry g_telemetry;
static bool g_initialized = false;
static float g_manual_throttle = 0.0f;
static float g_manual_brake = 0.0f;
static uint32_t g_sim_uptime_ms = 0;
static auto g_last_frame_clock = std::chrono::steady_clock::now();

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
};
#pragma pack(pop)

// -------------------------------------------------------------
// Simulation Logic
// -------------------------------------------------------------
static void runSimulationStep(float dt) {
    if (dt <= 0.0002f) return;

    float sim_t = g_manual_throttle;
    float sim_b = g_manual_brake;

    // Boost multiplier
    if (g_telemetry.boost_active) {
        sim_t *= 1.25f;
        if (sim_t > 100.0f) sim_t = 100.0f;
        if (g_sim_uptime_ms > g_telemetry.boost_end_ms) {
            g_telemetry.boost_active = false;
            Renderer.markScreenDirty();
        }
    }

    if (g_telemetry.is_locked) {
        sim_t = 0.0f;
        g_telemetry.throttle_pct = 0.0f;
        g_telemetry.speed_kmh = 0.0f;
        g_telemetry.current_amps = 0.0f;
        g_telemetry.phase_amps = 0.0f;
        g_telemetry.power_watts = 0.0f;
        g_telemetry.duty_cycle_pct = 0.0f;
        g_telemetry.rpm = 0.0f;
        return;
    }

    g_telemetry.throttle_pct = sim_t;

    // 1. Target Speed and Power by Ride Mode & Tuning State
    ModeLimits lim = ModeManager::instance().getActiveLimits();
    float max_possible_speed = lim.max_speed_kmh;
    float max_bat_a = lim.max_battery_amps;
    float max_pha_a = lim.max_phase_amps;
    float accel_rate = 22.0f;

    if (!g_telemetry.tuning_mode) {
        // Legal Modes: ECO (15 km/h), SPORT (20 km/h), RACE (22 km/h)
        // 1000W pull acceleration, 500W continuous at cruising speed
        switch (g_telemetry.ride_mode) {
            case MODE_ECO:   accel_rate = 18.0f; break;
            case MODE_SPORT: accel_rate = 24.0f; break;
            case MODE_RACE:  accel_rate = 28.0f; break;
            default: break;
        }
    } else {
        switch (g_telemetry.ride_mode) {
            case MODE_ECO:
                accel_rate = 32.0f;
                break;
            case MODE_SPORT:
                accel_rate = 46.0f;
                break;
            case MODE_RACE:
                accel_rate = 68.0f;
                break;
            default: break;
        }
        if (g_telemetry.boost_active) {
            max_possible_speed += 15.0f;
            max_bat_a += 25.0f;
            max_pha_a += 55.0f;
            accel_rate += 18.0f;
        }
    }

    // 2. Realistic Speed & Dynamic Powertrain Physics
    if (sim_b > 1.0f) {
        // Active Braking: strong regenerative decelerating force
        float brake_norm = sim_b / 100.0f;
        float decel = (6.0f + 28.0f * brake_norm) * dt;
        g_telemetry.speed_kmh -= decel;
        if (g_telemetry.speed_kmh < 0.0f) g_telemetry.speed_kmh = 0.0f;

        // Regen current back to battery (shown in Emerald Green)
        float target_regen_amps = (g_telemetry.speed_kmh > 2.0f) ? (-38.0f * brake_norm) : 0.0f;
        g_telemetry.current_amps += (target_regen_amps - g_telemetry.current_amps) * (dt * 6.5f);
        if (g_telemetry.speed_kmh < 0.2f) g_telemetry.current_amps = 0.0f;

    } else if (sim_t > 0.0f) {
        // Active Throttle: snappy, punchy electric vehicle acceleration
        float target_speed = (sim_t >= 90.0f) ? max_possible_speed : ((sim_t / 100.0f) * max_possible_speed);
        if (g_telemetry.speed_kmh < target_speed) {
            float speed_diff = target_speed - g_telemetry.speed_kmh;
            float step = (sim_t / 100.0f) * (accel_rate * dt + speed_diff * (dt * 3.5f));
            g_telemetry.speed_kmh += step;
            if (g_telemetry.speed_kmh > target_speed) g_telemetry.speed_kmh = target_speed;
        } else {
            // Smooth settling onto target speed
            g_telemetry.speed_kmh += (target_speed - g_telemetry.speed_kmh) * (dt * 3.5f);
        }

        // Positive battery current ramps smoothly with throttle
        float target_bat_amps = (sim_t / 100.0f) * max_bat_a;
        g_telemetry.current_amps += (target_bat_amps - g_telemetry.current_amps) * (dt * 6.5f);
        if (g_telemetry.current_amps < 0.0f) g_telemetry.current_amps = 0.0f;

    } else {
        // Coasting / Ausrollen: Throttle released, vehicle freewheels
        // Aerodynamic drag (v^2) + rolling resistance (tires/bearings)
        float v = g_telemetry.speed_kmh;
        if (v > 0.0f) {
            float drag_decel = (0.75f + (v / 100.0f) * (v / 100.0f) * 4.5f + v * 0.028f) * dt;
            g_telemetry.speed_kmh -= drag_decel;
            if (g_telemetry.speed_kmh < 0.2f) g_telemetry.speed_kmh = 0.0f;

            // Passive drag regen (mild back-EMF recuperation: -150W to -320W in Emerald Green)
            float coast_target_a = - (1.2f + (v / 100.0f) * 3.2f);
            g_telemetry.current_amps += (coast_target_a - g_telemetry.current_amps) * (dt * 3.8f);
        } else {
            g_telemetry.speed_kmh = 0.0f;
            g_telemetry.current_amps += (0.0f - g_telemetry.current_amps) * (dt * 8.0f);
            if (fabsf(g_telemetry.current_amps) < 0.1f) g_telemetry.current_amps = 0.0f;
        }
    }

    // 3. Realistic Duty Cycle
    float base_duty = (g_telemetry.speed_kmh / 48.0f) * 92.0f;
    if (sim_t > 85.0f && g_telemetry.speed_kmh > 46.0f) {
        float fw_extra = ((sim_t - 85.0f) / 15.0f) * 18.0f;
        g_telemetry.duty_cycle_pct = 96.0f + fw_extra;
    } else {
        g_telemetry.duty_cycle_pct = base_duty;
    }
    if (sim_t == 0.0f && sim_b <= 1.0f) {
        g_telemetry.duty_cycle_pct = (g_telemetry.speed_kmh / 52.0f) * 85.0f;
    }
    if (g_telemetry.duty_cycle_pct > 118.0f) g_telemetry.duty_cycle_pct = 118.0f;
    if (g_telemetry.duty_cycle_pct < 0.0f) g_telemetry.duty_cycle_pct = 0.0f;

    g_telemetry.rpm = (g_telemetry.speed_kmh / 45.0f) * 130.0f * 35.0f;

    // 4. Phase Amps
    float duty_fraction = g_telemetry.duty_cycle_pct / 100.0f;
    if (duty_fraction < 0.35f) duty_fraction = 0.35f;
    float calculated_phase = fabsf(g_telemetry.current_amps) / duty_fraction;
    if (calculated_phase > max_pha_a) calculated_phase = max_pha_a;
    g_telemetry.phase_amps = calculated_phase;

    // 5. Battery Voltage & Power (Nominal 72V pack: sag on load, rise on regen)
    g_telemetry.voltage = 76.7f - (g_telemetry.current_amps * 0.038f);
    g_telemetry.power_watts = g_telemetry.voltage * g_telemetry.current_amps;

    // Dynamic Power Limiting: Peak acceleration pull power up to max_pull_watts (e.g. 1000W),
    // and settling smoothly to max_power_watts (500W continuous at cruising speed).
    if (g_telemetry.current_amps > 0.0f) {
        float effective_power_cap = lim.max_pull_watts;
        if (max_possible_speed > 0.0f && g_telemetry.speed_kmh >= max_possible_speed * 0.88f) {
            float blend = (g_telemetry.speed_kmh - max_possible_speed * 0.88f) / (max_possible_speed * 0.12f);
            if (blend > 1.0f) blend = 1.0f;
            effective_power_cap = lim.max_pull_watts - blend * (lim.max_pull_watts - lim.max_power_watts);
        }
        if (g_telemetry.power_watts > effective_power_cap) {
            g_telemetry.power_watts = effective_power_cap;
            if (g_telemetry.voltage > 10.0f) {
                g_telemetry.current_amps = g_telemetry.power_watts / g_telemetry.voltage;
                calculated_phase = fabsf(g_telemetry.current_amps) / duty_fraction;
                if (calculated_phase > max_pha_a) calculated_phase = max_pha_a;
                g_telemetry.phase_amps = calculated_phase;
            }
        }
    }

    // 5. Battery Manager
    Battery.update(g_telemetry, dt);

    // 6. Dual Thermals (Rear & Front)
    if (g_telemetry.phase_amps > 30.0f) {
        g_telemetry.temp_fet_rear  += 0.35f * dt;
        g_telemetry.temp_mot_rear  += 0.45f * dt;
        g_telemetry.temp_fet_front += 0.28f * dt;
        g_telemetry.temp_mot_front += 0.36f * dt;
    } else {
        g_telemetry.temp_fet_rear  -= 0.08f * dt;
        g_telemetry.temp_mot_rear  -= 0.08f * dt;
        g_telemetry.temp_fet_front -= 0.08f * dt;
        g_telemetry.temp_mot_front -= 0.08f * dt;
    }
    if (g_telemetry.temp_fet_rear < 45.0f)  g_telemetry.temp_fet_rear = 45.0f;
    if (g_telemetry.temp_mot_rear < 58.0f)  g_telemetry.temp_mot_rear = 58.0f;
    if (g_telemetry.temp_fet_front < 39.0f) g_telemetry.temp_fet_front = 39.0f;
    if (g_telemetry.temp_mot_front < 54.0f) g_telemetry.temp_mot_front = 54.0f;
    g_telemetry.temp_esc = g_telemetry.temp_fet_rear;
    g_telemetry.temp_motor = g_telemetry.temp_mot_rear;

    // 7. Odometry & Trip
    float distance_delta = (g_telemetry.speed_kmh / 3600.0f) * dt;
    g_telemetry.trip_km += distance_delta;
    g_telemetry.odo_km  += distance_delta;

    // 8. Statistics
    if (g_telemetry.current_amps > g_telemetry.stats.peak_current_amps) g_telemetry.stats.peak_current_amps = g_telemetry.current_amps;
    if (g_telemetry.phase_amps > g_telemetry.stats.peak_phase_amps) g_telemetry.stats.peak_phase_amps = g_telemetry.phase_amps;
    if (g_telemetry.power_watts > g_telemetry.stats.peak_power_watts) g_telemetry.stats.peak_power_watts = g_telemetry.power_watts;
    if (g_telemetry.speed_kmh > g_telemetry.stats.max_speed_kmh) g_telemetry.stats.max_speed_kmh = g_telemetry.speed_kmh;
    if (g_telemetry.temp_motor > g_telemetry.stats.max_temp_motor) g_telemetry.stats.max_temp_motor = g_telemetry.temp_motor;
    if (g_telemetry.temp_esc > g_telemetry.stats.max_temp_esc) g_telemetry.stats.max_temp_esc = g_telemetry.temp_esc;
    
    static float s_sim_ride_time_accum = 0.0f;
    if (g_telemetry.stats.ride_time_sec == 0 && g_telemetry.trip_km == 0.0f) {
        s_sim_ride_time_accum = 0.0f;
    }
    if (g_telemetry.speed_kmh > 1.2f || g_telemetry.screen == SCREEN_RT_GRAPHS) {
        s_sim_ride_time_accum += dt;
        while (s_sim_ride_time_accum >= 1.0f) {
            g_telemetry.stats.ride_time_sec += 1;
            s_sim_ride_time_accum -= 1.0f;
        }
    }
    if (g_telemetry.stats.ride_time_sec > 2 && g_telemetry.trip_km > 0.01f) {
        g_telemetry.stats.avg_speed_kmh = g_telemetry.trip_km / (g_telemetry.stats.ride_time_sec / 3600.0f);
    }
    g_telemetry.vesc_connected = true;

    // 9. Update Waveform History every 1000ms
    if (g_sim_uptime_ms - g_telemetry.last_history_sample_ms >= 1000) {
        g_telemetry.last_history_sample_ms = g_sim_uptime_ms;
        for (int i = 0; i < WAVE_HISTORY_LEN - 1; i++) {
            g_telemetry.speed_history[i] = g_telemetry.speed_history[i + 1];
            g_telemetry.power_history[i] = g_telemetry.power_history[i + 1];
        }
        g_telemetry.speed_history[WAVE_HISTORY_LEN - 1] = g_telemetry.speed_kmh;
        g_telemetry.power_history[WAVE_HISTORY_LEN - 1] = g_telemetry.power_watts;
    }
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
    ModeManager::instance().begin();
    Display.init();

    Settings.get().dash_style = STYLE_HYPER_35KW;
    Renderer.begin(&Display);

    memset(&g_telemetry, 0, sizeof(g_telemetry));
    g_telemetry.vesc_connected = true;
    g_telemetry.screen = SCREEN_RIDE_DASH;
    g_telemetry.voltage = 76.7f;
    g_telemetry.battery_pct = 68.0f;
    g_telemetry.temp_fet_rear  = 60.0f;
    g_telemetry.temp_mot_rear  = 75.0f;
    g_telemetry.temp_fet_front = 53.0f;
    g_telemetry.temp_mot_front = 54.0f;
    g_telemetry.temp_esc   = 60.0f;
    g_telemetry.temp_motor = 75.0f;
    g_telemetry.trip_km = 14.2f;
    g_telemetry.odo_km = 206.6f;
    g_telemetry.battery_profile_id = 0;
    g_telemetry.battery_health_soh = 98.4f;
    g_telemetry.est_range_km = 68.0f;
    g_telemetry.ride_mode = MODE_SPORT;
    g_telemetry.boost_active = false;
    g_telemetry.light_mode = 2; // RAINBOW
    g_telemetry.light_hue = 180;
    g_telemetry.light_brightness = 200;
    g_telemetry.lights_enabled = true;
    g_telemetry.hazard_active = false;
    g_telemetry.rider_tag[0] = '\0';

    // Seed realistic sine waveform curve history (from video frame 00:11)
    for (int i = 0; i < WAVE_HISTORY_LEN; i++) {
        float phase = (float)i / (float)WAVE_HISTORY_LEN * 4.0f * 3.14159265f;
        g_telemetry.speed_history[i] = 45.0f + 35.0f * sinf(phase);
        g_telemetry.power_history[i] = 5500.0f + 4200.0f * sinf(phase);
    }
    g_telemetry.history_count = WAVE_HISTORY_LEN;
    g_telemetry.last_history_sample_ms = 1000;

    g_telemetry.stats.reset();
    g_telemetry.stats.trip_wh = 275.4f;
    g_telemetry.stats.trip_wh_km = 19.4f;
    g_telemetry.stats.peak_power_watts = 10380.0f;
    g_telemetry.stats.peak_current_amps = 85.0f;
    g_telemetry.stats.peak_phase_amps = 230.0f;
    g_telemetry.stats.max_speed_kmh = 0.0f;
    g_telemetry.stats.avg_speed_kmh = 28.4f;
    g_telemetry.stats.ride_time_sec = 2538;

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
    Buttons.update();

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
        }
    }

    NavAction nav = g_telemetry.is_locked ? NAV_NONE : Buttons.getNavAction();

    if (nav != NAV_NONE) {
        if (g_telemetry.screen == SCREEN_SETTINGS_MENU) {
            Renderer.handleMenuNav(nav, g_telemetry);
        } else {
            switch (nav) {
                case NAV_SCREEN_DASH:
                    g_telemetry.screen = SCREEN_RIDE_DASH;
                    Renderer.markScreenDirty();
                    Renderer.invalidateCache();
                    break;
                case NAV_BTN1_SHORT: // Prev Screen
                    {
                        if (g_telemetry.screen == SCREEN_RT_GRAPHS) {
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
                    }
                    break;
                case NAV_BTN2_SHORT: // Next Screen
                    {
                        if (g_telemetry.screen == SCREEN_RT_GRAPHS) {
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
                    g_telemetry.screen = SCREEN_RT_GRAPHS;
                    Renderer.markScreenDirty();
                    break;
                case NAV_BTN1_LONG: // Settings Menu
                    g_telemetry.screen = SCREEN_SETTINGS_MENU;
                    Renderer.markScreenDirty();
                    break;
                case NAV_BTN2_LONG: // Trip Reset
                    g_telemetry.trip_km = 0.0f;
                    g_telemetry.stats.reset();
                    Renderer.markScreenDirty();
                    break;
                case NAV_BOOST_TOGGLE:
                    g_telemetry.boost_active = !g_telemetry.boost_active;
                    if (g_telemetry.boost_active) {
                        g_telemetry.boost_end_ms = g_sim_uptime_ms + 10000;
                    }
                    Renderer.markScreenDirty();
                    break;
                case NAV_TUNING_ENABLE:
                    g_telemetry.tuning_mode = true;
                    Buttons.setTuningMode(true);
                    ModeManager::instance().setTuningMode(true);
                    g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
                    Renderer.markScreenDirty();
                    break;
                case NAV_TUNING_DISABLE:
                    g_telemetry.tuning_mode = false;
                    Buttons.setTuningMode(false);
                    ModeManager::instance().setTuningMode(false);
                    g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
                    Renderer.markScreenDirty();
                    break;
                case NAV_MODE_CYCLE:
                    g_telemetry.ride_mode = (RideMode)(((int)g_telemetry.ride_mode + 1) % MODE_COUNT);
                    ModeManager::instance().setRideMode(g_telemetry.ride_mode);
                    g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
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

    // 2. Physics & Motor Telemetry update
    float dt = step_ms / 1000.0f;
    runSimulationStep(dt);

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

    // Interactive Touch processing on special screens
    if (is_down && g_telemetry.screen == SCREEN_LIGHTS) {
        // Color wheel check (cx: 220, cy: 230, r_in: 88, r_out: 125)
        int cx = 220, cy = 230;
        float dist = sqrtf((float)((x - cx)*(x - cx) + (y - cy)*(y - cy)));
        if (dist >= 75.0f && dist <= 138.0f) {
            float a = atan2f((float)(y - cy), (float)(x - cx)) * 180.0f / 3.14159265f;
            if (a < 0) a += 360.0f;
            g_telemetry.light_hue = (uint8_t)(a * 255.0f / 360.0f);
            Renderer.markScreenDirty();
        } else if (dist < 72.0f && is_new_down) {
            g_telemetry.lights_enabled = !g_telemetry.lights_enabled;
            Renderer.markScreenDirty();
        } else if (x >= 675 && x <= 765 && y >= 12 && y <= 55 && is_new_down) {
            // Master Toggle Switch
            g_telemetry.lights_enabled = !g_telemetry.lights_enabled;
            Renderer.markScreenDirty();
        } else if (x >= 405 && x <= 775 && y >= 60 && y <= 245 && is_new_down) {
            // 9 Preset buttons (3x3)
            int col = (x - 415) / 120;
            int row = (y - 65) / 56;
            if (col >= 0 && col < 3 && row >= 0 && row < 3) {
                g_telemetry.light_mode = row * 3 + col;
                Renderer.markScreenDirty();
            }
        } else if (x >= 405 && x <= 775 && y >= 265 && y <= 315) {
            // Brightness Slider
            float bPct = (float)(x - 415) / 345.0f;
            if (bPct < 0.0f) bPct = 0.0f;
            if (bPct > 1.0f) bPct = 1.0f;
            g_telemetry.light_brightness = (uint8_t)(bPct * 255.0f);
            Renderer.markScreenDirty();
        } else if (x >= 400 && x <= 775 && y >= 320 && y <= 435 && is_new_down) {
            // Hazard button
            g_telemetry.hazard_active = !g_telemetry.hazard_active;
            Renderer.markScreenDirty();
        } else if (x >= 80 && x <= 260 && y >= 390 && y <= 450 && is_new_down) {
            // Bottom-left Hazard Toggle Switch
            g_telemetry.hazard_active = !g_telemetry.hazard_active;
            Renderer.markScreenDirty();
        }
    } else if (g_telemetry.screen == SCREEN_RT_GRAPHS) {
        if (is_new_down) {
            if (x <= 150 && y <= 65) {
                // Top Left [< DASH] button returns to Ride Dash
                g_telemetry.screen = SCREEN_RIDE_DASH;
                Renderer.markScreenDirty();
                Display.setSimTouch(false, -1, -1);
                Buttons.setActiveScreen(SCREEN_RIDE_DASH);
                return;
            } else if (x >= 600 && y <= 65) {
                // Top Right [RESET TRIP] button
                g_telemetry.trip_km = 0.0f;
                g_telemetry.stats.reset();
                Renderer.markScreenDirty();
                Display.setSimTouch(false, -1, -1);
                return;
            }
        }
        // Brightness Controls on SCREEN_RT_GRAPHS
        if (is_down && y >= 360 && y <= 465 && x >= 270 && x <= 560) {
            float pct = (float)(x - 280) / 270.0f;
            int val = (int)(20.0f + pct * 80.0f + 0.5f);
            if (val < 20) val = 20;
            if (val > 100) val = 100;
            DashSettings &s = Settings.get();
            if (s.brightness_pct != val) {
                s.brightness_pct = val;
                Display.setBrightness((uint8_t)((val * 255) / 100));
                Renderer.markScreenDirty();
            }
        } else if (is_new_down && y >= 360 && y <= 465) {
            DashSettings &s = Settings.get();
            bool changed = false;
            if (x >= 195 && x <= 275) {
                if (s.brightness_pct > 20) {
                    s.brightness_pct = (s.brightness_pct <= 30) ? 20 : (s.brightness_pct - 10);
                    changed = true;
                }
            } else if (x >= 556 && x <= 628) {
                if (s.brightness_pct < 100) {
                    s.brightness_pct = (s.brightness_pct >= 90) ? 100 : (s.brightness_pct + 10);
                    changed = true;
                }
            } else if (x >= 630 && x <= 702) {
                if (s.brightness_pct != 25) {
                    s.brightness_pct = 25;
                    changed = true;
                }
            } else if (x >= 705 && x <= 778) {
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
    } else if (is_new_down && g_telemetry.screen == SCREEN_RIDE_DASH) {
        if (x >= 18 && x <= 125 && y >= 12 && y <= 60) {
            // Tapping [ STATS ] button takes the rider directly to SCREEN_RT_GRAPHS
            g_telemetry.screen = SCREEN_RT_GRAPHS;
            Renderer.markScreenDirty();
            Display.setSimTouch(false, -1, -1);
            Buttons.setActiveScreen(SCREEN_RT_GRAPHS);
            return;
        }
    }

    Buttons.setActiveScreen(g_telemetry.screen);
    Display.setSimTouch(is_down != 0, (int16_t)x, (int16_t)y);
    Buttons.update();

    NavAction act = g_telemetry.is_locked ? NAV_NONE : Buttons.getNavAction();
    if (act != NAV_NONE) {
        sim_trigger_nav((int)act);
    }
}

void sim_trigger_nav(int nav_action) {
    if (g_telemetry.screen == SCREEN_SETTINGS_MENU) {
        Renderer.handleMenuNav((NavAction)nav_action, g_telemetry);
    } else {
        switch ((NavAction)nav_action) {
            case NAV_BTN1_SHORT: {
                if (g_telemetry.screen == SCREEN_RT_GRAPHS) {
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
                if (g_telemetry.screen == SCREEN_RT_GRAPHS) {
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
                g_telemetry.screen = SCREEN_RT_GRAPHS;
                break;
            case NAV_SCREEN_DASH:
                g_telemetry.screen = SCREEN_RIDE_DASH;
                Renderer.markScreenDirty();
                Renderer.invalidateCache();
                break;
            case NAV_BTN1_LONG:
                g_telemetry.screen = SCREEN_SETTINGS_MENU;
                break;
            case NAV_BTN2_LONG:
                g_telemetry.trip_km = 0.0f;
                g_telemetry.stats.reset();
                break;
            case NAV_BOOST_TOGGLE:
                g_telemetry.boost_active = !g_telemetry.boost_active;
                if (g_telemetry.boost_active) {
                    g_telemetry.boost_end_ms = g_sim_uptime_ms + 10000;
                }
                break;
            case NAV_TUNING_ENABLE:
                g_telemetry.tuning_mode = true;
                Buttons.setTuningMode(true);
                ModeManager::instance().setTuningMode(true);
                g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
                break;
            case NAV_TUNING_DISABLE:
                g_telemetry.tuning_mode = false;
                Buttons.setTuningMode(false);
                ModeManager::instance().setTuningMode(false);
                g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
                break;
            case NAV_MODE_CYCLE:
                g_telemetry.ride_mode = (RideMode)(((int)g_telemetry.ride_mode + 1) % MODE_COUNT);
                ModeManager::instance().setRideMode(g_telemetry.ride_mode);
                g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
                break;
            case NAV_HAZARD_TOGGLE:
                g_telemetry.hazard_active = !g_telemetry.hazard_active;
                break;
            default:
                break;
        }
    }
    Renderer.markScreenDirty();
}

void sim_toggle_boost() {
    g_telemetry.boost_active = !g_telemetry.boost_active;
    if (g_telemetry.boost_active) {
        g_telemetry.boost_end_ms = g_sim_uptime_ms + 10000;
    }
    Renderer.markScreenDirty();
}

void sim_cycle_mode() {
    g_telemetry.ride_mode = (RideMode)(((int)g_telemetry.ride_mode + 1) % MODE_COUNT);
    ModeManager::instance().setRideMode(g_telemetry.ride_mode);
    g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
    Renderer.markScreenDirty();
}

void sim_set_ride_mode(int mode) {
    if (mode >= 0 && mode < MODE_COUNT) {
        g_telemetry.ride_mode = (RideMode)mode;
        ModeManager::instance().setRideMode(g_telemetry.ride_mode);
        g_telemetry.mode_badge_until = g_sim_uptime_ms + 2500;
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
    Renderer.markScreenDirty();
}

void sim_toggle_tuning_mode() {
    g_telemetry.tuning_mode = !g_telemetry.tuning_mode;
    Buttons.setTuningMode(g_telemetry.tuning_mode);
    ModeManager::instance().setTuningMode(g_telemetry.tuning_mode);
    Renderer.markScreenDirty();
}

void sim_set_auto_mode(int enable) {
    (void)enable;
}

void sim_toggle_auto_mode() {
}

void sim_set_manual_throttle(float throttle_pct) {
    if (throttle_pct < 0.0f) throttle_pct = 0.0f;
    if (throttle_pct > 100.0f) throttle_pct = 100.0f;
    g_manual_throttle = throttle_pct;
}

void sim_set_manual_brake(float brake_pct) {
    if (brake_pct < 0.0f) brake_pct = 0.0f;
    if (brake_pct > 100.0f) brake_pct = 100.0f;
    g_manual_brake = brake_pct;
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
