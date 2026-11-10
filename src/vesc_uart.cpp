#include "vesc_uart.h"
#include "settings_manager.h"
#include "battery_manager.h"
#include "fault_manager.h"
#include "mode_manager.h"
#include <math.h>

#if !defined(NATIVE_SIMULATOR)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#if VESC_USE_CAN_BUS
#include "driver/twai.h"
#endif
static SemaphoreHandle_t s_uartMutex = NULL;
struct UartLock {
    UartLock() { if (s_uartMutex) xSemaphoreTake(s_uartMutex, portMAX_DELAY); }
    ~UartLock() { if (s_uartMutex) xSemaphoreGive(s_uartMutex); }
};
#else
struct UartLock { UartLock() {} ~UartLock() {} };
#endif

VescHandler Vesc;

// Standard CRC-CCITT for VESC packets
static uint16_t crc16(const uint8_t *buf, uint32_t len) {
    uint16_t crc = 0;
    for (uint32_t i = 0; i < len; i++) {
        crc = (uint8_t)(crc >> 8) | (crc << 8);
        crc ^= buf[i];
        crc ^= (uint8_t)(crc & 0xff) >> 4;
        crc ^= (crc << 8) << 4;
        crc ^= ((crc & 0xff) << 4) << 1;
    }
    return crc;
}

// Deserializes VESC IEEE-754 auto-float
static float buffer_get_float32_auto(const uint8_t *buffer, int32_t *index) {
    uint32_t res = ((uint32_t)buffer[*index] << 24) |
                   ((uint32_t)buffer[*index + 1] << 16) |
                   ((uint32_t)buffer[*index + 2] << 8) |
                   ((uint32_t)buffer[*index + 3]);
    *index += 4;

    int e = (res >> 23) & 0xFF;
    uint32_t sig_i = res & 0x7FFFFF;
    bool neg = (res & (1U << 31)) != 0;

    float sig = 0.0f;
    if (e != 0 || sig_i != 0) {
        sig = (float)sig_i / (8388608.0f * 2.0f) + 0.5f;
        e -= 126;
    }

    if (neg) {
        sig = -sig;
    }

    return ldexpf(sig, e);
}

static float _last_adc1_v = 0.0f;
static float _last_adc1_pct = 0.0f;
static float _last_adc2_v = 0.0f;
static float _last_adc2_pct = 0.0f;

float VescHandler::getLastAdc1Voltage() const { return _last_adc1_v; }
float VescHandler::getLastAdc1Pct() const { return _last_adc1_pct; }
float VescHandler::getLastAdc2Voltage() const { return _last_adc2_v; }
float VescHandler::getLastAdc2Pct() const { return _last_adc2_pct; }

void VescHandler::resetTripOdometer(int32_t current_tacho) {
    if (current_tacho != 0) {
        _tacho_trip_offset = current_tacho;
        _dist_trip_offset = current_tacho;
    } else {
        _tacho_trip_offset = _last_tacho;
        _dist_trip_offset = _last_dist;
    }
}

VescHandler::VescHandler()
    : _transport(nullptr),
      _pole_pairs(15),
      _wheel_diameter_mm(660.0f),
      _gear_ratio(1.0f),
      _tacho_trip_offset(0),
      _last_tacho(0),
      _dist_trip_offset(0),
      _last_dist(0),
      _sim_throttle(0.0f),
      _target_speed(0.0f),
      _target_rpm(0.0f),
      _sim_fault_code(0),
      _last_poll_ms(0),
      _sim_last_update_ms(0),
      _bridge_active(false),
      _bridge_pc_to_vesc(0),
      _bridge_vesc_to_pc(0),
      _wifi_bridge_active(false),
      _wifi_client_connected(false),
      _bridge_wifi_to_vesc(0),
      _bridge_vesc_to_wifi(0),
      _sync_status(SYNC_IDLE),
      _mcconf_req_ms(0),
      _sync_ms(0) {
    _wifi_client_ip[0] = '\0';
}

void VescHandler::begin() {
#if !defined(NATIVE_SIMULATOR)
    if (s_uartMutex == NULL) {
        s_uartMutex = xSemaphoreCreateMutex();
    }
#if !SIMULATION_MODE
#if VESC_USE_CAN_BUS
    initCanBus();
#else
    VESC_UART_PORT.setRxBufferSize(4096);
    VESC_UART_PORT.setTxBufferSize(4096);
    VESC_UART_PORT.begin(VESC_UART_BAUDRATE, SERIAL_8N1, VESC_UART_RX_PIN, VESC_UART_TX_PIN);
    static HardwareSerialTransport s_hwTransport(VESC_UART_PORT);
    if (!_transport) {
        _transport = &s_hwTransport;
    }
#endif
#endif
#else
    static VescLoopbackTransport s_defaultSimTransport;
    if (!_transport) {
        _transport = &s_defaultSimTransport.getSideA();
    }
#endif
    _sim_last_update_ms = millis();
    _last_poll_ms = millis();
}

void VescHandler::setSimThrottle(float pct) {
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    _sim_throttle = pct;
}

void VescHandler::update(DashTelemetry &telemetry) {
#if SIMULATION_MODE
    runSimulation(telemetry);
#elif defined(NATIVE_SIMULATOR)
    pollRealVesc(telemetry);
#elif VESC_USE_CAN_BUS
    pollCanVesc(telemetry);
#else
    pollRealVesc(telemetry);
#endif
}

// ==============================================================================
// Realistic eBike Telemetry Simulation Engine (Bafang Mid-Drive Profile)
// ==============================================================================
void VescHandler::runSimulation(DashTelemetry &telemetry) {
    uint32_t now = millis();
    float dt = (now - _sim_last_update_ms) / 1000.0f;
    if (dt <= 0.001f) return;
    _sim_last_update_ms = now;

    // Automated 45-second dynamic riding cycle
    uint32_t cycle_ms = now % 45000;
    float sim_t = 0.0f;

    if (cycle_ms < 4000) {
        // Stopped at intersection
        sim_t = 0.0f;
    } else if (cycle_ms < 14000) {
        // Acceleration phase (heavy torque demand)
        float p = (cycle_ms - 4000) / 10000.0f;
        sim_t = p * 75.0f;
    } else if (cycle_ms < 22000) {
        // Steady cruise at ~35 km/h
        sim_t = 40.0f;
    } else if (cycle_ms < 34000) {
        // Hard sprint into Field Weakening territory (>100% duty)
        float p = (cycle_ms - 22000) / 12000.0f;
        sim_t = 40.0f + p * 58.0f; // up to 98% throttle
    } else {
        // Coasting down to stop (BBS mid-drive freewheeling, no regen)
        sim_t = 0.0f;
    }

    // Check if vehicle is locked by boot PIN
    if (ModeManager::instance().isLocked() || telemetry.is_locked) {
        sim_t = 0.0f;
    }

    _sim_throttle = sim_t;
    telemetry.throttle_pct = _sim_throttle;

    // Fault injection/simulation
    if (_sim_fault_code != 0) {
        telemetry.fault_code = _sim_fault_code;
        FaultManager::instance().recordFault(_sim_fault_code, telemetry.voltage, telemetry.current_amps, telemetry.phase_amps, telemetry.temp_esc, telemetry.temp_motor, telemetry.uptime_sec);
    } else {
        telemetry.fault_code = 0;
        FaultManager::instance().setActiveFault(0);
    }

    // 1. Target Speed and Duty Cycle calculation
    uint8_t speed_limit = Settings.get().max_speed_kmh;
    float max_possible_speed = (speed_limit > 0) ? (float)speed_limit : 95.0f;

    _target_speed = (_sim_throttle / 100.0f) * max_possible_speed;
    float prev_sim_spd = telemetry.speed_kmh;
    telemetry.speed_kmh += (_target_speed - telemetry.speed_kmh) * (dt * 1.6f);
    if (telemetry.speed_kmh < 0.2f) telemetry.speed_kmh = 0.0f;
    if (dt > 0.001f) {
        float dv_ms = (telemetry.speed_kmh - prev_sim_spd) / 3.6f;
        float g_inst = (dv_ms / dt) / 9.80665f;
        if (g_inst > 3.0f) g_inst = 3.0f;
        if (g_inst < -3.0f) g_inst = -3.0f;
        telemetry.g_force += (g_inst - telemetry.g_force) * (dt * 5.0f);
        if (fabsf(telemetry.g_force) < 0.02f) telemetry.g_force = 0.0f;
        if (fabsf(telemetry.g_force) > telemetry.stats.peak_g_force) {
            telemetry.stats.peak_g_force = fabsf(telemetry.g_force);
        }
    }

    // 2. Realistic Duty Cycle calculation (0.0% to 120.0%)
    // Base duty cycle tracks vehicle speed / back-EMF headroom
    float base_duty = (telemetry.speed_kmh / 48.0f) * 92.0f;
    if (_sim_throttle > 85.0f && telemetry.speed_kmh > 46.0f) {
        // Field Weakening engaged: duty cycle pushes beyond 100% (up to 114%)
        float fw_extra = ((_sim_throttle - 85.0f) / 15.0f) * 18.0f;
        telemetry.duty_cycle_pct = 96.0f + fw_extra;
    } else {
        telemetry.duty_cycle_pct = base_duty;
    }
    if (telemetry.duty_cycle_pct > 118.0f) telemetry.duty_cycle_pct = 118.0f;
    if (telemetry.duty_cycle_pct < 0.0f) telemetry.duty_cycle_pct = 0.0f;

    // RPM equivalent for BBS mid-drive (crank/motor cadence)
    telemetry.rpm = (telemetry.speed_kmh / 45.0f) * 130.0f * 35.0f; // ~4500 motor RPM

    // 3. Current & Phase Amps (Bafang Mid-Drive tuning)
    uint16_t max_bat_a = Settings.get().max_battery_amps; // e.g. 28A
    uint16_t max_pha_a = Settings.get().max_phase_amps;   // e.g. 160A / up to 300A

    float target_bat_amps = (_sim_throttle / 100.0f) * (float)max_bat_a;
    telemetry.current_amps += (target_bat_amps - telemetry.current_amps) * (dt * 3.5f);
    if (telemetry.current_amps < 0.0f) telemetry.current_amps = 0.0f;

    // Phase current is higher during low-speed high-torque launches
    float duty_fraction = telemetry.duty_cycle_pct / 100.0f;
    if (duty_fraction > 0.95f) duty_fraction = 0.95f;
    if (duty_fraction < 0.35f) duty_fraction = 0.35f;
    float calculated_phase = telemetry.current_amps / duty_fraction;
    if (calculated_phase < telemetry.current_amps) calculated_phase = telemetry.current_amps;
    if (calculated_phase > (float)max_pha_a) calculated_phase = (float)max_pha_a;
    telemetry.phase_amps = calculated_phase;

    // 4. Battery Voltage & Power
    // Nominal 53.8V for 14S pack, minor IR sag under load (~0.05 ohm internal resistance)
    telemetry.voltage = 53.8f - (telemetry.current_amps * 0.048f);
    telemetry.power_watts = telemetry.voltage * telemetry.current_amps;

    // 5. Update Smart Battery SoC, Health & Range Learning
    Battery.update(telemetry, dt);

    // 6. Thermals: gradual warming under high phase amps
    if (telemetry.phase_amps > 30.0f) {
        telemetry.temp_motor += 0.35f * dt;
        telemetry.temp_esc   += 0.18f * dt;
    } else {
        telemetry.temp_motor -= 0.08f * dt;
        telemetry.temp_esc   -= 0.08f * dt;
    }
    if (telemetry.temp_motor < 36.0f) telemetry.temp_motor = 36.0f;
    if (telemetry.temp_esc < 31.0f)   telemetry.temp_esc = 31.0f;

    // 7. Distance & Odometry
    float distance_delta = (telemetry.speed_kmh / 3600.0f) * dt;
    telemetry.trip_km += distance_delta;
    telemetry.odo_km  += distance_delta;

    // 8. Statistics Tracking
    if (telemetry.current_amps > telemetry.stats.peak_current_amps) {
        telemetry.stats.peak_current_amps = telemetry.current_amps;
    }
    if (telemetry.phase_amps > telemetry.stats.peak_phase_amps) {
        telemetry.stats.peak_phase_amps = telemetry.phase_amps;
    }
    if (telemetry.power_watts > telemetry.stats.peak_power_watts) {
        telemetry.stats.peak_power_watts = telemetry.power_watts;
    }
    if (telemetry.speed_kmh > telemetry.stats.max_speed_kmh) {
        telemetry.stats.max_speed_kmh = telemetry.speed_kmh;
    }
    if (telemetry.temp_motor > telemetry.stats.max_temp_motor) {
        telemetry.stats.max_temp_motor = telemetry.temp_motor;
    }
    if (telemetry.temp_esc > telemetry.stats.max_temp_esc) {
        telemetry.stats.max_temp_esc = telemetry.temp_esc;
    }
    static float s_vesc_ride_time_accum = 0.0f;
    if (telemetry.stats.ride_time_sec == 0 && telemetry.trip_km == 0.0f) {
        s_vesc_ride_time_accum = 0.0f;
    }
    if (telemetry.speed_kmh > 1.2f) {
        s_vesc_ride_time_accum += dt;
        while (s_vesc_ride_time_accum >= 1.0f) {
            telemetry.stats.ride_time_sec += 1;
            s_vesc_ride_time_accum -= 1.0f;
        }
    }
    if (telemetry.stats.ride_time_sec > 2 && telemetry.trip_km > 0.01f) {
        telemetry.stats.avg_speed_kmh = telemetry.trip_km / (telemetry.stats.ride_time_sec / 3600.0f);
    }

    telemetry.vesc_connected = true;
}

// ==============================================================================
// Live Spintend Ubox / VESC CAN Bus Communication (ESP32-S3 TWAI)
// ==============================================================================
#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
static bool s_can_initialized = false;

void VescHandler::initCanBus() {
    if (s_can_initialized) return;

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = 32;
    g_config.tx_queue_len = 16;
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err == ESP_OK) {
        err = twai_start();
        if (err == ESP_OK) {
            s_can_initialized = true;
            Serial.println("[VESC_CAN] ESP32-S3 TWAI CAN driver started at 500kbit/s (TX: GPIO 20, RX: GPIO 19).");
        } else {
            Serial.printf("[VESC_CAN ERROR] twai_start failed: 0x%x\n", err);
        }
    } else {
        Serial.printf("[VESC_CAN ERROR] twai_driver_install failed: 0x%x\n", err);
    }
}

void VescHandler::pollCanVesc(DashTelemetry &telemetry) {
#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    if (!s_can_initialized) return;

    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
        if (st.state == TWAI_STATE_BUS_OFF) {
            twai_initiate_recovery();
        } else if (st.state == TWAI_STATE_STOPPED) {
            twai_start();
        }
    }
#endif
    uint32_t now = millis();
    static uint32_t s_last_update_ms = millis();
    static uint32_t s_last_can_rx_ms = 0;
    static uint32_t s_last_can_diag_ms = 0;
    float dt = (now - s_last_update_ms) / 1000.0f;
    s_last_update_ms = now;

    twai_message_t msg;
    int frames_read = 0;
    while (twai_receive(&msg, pdMS_TO_TICKS(0)) == ESP_OK && frames_read < 32) {
        frames_read++;
        if (!msg.extd) continue;

        uint8_t controller_id = (uint8_t)(msg.identifier & 0xFF);
        uint8_t cmd_id = (uint8_t)((msg.identifier >> 8) & 0xFF);

        if (cmd_id == 9 && msg.data_length_code >= 8) { // CAN_PACKET_STATUS
            int32_t erpm = (int32_t)(((uint32_t)msg.data[0] << 24) |
                                     ((uint32_t)msg.data[1] << 16) |
                                     ((uint32_t)msg.data[2] << 8)  |
                                     (uint32_t)msg.data[3]);
            int16_t cur_raw = (int16_t)(((uint16_t)msg.data[4] << 8) | msg.data[5]);
            int16_t duty_raw = (int16_t)(((uint16_t)msg.data[6] << 8) | msg.data[7]);

            float mot_i = fabsf((float)cur_raw / 10.0f);
            float duty = fabsf((float)duty_raw / 10.0f);

            if (controller_id == 0) {
                int pole_pairs = Settings.get().motor_pole_pairs > 1 ? Settings.get().motor_pole_pairs : 15;
                float wheel_circ_m = (Settings.get().wheel_diameter_in * 0.0254f) * M_PI;
                float gear_ratio = Settings.get().gear_ratio > 0.1f ? Settings.get().gear_ratio : 1.0f;
                float mech_rpm = fabsf((float)erpm / (float)pole_pairs);
                telemetry.rpm = mech_rpm;
                float wheel_rpm = mech_rpm / gear_ratio;
                telemetry.speed_kmh = (wheel_rpm * wheel_circ_m * 60.0f) / 1000.0f;
                if (telemetry.speed_kmh < 0.8f) telemetry.speed_kmh = 0.0f;
                telemetry.phase_amps = mot_i;
                telemetry.duty_cycle_pct = duty;
                telemetry.vesc_connected = true;
                s_last_can_rx_ms = now;
            } else if (controller_id == 1) {
                telemetry.vesc2_max_phase = mot_i;
                telemetry.vesc_connected = true;
                s_last_can_rx_ms = now;
            }
        }
        else if (cmd_id == 16 && msg.data_length_code >= 8) { // CAN_PACKET_STATUS_4
            int16_t fet_raw = (int16_t)(((uint16_t)msg.data[0] << 8) | msg.data[1]);
            int16_t mot_raw = (int16_t)(((uint16_t)msg.data[2] << 8) | msg.data[3]);
            int16_t in_raw  = (int16_t)(((uint16_t)msg.data[4] << 8) | msg.data[5]);

            float t_fet = (float)fet_raw / 10.0f;
            float t_mot = (float)mot_raw / 10.0f;
            float bat_i = (float)in_raw / 10.0f;

            if (controller_id == 0) {
                telemetry.temp_esc = t_fet;
                telemetry.temp_motor = t_mot;
                telemetry.temp_fet_rear = t_fet;
                telemetry.temp_mot_rear = t_mot;
                telemetry.current_amps = bat_i;
                telemetry.power_watts = telemetry.voltage * telemetry.current_amps;
                telemetry.vesc_connected = true;
                s_last_can_rx_ms = now;
            } else if (controller_id == 1) {
                telemetry.temp_fet_front = t_fet;
                telemetry.temp_mot_front = t_mot;
                telemetry.vesc2_max_power = telemetry.voltage * bat_i;
            }
        }
        else if (cmd_id == 27 && msg.data_length_code >= 6) { // CAN_PACKET_STATUS_5
            int32_t tacho = (int32_t)(((uint32_t)msg.data[0] << 24) |
                                     ((uint32_t)msg.data[1] << 16) |
                                     ((uint32_t)msg.data[2] << 8)  |
                                     (uint32_t)msg.data[3]);
            _last_tacho = tacho;
            int16_t v_raw = (int16_t)(((uint16_t)msg.data[4] << 8) | msg.data[5]);
            float v_in = ((float)v_raw / 10.0f) + Settings.get().voltage_trim_v;

            if (controller_id == 0) {
                telemetry.voltage = v_in;
                telemetry.power_watts = telemetry.voltage * telemetry.current_amps;

                static bool s_can_tacho_init = false;
                if (!s_can_tacho_init) {
                    _tacho_trip_offset = tacho;
                    s_can_tacho_init = true;
                }

                int pole_pairs = Settings.get().motor_pole_pairs > 1 ? Settings.get().motor_pole_pairs : 15;
                float wheel_circ_m = (Settings.get().wheel_diameter_in * 0.0254f) * M_PI;
                float gear_ratio = Settings.get().gear_ratio > 0.1f ? Settings.get().gear_ratio : 1.0f;
                int32_t rel_tacho = tacho - _tacho_trip_offset;
                telemetry.trip_km = fabsf(((float)rel_tacho / ((float)pole_pairs * 6.0f)) * wheel_circ_m) / (gear_ratio * 1000.0f);
                telemetry.vesc_connected = true;
                s_last_can_rx_ms = now;
            }
        }
        else if (cmd_id == 14 && msg.data_length_code >= 8) { // CAN_PACKET_STATUS_2
            int32_t ah_raw = (int32_t)(((uint32_t)msg.data[0] << 24) |
                                       ((uint32_t)msg.data[1] << 16) |
                                       ((uint32_t)msg.data[2] << 8)  |
                                       (uint32_t)msg.data[3]);
            if (controller_id == 0) {
                telemetry.amphours_used = (float)ah_raw / 10000.0f;
            }
        }
    }

    if (now - s_last_can_rx_ms > 1500) {
        telemetry.vesc_connected = false;
    }

    if (now - s_last_can_diag_ms >= 2000) {
        s_last_can_diag_ms = now;
        Serial.printf("[VESC_CAN] Link: %s | V_in: %.1fV | Spd: %.1f km/h | MotI: %.1fA | BatI: %.1fA | ESC: %.1fC | Mot: %.1fC\n",
                      telemetry.vesc_connected ? "CONNECTED" : "NO_DATA",
                      telemetry.voltage, telemetry.speed_kmh, telemetry.phase_amps, telemetry.current_amps,
                      telemetry.temp_esc, telemetry.temp_motor);
    }

    if (telemetry.is_locked || ModeManager::instance().isLocked()) {
        telemetry.speed_kmh = 0.0f;
        telemetry.current_amps = 0.0f;
        telemetry.phase_amps = 0.0f;
        telemetry.power_watts = 0.0f;
        telemetry.duty_cycle_pct = 0.0f;
    }

    if (dt > 0.001f && dt < 1.0f) {
        Battery.update(telemetry, dt);

        if (telemetry.current_amps > telemetry.stats.peak_current_amps) telemetry.stats.peak_current_amps = telemetry.current_amps;
        if (telemetry.phase_amps > telemetry.stats.peak_phase_amps) telemetry.stats.peak_phase_amps = telemetry.phase_amps;
        if (telemetry.power_watts > telemetry.stats.peak_power_watts) telemetry.stats.peak_power_watts = telemetry.power_watts;
        if (telemetry.speed_kmh > telemetry.stats.max_speed_kmh) telemetry.stats.max_speed_kmh = telemetry.speed_kmh;
        if (telemetry.temp_motor > telemetry.stats.max_temp_motor) telemetry.stats.max_temp_motor = telemetry.temp_motor;
        if (telemetry.temp_esc > telemetry.stats.max_temp_esc) telemetry.stats.max_temp_esc = telemetry.temp_esc;

        static float s_last_spd_can = 0.0f;
        if (dt > 0.005f) {
            float dv_ms = (telemetry.speed_kmh - s_last_spd_can) / 3.6f;
            float g_inst = (dv_ms / dt) / 9.80665f;
            if (g_inst > 3.0f) g_inst = 3.0f;
            if (g_inst < -3.0f) g_inst = -3.0f;
            telemetry.g_force += (g_inst - telemetry.g_force) * (dt * 5.0f);
            if (fabsf(telemetry.g_force) < 0.02f) telemetry.g_force = 0.0f;
            if (fabsf(telemetry.g_force) > telemetry.stats.peak_g_force) {
                telemetry.stats.peak_g_force = fabsf(telemetry.g_force);
            }
            s_last_spd_can = telemetry.speed_kmh;
        }

        static float s_real_ride_time_accum = 0.0f;
        if (telemetry.stats.ride_time_sec == 0 && telemetry.trip_km == 0.0f) {
            s_real_ride_time_accum = 0.0f;
        }
        if (telemetry.speed_kmh > 1.2f) {
            s_real_ride_time_accum += dt;
            while (s_real_ride_time_accum >= 1.0f) {
                telemetry.stats.ride_time_sec += 1;
                s_real_ride_time_accum -= 1.0f;
            }
        }
        if (telemetry.stats.ride_time_sec > 2 && telemetry.trip_km > 0.01f) {
            telemetry.stats.avg_speed_kmh = telemetry.trip_km / (telemetry.stats.ride_time_sec / 3600.0f);
        }
    }
}
#else
void VescHandler::initCanBus() {}
void VescHandler::pollCanVesc(DashTelemetry &telemetry) {
    pollRealVesc(telemetry);
}
#endif

// ==============================================================================
// Live Flipsky 75100 VESC UART Communication
// ==============================================================================
void VescHandler::pollRealVesc(DashTelemetry &telemetry) {
    UartLock lock;
    uint32_t now = millis();
    static uint32_t last_update_time_ms = millis();
    float dt = (now - last_update_time_ms) / 1000.0f;
    last_update_time_ms = now;

    // Interleave polling at 25ms interval:
    // Phase 0: COMM_GET_VALUES (20Hz)
    // Phase 1: COMM_GET_DECODED_ADC (20Hz)
    static uint8_t poll_phase = 0;
    if (now - _last_poll_ms >= 25) {
        _last_poll_ms = now;
        if (poll_phase == 0) {
            sendVescGetValues();
            poll_phase = 1;
        } else {
            sendVescGetDecodedAdc();
            poll_phase = 0;
        }
    }

    // Process incoming packet from UART
    static uint8_t rx_buffer[1024];
    static size_t rx_index = 0;
    static uint32_t last_rx_byte_ms = 0;

    static uint32_t total_rx_bytes = 0;
    static uint32_t last_diag_ms = 0;

    if (!_transport) return;

    while (_transport->available()) {
        uint8_t b = (uint8_t)_transport->read();
        total_rx_bytes++;
        last_rx_byte_ms = now;

        if (rx_index == 0) {
            if (b == 0x02 || b == 0x03) { // Start byte
                rx_buffer[rx_index++] = b;
            }
        } else {
            if (rx_index < sizeof(rx_buffer)) {
                rx_buffer[rx_index++] = b;

                // Short packet (header 0x02, payload <= 256)
                if (rx_buffer[0] == 0x02 && rx_index >= 4) {
                    uint8_t payload_len = rx_buffer[1];
                    if (rx_index == (size_t)(payload_len + 5)) {
                        if (rx_buffer[rx_index - 1] == 0x03) {
                            parseVescPacket(rx_buffer, rx_index, telemetry);
                        }
                        rx_index = 0;
                    }
                }
                // Long packet (header 0x03, payload > 256, e.g. COMM_GET_MCCONF)
                else if (rx_buffer[0] == 0x03 && rx_index >= 5) {
                    uint16_t payload_len = ((uint16_t)rx_buffer[1] << 8) | rx_buffer[2];
                    if (rx_index == (size_t)(payload_len + 6)) {
                        if (rx_buffer[rx_index - 1] == 0x03) {
                            parseVescPacket(rx_buffer, rx_index, telemetry);
                        }
                        rx_index = 0;
                    }
                }
            } else {
                rx_index = 0;
            }
        }
    }

    // Timeout reset
    if (rx_index > 0 && (now - last_rx_byte_ms > 150)) {
        rx_index = 0;
        telemetry.vesc_connected = false;
    }

    // Check on-demand MCCONF pull timeout
    if (_sync_status == SYNC_REQUESTED && (now - _mcconf_req_ms > 2500)) {
        _sync_status = SYNC_FAILED;
        _sync_ms = now;
        Serial.println("[VESC_UART] COMM_GET_MCCONF request timed out (VESC off or disconnected).");
    }

    // Diagnostics every 2 seconds
    if (now - last_diag_ms >= 2000) {
        last_diag_ms = now;
        Serial.printf("[VESC_UART] Link: %s | V_in: %.1fV | Thr(ADC%d): %.1f%% [ADC1: %.2fV (%.1f%%) | ADC2: %.2fV (%.1f%%)]\n",
                      telemetry.vesc_connected ? "CONNECTED" : "NO_DATA",
                      telemetry.voltage,
                      Settings.get().throttle_adc_channel + 1,
                      telemetry.throttle_pct,
                      _last_adc1_v, _last_adc1_pct,
                      _last_adc2_v, _last_adc2_pct);
    }

    // Update Battery State & Real-Time Statistics
    if (dt > 0.001f && dt < 1.0f) {
        Battery.update(telemetry, dt);

        if (telemetry.current_amps > telemetry.stats.peak_current_amps) telemetry.stats.peak_current_amps = telemetry.current_amps;
        if (telemetry.phase_amps > telemetry.stats.peak_phase_amps) telemetry.stats.peak_phase_amps = telemetry.phase_amps;
        if (telemetry.power_watts > telemetry.stats.peak_power_watts) telemetry.stats.peak_power_watts = telemetry.power_watts;
        if (telemetry.speed_kmh > telemetry.stats.max_speed_kmh) telemetry.stats.max_speed_kmh = telemetry.speed_kmh;
        if (telemetry.temp_motor > telemetry.stats.max_temp_motor) telemetry.stats.max_temp_motor = telemetry.temp_motor;
        if (telemetry.temp_esc > telemetry.stats.max_temp_esc) telemetry.stats.max_temp_esc = telemetry.temp_esc;

        static float s_last_spd_can = 0.0f;
        if (dt > 0.005f) {
            float dv_ms = (telemetry.speed_kmh - s_last_spd_can) / 3.6f;
            float g_inst = (dv_ms / dt) / 9.80665f;
            if (g_inst > 3.0f) g_inst = 3.0f;
            if (g_inst < -3.0f) g_inst = -3.0f;
            telemetry.g_force += (g_inst - telemetry.g_force) * (dt * 5.0f);
            if (fabsf(telemetry.g_force) < 0.02f) telemetry.g_force = 0.0f;
            if (fabsf(telemetry.g_force) > telemetry.stats.peak_g_force) {
                telemetry.stats.peak_g_force = fabsf(telemetry.g_force);
            }
            s_last_spd_can = telemetry.speed_kmh;
        }

        static float s_real_ride_time_accum = 0.0f;
        if (telemetry.stats.ride_time_sec == 0 && telemetry.trip_km == 0.0f) {
            s_real_ride_time_accum = 0.0f;
        }
        if (telemetry.speed_kmh > 1.2f) {
            s_real_ride_time_accum += dt;
            while (s_real_ride_time_accum >= 1.0f) {
                telemetry.stats.ride_time_sec += 1;
                s_real_ride_time_accum -= 1.0f;
            }
        }
        if (telemetry.stats.ride_time_sec > 2 && telemetry.trip_km > 0.01f) {
            telemetry.stats.avg_speed_kmh = telemetry.trip_km / (telemetry.stats.ride_time_sec / 3600.0f);
        }
    }
}

void VescHandler::sendVescGetValues() {
    // Request COMM_GET_VALUES_SETUP (0x2F / 47) which contains mc_interface_get_speed()
    static const uint8_t req[] = { 0x02, 0x01, 0x2F, 0xD5, 0x8D, 0x03 };
    if (_transport) _transport->write(req, sizeof(req));
}

void VescHandler::sendVescGetDecodedAdc() {
    static const uint8_t req[] = { 0x02, 0x01, 0x20, 0x24, 0x62, 0x03 };
    if (_transport) _transport->write(req, sizeof(req));
}

bool VescHandler::parseVescPacket(uint8_t *buffer, size_t len, DashTelemetry &telemetry) {
    if (len < 6) return false;

    uint8_t *payload;
    size_t payload_len;

    if (buffer[0] == 0x02) {
        payload = &buffer[2];
        payload_len = buffer[1];
    } else if (buffer[0] == 0x03) {
        payload = &buffer[3];
        payload_len = ((size_t)buffer[1] << 8) | buffer[2];
    } else {
        return false;
    }

    uint16_t expected_crc = ((uint16_t)buffer[len - 3] << 8) | buffer[len - 2];
    uint16_t calc_crc = crc16(payload, payload_len);
    if (expected_crc != calc_crc) {
        return false;
    }

    telemetry.vesc_connected = true;
    uint8_t cmd_id = payload[0];

    // 1. Decoded ADC Packet (COMM_GET_DECODED_ADC = 0x20 / 32)
    if (cmd_id == 0x20) {
        if (payload_len >= 5) {
            int32_t raw_level1 = (int32_t)(((uint32_t)payload[1] << 24) | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 8) | (uint32_t)payload[4]);
            int32_t raw_volt1  = (payload_len >= 9)  ? (int32_t)(((uint32_t)payload[5] << 24) | ((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 8) | (uint32_t)payload[8]) : 0;
            int32_t raw_level2 = (payload_len >= 13) ? (int32_t)(((uint32_t)payload[9] << 24) | ((uint32_t)payload[10] << 16) | ((uint32_t)payload[11] << 8) | (uint32_t)payload[12]) : 0;
            int32_t raw_volt2  = (payload_len >= 17) ? (int32_t)(((uint32_t)payload[13] << 24) | ((uint32_t)payload[14] << 16) | ((uint32_t)payload[15] << 8) | (uint32_t)payload[16]) : 0;

            _last_adc1_v = raw_volt1 / 1000000.0f;
            _last_adc1_pct = (raw_level1 / 1000000.0f) * 100.0f;
            _last_adc2_v = raw_volt2 / 1000000.0f;
            _last_adc2_pct = (raw_level2 / 1000000.0f) * 100.0f;

            // Choose ADC1 (default 0) or ADC2 (1) based on settings
            int32_t chosen_level = (Settings.get().throttle_adc_channel == 1 && payload_len >= 13) ? raw_level2 : raw_level1;
            float pct = (chosen_level / 1000000.0f) * 100.0f;
            if (pct < 0.0f) pct = 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            if (telemetry.is_locked || ModeManager::instance().isLocked()) {
                pct = 0.0f;
            }
            telemetry.throttle_pct = pct;
        }
        return true;
    }

    // 2. Setup Telemetry Packet with True Ground Speed (COMM_GET_VALUES_SETUP = 0x2F / 47)
    if (cmd_id == 0x2F) {
        if (len < 50) return false;
        int idx = 1;
        int16_t temp_mos = (int16_t)((payload[idx] << 8) | payload[idx + 1]); idx += 2;
        int16_t temp_mot = (int16_t)((payload[idx] << 8) | payload[idx + 1]); idx += 2;
        int32_t current_mot = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]); idx += 4;
        int32_t current_in  = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]); idx += 4;
        int16_t duty_now = (int16_t)((payload[idx] << 8) | payload[idx + 1]); idx += 2;
        int32_t erpm = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]); idx += 4;
        int32_t speed_m_s = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]); idx += 4;
        int16_t v_in = (int16_t)((payload[idx] << 8) | payload[idx + 1]); idx += 2;
        idx += 2; // skip batt_level
        int32_t ah_used = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]); idx += 4;
        idx += 4; // skip ah_charge_tot
        idx += 4; // skip wh_used
        idx += 4; // skip wh_charge_tot
        int32_t dist_m = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]); idx += 4;
        int32_t dist_tot = (payload_len >= 51) ? (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]) : 0; idx += 4;
        _last_dist = dist_m;

        telemetry.temp_esc       = temp_mos / 10.0f;
        telemetry.temp_motor     = temp_mot / 10.0f;
        telemetry.phase_amps     = fabsf(current_mot / 100.0f);
        telemetry.current_amps   = (float)current_in / 100.0f;
        telemetry.duty_cycle_pct = fabsf((float)duty_now / 10.0f);
        int pole_pairs = Settings.get().motor_pole_pairs;
        if (pole_pairs < 2) pole_pairs = 15;
        telemetry.rpm            = fabsf((float)erpm / (float)pole_pairs);
        telemetry.voltage        = (v_in / 10.0f) + Settings.get().voltage_trim_v;
        if (telemetry.voltage < 0.0f) telemetry.voltage = 0.0f;
        telemetry.power_watts    = telemetry.voltage * telemetry.current_amps;
        telemetry.amphours_used  = ah_used / 10000.0f;

        // Ground speed from VESC (injected by Port 4 LispBM wheel sensor override)
        telemetry.speed_kmh      = fabsf((speed_m_s / 1000.0f) * 3.6f);
        if (telemetry.speed_kmh < 0.8f) telemetry.speed_kmh = 0.0f;

        int32_t rel_dist         = dist_m - _dist_trip_offset;
        telemetry.trip_km        = fabsf(((float)rel_dist / 1000.0f) / 1000.0f);
        if (payload_len >= 51) {
            telemetry.odo_km     = fabsf((dist_tot / 1000.0f) / 1000.0f);
        }

        // Fault code extraction (idx 55 in standard COMM_GET_VALUES_SETUP, backwards compatible with idx 51)
        uint8_t fault_code = (payload_len > 55) ? payload[55] : ((payload_len > 51) ? payload[51] : 0);
        telemetry.fault_code = fault_code;
        if (fault_code != 0) {
            FaultManager::instance().recordFault(fault_code, telemetry.voltage, telemetry.current_amps, telemetry.phase_amps, telemetry.temp_esc, telemetry.temp_motor, telemetry.uptime_sec);
        } else {
            FaultManager::instance().setActiveFault(0);
        }

        if (telemetry.is_locked || ModeManager::instance().isLocked()) {
            telemetry.speed_kmh = 0.0f;
            telemetry.current_amps = 0.0f;
            telemetry.phase_amps = 0.0f;
            telemetry.power_watts = 0.0f;
            telemetry.throttle_pct = 0.0f;
            telemetry.duty_cycle_pct = 0.0f;
        }
        return true;
    }

    // 3. Fallback: Standard Telemetry Packet (COMM_GET_VALUES = 0x04)
    if (cmd_id == 0x04) {
        if (len < 60) return false;

        // Unpack big-endian payload
        int idx = 1;
        int16_t temp_mos = (int16_t)((payload[idx] << 8) | payload[idx + 1]); idx += 2;
        int16_t temp_mot = (int16_t)((payload[idx] << 8) | payload[idx + 1]); idx += 2;
        int32_t current_mot = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]); idx += 4;
        int32_t current_in  = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]); idx += 4;
        idx += 8; // skip id, iq
        int16_t duty_now = (int16_t)((payload[idx] << 8) | payload[idx + 1]); idx += 2;
        int32_t erpm = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]); idx += 4;
        int16_t v_in = (int16_t)((payload[idx] << 8) | payload[idx + 1]); idx += 2;
        int32_t ah_used = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]); idx += 4;
        idx += 4; // skip ah_charged
        idx += 4; // skip wh_used
        idx += 4; // skip wh_charged
        int32_t tachometer = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]); idx += 4;
        _last_tacho = tachometer;

        telemetry.temp_esc       = temp_mos / 10.0f;
        telemetry.temp_motor     = temp_mot / 10.0f;
        telemetry.phase_amps     = fabsf(current_mot / 100.0f);
        telemetry.current_amps   = (float)current_in / 100.0f;
        telemetry.duty_cycle_pct = fabsf((float)duty_now / 10.0f);
        telemetry.voltage        = (v_in / 10.0f) + Settings.get().voltage_trim_v;
        if (telemetry.voltage < 0.0f) telemetry.voltage = 0.0f;
        telemetry.power_watts    = telemetry.voltage * telemetry.current_amps;
        telemetry.amphours_used  = ah_used / 10000.0f;

        // Convert ERPM to Vehicle Speed (km/h)
        int pole_pairs = Settings.get().motor_pole_pairs;
        if (pole_pairs < 2) pole_pairs = 15;
        float wheel_circ_m = (Settings.get().wheel_diameter_in * 0.0254f) * M_PI;
        float gear_ratio = Settings.get().gear_ratio;
        if (gear_ratio < 0.1f) gear_ratio = 1.0f;

        float mech_rpm = (float)erpm / (float)pole_pairs;
        telemetry.rpm = fabsf(mech_rpm);
        float wheel_rpm = mech_rpm / gear_ratio;
        telemetry.speed_kmh = fabsf((wheel_rpm * wheel_circ_m * 60.0f) / 1000.0f);
        if (telemetry.speed_kmh < 0.8f) telemetry.speed_kmh = 0.0f;

        // Odometer & Trip from tachometer step counts
        static bool s_uart_tacho_init = false;
        if (!s_uart_tacho_init) {
            _tacho_trip_offset = tachometer;
            s_uart_tacho_init = true;
        }

        int32_t rel_tacho = tachometer - _tacho_trip_offset;
        float revs = (float)rel_tacho / ((float)pole_pairs * 6.0f);
        telemetry.trip_km = fabsf((revs * wheel_circ_m) / (gear_ratio * 1000.0f));

        // Fault code extraction (idx 53 in COMM_GET_VALUES)
        uint8_t fault_code = (payload_len > 53) ? payload[53] : 0;
        telemetry.fault_code = fault_code;
        if (fault_code != 0) {
            FaultManager::instance().recordFault(fault_code, telemetry.voltage, telemetry.current_amps, telemetry.phase_amps, telemetry.temp_esc, telemetry.temp_motor, telemetry.uptime_sec);
        } else {
            FaultManager::instance().setActiveFault(0);
        }

        if (telemetry.is_locked || ModeManager::instance().isLocked()) {
            telemetry.speed_kmh = 0.0f;
            telemetry.current_amps = 0.0f;
            telemetry.phase_amps = 0.0f;
            telemetry.power_watts = 0.0f;
            telemetry.throttle_pct = 0.0f;
            telemetry.duty_cycle_pct = 0.0f;
        }
        return true;
    }

    // 4. Motor Configuration Packet (COMM_GET_MCCONF = 0x0E / 14)
    if (cmd_id == 0x0E) {
        if (payload_len >= 25) {
            int32_t ind = 5; // skip cmd_id (1) and MCCONF_SIGNATURE (4)
            ind += 4;        // skip pwm_mode, comm_mode, motor_type, sensor_mode
            float l_current_max    = buffer_get_float32_auto(payload, &ind);
            (void)buffer_get_float32_auto(payload, &ind); // skip l_current_min
            float l_in_current_max = buffer_get_float32_auto(payload, &ind);
            (void)buffer_get_float32_auto(payload, &ind); // skip l_in_current_min

            if (l_current_max > 5.0f && l_current_max < 400.0f &&
                l_in_current_max > 5.0f && l_in_current_max < 250.0f) {
                
                DashSettings &s = Settings.get();
                s.max_phase_amps   = (uint16_t)roundf(fabsf(l_current_max));
                s.max_battery_amps = (uint16_t)roundf(fabsf(l_in_current_max));
                Settings.save();

                _sync_status = SYNC_SUCCESS;
                _sync_ms = millis();
                Serial.printf("[VESC_UART] Pulled MCCONF successfully: Phase=%dA, Bat=%dA (saved to flash)\n",
                              s.max_phase_amps, s.max_battery_amps);
            } else {
                _sync_status = SYNC_FAILED;
                _sync_ms = millis();
            }
        } else {
            _sync_status = SYNC_FAILED;
            _sync_ms = millis();
        }
        return true;
    }

    return false;
}

void VescHandler::requestMcconf() {
    static const uint8_t req[] = { 0x02, 0x01, 0x0E, 0xE1, 0xCE, 0x03 };
    if (_transport) _transport->write(req, sizeof(req));
    _sync_status = SYNC_REQUESTED;
    _mcconf_req_ms = millis();
    Serial.println("[VESC_UART] Sent COMM_GET_MCCONF request to VESC.");
}

#if !defined(LGFX_LINUX_FB) && !defined(SIMULATOR)
#include <WiFi.h>
static WiFiServer _wifi_server(6510);
static WiFiClient _wifi_client;
#endif

// ==============================================================================
// Transparent USB CDC <-> UART Passthrough Bridge for VESC Tool
// ==============================================================================
void VescHandler::enterBridgeMode() {
    _bridge_active = true;
    _bridge_pc_to_vesc = 0;
    _bridge_vesc_to_pc = 0;

    // Ensure 4KB queues for high-volume configuration bursts (e.g. COMM_SET_MCCONF)
    Serial.setRxBufferSize(4096);
    Serial.setTxBufferSize(4096);
#if !defined(NATIVE_SIMULATOR)
    VESC_UART_PORT.setRxBufferSize(4096);
    VESC_UART_PORT.setTxBufferSize(4096);
#endif

    // Allow in-flight telemetry frames to conclude, then flush both pipelines
    delay(30);
    while (Serial.available()) Serial.read();
    if (_transport) {
        while (_transport->available()) _transport->read();
    }
}

void VescHandler::exitBridgeMode() {
    _bridge_active = false;
    _last_poll_ms = millis();
}

void VescHandler::updateUsbBridge() {
    if (!_bridge_active || !_transport) return;

    // 1. Forward USB CDC (PC / VESC Tool) -> Flipsky 75100 VESC Transport
    while (Serial.available() > 0) {
        uint8_t buf[512];
        size_t n = Serial.read(buf, sizeof(buf));
        if (n > 0) {
            _transport->write(buf, n);
            _bridge_pc_to_vesc += n;
        } else {
            break;
        }
    }

    // 2. Forward Flipsky 75100 VESC Transport -> USB CDC (PC / VESC Tool)
    while (_transport->available() > 0) {
        uint8_t buf[512];
        size_t n = _transport->readBytes(buf, sizeof(buf));
        if (n > 0) {
            Serial.write(buf, n);
            _bridge_vesc_to_pc += n;
        } else {
            break;
        }
    }
}

// ==============================================================================
// Transparent Wireless Wi-Fi TCP <-> UART Passthrough Bridge for VESC Tool
// ==============================================================================
void VescHandler::enterWifiBridgeMode() {
    _wifi_bridge_active = true;
    _wifi_client_connected = false;
    _wifi_client_ip[0] = '\0';
    _bridge_wifi_to_vesc = 0;
    _bridge_vesc_to_wifi = 0;

    // Ensure 4KB UART queues for high-volume configuration bursts
#if !defined(NATIVE_SIMULATOR)
    VESC_UART_PORT.setRxBufferSize(4096);
    VESC_UART_PORT.setTxBufferSize(4096);
#endif

    // Allow in-flight telemetry to conclude, then flush buffers
    delay(30);
    while (Serial.available()) Serial.read();
    if (_transport) {
        while (_transport->available()) _transport->read();
    }

#if !defined(LGFX_LINUX_FB) && !defined(SIMULATOR)
    WiFi.mode(WIFI_AP);
    IPAddress local_ip(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(local_ip, gateway, subnet);
    WiFi.softAP("VESC-DASH-AP");
    _wifi_server.begin(6510);
    _wifi_server.setNoDelay(true);
    Serial.println("[WIFI_BRIDGE] SoftAP started: SSID='VESC-DASH-AP', IP=192.168.4.1, Port=6510");
#endif
}

void VescHandler::exitWifiBridgeMode() {
    _wifi_bridge_active = false;
    _wifi_client_connected = false;
    _wifi_client_ip[0] = '\0';
    _last_poll_ms = millis();

#if !defined(LGFX_LINUX_FB) && !defined(SIMULATOR)
    if (_wifi_client) {
        _wifi_client.stop();
    }
    _wifi_server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[WIFI_BRIDGE] Wi-Fi powered down completely.");
#endif
}

void VescHandler::updateWifiBridge() {
#if !defined(LGFX_LINUX_FB) && !defined(SIMULATOR)
    if (!_wifi_bridge_active) return;

    if (_wifi_server.hasClient()) {
        if (_wifi_client && _wifi_client.connected()) {
            WiFiClient rej = _wifi_server.available();
            rej.stop();
        } else {
            _wifi_client = _wifi_server.available();
            _wifi_client.setNoDelay(true);
            _wifi_client_connected = true;
            IPAddress ip = _wifi_client.remoteIP();
            snprintf(_wifi_client_ip, sizeof(_wifi_client_ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
            Serial.printf("[WIFI_BRIDGE] Client connected: %s\n", _wifi_client_ip);
        }
    }

    if (_wifi_client && _wifi_client.connected()) {
        _wifi_client_connected = true;

        // 1. Forward TCP -> VESC UART (bulk read/write loop)
        while (_wifi_client.available() > 0) {
            uint8_t buf[512];
            int bytes_read = _wifi_client.read(buf, sizeof(buf));
            if (bytes_read > 0) {
                if (_transport) _transport->write(buf, bytes_read);
                _bridge_wifi_to_vesc += bytes_read;
            } else {
                break;
            }
        }

        // 2. Forward VESC UART -> TCP (bulk read/write loop)
        if (_transport) {
            while (_transport->available() > 0) {
                uint8_t buf[512];
                size_t n = _transport->readBytes(buf, sizeof(buf));
                if (n > 0) {
                    _wifi_client.write(buf, n);
                    _bridge_vesc_to_wifi += n;
                } else {
                    break;
                }
            }
        }
    } else {
        if (_wifi_client_connected) {
            _wifi_client_connected = false;
            _wifi_client_ip[0] = '\0';
            if (_wifi_client) {
                _wifi_client.stop();
            }
            Serial.println("[WIFI_BRIDGE] Client disconnected.");
        }
    }
#endif
}

void VescHandler::updateBridge() {
    if (_bridge_active) {
        updateUsbBridge();
    } else if (_wifi_bridge_active) {
        updateWifiBridge();
    }
}

void VescHandler::sendTerminalCommand(const char *str) {
#if !SIMULATION_MODE
    if (!_transport) return;
    size_t slen = strlen(str);
    if (slen > 68) slen = 68;
    uint8_t t_payload[70];
    t_payload[0] = 0x14; // COMM_TERMINAL_CMD
    memcpy(&t_payload[1], str, slen);
    uint8_t t_packet[76];
    t_packet[0] = 0x02;
    t_packet[1] = (uint8_t)(slen + 1);
    memcpy(&t_packet[2], t_payload, slen + 1);
    uint16_t tcrc = crc16(t_payload, slen + 1);
    t_packet[3 + slen] = (uint8_t)(tcrc >> 8);
    t_packet[4 + slen] = (uint8_t)(tcrc & 0xFF);
    t_packet[5 + slen] = 0x03;
    _transport->write(t_packet, slen + 6);
#endif
}

void VescHandler::sendModeLimits(float max_speed_kmh, float max_power_watts, float max_battery_amps, float max_phase_amps, float cutoff_start, float cutoff_end) {
    UartLock lock;
    // 1. Calculate ERPM from speed limit
    int pole_pairs = Settings.get().motor_pole_pairs;
    if (pole_pairs < 2) pole_pairs = 15;
    float wheel_diam_in = Settings.get().wheel_diameter_in;
    if (wheel_diam_in < 5.0f) wheel_diam_in = 26.0f;
    float wheel_circ_m = (wheel_diam_in * 0.0254f) * M_PI;
    float gear_ratio = Settings.get().gear_ratio;
    if (gear_ratio < 0.1f) gear_ratio = 1.0f;

    bool is_locked = (max_power_watts <= 0.0f && max_battery_amps <= 0.0f && max_phase_amps <= 0.0f);
    float max_erpm = 100000.0f; // Default unlimited

    if (is_locked) {
        max_erpm = 0.0f;
        max_power_watts = 0.0f;
        max_battery_amps = 0.0f;
        max_phase_amps = 0.0f;
        max_speed_kmh = 0.0f;
    } else {
        if (max_speed_kmh > 0.0f && max_speed_kmh < 150.0f) {
            float wheel_rpm = (max_speed_kmh * 1000.0f / 60.0f) / wheel_circ_m;
            float mech_rpm = wheel_rpm * gear_ratio;
            max_erpm = mech_rpm * (float)pole_pairs;
            if (max_erpm < 500.0f) max_erpm = 500.0f;
        }

        // Sanity clamp power and currents for active driving
        if (max_power_watts < 50.0f) max_power_watts = 50.0f;
        if (max_power_watts > 40000.0f) max_power_watts = 40000.0f;
        if (max_battery_amps < 2.0f) max_battery_amps = 2.0f;
        if (max_battery_amps > 200.0f) max_battery_amps = 200.0f;
        if (max_phase_amps < 5.0f) max_phase_amps = 5.0f;
        if (max_phase_amps > 300.0f) max_phase_amps = 300.0f;

        // Keep active UI settings in sync without uint8 overflow
        Settings.get().max_speed_kmh    = (uint8_t)roundf(max_speed_kmh);
        Settings.get().max_battery_amps = (uint16_t)roundf(max_battery_amps);
        Settings.get().max_phase_amps   = (uint16_t)roundf(max_phase_amps);
    }

    Serial.printf("[VESC_CMD] Dynamic Limits -> Spd: %.1f km/h (ERPM: %.0f) | Pwr: %.0f W | Bat: %.1f A | Phase: %.1f A\n",
                  max_speed_kmh, max_erpm, max_power_watts, max_battery_amps, max_phase_amps);

#if !SIMULATION_MODE
    // 2. Transmit COMM_SET_MCCONF_TEMP (Command ID 0x3E / 62)
    // Structure:
    // [0] = 0x3E
    // [1..4] = current_min (float32 auto)
    // [5..8] = current_max (float32 auto)
    // [9..12] = min_erpm (float32 auto)
    // [13..16] = max_erpm (float32 auto)
    // [17..20] = in_current_min (float32 auto)
    // [21..24] = in_current_max (float32 auto)
    // [25..26] = duty_min (int16 / 1000)
    // [27..28] = duty_max (int16 / 1000)
    // [29..32] = watt_min (float32 auto)
    // [33..36] = watt_max (float32 auto)
    uint8_t payload[37];
    int32_t p_idx = 0;
    payload[p_idx++] = 0x3E;

    auto append_f32 = [](uint8_t *b, float val, int32_t *idx) {
        union { float f; uint32_t u; } u;
        u.f = val;
        b[(*idx)++] = (uint8_t)(u.u >> 24);
        b[(*idx)++] = (uint8_t)(u.u >> 16);
        b[(*idx)++] = (uint8_t)(u.u >> 8);
        b[(*idx)++] = (uint8_t)(u.u);
    };
    auto append_i16 = [](uint8_t *b, int16_t val, int32_t *idx) {
        b[(*idx)++] = (uint8_t)(val >> 8);
        b[(*idx)++] = (uint8_t)(val);
    };

    append_f32(payload, -max_phase_amps, &p_idx);
    append_f32(payload, max_phase_amps, &p_idx);
    append_f32(payload, -max_erpm, &p_idx);
    append_f32(payload, max_erpm, &p_idx);
    append_f32(payload, -max_battery_amps, &p_idx);
    append_f32(payload, max_battery_amps, &p_idx);
    append_i16(payload, 0, &p_idx);        // duty_min: 0.00
    append_i16(payload, 950, &p_idx);      // duty_max: 0.95
    append_f32(payload, -max_power_watts, &p_idx);
    append_f32(payload, max_power_watts, &p_idx);

    // Short packet: 0x02, len, payload..., crc_hi, crc_lo, 0x03
    uint8_t packet[45];
    packet[0] = 0x02;
    packet[1] = (uint8_t)p_idx;
    memcpy(&packet[2], payload, p_idx);
    uint16_t crc = crc16(payload, p_idx);
    packet[2 + p_idx] = (uint8_t)(crc >> 8);
    packet[3 + p_idx] = (uint8_t)(crc & 0xFF);
    packet[4 + p_idx] = 0x03;
    if (_transport) _transport->write(packet, p_idx + 5);

    // Also forward to Ubox secondary VESC via CAN (CAN ID 1)
    uint8_t can_payload[39];
    can_payload[0] = 0x22; // COMM_FORWARD_CAN
    can_payload[1] = 0x01; // CAN ID 1
    memcpy(&can_payload[2], payload, p_idx);
    uint8_t can_packet[47];
    can_packet[0] = 0x02;
    can_packet[1] = (uint8_t)(p_idx + 2);
    memcpy(&can_packet[2], can_payload, p_idx + 2);
    uint16_t can_crc = crc16(can_payload, p_idx + 2);
    can_packet[4 + p_idx] = (uint8_t)(can_crc >> 8);
    can_packet[5 + p_idx] = (uint8_t)(can_crc & 0xFF);
    can_packet[6 + p_idx] = 0x03;
    if (_transport) _transport->write(can_packet, p_idx + 7);

#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    if (s_can_initialized) {
        twai_message_t can_lim;
        can_lim.extd = 1;
        can_lim.identifier = (21 << 8) | 0; // CAN_PACKET_CONF_CURRENT_LIMITS -> VESC ID 0
        can_lim.data_length_code = 8;
        int32_t min_mot = (int32_t)(-max_phase_amps * 1000.0f);
        int32_t max_mot = (int32_t)(max_phase_amps * 1000.0f);
        can_lim.data[0] = (min_mot >> 24) & 0xFF;
        can_lim.data[1] = (min_mot >> 16) & 0xFF;
        can_lim.data[2] = (min_mot >> 8) & 0xFF;
        can_lim.data[3] = min_mot & 0xFF;
        can_lim.data[4] = (max_mot >> 24) & 0xFF;
        can_lim.data[5] = (max_mot >> 16) & 0xFF;
        can_lim.data[6] = (max_mot >> 8) & 0xFF;
        can_lim.data[7] = max_mot & 0xFF;
        twai_transmit(&can_lim, pdMS_TO_TICKS(10));
        can_lim.identifier = (21 << 8) | 1; // Also dispatch to VESC ID 1 (Dual VESC)
        twai_transmit(&can_lim, pdMS_TO_TICKS(10));

        twai_message_t can_lim_in;
        can_lim_in.extd = 1;
        can_lim_in.identifier = (23 << 8) | 0; // CAN_PACKET_CONF_CURRENT_LIMITS_IN -> VESC ID 0
        can_lim_in.data_length_code = 8;
        int32_t min_bat = (int32_t)(-max_battery_amps * 1000.0f);
        int32_t max_bat = (int32_t)(max_battery_amps * 1000.0f);
        can_lim_in.data[0] = (min_bat >> 24) & 0xFF;
        can_lim_in.data[1] = (min_bat >> 16) & 0xFF;
        can_lim_in.data[2] = (min_bat >> 8) & 0xFF;
        can_lim_in.data[3] = min_bat & 0xFF;
        can_lim_in.data[4] = (max_bat >> 24) & 0xFF;
        can_lim_in.data[5] = (max_bat >> 16) & 0xFF;
        can_lim_in.data[6] = (max_bat >> 8) & 0xFF;
        can_lim_in.data[7] = max_bat & 0xFF;
        twai_transmit(&can_lim_in, pdMS_TO_TICKS(10));
        can_lim_in.identifier = (23 << 8) | 1; // Also dispatch to VESC ID 1 (Dual VESC)
        twai_transmit(&can_lim_in, pdMS_TO_TICKS(10));
    }
#endif

    // 3. Fallback / Universal Terminal Commands
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "conf_set l_current_max %.1f", max_phase_amps);
    sendTerminalCommand(cmd);
    snprintf(cmd, sizeof(cmd), "conf_set l_in_current_max %.1f", max_battery_amps);
    sendTerminalCommand(cmd);
    snprintf(cmd, sizeof(cmd), "conf_set l_watt_max %.1f", max_power_watts);
    sendTerminalCommand(cmd);
    snprintf(cmd, sizeof(cmd), "conf_set l_max_erpm %.0f", max_erpm);
    sendTerminalCommand(cmd);

    // Battery cutoff voltages if configured
    if (cutoff_start > 20.0f && cutoff_end > 20.0f) {
        snprintf(cmd, sizeof(cmd), "conf_set l_battery_cut_start %.1f", cutoff_start);
        sendTerminalCommand(cmd);
        snprintf(cmd, sizeof(cmd), "conf_set l_battery_cut_end %.1f", cutoff_end);
        sendTerminalCommand(cmd);
    }
#endif
}


