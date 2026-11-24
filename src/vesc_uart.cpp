#include "vesc_uart.h"
#include "settings_manager.h"
#include "battery_manager.h"
#include "fault_manager.h"
#include "mode_manager.h"
#include "ota_manager.h"
#include <math.h>

#if !defined(NATIVE_SIMULATOR)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#if VESC_USE_CAN_BUS
#include "driver/twai.h"
#include <esp_rom_sys.h>
#endif
static SemaphoreHandle_t s_uartMutex = NULL;
struct UartLock {
    UartLock() { if (s_uartMutex) xSemaphoreTakeRecursive(s_uartMutex, portMAX_DELAY); }
    ~UartLock() { if (s_uartMutex) xSemaphoreGiveRecursive(s_uartMutex); }
};
#else
struct UartLock { UartLock() {} ~UartLock() {} };
#endif

VescHandler Vesc;
static IVescTransport *s_lisp_transport = nullptr;

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

// Serializes VESC IEEE-754 auto-float (matches VESC buffer_append_float32_auto)
static void buffer_append_float32_auto(uint8_t *b, float number, int32_t *idx) {
    if (fabsf(number) < 1.5e-38f) {
        number = 0.0f;
    }
    int e = 0;
    float sig = frexpf(number, &e);
    float sig_abs = fabsf(sig);
    uint32_t sig_i = 0;
    if (sig_abs >= 0.5f) {
        sig_i = (uint32_t)((sig_abs - 0.5f) * 2.0f * 8388608.0f);
        e += 126;
    }
    uint32_t res = ((e & 0xFF) << 23) | (sig_i & 0x7FFFFF);
    if (sig < 0) {
        res |= (1U << 31);
    }
    b[(*idx)++] = (uint8_t)(res >> 24);
    b[(*idx)++] = (uint8_t)(res >> 16);
    b[(*idx)++] = (uint8_t)(res >> 8);
    b[(*idx)++] = (uint8_t)(res);
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
    _wh_trip_offset = _last_wh;
    _ah_trip_offset = _last_ah;
    _wh_trip_inited = true;
    _ah_trip_inited = true;
}

VescHandler::VescHandler()
    : _transport(nullptr),
      _pole_pairs(15),
      _wheel_diameter_mm(279.4f), // 11" wheel = 279.4 mm
      _gear_ratio(1.0f),
      _tacho_trip_offset(0),
      _last_tacho(0),
      _dist_trip_offset(0),
      _last_dist(0),
      _wh_trip_offset(0),
      _last_wh(0),
      _ah_trip_offset(0),
      _last_ah(0),
      _wh_trip_inited(false),
      _ah_trip_inited(false),
      _vesc_trip_wh(0.0f),
      _sim_throttle(0.0f),
      _sim_incline(0.0f),
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
      _sync_ms(0),
      _cached_wheel_diam_in(-1.0f),
      _cached_gear_ratio(-1.0f),
      _cached_pole_pairs(-1),
      _wheel_circ_m((11.0f * 0.0254f) * (float)M_PI),
      _inv_pole_pairs(1.0f / 15.0f),
      _erpm_to_kmh_factor(0.0f),
      _tacho_to_km_factor(0.0f),
      _last_g_speed_kmh(0.0f),
      _ride_time_accum_sec(0.0f),
      _horn_active(false),
      _last_horn_cmd_ms(0),
      _horn_cadence(0) {
    _wifi_client_ip[0] = '\0';
}

void VescHandler::updateWheelFactors() {
    float diam_in = Settings.get().wheel_diameter_in;
    if (diam_in < 1.0f) diam_in = 11.0f;
    float gear_ratio = Settings.get().gear_ratio;
    if (gear_ratio < 0.1f) gear_ratio = 1.0f;
    int pole_pairs = Settings.get().motor_pole_pairs;
    if (pole_pairs < 2) pole_pairs = 15;

    if (diam_in != _cached_wheel_diam_in ||
        gear_ratio != _cached_gear_ratio ||
        pole_pairs != _cached_pole_pairs) {
        _cached_wheel_diam_in = diam_in;
        _cached_gear_ratio = gear_ratio;
        _cached_pole_pairs = pole_pairs;

        _wheel_circ_m = (diam_in * 0.0254f) * (float)M_PI;
        _inv_pole_pairs = 1.0f / (float)pole_pairs;
        _erpm_to_kmh_factor = (_wheel_circ_m * 0.06f) / ((float)pole_pairs * gear_ratio);
        _tacho_to_km_factor = _wheel_circ_m / ((float)pole_pairs * 6.0f * gear_ratio * 1000.0f);
    }
}

void VescHandler::updateDerivedStats(DashTelemetry &telemetry, float dt, bool compute_gforce) {
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

    if (compute_gforce && dt > 0.005f) {
        float dv_ms = (telemetry.speed_kmh - _last_g_speed_kmh) / 3.6f;
        float g_inst = (dv_ms / dt) / 9.80665f;
        if (g_inst > 3.0f) g_inst = 3.0f;
        if (g_inst < -3.0f) g_inst = -3.0f;
        telemetry.g_force += (g_inst - telemetry.g_force) * (dt * 5.0f);
        if (fabsf(telemetry.g_force) < 0.02f) telemetry.g_force = 0.0f;
        _last_g_speed_kmh = telemetry.speed_kmh;
    }
    if (fabsf(telemetry.g_force) > telemetry.stats.peak_g_force) {
        telemetry.stats.peak_g_force = fabsf(telemetry.g_force);
    }

    if (telemetry.stats.ride_time_sec == 0 && telemetry.trip_km == 0.0f) {
        _ride_time_accum_sec = 0.0f;
    }
    if (telemetry.speed_kmh > 1.2f) {
        _ride_time_accum_sec += dt;
        while (_ride_time_accum_sec >= 1.0f) {
            telemetry.stats.ride_time_sec += 1;
            _ride_time_accum_sec -= 1.0f;
        }
    }
    if (telemetry.stats.ride_time_sec > 2 && telemetry.trip_km > 0.01f) {
        telemetry.stats.avg_speed_kmh = telemetry.trip_km / (telemetry.stats.ride_time_sec / 3600.0f);
    }
}

void VescHandler::begin() {
#if !defined(NATIVE_SIMULATOR)
    if (s_uartMutex == NULL) {
        s_uartMutex = xSemaphoreCreateRecursiveMutex();
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
    s_lisp_transport = _transport;
}

void VescHandler::setSimThrottle(float pct) {
    if (pct <= 0.01f) {
        _sim_throttle = 0.0f;
    } else {
        if (pct > 100.0f) pct = 100.0f;
        _sim_throttle = pct;
    }
}

void VescHandler::update(DashTelemetry &telemetry) {
#if defined(NATIVE_SIMULATOR)
    pollRealVesc(telemetry);
#elif SIMULATION_MODE
    runSimulation(telemetry);
#elif VESC_USE_CAN_BUS
    pollCanVesc(telemetry);
#else
    pollRealVesc(telemetry);
#endif
}

// ==============================================================================
// Realistic eBike Telemetry Simulation Engine (Hyper-VESC 35kW+ Profile)
// ==============================================================================
void VescHandler::runSimulation(DashTelemetry &telemetry) {
    uint32_t now = millis();
    float dt = (now - _sim_last_update_ms) / 1000.0f;
    if (dt <= 0.001f) return;
    _sim_last_update_ms = now;

    float sim_t = 0.0f;
    float sim_b = 0.0f;

    if (_sim_throttle > 0.01f) {
        sim_t = _sim_throttle;
    }

    // Check if vehicle is locked by boot PIN
    if (ModeManager::instance().isLocked() || telemetry.is_locked) {
        sim_t = 0.0f;
        sim_b = 100.0f;
    }

    telemetry.throttle_pct = sim_t;

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
    ModeLimits activeLim = ModeManager::instance().getActiveLimits();
    float max_possible_speed = (activeLim.max_speed_kmh > 0.0f) ? activeLim.max_speed_kmh : 100.0f;
    if (speed_limit > 0 && (float)speed_limit < max_possible_speed) {
        max_possible_speed = (float)speed_limit;
    }
    _target_speed = (sim_t / 100.0f) * max_possible_speed;

    float prev_sim_spd = telemetry.speed_kmh;
    uint8_t brake_mode = Settings.get().brake_mode;
    if (sim_b > 0.01f) {
        if (brake_mode == 0) {
            // brake_mode == 0 (AUS / FREILAUF): Pure free coasting, no electronic regen braking
            telemetry.speed_kmh -= 3.5f * dt;
            if (telemetry.speed_kmh < 0.5f) telemetry.speed_kmh = 0.0f;
            telemetry.current_amps = 0.0f;
            telemetry.phase_amps = 0.0f;
        } else if (brake_mode == 1) {
            // brake_mode == 1 (MILD): Reduced regen braking current (e.g. max 12A phase / 8A battery)
            float decel_k = 1.2f + (sim_b / 100.0f) * 2.2f;
            telemetry.speed_kmh -= decel_k * 9.81f * 3.6f * dt * 0.22f;
            if (telemetry.speed_kmh < 0.5f) telemetry.speed_kmh = 0.0f;
        } else {
            // brake_mode == 2 (MAXIMAL): Full normal regen braking
            float decel_k = 1.6f + (sim_b / 100.0f) * 4.2f;
            telemetry.speed_kmh -= decel_k * 9.81f * 3.6f * dt * 0.22f;
            if (telemetry.speed_kmh < 0.5f) telemetry.speed_kmh = 0.0f;
        }
    } else if (sim_t > 0.01f) {
        // Accelerating
        telemetry.speed_kmh += (_target_speed - telemetry.speed_kmh) * (dt * 1.5f);
    } else {
        // Natural rolling resistance coasting (lower drag in freewheel mode)
        telemetry.speed_kmh -= (brake_mode == 0 ? 2.5f : 3.5f) * dt;
        if (telemetry.speed_kmh < 0.5f) telemetry.speed_kmh = 0.0f;
    }
    if (telemetry.speed_kmh <= 0.5f && sim_t <= 0.01f) {
        telemetry.speed_kmh = 0.0f;
        telemetry.current_amps = 0.0f;
        telemetry.phase_amps = 0.0f;
        telemetry.power_watts = 0.0f;
        telemetry.duty_cycle_pct = 0.0f;
        telemetry.rpm = 0.0f;
    }
    telemetry.vesc_motor_speed = telemetry.speed_kmh;

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
    telemetry.incline_deg = _sim_incline;

    // 2. Realistic Duty Cycle calculation (0.0% to 120.0%)
    float base_duty = (telemetry.speed_kmh / 48.0f) * 92.0f;
    if (sim_t > 85.0f && telemetry.speed_kmh > 46.0f) {
        // Field Weakening engaged
        float fw_extra = ((sim_t - 85.0f) / 15.0f) * 18.0f;
        telemetry.duty_cycle_pct = 96.0f + fw_extra;
    } else {
        telemetry.duty_cycle_pct = base_duty;
    }
    if (telemetry.duty_cycle_pct > 100.0f) telemetry.duty_cycle_pct = 100.0f;
    if (telemetry.duty_cycle_pct < 0.0f) telemetry.duty_cycle_pct = 0.0f;

    // RPM equivalent
    telemetry.rpm = (telemetry.speed_kmh / 45.0f) * 130.0f * 35.0f; // ~4500 motor RPM

    // 3. Current & Phase Amps (Bafang Mid-Drive tuning)
    uint16_t max_bat_a = Settings.get().max_battery_amps > 0 ? Settings.get().max_battery_amps : 45;
    uint16_t max_pha_a = Settings.get().max_phase_amps > 0 ? Settings.get().max_phase_amps : 160;

    if (sim_b > 0.01f && telemetry.speed_kmh > 1.5f) {
        if (brake_mode == 0) {
            // Freewheel / AUS: electronic brake current clamped to 0A, zero regen!
            telemetry.current_amps = 0.0f;
            telemetry.phase_amps = 0.0f;
        } else if (brake_mode == 1) {
            // MILD: Reduced regen braking current (e.g. max 12A phase / 8A battery)
            float target_regen_a = -((sim_b / 100.0f) * 8.0f);
            telemetry.current_amps += (target_regen_a - telemetry.current_amps) * (dt * 4.0f);
            telemetry.phase_amps = fabsf(telemetry.current_amps) * 1.5f;
            if (telemetry.phase_amps > 12.0f) telemetry.phase_amps = 12.0f;
        } else {
            // MAXIMAL: Full normal regen braking
            float target_regen_a = -((sim_b / 100.0f) * 18.0f);
            telemetry.current_amps += (target_regen_a - telemetry.current_amps) * (dt * 4.0f);
            telemetry.phase_amps = fabsf(telemetry.current_amps) * 1.8f;
        }
    } else if (sim_t > 0.01f) {
        float target_bat_amps = (sim_t / 100.0f) * (float)max_bat_a;
        telemetry.current_amps += (target_bat_amps - telemetry.current_amps) * (dt * 3.5f);
        if (telemetry.current_amps < 0.0f) telemetry.current_amps = 0.0f;

        float duty_fraction = telemetry.duty_cycle_pct / 100.0f;
        if (duty_fraction > 0.95f) duty_fraction = 0.95f;
        if (duty_fraction < 0.35f) duty_fraction = 0.35f;
        float calculated_phase = telemetry.current_amps / duty_fraction;
        if (calculated_phase < telemetry.current_amps) calculated_phase = telemetry.current_amps;
        if (calculated_phase > (float)max_pha_a) calculated_phase = (float)max_pha_a;
        telemetry.phase_amps = calculated_phase;
    } else {
        telemetry.current_amps += (0.0f - telemetry.current_amps) * (dt * 5.0f);
        if (fabsf(telemetry.current_amps) < 0.1f) telemetry.current_amps = 0.0f;
        telemetry.phase_amps = 0.0f;
    }

    // 4. Battery Voltage & Power
    telemetry.voltage = 53.8f - (telemetry.current_amps * 0.048f);
    telemetry.power_watts = telemetry.voltage * telemetry.current_amps;
    if (telemetry.speed_kmh == 0.0f && sim_t == 0.0f) {
        telemetry.power_watts = 0.0f;
        telemetry.current_amps = 0.0f;
        telemetry.phase_amps = 0.0f;
        telemetry.duty_cycle_pct = 0.0f;
    }

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

    // 8. Statistics Tracking (g_force already integrated in sim step 1)
    updateDerivedStats(telemetry, dt, false);

    telemetry.vesc_connected = true;
}

// ==============================================================================
// Live Spintend Ubox / VESC CAN Bus Communication (ESP32-S3 TWAI)
// ==============================================================================
#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
static bool s_can_initialized = false;
static uint8_t s_primary_vesc_id = VESC_CAN_ID_DEFAULT;
static uint8_t s_secondary_vesc_id = 0xFF;
static bool s_has_primary = false;
static bool s_has_76 = false;
static bool s_has_secondary = false;
static uint32_t s_sec_last_rx_ms = 0;

static constexpr size_t CAN_RX_BUF_SIZE = 2048;
static uint8_t s_can_rx_buf[CAN_RX_BUF_SIZE];

static void ensureCanBusHealthy(bool force = false) {
    if (!s_can_initialized) return;
    static uint32_t s_last_can_health_check_ms = 0;
    uint32_t now = millis();
    if (!force && (uint32_t)(now - s_last_can_health_check_ms) < 250) {
        return;
    }
    s_last_can_health_check_ms = now;

    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
        if (st.state == TWAI_STATE_BUS_OFF) {
            twai_initiate_recovery();
        } else if (st.state == TWAI_STATE_STOPPED) {
            twai_start();
        }
    }
}

static void updateDetectedCanIds(uint8_t controller_id, uint8_t cmd_id, uint32_t now) {
    if (controller_id == VescHandler::ESP_CAN_ID) return;
    // Only learn VESC node IDs from VESC broadcast status packets
    if (cmd_id != 9 && cmd_id != 14 && cmd_id != 15 && cmd_id != 16 && cmd_id != 27) return;

    if (controller_id == VESC_CAN_ID_DEFAULT) {
        s_has_76 = true;
        s_primary_vesc_id = VESC_CAN_ID_DEFAULT;
        s_has_primary = true;
    } else if (!s_has_76 && !s_has_primary && controller_id != 77) {
        s_primary_vesc_id = controller_id;
        s_has_primary = true;
        Serial.printf("[VESC_CAN] Auto-detected primary VESC ID: %u\n", s_primary_vesc_id);
    } else if (controller_id != s_primary_vesc_id) {
        if (!s_has_secondary || s_secondary_vesc_id != controller_id) {
            Serial.printf("[VESC_CAN] Auto-detected secondary VESC ID: %u\n", controller_id);
        }
        s_secondary_vesc_id = controller_id;
        s_has_secondary = true;
        s_sec_last_rx_ms = now;
    }
}

static void twaiTransmitToDetected(twai_message_t &msg, uint8_t cmd_id, TickType_t timeout_ticks = pdMS_TO_TICKS(5)) {
    if (!s_can_initialized) return;
    msg.extd = 1;
    msg.rtr = 0;
    msg.identifier = ((uint32_t)cmd_id << 8) | s_primary_vesc_id;
    if (twai_transmit(&msg, timeout_ticks) != ESP_OK) {
        ensureCanBusHealthy(true);
    }

    if (s_has_secondary && s_secondary_vesc_id != s_primary_vesc_id) {
        msg.identifier = ((uint32_t)cmd_id << 8) | s_secondary_vesc_id;
        if (twai_transmit(&msg, timeout_ticks) != ESP_OK) {
            ensureCanBusHealthy(true);
        }
    }
}

// VESC CAN buffer TX (supports short <= 6B, standard FILL_RX_BUFFER < 256, and FILL_RX_BUFFER_LONG >= 256)
// send_flag in VESC comm_can.c:
//   0 = process packet & send reply back to src_id (used for Bridge & telemetry queries)
//   1 = forward directly to VESC USB/UART (used by VESC when replying)
//   2 = process packet WITHOUT sending any reply (used for fire-and-forget commands)
static void sendVescCanBuffer(uint8_t controller_id, const uint8_t *data, size_t len,
                              uint8_t src_id = VescHandler::ESP_CAN_ID, uint8_t send_flag = 2) {
    if (!s_can_initialized || !data || len == 0 || len > CAN_RX_BUF_SIZE) return;

    if (len <= 6) {
        twai_message_t msg = {};
        msg.extd = 1;
        msg.identifier = (8U << 8) | controller_id; // CAN_PACKET_PROCESS_SHORT_BUFFER (ID 8)
        msg.data_length_code = (uint8_t)(len + 2);
        msg.data[0] = src_id;
        msg.data[1] = send_flag;
        memcpy(&msg.data[2], data, len);
        if (twai_transmit(&msg, pdMS_TO_TICKS(5)) != ESP_OK) {
            ensureCanBusHealthy(true);
        }
    } else {
        size_t i = 0;
        while (i < len) {
            twai_message_t msg = {};
            msg.extd = 1;
            if (i < 256) {
                // CAN_PACKET_FILL_RX_BUFFER (ID 5): 1-byte offset + up to 7 bytes
                size_t chunk_len = (len - i > 7) ? 7 : (len - i);
                msg.identifier = (5U << 8) | controller_id;
                msg.data_length_code = (uint8_t)(chunk_len + 1);
                msg.data[0] = (uint8_t)i;
                memcpy(&msg.data[1], data + i, chunk_len);
                i += chunk_len;
            } else {
                // CAN_PACKET_FILL_RX_BUFFER_LONG (ID 6): 2-byte big-endian offset + up to 6 bytes
                size_t chunk_len = (len - i > 6) ? 6 : (len - i);
                msg.identifier = (6U << 8) | controller_id;
                msg.data_length_code = (uint8_t)(chunk_len + 2);
                msg.data[0] = (uint8_t)((i >> 8) & 0xFF);
                msg.data[1] = (uint8_t)(i & 0xFF);
                memcpy(&msg.data[2], data + i, chunk_len);
                i += chunk_len;
            }
            esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(10));
            if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(1);
            } else if (err != ESP_OK) {
                ensureCanBusHealthy(true);
            }
        }

        // CAN_PACKET_PROCESS_RX_BUFFER (ID 7)
        uint16_t crc = crc16(data, len);
        twai_message_t proc = {};
        proc.extd = 1;
        proc.identifier = (7U << 8) | controller_id;
        proc.data_length_code = 6;
        proc.data[0] = src_id;
        proc.data[1] = send_flag;
        proc.data[2] = (uint8_t)((len >> 8) & 0xFF);
        proc.data[3] = (uint8_t)(len & 0xFF);
        proc.data[4] = (uint8_t)((crc >> 8) & 0xFF);
        proc.data[5] = (uint8_t)(crc & 0xFF);
        if (twai_transmit(&proc, pdMS_TO_TICKS(10)) != ESP_OK) {
            ensureCanBusHealthy(true);
        }
    }
}

static void sendVescCanBufferToDetected(const uint8_t *data, size_t len,
                                        uint8_t src_id = VescHandler::ESP_CAN_ID, uint8_t send_flag = 2) {
    sendVescCanBuffer(s_primary_vesc_id, data, len, src_id, send_flag);
    if (s_has_secondary && s_secondary_vesc_id != s_primary_vesc_id) {
        sendVescCanBuffer(s_secondary_vesc_id, data, len, src_id, send_flag);
    }
}

// Reassemble VESC CAN buffer replies addressed to ESP_CAN_ID (cmd 5, 6, 7, 8)
static bool processCanRxFrameForEsp(const twai_message_t &msg, const uint8_t **out_payload, size_t *out_len) {
    static uint32_t s_last_rx_frame_ms = 0;
    static uint8_t  s_last_src_id = 0xFF;
    static bool     s_in_progress = false;

    uint32_t now_ms = millis();
    uint8_t cmd_id = (uint8_t)((msg.identifier >> 8) & 0xFF);
    uint8_t src_id = (uint8_t)((msg.identifier >> 16) & 0xFF);

    // Timeout: if no subsequent frame arrived for >500ms, abort incomplete reassembly
    if (s_in_progress && (now_ms - s_last_rx_frame_ms > 500)) {
        s_in_progress = false;
        s_last_src_id = 0xFF;
    }

    if (cmd_id == 5 && msg.data_length_code >= 2) {
        // CAN_PACKET_FILL_RX_BUFFER
        size_t offset = msg.data[0];
        if (offset == 0) {
            s_in_progress = true;
            s_last_src_id = src_id;
        } else if (!s_in_progress || src_id != s_last_src_id) {
            return false; // Interleaved frame from another node or orphaned fragment
        }

        s_last_rx_frame_ms = now_ms;
        size_t chunk_len = (size_t)msg.data_length_code - 1;
        if (offset + chunk_len <= CAN_RX_BUF_SIZE) {
            memcpy(s_can_rx_buf + offset, &msg.data[1], chunk_len);
        }
        return false;
    } else if (cmd_id == 6 && msg.data_length_code >= 3) {
        // CAN_PACKET_FILL_RX_BUFFER_LONG
        size_t offset = ((size_t)msg.data[0] << 8) | msg.data[1];
        if (offset == 0) {
            s_in_progress = true;
            s_last_src_id = src_id;
        } else if (!s_in_progress || src_id != s_last_src_id) {
            return false; // Interleaved frame from another node or orphaned fragment
        }

        s_last_rx_frame_ms = now_ms;
        size_t chunk_len = (size_t)msg.data_length_code - 2;
        if (offset + chunk_len <= CAN_RX_BUF_SIZE) {
            memcpy(s_can_rx_buf + offset, &msg.data[2], chunk_len);
        }
        return false;
    } else if (cmd_id == 7 && msg.data_length_code >= 6) {
        // CAN_PACKET_PROCESS_RX_BUFFER
        if (!s_in_progress || src_id != s_last_src_id) {
            return false; // Not matching current reassembly session
        }

        s_in_progress = false;
        s_last_src_id = 0xFF;

        uint16_t rx_len = ((uint16_t)msg.data[2] << 8) | msg.data[3];
        uint16_t rx_crc = ((uint16_t)msg.data[4] << 8) | msg.data[5];
        if (rx_len > 0 && rx_len <= CAN_RX_BUF_SIZE && crc16(s_can_rx_buf, rx_len) == rx_crc) {
            *out_payload = s_can_rx_buf;
            *out_len = rx_len;
            return true;
        }
        return false;
    } else if (cmd_id == 8 && msg.data_length_code >= 3) {
        // CAN_PACKET_PROCESS_SHORT_BUFFER
        size_t rx_len = (size_t)msg.data_length_code - 2;
        if (rx_len <= 6) {
            memcpy(s_can_rx_buf, &msg.data[2], rx_len);
            *out_payload = s_can_rx_buf;
            *out_len = rx_len;
            return true;
        }
        return false;
    }
    return false;
}

// Stream parser for VESC UART-framed packets arriving over TCP/USB Bridge (tunneled to CAN)
struct VescBridgeFramer {
    uint8_t  buf[CAN_RX_BUF_SIZE + 8];
    size_t   idx = 0;
    uint32_t last_byte_ms = 0;

    void reset() {
        idx = 0;
        last_byte_ms = 0;
    }

    bool feed(uint8_t b, uint32_t now_ms, const uint8_t **out_payload, size_t *out_len) {
        if (idx > 0 && (uint32_t)(now_ms - last_byte_ms) > 500) {
            idx = 0;
        }
        last_byte_ms = now_ms;

        if (idx == 0) {
            if (b == 0x02 || b == 0x03) {
                buf[idx++] = b;
            }
            return false;
        }

        if (idx >= sizeof(buf)) {
            idx = 0;
            return false;
        }
        buf[idx++] = b;

        size_t hdr_len = 0;
        size_t payload_len = 0;
        if (buf[0] == 0x02) {
            if (idx < 2) return false;
            hdr_len = 2;
            payload_len = buf[1];
        } else if (buf[0] == 0x03) {
            if (idx < 3) return false;
            hdr_len = 3;
            payload_len = ((size_t)buf[1] << 8) | buf[2];
        } else {
            idx = 0;
            return false;
        }

        if (payload_len == 0 || payload_len > CAN_RX_BUF_SIZE) {
            idx = 0;
            return false;
        }

        size_t total_len = hdr_len + payload_len + 3; // hdr + payload + crc16 + 0x03
        if (idx < total_len) {
            return false;
        }

        bool ok = false;
        if (buf[total_len - 1] == 0x03) {
            uint16_t rx_crc = ((uint16_t)buf[total_len - 3] << 8) | buf[total_len - 2];
            if (crc16(&buf[hdr_len], payload_len) == rx_crc) {
                *out_payload = &buf[hdr_len];
                *out_len = payload_len;
                ok = true;
            }
        }
        idx = 0;
        return ok;
    }
};

static size_t frameVescUartPacket(const uint8_t *payload, size_t payload_len, uint8_t *out_buf, size_t out_cap) {
    if (!payload || payload_len == 0 || payload_len > CAN_RX_BUF_SIZE) return 0;
    uint16_t crc = crc16(payload, payload_len);
    size_t pos = 0;
    if (payload_len <= 255) {
        if (payload_len + 5 > out_cap) return 0;
        out_buf[pos++] = 0x02;
        out_buf[pos++] = (uint8_t)payload_len;
    } else {
        if (payload_len + 6 > out_cap) return 0;
        out_buf[pos++] = 0x03;
        out_buf[pos++] = (uint8_t)((payload_len >> 8) & 0xFF);
        out_buf[pos++] = (uint8_t)(payload_len & 0xFF);
    }
    memcpy(out_buf + pos, payload, payload_len);
    pos += payload_len;
    out_buf[pos++] = (uint8_t)((crc >> 8) & 0xFF);
    out_buf[pos++] = (uint8_t)(crc & 0xFF);
    out_buf[pos++] = 0x03;
    return pos;
}

static void dispatchBridgePayloadToCan(const uint8_t *payload, size_t payload_len) {
    if (!payload || payload_len == 0) return;
    if (payload[0] == 0x22 && payload_len >= 3) {
        // COMM_FORWARD_CAN: tunnel inner packet directly to requested CAN target so reply returns to ESP_CAN_ID
        uint8_t target_id = payload[1];
        sendVescCanBuffer(target_id, payload + 2, payload_len - 2, VescHandler::ESP_CAN_ID, 0);
    } else {
        sendVescCanBuffer(s_primary_vesc_id, payload, payload_len, VescHandler::ESP_CAN_ID, 0);
    }
}

static VescBridgeFramer s_bridge_framer;
static uint8_t s_bridge_tx_frame[CAN_RX_BUF_SIZE + 8];

void VescHandler::initCanBus() {
    if (s_can_initialized) return;

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = 64;
    g_config.tx_queue_len = 64;
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err == ESP_OK) {
        err = twai_start();
        if (err == ESP_OK) {
            s_can_initialized = true;
            Serial.printf("[VESC_CAN] ESP32-S3 TWAI CAN started at 500kbit/s (TX: GPIO 20, RX: GPIO 19, Local ID: %u).\n",
                          (unsigned)ESP_CAN_ID);
        } else {
            Serial.printf("[VESC_CAN ERROR] twai_start failed: 0x%x\n", err);
        }
    } else {
        Serial.printf("[VESC_CAN ERROR] twai_driver_install failed: 0x%x\n", err);
    }
}

void VescHandler::pollCanVesc(DashTelemetry &telemetry) {
    if (!s_can_initialized) return;

    ensureCanBusHealthy();
    uint32_t now = millis();
    static uint32_t s_last_can_rx_ms = 0;
    static uint32_t s_last_can_diag_ms = 0;
    static uint32_t s_last_can_req_ms = 0;
    static uint8_t  s_can_poll_phase = 0;
    static bool s_has_imu_g = false;

    // Interleaved 5 Hz (200ms) active polling for CAN telemetry blindspots (Fault Code & Throttle ADC)
    if (!isBridgeActive() && (uint32_t)(now - s_last_can_req_ms) >= 200) {
        s_last_can_req_ms = now;
        if (s_can_poll_phase == 0) {
            // Phase 0: COMM_GET_VALUES_SELECTIVE (0x32 / 50), mask = (1U << 15) for fault_code only (5B -> single short CAN frame!)
            static const uint8_t req_fault[5] = { 0x32, 0x00, 0x00, 0x80, 0x00 };
            sendVescCanBuffer(s_primary_vesc_id, req_fault, sizeof(req_fault), ESP_CAN_ID, 0);
            s_can_poll_phase = 1;
        } else {
            // Phase 1: COMM_GET_DECODED_ADC (0x20 / 32) for throttle_pct & ADC1/ADC2 diagnostics
            static const uint8_t req_adc[1] = { 0x20 };
            sendVescCanBuffer(s_primary_vesc_id, req_adc, sizeof(req_adc), ESP_CAN_ID, 0);
            s_can_poll_phase = 0;
        }
    }

    twai_message_t msg;
    int frames_read = 0;
    while (twai_receive(&msg, pdMS_TO_TICKS(0)) == ESP_OK && frames_read < 48) {
        frames_read++;
        if (!msg.extd) continue;

        uint8_t controller_id = (uint8_t)(msg.identifier & 0xFF);
        uint8_t cmd_id = (uint8_t)((msg.identifier >> 8) & 0xFF);

        // Handle CAN RX buffer replies addressed specifically to our ESP32 node ID (ESP_CAN_ID)
        if (controller_id == ESP_CAN_ID) {
            const uint8_t *rx_payload = nullptr;
            size_t rx_len = 0;
            if (processCanRxFrameForEsp(msg, &rx_payload, &rx_len)) {
                parseVescPayload(rx_payload, rx_len, telemetry);
                telemetry.vesc_connected = true;
                s_last_can_rx_ms = now;
            }
            continue;
        }

        static bool s_first_frame_logged = false;
        if (!s_first_frame_logged) {
            s_first_frame_logged = true;
            Serial.printf("[VESC_CAN] First frame RX -> ID: %u, Cmd: %u, DLC: %u\n", controller_id, cmd_id, msg.data_length_code);
        }

        updateDetectedCanIds(controller_id, cmd_id, now);

        bool is_primary = (controller_id == s_primary_vesc_id);
        bool is_secondary = (s_has_secondary && !is_primary && controller_id == s_secondary_vesc_id);

        if (is_secondary) {
            s_sec_last_rx_ms = now;
        }

        if (cmd_id == 9 && msg.data_length_code >= 8) { // CAN_PACKET_STATUS
            int32_t erpm = (int32_t)(((uint32_t)msg.data[0] << 24) |
                                     ((uint32_t)msg.data[1] << 16) |
                                     ((uint32_t)msg.data[2] << 8)  |
                                     (uint32_t)msg.data[3]);
            int16_t cur_raw = (int16_t)(((uint16_t)msg.data[4] << 8) | msg.data[5]);
            int16_t duty_raw = (int16_t)(((uint16_t)msg.data[6] << 8) | msg.data[7]);

            float mot_i = fabsf((float)cur_raw / 10.0f);
            float duty = fabsf((float)duty_raw / 10.0f);

            if (is_primary) {
                updateWheelFactors();
                float abs_erpm = fabsf((float)erpm);
                telemetry.rpm = abs_erpm * _inv_pole_pairs;
                float v_spd = abs_erpm * _erpm_to_kmh_factor;
                if (v_spd < 0.8f) v_spd = 0.0f;
                telemetry.vesc_motor_speed = v_spd;
                telemetry.speed_kmh = v_spd;
                telemetry.phase_amps = mot_i;
                telemetry.duty_cycle_pct = (duty > 100.0f) ? 100.0f : duty;

                telemetry.vesc_connected = true;
                s_last_can_rx_ms = now;
            } else if (is_secondary) {
                telemetry.vesc2_max_phase = mot_i;
                s_sec_last_rx_ms = now;
            }
        }
        else if (cmd_id == 16 && msg.data_length_code >= 8) { // CAN_PACKET_STATUS_4
            int16_t fet_raw = (int16_t)(((uint16_t)msg.data[0] << 8) | msg.data[1]);
            int16_t mot_raw = (int16_t)(((uint16_t)msg.data[2] << 8) | msg.data[3]);
            int16_t in_raw  = (int16_t)(((uint16_t)msg.data[4] << 8) | msg.data[5]);

            float t_fet = (float)fet_raw / 10.0f;
            float t_mot = (float)mot_raw / 10.0f;
            float bat_i = (float)in_raw / 10.0f;

            if (is_primary) {
                telemetry.temp_esc = t_fet;
                telemetry.temp_motor = t_mot;
                telemetry.temp_fet_rear = t_fet;
                telemetry.temp_mot_rear = t_mot;

                // Direct battery current and real-time physical power (P = V * I) from VESC
                telemetry.current_amps = bat_i;
                if (telemetry.voltage > 10.0f) {
                    telemetry.power_watts = telemetry.voltage * telemetry.current_amps;
                }
                telemetry.vesc_connected = true;
                s_last_can_rx_ms = now;
            } else if (is_secondary) {
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

            if (is_primary) {
                telemetry.voltage = v_in;
                if (telemetry.voltage > 10.0f) {
                    telemetry.power_watts = telemetry.voltage * telemetry.current_amps;
                }

                static bool s_can_tacho_init = false;
                static int32_t s_last_can_tacho = 0;
                if (!s_can_tacho_init) {
                    _tacho_trip_offset = tacho;
                    s_last_can_tacho = tacho;
                    s_can_tacho_init = true;
                }

                int32_t delta_tacho = tacho - s_last_can_tacho;
                if (delta_tacho != 0) {
                    updateWheelFactors();
                    float delta_km = fabsf((float)delta_tacho) * _tacho_to_km_factor;
                    if (delta_km < 1.0f) { // sanity clamp against sudden jumps
                        telemetry.odo_km += delta_km;
                        telemetry.trip_km += delta_km;
                    }
                    s_last_can_tacho = tacho;
                }
                telemetry.vesc_connected = true;
                s_last_can_rx_ms = now;
            }
        }
        else if (cmd_id == 14 && msg.data_length_code >= 8) { // CAN_PACKET_STATUS_2
            int32_t ah_raw = (int32_t)(((uint32_t)msg.data[0] << 24) |
                                       ((uint32_t)msg.data[1] << 16) |
                                       ((uint32_t)msg.data[2] << 8)  |
                                       (uint32_t)msg.data[3]);
            if (is_primary) {
                _last_ah = ah_raw;
                if (!_ah_trip_inited) {
                    _ah_trip_offset = ah_raw;
                    _ah_trip_inited = true;
                }
                int32_t rel_ah = ah_raw - _ah_trip_offset;
                if (rel_ah < 0) rel_ah = 0;
                telemetry.amphours_used = (float)rel_ah / 10000.0f;
            }
        }
        else if (cmd_id == 15 && msg.data_length_code >= 8) { // CAN_PACKET_STATUS_3
            int32_t wh_raw = (int32_t)(((uint32_t)msg.data[0] << 24) |
                                       ((uint32_t)msg.data[1] << 16) |
                                       ((uint32_t)msg.data[2] << 8)  |
                                       (uint32_t)msg.data[3]);
            if (is_primary) {
                _last_wh = wh_raw;
                if (!_wh_trip_inited) {
                    _wh_trip_offset = wh_raw;
                    _wh_trip_inited = true;
                }
                int32_t rel_wh = wh_raw - _wh_trip_offset;
                if (rel_wh < 0) rel_wh = 0;
                // Keep VESC-reported Wh separate so BatteryManager remains the single source of truth for telemetry.stats.trip_wh
                _vesc_trip_wh = (float)rel_wh / 10000.0f;
            }
        }
        else if (cmd_id == 65 && msg.data_length_code >= 4) { // Custom VESC IMU telemetry (Incline & Accel)
            int16_t roll_raw  = (int16_t)(((uint16_t)msg.data[0] << 8) | msg.data[1]);
            int16_t pitch_raw = (int16_t)(((uint16_t)msg.data[2] << 8) | msg.data[3]);
            (void)roll_raw;
            float pitch_deg = (float)pitch_raw / 10.0f;
            if (is_primary && fabsf(pitch_deg) <= 45.0f) {
                telemetry.incline_deg = pitch_deg;
            }
            if (is_primary && msg.data_length_code >= 8) {
                int16_t acc_raw = (int16_t)(((uint16_t)msg.data[6] << 8) | msg.data[7]);
                float g_imu = (float)acc_raw / 100.0f;
                if (fabsf(g_imu) <= 3.0f) {
                    telemetry.g_force = g_imu;
                    s_has_imu_g = true;
                }
            }
        }
    }

    // Check on-demand MCCONF pull timeout
    if (_sync_status == SYNC_REQUESTED && (uint32_t)(now - _mcconf_req_ms) > 2500) {
        _sync_status = SYNC_FAILED;
        _sync_ms = now;
        Serial.println("[VESC_CAN] COMM_GET_MCCONF request timed out.");
    }

    if (now - s_last_can_rx_ms > 1500) {
        telemetry.vesc_connected = false;
        telemetry.speed_kmh = 0.0f;
        telemetry.vesc_motor_speed = 0.0f;
        telemetry.throttle_pct = 0.0f;
        telemetry.current_amps = 0.0f;
        telemetry.phase_amps = 0.0f;
        telemetry.power_watts = 0.0f;
        telemetry.duty_cycle_pct = 0.0f;
        telemetry.rpm = 0.0f;
    }

    if (now - s_last_can_diag_ms >= 2000) {
        s_last_can_diag_ms = now;
        Serial.printf("[VESC_CAN] Link: %s | ID: %u | V_in: %.1fV | Spd: %.1f km/h | Thr: %.0f%% | Fault: %u | MotI: %.1fA | BatI: %.1fA | ESC: %.1fC | Mot: %.1fC\n",
                      telemetry.vesc_connected ? "CONNECTED" : "NO_DATA",
                      (unsigned)s_primary_vesc_id,
                      telemetry.voltage, telemetry.speed_kmh, telemetry.throttle_pct,
                      (unsigned)telemetry.fault_code,
                      telemetry.phase_amps, telemetry.current_amps,
                      telemetry.temp_esc, telemetry.temp_motor);
    }

    if (telemetry.is_locked || ModeManager::instance().isLocked()) {
        telemetry.speed_kmh = 0.0f;
        telemetry.current_amps = 0.0f;
        telemetry.phase_amps = 0.0f;
        telemetry.power_watts = 0.0f;
        telemetry.duty_cycle_pct = 0.0f;
    } else if (telemetry.speed_kmh < 0.5f && fabsf(telemetry.current_amps) < 0.15f && fabsf(telemetry.phase_amps) < 0.3f) {
        telemetry.power_watts = 0.0f;
        telemetry.current_amps = 0.0f;
        telemetry.phase_amps = 0.0f;
    }

    // Per-tick Physics, Battery and Peak Statistics Integration for CAN mode
    static uint32_t s_last_can_calc_ms = millis();
    float dt = (now - s_last_can_calc_ms) / 1000.0f;
    s_last_can_calc_ms = now;
    if (dt < 0.001f) dt = 0.016f;
    if (dt > 0.5f) dt = 0.05f;

    // 1. Dual ESC current / power combination (respect lock & idle 0W clamp)
    if (!telemetry.is_locked && !ModeManager::instance().isLocked() &&
        !(telemetry.speed_kmh < 0.5f && fabsf(telemetry.current_amps) < 0.15f && fabsf(telemetry.phase_amps) < 0.3f) &&
        s_sec_last_rx_ms > 0 && (now - s_sec_last_rx_ms < 1000)) {
        telemetry.power_watts = (telemetry.voltage * telemetry.current_amps) + telemetry.vesc2_max_power;
    }

    // 2. Battery State of Charge, Remaining Wh & Degradation Learning Filter
    Battery.update(telemetry, dt);

    // 3. Peak, G-Force & Trip Average Statistics
    updateDerivedStats(telemetry, dt, !s_has_imu_g);
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

    static uint32_t last_diag_ms = 0;

    if (!_transport) return;

    uint8_t read_chunk[64];
    while (_transport->available() > 0) {
        size_t avail = (size_t)_transport->available();
        size_t to_read = (avail > sizeof(read_chunk)) ? sizeof(read_chunk) : avail;
        size_t bytes_read = _transport->readBytes(read_chunk, to_read);
        if (bytes_read == 0) break;

        last_rx_byte_ms = now;

        for (size_t i = 0; i < bytes_read; ++i) {
            uint8_t b = read_chunk[i];

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
    }

    // Timeout reset
    if (rx_index > 0 && (now - last_rx_byte_ms > 150)) {
        rx_index = 0;
        telemetry.vesc_connected = false;
    }
    if (last_rx_byte_ms > 0 && (now - last_rx_byte_ms > 1500)) {
        telemetry.vesc_connected = false;
        telemetry.speed_kmh = 0.0f;
        telemetry.vesc_motor_speed = 0.0f;
        telemetry.current_amps = 0.0f;
        telemetry.phase_amps = 0.0f;
        telemetry.power_watts = 0.0f;
        telemetry.duty_cycle_pct = 0.0f;
        telemetry.rpm = 0.0f;
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
        updateDerivedStats(telemetry, dt, true);
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
        if (len < payload_len + 5) return false;
    } else if (buffer[0] == 0x03) {
        payload = &buffer[3];
        payload_len = ((size_t)buffer[1] << 8) | buffer[2];
        if (len < payload_len + 6) return false;
    } else {
        return false;
    }

    uint16_t expected_crc = ((uint16_t)buffer[len - 3] << 8) | buffer[len - 2];
    uint16_t calc_crc = crc16(payload, payload_len);
    if (expected_crc != calc_crc) {
        return false;
    }

    return parseVescPayload(payload, payload_len, telemetry);
}

bool VescHandler::parseVescPayload(const uint8_t *payload, size_t payload_len, DashTelemetry &telemetry) {
    if (!payload || payload_len < 1) return false;

    telemetry.vesc_connected = true;
    uint8_t cmd_id = payload[0];

    // 0. Selective Telemetry Packet (COMM_GET_VALUES_SELECTIVE = 0x32 / 50)
    // Used over CAN to poll fault_code (bit 15) cleanly in a single short frame
    if (cmd_id == 0x32) {
        if (payload_len < 6) return false;
        uint32_t mask = ((uint32_t)payload[1] << 24) |
                        ((uint32_t)payload[2] << 16) |
                        ((uint32_t)payload[3] << 8)  |
                        (uint32_t)payload[4];
        if (mask & (1U << 15)) {
            static const uint8_t bit_sizes[15] = {
                2, 2, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4, 4
            };
            size_t offset = 5;
            for (int b = 0; b < 15; b++) {
                if (mask & (1U << b)) offset += bit_sizes[b];
            }
            if (offset < payload_len) {
                uint8_t fault_code = payload[offset];
                telemetry.fault_code = fault_code;
                if (fault_code != 0) {
                    FaultManager::instance().recordFault(
                        fault_code, telemetry.voltage, telemetry.current_amps,
                        telemetry.phase_amps, telemetry.temp_esc, telemetry.temp_motor,
                        telemetry.uptime_sec);
                } else {
                    FaultManager::instance().setActiveFault(0);
                }
            }
        }
        return true;
    }

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

            // Motor brake lever processing via UART transport only (on CAN mode, VESC handles its own ADC brake)
            if (_transport != nullptr) {
                float brake_pct = (Settings.get().throttle_adc_channel == 0 && payload_len >= 13) ? _last_adc2_pct : _last_adc1_pct;
                if (brake_pct < 0.0f) brake_pct = 0.0f;
                if (brake_pct > 100.0f) brake_pct = 100.0f;

                if (brake_pct > 3.0f) {
                    if (Settings.get().brake_mode == 0) {
                        sendCurrentBrake(0.0f);
                    } else if (Settings.get().brake_mode == 1) {
                        sendCurrentBrake((brake_pct / 100.0f) * 12.0f);
                    } else {
                        float max_pha = Settings.get().max_phase_amps > 0 ? (float)Settings.get().max_phase_amps * 0.75f : 45.0f;
                        sendCurrentBrake((brake_pct / 100.0f) * max_pha);
                    }
                } else if (telemetry.throttle_pct <= 0.5f && Settings.get().brake_mode == 0) {
                    sendCurrentBrake(0.0f);
                }
            }
        }
        return true;
    }

    // 2. Setup Telemetry Packet with True Ground Speed (COMM_GET_VALUES_SETUP = 0x2F / 47)
    if (cmd_id == 0x2F) {
        if (payload_len < 45) return false;
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
        int32_t dist_tot = 0;
        if (payload_len >= 51) {
            dist_tot = (int32_t)(((uint32_t)payload[idx] << 24) | ((uint32_t)payload[idx + 1] << 16) | ((uint32_t)payload[idx + 2] << 8) | (uint32_t)payload[idx + 3]);
            idx += 4;
        }
        _last_dist = dist_m;

        telemetry.temp_esc       = temp_mos / 10.0f;
        telemetry.temp_motor     = temp_mot / 10.0f;
        telemetry.phase_amps     = fabsf(current_mot / 100.0f);
        telemetry.current_amps   = (float)current_in / 100.0f;
        telemetry.duty_cycle_pct = fabsf((float)duty_now / 10.0f);
        if (telemetry.duty_cycle_pct > 100.0f) telemetry.duty_cycle_pct = 100.0f;
        int pole_pairs = Settings.get().motor_pole_pairs;
        if (pole_pairs < 2) pole_pairs = 15;
        telemetry.rpm            = fabsf((float)erpm / (float)pole_pairs);
        telemetry.voltage        = (v_in / 10.0f) + Settings.get().voltage_trim_v;
        if (telemetry.voltage < 0.0f) telemetry.voltage = 0.0f;
        telemetry.power_watts    = telemetry.voltage * telemetry.current_amps;
        telemetry.amphours_used  = ah_used / 10000.0f;

        // Ground speed from VESC (injected by Port 4 LispBM wheel sensor override)
        float v_spd_setup = fabsf((speed_m_s / 1000.0f) * 3.6f);
        if (v_spd_setup < 0.8f) v_spd_setup = 0.0f;
        telemetry.vesc_motor_speed = v_spd_setup;
        telemetry.speed_kmh = v_spd_setup;

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
        if (payload_len < 54) return false;

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
        if (telemetry.duty_cycle_pct > 100.0f) telemetry.duty_cycle_pct = 100.0f;
        telemetry.voltage        = (v_in / 10.0f) + Settings.get().voltage_trim_v;
        if (telemetry.voltage < 0.0f) telemetry.voltage = 0.0f;
        telemetry.power_watts    = telemetry.voltage * telemetry.current_amps;
        telemetry.amphours_used  = ah_used / 10000.0f;

        // Convert ERPM to Vehicle Speed (km/h) using cached factors
        updateWheelFactors();
        float abs_erpm = fabsf((float)erpm);
        telemetry.rpm = abs_erpm * _inv_pole_pairs;
        float v_spd_std = abs_erpm * _erpm_to_kmh_factor;
        if (v_spd_std < 0.8f) v_spd_std = 0.0f;
        telemetry.vesc_motor_speed = v_spd_std;
        telemetry.speed_kmh = v_spd_std;

        // Odometer & Trip from tachometer step counts
        static int32_t s_last_uart_tacho = 0;
        static bool s_uart_tacho_init = false;
        if (!s_uart_tacho_init) {
            _tacho_trip_offset = tachometer;
            s_last_uart_tacho = tachometer;
            s_uart_tacho_init = true;
        }

        int32_t delta_uart_tacho = tachometer - s_last_uart_tacho;
        if (delta_uart_tacho != 0) {
            float delta_km = fabsf((float)delta_uart_tacho) * _tacho_to_km_factor;
            if (delta_km < 1.0f) { // sanity clamp against sudden jumps
                telemetry.trip_km += delta_km;
            }
            s_last_uart_tacho = tachometer;
        }

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
    if (_transport) {
        _transport->write(req, sizeof(req));
    }
#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    else if (s_can_initialized) {
        static const uint8_t req_can[1] = { 0x0E };
        sendVescCanBuffer(s_primary_vesc_id, req_can, sizeof(req_can), ESP_CAN_ID, 0);
    }
#endif
    _sync_status = SYNC_REQUESTED;
    _mcconf_req_ms = millis();
    Serial.println("[VESC] Sent COMM_GET_MCCONF request to VESC.");
}

#if !defined(LGFX_LINUX_FB) && !defined(SIMULATOR)
#include <WiFi.h>
static WiFiServer _wifi_server(6510);
static WiFiClient _wifi_client;
#endif

// ==============================================================================
// Transparent USB CDC <-> UART / CAN Bridge for VESC Tool
// ==============================================================================
void VescHandler::enterBridgeMode() {
    _bridge_active = true;
    _bridge_pc_to_vesc = 0;
    _bridge_vesc_to_pc = 0;

    // Ensure 4KB queues for high-volume configuration bursts (e.g. COMM_SET_MCCONF)
    Serial.setRxBufferSize(4096);
    Serial.setTxBufferSize(4096);
#if !defined(NATIVE_SIMULATOR) && !VESC_USE_CAN_BUS
    VESC_UART_PORT.setRxBufferSize(4096);
    VESC_UART_PORT.setTxBufferSize(4096);
#endif
#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    s_bridge_framer.reset();
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
#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    s_bridge_framer.reset();
#endif
    _last_poll_ms = millis();
}

void VescHandler::updateUsbBridge() {
    if (!_bridge_active) return;

    if (_transport) {
        // UART Bridge Path: Transparent byte-stream passthrough
        while (Serial.available() > 0) {
            uint8_t buf[128];
            size_t n = Serial.read(buf, sizeof(buf));
            if (n > 0) {
                _transport->write(buf, n);
                _bridge_pc_to_vesc += n;
            } else {
                break;
            }
        }

        while (_transport->available() > 0) {
            uint8_t buf[128];
            size_t n = _transport->readBytes(buf, sizeof(buf));
            if (n > 0) {
                Serial.write(buf, n);
                _bridge_vesc_to_pc += n;
            } else {
                break;
            }
        }
        return;
    }

#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    // CAN Bridge Path: Bidirectional VESC UART-frame <-> CAN RX buffer tunneling
    if (!s_can_initialized) return;
    ensureCanBusHealthy();
    uint32_t now = millis();

    // 1. Parse framed VESC packets from USB Serial -> transmit to VESC over CAN
    while (Serial.available() > 0) {
        uint8_t buf[256];
        size_t n = Serial.read(buf, sizeof(buf));
        if (n == 0) break;
        _bridge_pc_to_vesc += n;
        for (size_t i = 0; i < n; i++) {
            const uint8_t *payload = nullptr;
            size_t payload_len = 0;
            if (s_bridge_framer.feed(buf[i], now, &payload, &payload_len)) {
                dispatchBridgePayloadToCan(payload, payload_len);
            }
        }
    }

    // 2. Receive CAN buffer replies addressed to ESP_CAN_ID -> re-frame & write to USB Serial
    twai_message_t msg;
    int frames_read = 0;
    while (twai_receive(&msg, pdMS_TO_TICKS(0)) == ESP_OK && frames_read < 64) {
        frames_read++;
        if (!msg.extd) continue;
        uint8_t controller_id = (uint8_t)(msg.identifier & 0xFF);
        uint8_t cmd_id = (uint8_t)((msg.identifier >> 8) & 0xFF);
        if (controller_id == ESP_CAN_ID) {
            const uint8_t *rx_payload = nullptr;
            size_t rx_len = 0;
            if (processCanRxFrameForEsp(msg, &rx_payload, &rx_len)) {
                size_t framed_len = frameVescUartPacket(rx_payload, rx_len, s_bridge_tx_frame, sizeof(s_bridge_tx_frame));
                if (framed_len > 0) {
                    Serial.write(s_bridge_tx_frame, framed_len);
                    _bridge_vesc_to_pc += framed_len;
                }
            }
        } else {
            updateDetectedCanIds(controller_id, cmd_id, now);
        }
    }
#endif
}

// ==============================================================================
// Transparent Wireless Wi-Fi TCP <-> UART / CAN Bridge for VESC Tool
// ==============================================================================
void VescHandler::enterWifiBridgeMode() {
    _wifi_bridge_active = true;
    _wifi_client_connected = false;
    _wifi_client_ip[0] = '\0';
    _bridge_wifi_to_vesc = 0;
    _bridge_vesc_to_wifi = 0;

    // Ensure 4KB UART queues only when UART transport is in use
#if !defined(NATIVE_SIMULATOR) && !VESC_USE_CAN_BUS
    VESC_UART_PORT.setRxBufferSize(4096);
    VESC_UART_PORT.setTxBufferSize(4096);
#endif
#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    s_bridge_framer.reset();
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
    WiFi.setTxPower(WIFI_POWER_11dBm);
    _wifi_server.begin(6510);
    _wifi_server.setNoDelay(true);
    OtaManager::instance().begin();
    Serial.printf("[WIFI_BRIDGE] SoftAP started: SSID='VESC-DASH-AP', IP=192.168.4.1, Port=6510 (Mode: %s)\n",
                  _transport ? "UART" : "CAN_TUNNEL");
#endif
}

void VescHandler::exitWifiBridgeMode() {
    _wifi_bridge_active = false;
    _wifi_client_connected = false;
    _wifi_client_ip[0] = '\0';
#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    s_bridge_framer.reset();
#endif
    _last_poll_ms = millis();

#if !defined(LGFX_LINUX_FB) && !defined(SIMULATOR)
    OtaManager::instance().stop();
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

    // Handle OTA Web Server & ArduinoOTA
    OtaManager::instance().update();

    if (_wifi_server.hasClient()) {
        if (_wifi_client && _wifi_client.connected()) {
            WiFiClient rej = _wifi_server.accept();
            rej.stop();
        } else {
            _wifi_client = _wifi_server.accept();
            _wifi_client.setNoDelay(true);
            _wifi_client.setTimeout(5); // Non-blocking 5ms TX timeout prevents freezing Core 0
            _wifi_client_connected = true;
#if VESC_USE_CAN_BUS
            s_bridge_framer.reset();
#endif
            IPAddress ip = _wifi_client.remoteIP();
            snprintf(_wifi_client_ip, sizeof(_wifi_client_ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
            Serial.printf("[WIFI_BRIDGE] Client connected: %s\n", _wifi_client_ip);
        }
    }

    auto disconnectWifiClient = [&](const char *reason) {
        _wifi_client_connected = false;
        _wifi_client_ip[0] = '\0';
#if VESC_USE_CAN_BUS
        s_bridge_framer.reset();
#endif
        if (_wifi_client) {
            _wifi_client.stop();
        }
        Serial.printf("[WIFI_BRIDGE] %s\n", reason);
    };

    auto writeWifiNonBlocking = [&](const uint8_t *data, size_t len) -> bool {
        if (!_wifi_client || !_wifi_client.connected()) {
            disconnectWifiClient("Client disconnected before TX.");
            return false;
        }
        size_t written = _wifi_client.write(data, len);
        _bridge_vesc_to_wifi += (uint32_t)written;
        if (written != len || !_wifi_client.connected()) {
            disconnectWifiClient("TCP write backpressure/error -> dropped client to protect Core 0.");
            return false;
        }
        return true;
    };

    if (_wifi_client && _wifi_client.connected()) {
        _wifi_client_connected = true;

        if (_transport) {
            // UART Bridge Path
            int rx_chunks = 0;
            while (_wifi_client.available() > 0 && rx_chunks++ < 8) {
                uint8_t buf[128];
                int bytes_read = _wifi_client.read(buf, sizeof(buf));
                if (bytes_read > 0) {
                    _transport->write(buf, bytes_read);
                    _bridge_wifi_to_vesc += bytes_read;
                } else {
                    break;
                }
            }

            int tx_chunks = 0;
            while (_transport->available() > 0 && tx_chunks++ < 8) {
                uint8_t buf[128];
                size_t n = _transport->readBytes(buf, sizeof(buf));
                if (n > 0) {
                    if (!writeWifiNonBlocking(buf, n)) {
                        return;
                    }
                } else {
                    break;
                }
            }
        }
#if VESC_USE_CAN_BUS
        else if (s_can_initialized) {
            // CAN Bridge Path: Bidirectional VESC TCP-frame <-> CAN RX buffer tunneling
            ensureCanBusHealthy();
            uint32_t now = millis();

            // 1. Parse framed VESC packets from TCP client -> transmit over CAN to VESC (bounded per 5ms tick)
            int rx_chunks = 0;
            while (_wifi_client.available() > 0 && rx_chunks++ < 8) {
                uint8_t buf[256];
                int bytes_read = _wifi_client.read(buf, sizeof(buf));
                if (bytes_read <= 0) break;
                _bridge_wifi_to_vesc += (uint32_t)bytes_read;
                for (int i = 0; i < bytes_read; i++) {
                    const uint8_t *payload = nullptr;
                    size_t payload_len = 0;
                    if (s_bridge_framer.feed(buf[i], now, &payload, &payload_len)) {
                        dispatchBridgePayloadToCan(payload, payload_len);
                    }
                }
            }

            // 2. Receive CAN buffer replies addressed to ESP_CAN_ID -> re-frame & send over TCP
            twai_message_t msg;
            int frames_read = 0;
            while (twai_receive(&msg, pdMS_TO_TICKS(0)) == ESP_OK && frames_read < 64) {
                frames_read++;
                if (!msg.extd) continue;
                uint8_t controller_id = (uint8_t)(msg.identifier & 0xFF);
                uint8_t cmd_id = (uint8_t)((msg.identifier >> 8) & 0xFF);
                if (controller_id == ESP_CAN_ID) {
                    const uint8_t *rx_payload = nullptr;
                    size_t rx_len = 0;
                    if (processCanRxFrameForEsp(msg, &rx_payload, &rx_len)) {
                        size_t framed_len = frameVescUartPacket(rx_payload, rx_len, s_bridge_tx_frame, sizeof(s_bridge_tx_frame));
                        if (framed_len > 0) {
                            if (!writeWifiNonBlocking(s_bridge_tx_frame, framed_len)) {
                                return;
                            }
                        }
                    }
                } else {
                    updateDetectedCanIds(controller_id, cmd_id, now);
                }
            }
        }
#endif
    } else {
        if (_wifi_client_connected) {
            disconnectWifiClient("Client disconnected.");
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

void VescHandler::sendCurrent(float current_amps) {
    UartLock lock;
    if (current_amps < 0.0f) current_amps = 0.0f;
    // Safety clamp to 60.0A max for bench test
    if (current_amps > 60.0f) current_amps = 60.0f;

    int32_t current_mA = (int32_t)roundf(current_amps * 1000.0f);

    uint8_t payload[5];
    payload[0] = 6; // COMM_SET_CURRENT (ID 6)
    payload[1] = (uint8_t)(current_mA >> 24);
    payload[2] = (uint8_t)(current_mA >> 16);
    payload[3] = (uint8_t)(current_mA >> 8);
    payload[4] = (uint8_t)(current_mA & 0xFF);

    if (_transport) {
        uint8_t packet[11];
        packet[0] = 0x02;
        packet[1] = 5;
        memcpy(&packet[2], payload, 5);
        uint16_t crc = crc16(payload, 5);
        packet[7] = (uint8_t)(crc >> 8);
        packet[8] = (uint8_t)(crc & 0xFF);
        packet[9] = 0x03;
        _transport->write(packet, 10);
    }

#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    if (s_can_initialized) {
        // CAN_PACKET_SET_CURRENT (ID 1) -> Primary VESC (+ detected secondary only)
        twai_message_t can_cur = {};
        can_cur.data_length_code = 4;
        can_cur.data[0] = payload[1];
        can_cur.data[1] = payload[2];
        can_cur.data[2] = payload[3];
        can_cur.data[3] = payload[4];
        twaiTransmitToDetected(can_cur, 1, pdMS_TO_TICKS(5));
    }
#endif
}

void VescHandler::sendDuty(float duty) {
    UartLock lock;
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 0.90f) duty = 0.90f; // clamp to max 90% duty

    int32_t duty_int = (int32_t)roundf(duty * 100000.0f);

    uint8_t payload[5];
    payload[0] = 5; // COMM_SET_DUTY (ID 5)
    payload[1] = (uint8_t)(duty_int >> 24);
    payload[2] = (uint8_t)(duty_int >> 16);
    payload[3] = (uint8_t)(duty_int >> 8);
    payload[4] = (uint8_t)(duty_int & 0xFF);

    if (_transport) {
        uint8_t packet[11];
        packet[0] = 0x02;
        packet[1] = 5;
        memcpy(&packet[2], payload, 5);
        uint16_t crc = crc16(payload, 5);
        packet[7] = (uint8_t)(crc >> 8);
        packet[8] = (uint8_t)(crc & 0xFF);
        packet[9] = 0x03;
        _transport->write(packet, 10);
    }

#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    if (s_can_initialized) {
        // CAN_PACKET_SET_DUTY (ID 0) -> Primary VESC (+ detected secondary only)
        twai_message_t can_duty = {};
        can_duty.data_length_code = 4;
        can_duty.data[0] = payload[1];
        can_duty.data[1] = payload[2];
        can_duty.data[2] = payload[3];
        can_duty.data[3] = payload[4];
        twaiTransmitToDetected(can_duty, 0, pdMS_TO_TICKS(5));
    }
#endif
}

void VescHandler::sendCurrentBrake(float brake_amps) {
    UartLock lock;
    uint8_t brake_mode = Settings.get().brake_mode;
    if (brake_mode == 0) {
        // Freilauf / AUS: electronic brake current clamped to 0A (zero regen)
        brake_amps = 0.0f;
    } else if (brake_mode == 1) {
        // MILD: clamp to max 12.0A phase
        if (brake_amps > 12.0f) brake_amps = 12.0f;
    } else {
        // MAXIMAL: normal configured limit
        float max_pha = Settings.get().max_phase_amps > 0 ? (float)Settings.get().max_phase_amps : 120.0f;
        if (brake_amps > max_pha) brake_amps = max_pha;
    }
    if (brake_amps < 0.0f) brake_amps = 0.0f;

    int32_t current_mA = (int32_t)roundf(brake_amps * 1000.0f);

    uint8_t payload[5];
    payload[0] = 7; // COMM_SET_CURRENT_BRAKE
    payload[1] = (uint8_t)(current_mA >> 24);
    payload[2] = (uint8_t)(current_mA >> 16);
    payload[3] = (uint8_t)(current_mA >> 8);
    payload[4] = (uint8_t)(current_mA & 0xFF);

    if (_transport) {
        uint8_t packet[11];
        packet[0] = 0x02;
        packet[1] = 5;
        memcpy(&packet[2], payload, 5);
        uint16_t crc = crc16(payload, 5);
        packet[7] = (uint8_t)(crc >> 8);
        packet[8] = (uint8_t)(crc & 0xFF);
        packet[9] = 0x03;
        _transport->write(packet, 10);
    }

#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    if (s_can_initialized) {
        // CAN_PACKET_SET_CURRENT_BRAKE (ID 2) -> Primary VESC (+ detected secondary only)
        twai_message_t can_brake = {};
        can_brake.data_length_code = 4;
        can_brake.data[0] = payload[1];
        can_brake.data[1] = payload[2];
        can_brake.data[2] = payload[3];
        can_brake.data[3] = payload[4];
        twaiTransmitToDetected(can_brake, 2, pdMS_TO_TICKS(5));
    }
#endif
}

void VescHandler::sendTerminalCommand(const char *str) {
#if !SIMULATION_MODE || defined(NATIVE_SIMULATOR)
    size_t slen = strlen(str);
    if (slen > 68) slen = 68;
    uint8_t t_payload[70];
    t_payload[0] = 0x14; // COMM_TERMINAL_CMD
    memcpy(&t_payload[1], str, slen);

    if (_transport) {
        uint8_t t_packet[76];
        t_packet[0] = 0x02;
        t_packet[1] = (uint8_t)(slen + 1);
        memcpy(&t_packet[2], t_payload, slen + 1);
        uint16_t tcrc = crc16(t_payload, slen + 1);
        t_packet[3 + slen] = (uint8_t)(tcrc >> 8);
        t_packet[4 + slen] = (uint8_t)(tcrc & 0xFF);
        t_packet[5 + slen] = 0x03;
        _transport->write(t_packet, slen + 6);
    }

#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    if (s_can_initialized) {
        sendVescCanBufferToDetected(t_payload, slen + 1, ESP_CAN_ID, 2);
    }
#endif
#endif
}

static void sendLispCmd(const char *cmd) {
    if (!cmd) return;
    size_t slen = strlen(cmd);
    if (slen > 60) slen = 60;
    uint8_t payload[64];
    payload[0] = 138; // COMM_LISP_REPL_CMD (0x8A)
    memcpy(&payload[1], cmd, slen);

    if (s_lisp_transport) {
        uint8_t packet[70];
        packet[0] = 0x02;
        packet[1] = (uint8_t)(slen + 1);
        memcpy(&packet[2], payload, slen + 1);
        uint16_t tcrc = crc16(payload, slen + 1);
        packet[3 + slen] = (uint8_t)(tcrc >> 8);
        packet[4 + slen] = (uint8_t)(tcrc & 0xFF);
        packet[5 + slen] = 0x03;
        s_lisp_transport->write(packet, slen + 6);
    }

#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    if (s_can_initialized) {
        sendVescCanBufferToDetected(payload, slen + 1, VescHandler::ESP_CAN_ID, 2);
    }
#endif
}

void VescHandler::triggerFocBeep(float freq, float duration_s, float voltage) {
    UartLock lock;
    char lisp_cmd[64];
    snprintf(lisp_cmd, sizeof(lisp_cmd), "(foc-beep %.0f %.2f %.1f)", freq, duration_s, voltage);

#if !SIMULATION_MODE
    sendLispCmd(lisp_cmd);
#endif

    Serial.printf("[VESC_ALARM] FOC Motor Siren Beep: %.0f Hz, %.2f s, %.1f V -> '%s'\n",
                  freq, duration_s, voltage, lisp_cmd);
}

void VescHandler::playFocTone(uint8_t channel, float freq, float voltage) {
    UartLock lock;
    if (!Settings.get().turbine_sound_enabled && freq > 0.0f) {
        // When turbine sound is turned OFF in Settings, absolutely NO sound or tone is played (completely silent normal operation)
        return;
    }
    char lisp_cmd[64];
    if (freq <= 0.0f || voltage <= 0.0f) {
        snprintf(lisp_cmd, sizeof(lisp_cmd), "(foc-play-stop)");
    } else {
        snprintf(lisp_cmd, sizeof(lisp_cmd), "(foc-play-tone %u %.0f %.1f)", channel, freq, voltage);
    }

#if !SIMULATION_MODE
    sendLispCmd(lisp_cmd);
#endif
}

void VescHandler::stopFocTone(uint8_t channel) {
    (void)channel;
    sendFocPlayStop();
}

void VescHandler::sendFocPlayStop() {
    UartLock lock;
    _horn_active = false;
    char lisp_cmd[64];
    snprintf(lisp_cmd, sizeof(lisp_cmd), "(foc-play-stop)");

#if !SIMULATION_MODE || defined(NATIVE_SIMULATOR)
    sendLispCmd(lisp_cmd);
#endif
}

void VescHandler::triggerFocHorn(bool active) {
    if (!active) {
        if (_horn_active) {
            _horn_active = false;
            sendFocPlayStop();
            Serial.println("[VESC_HORN] FOC Horn RELEASED -> (foc-play-stop)");
        }
        return;
    }

    uint32_t now = millis();
    if (!_horn_active || (now - _last_horn_cmd_ms >= 140)) {
        _horn_active = true;
        _last_horn_cmd_ms = now;

        // Piercing shrill FOC horn (dual-alternating 2800 Hz / 3200 Hz tone at 5.8V amplitude)
        float freq = (_horn_cadence % 2 == 0) ? 2800.0f : 3200.0f;
        _horn_cadence++;

        char lisp_cmd[64];
        snprintf(lisp_cmd, sizeof(lisp_cmd), "(foc-beep %.0f 200 5.8)", freq);

#if !SIMULATION_MODE || defined(NATIVE_SIMULATOR)
        sendLispCmd(lisp_cmd);
#endif

        Serial.printf("[VESC_HORN] FOC Motor Horn ACTIVE: %.0f Hz, 200 ms, 5.8 V -> '%s'\n",
                      freq, lisp_cmd);
    }
}

void VescHandler::sendModeLimits(float max_speed_kmh, float max_power_watts, float max_battery_amps, float max_phase_amps, float cutoff_start, float cutoff_end) {
    UartLock lock;
    // 1. Calculate ERPM from speed limit
    int pole_pairs = Settings.get().motor_pole_pairs;
    if (pole_pairs < 2) pole_pairs = 15;
    float wheel_diam_in = Settings.get().wheel_diameter_in;
    if (wheel_diam_in < 5.0f) wheel_diam_in = 11.0f;
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
        // Speeds >= 100 km/h represent "Komplett Offen" (unlimited ERPM 100.000)
        if (max_speed_kmh > 0.0f && max_speed_kmh < 100.0f) {
            float wheel_rpm = (max_speed_kmh * 1000.0f / 60.0f) / wheel_circ_m;
            float mech_rpm = wheel_rpm * gear_ratio;
            max_erpm = mech_rpm * (float)pole_pairs;
            if (max_erpm < 500.0f) max_erpm = 500.0f;
        } else {
            max_erpm = 100000.0f; // Vollstaendig unbegrenzt (keine ERPM Drossel)
        }

        // Sanity clamp power and currents for active driving
        if (max_power_watts < 50.0f) max_power_watts = 50.0f;
        if (max_power_watts > 40000.0f) max_power_watts = 40000.0f;
        if (max_battery_amps < 2.0f) max_battery_amps = 2.0f;
        if (max_battery_amps > 200.0f) max_battery_amps = 200.0f;
        if (max_phase_amps < 5.0f) max_phase_amps = 5.0f;
        if (max_phase_amps > 300.0f) max_phase_amps = 300.0f;
    }

    Serial.printf("[VESC_CMD] Dynamic Limits -> Spd: %.1f km/h (ERPM: %.0f) | Pwr: %.0f W | Bat: %.1f A | Phase: %.1f A\n",
                  max_speed_kmh, max_erpm, max_power_watts, max_battery_amps, max_phase_amps);

#if !SIMULATION_MODE || defined(NATIVE_SIMULATOR)
    // 2. Official VESC COMM_SET_MCCONF_TEMP (Command ID 48 / 0x30)
    uint8_t payload[45];
    int32_t p_idx = 0;
    payload[p_idx++] = 48; // COMM_SET_MCCONF_TEMP
    payload[p_idx++] = 0;  // store = false
    payload[p_idx++] = 1;  // forward_can = true
    payload[p_idx++] = 0;  // ack = false
    payload[p_idx++] = 0;  // divide_by_controllers = false

    float cur_scale = is_locked ? 0.0f : 1.0f;
    float cur_min_scale = (is_locked || Settings.get().brake_mode == 0) ? 0.0f : (Settings.get().brake_mode == 1 ? 0.25f : 1.0f);
    buffer_append_float32_auto(payload, cur_min_scale, &p_idx); // l_current_min_scale
    buffer_append_float32_auto(payload, cur_scale, &p_idx);     // l_current_max_scale
    buffer_append_float32_auto(payload, is_locked ? 0.0f : -max_erpm, &p_idx);
    buffer_append_float32_auto(payload, is_locked ? 0.0f : max_erpm, &p_idx);
    buffer_append_float32_auto(payload, is_locked ? 0.0f : 0.005f, &p_idx); // l_min_duty
    buffer_append_float32_auto(payload, is_locked ? 0.0f : 0.95f, &p_idx);  // l_max_duty
    buffer_append_float32_auto(payload, is_locked ? 0.0f : -max_power_watts, &p_idx);
    buffer_append_float32_auto(payload, is_locked ? 0.0f : max_power_watts, &p_idx);
    buffer_append_float32_auto(payload, is_locked ? 0.0f : -max_battery_amps, &p_idx);
    buffer_append_float32_auto(payload, is_locked ? 0.0f : max_battery_amps, &p_idx);

    if (_transport) {
        // UART / Simulator Fallback Path
        uint8_t packet[55];
        packet[0] = 0x02;
        packet[1] = (uint8_t)p_idx;
        memcpy(&packet[2], payload, p_idx);
        uint16_t crc = crc16(payload, p_idx);
        packet[2 + p_idx] = (uint8_t)(crc >> 8);
        packet[3 + p_idx] = (uint8_t)(crc & 0xFF);
        packet[4 + p_idx] = 0x03;
        _transport->write(packet, p_idx + 5);

        // Also forward to secondary CAN ID 1 via UART COMM_FORWARD_CAN
        uint8_t can_payload[47];
        can_payload[0] = 0x22; // COMM_FORWARD_CAN
        can_payload[1] = 0x01; // CAN ID 1
        memcpy(&can_payload[2], payload, p_idx);
        uint8_t can_packet[55];
        can_packet[0] = 0x02;
        can_packet[1] = (uint8_t)(p_idx + 2);
        memcpy(&can_packet[2], can_payload, p_idx + 2);
        uint16_t can_crc = crc16(can_payload, p_idx + 2);
        can_packet[4 + p_idx] = (uint8_t)(can_crc >> 8);
        can_packet[5 + p_idx] = (uint8_t)(can_crc & 0xFF);
        can_packet[6 + p_idx] = 0x03;
        _transport->write(can_packet, p_idx + 7);

        // Terminal fallback commands ONLY on UART transport (never spam over CAN!)
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "conf_set l_current_max %.1f", max_phase_amps);
        sendTerminalCommand(cmd);
        snprintf(cmd, sizeof(cmd), "conf_set l_in_current_max %.1f", max_battery_amps);
        sendTerminalCommand(cmd);
        snprintf(cmd, sizeof(cmd), "conf_set l_watt_max %.1f", max_power_watts);
        sendTerminalCommand(cmd);
        snprintf(cmd, sizeof(cmd), "conf_set l_max_erpm %.0f", max_erpm);
        sendTerminalCommand(cmd);
        snprintf(cmd, sizeof(cmd), "conf_set si_wheel_diameter %.4f", wheel_diam_in * 0.0254f);
        sendTerminalCommand(cmd);

        if (is_locked || Settings.get().brake_mode == 0) {
            sendTerminalCommand("conf_set l_current_min 0.0");
            sendTerminalCommand("conf_set l_in_current_min 0.0");
            sendCurrentBrake(0.0f);
        } else if (Settings.get().brake_mode == 1) {
            sendTerminalCommand("conf_set l_current_min -12.0");
            sendTerminalCommand("conf_set l_in_current_min -8.0");
        } else {
            // brake_mode == 2 (MAX): properly restore full regen brake current limits
            snprintf(cmd, sizeof(cmd), "conf_set l_current_min %.1f", -max_phase_amps);
            sendTerminalCommand(cmd);
            snprintf(cmd, sizeof(cmd), "conf_set l_in_current_min %.1f", -max_battery_amps);
            sendTerminalCommand(cmd);
        }

        if (cutoff_start > 20.0f && cutoff_end > 20.0f) {
            snprintf(cmd, sizeof(cmd), "conf_set l_battery_cut_start %.1f", cutoff_start);
            sendTerminalCommand(cmd);
            snprintf(cmd, sizeof(cmd), "conf_set l_battery_cut_end %.1f", cutoff_end);
            sendTerminalCommand(cmd);
        }
    }

#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    if (s_can_initialized) {
        // Clean CAN Mode: Send ONLY COMM_SET_MCCONF_TEMP (48) + native CAN limit frames to detected VESC(s)
        // (Zero redundant conf_set terminal commands -> zero RAM config corruption, zero main-loop freezes!)
        sendVescCanBufferToDetected(payload, p_idx, ESP_CAN_ID, 2);

        // Native CAN Motor Phase Current Limits (CAN_PACKET_CONF_CURRENT_LIMITS ID 21)
        twai_message_t can_lim = {};
        can_lim.data_length_code = 8;
        int32_t min_mot = (is_locked || Settings.get().brake_mode == 0) ? 0 :
                          ((Settings.get().brake_mode == 1) ? (int32_t)(-12.0f * 1000.0f) : (int32_t)(-max_phase_amps * 1000.0f));
        int32_t max_mot = (int32_t)(max_phase_amps * 1000.0f);
        can_lim.data[0] = (uint8_t)((min_mot >> 24) & 0xFF);
        can_lim.data[1] = (uint8_t)((min_mot >> 16) & 0xFF);
        can_lim.data[2] = (uint8_t)((min_mot >> 8) & 0xFF);
        can_lim.data[3] = (uint8_t)(min_mot & 0xFF);
        can_lim.data[4] = (uint8_t)((max_mot >> 24) & 0xFF);
        can_lim.data[5] = (uint8_t)((max_mot >> 16) & 0xFF);
        can_lim.data[6] = (uint8_t)((max_mot >> 8) & 0xFF);
        can_lim.data[7] = (uint8_t)(max_mot & 0xFF);
        twaiTransmitToDetected(can_lim, 21, pdMS_TO_TICKS(10));

        // Native CAN Battery Current Limits (CAN_PACKET_CONF_CURRENT_LIMITS_IN ID 23)
        twai_message_t can_lim_in = {};
        can_lim_in.data_length_code = 8;
        int32_t min_bat = (is_locked || Settings.get().brake_mode == 0) ? 0 :
                          ((Settings.get().brake_mode == 1) ? (int32_t)(-8.0f * 1000.0f) : (int32_t)(-max_battery_amps * 1000.0f));
        int32_t max_bat = (int32_t)(max_battery_amps * 1000.0f);
        can_lim_in.data[0] = (uint8_t)((min_bat >> 24) & 0xFF);
        can_lim_in.data[1] = (uint8_t)((min_bat >> 16) & 0xFF);
        can_lim_in.data[2] = (uint8_t)((min_bat >> 8) & 0xFF);
        can_lim_in.data[3] = (uint8_t)(min_bat & 0xFF);
        can_lim_in.data[4] = (uint8_t)((max_bat >> 24) & 0xFF);
        can_lim_in.data[5] = (uint8_t)((max_bat >> 16) & 0xFF);
        can_lim_in.data[6] = (uint8_t)((max_bat >> 8) & 0xFF);
        can_lim_in.data[7] = (uint8_t)(max_bat & 0xFF);
        twaiTransmitToDetected(can_lim_in, 23, pdMS_TO_TICKS(10));

        // Native CAN Battery Cutoff Voltages (CAN_PACKET_CONF_BATTERY_CUT ID 29)
        if (cutoff_start > 20.0f && cutoff_end > 20.0f) {
            twai_message_t can_cut = {};
            can_cut.data_length_code = 8;
            int32_t cut_s = (int32_t)(cutoff_start * 1000.0f);
            int32_t cut_e = (int32_t)(cutoff_end * 1000.0f);
            can_cut.data[0] = (uint8_t)((cut_s >> 24) & 0xFF);
            can_cut.data[1] = (uint8_t)((cut_s >> 16) & 0xFF);
            can_cut.data[2] = (uint8_t)((cut_s >> 8) & 0xFF);
            can_cut.data[3] = (uint8_t)(cut_s & 0xFF);
            can_cut.data[4] = (uint8_t)((cut_e >> 24) & 0xFF);
            can_cut.data[5] = (uint8_t)((cut_e >> 16) & 0xFF);
            can_cut.data[6] = (uint8_t)((cut_e >> 8) & 0xFF);
            can_cut.data[7] = (uint8_t)(cut_e & 0xFF);
            twaiTransmitToDetected(can_cut, 29, pdMS_TO_TICKS(10));
        }
    }
#endif
#endif
}

void VescHandler::sendTcsPhaseLimit(float max_phase_amps) {
    UartLock lock;
    // Safety invariant: NEVER raise the 0A lock limit when vehicle is locked!
    if (ModeManager::instance().isLocked()) {
        return;
    }
    if (max_phase_amps < 2.0f) max_phase_amps = 2.0f;
    if (max_phase_amps > 300.0f) max_phase_amps = 300.0f;

#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    if (s_can_initialized) {
        twai_message_t can_lim = {};
        can_lim.data_length_code = 8;
        float full_phase = ModeManager::instance().getActiveLimits().max_phase_amps;
        if (full_phase < 20.0f) full_phase = 20.0f;
        int32_t min_mot = (Settings.get().brake_mode == 0) ? 0 :
                          ((Settings.get().brake_mode == 1) ? (int32_t)(-12.0f * 1000.0f) : (int32_t)(-full_phase * 1000.0f));
        int32_t max_mot = (int32_t)(max_phase_amps * 1000.0f);
        can_lim.data[0] = (uint8_t)((min_mot >> 24) & 0xFF);
        can_lim.data[1] = (uint8_t)((min_mot >> 16) & 0xFF);
        can_lim.data[2] = (uint8_t)((min_mot >> 8) & 0xFF);
        can_lim.data[3] = (uint8_t)(min_mot & 0xFF);
        can_lim.data[4] = (uint8_t)((max_mot >> 24) & 0xFF);
        can_lim.data[5] = (uint8_t)((max_mot >> 16) & 0xFF);
        can_lim.data[6] = (uint8_t)((max_mot >> 8) & 0xFF);
        can_lim.data[7] = (uint8_t)(max_mot & 0xFF);
        twaiTransmitToDetected(can_lim, 21, pdMS_TO_TICKS(5));
    }
#endif

    if (_transport) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "conf_set l_current_max %.1f", max_phase_amps);
        sendTerminalCommand(cmd);
    }
}

void VescHandler::syncWheelDiameterToVesc(float wheel_diam_in) {
    UartLock lock;
    if (wheel_diam_in < 5.0f || wheel_diam_in > 30.0f) return;
    _wheel_diameter_mm = wheel_diam_in * 25.4f;

    float diam_m = wheel_diam_in * 0.0254f;

    // 1. VESC Terminal Command: conf_set si_wheel_diameter <meters>
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "conf_set si_wheel_diameter %.4f", diam_m);
    sendTerminalCommand(cmd);

    // 2. VESC LispBM Command: (conf-set 'si-wheel-diameter <meters>)
    char lisp_cmd[64];
    snprintf(lisp_cmd, sizeof(lisp_cmd), "(conf-set 'si-wheel-diameter %.4f)", diam_m);
    sendLispCmd(lisp_cmd);

#if !defined(NATIVE_SIMULATOR)
    Serial.printf("[VESC] Synchronized calibrated wheel diameter to VESC: %.1f in (%.4f m)\n",
                  wheel_diam_in, diam_m);
#else
    printf("[VESC] Synchronized calibrated wheel diameter to VESC: %.1f in (%.4f m)\n",
           wheel_diam_in, diam_m);
#endif
}

void VescHandler::sendGnssData(const GpsTelemetry &gps) {
#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
    if (!s_can_initialized || !gps.valid) return;

    // 1. CAN_PACKET_GNSS_TIME (ID 59)
    // ms_today (int32), yy (int16), mo (uint8), dd (uint8)
    twai_message_t msg_time = {};
    msg_time.extd = 1;
    msg_time.identifier = (59U << 8) | s_primary_vesc_id;
    msg_time.data_length_code = 8;
    int32_t ms_today = ((int32_t)gps.hour * 3600 + (int32_t)gps.minute * 60 + (int32_t)gps.second) * 1000;
    msg_time.data[0] = (uint8_t)((ms_today >> 24) & 0xFF);
    msg_time.data[1] = (uint8_t)((ms_today >> 16) & 0xFF);
    msg_time.data[2] = (uint8_t)((ms_today >> 8) & 0xFF);
    msg_time.data[3] = (uint8_t)(ms_today & 0xFF);
    int16_t yy = (int16_t)gps.year;
    msg_time.data[4] = (uint8_t)((yy >> 8) & 0xFF);
    msg_time.data[5] = (uint8_t)(yy & 0xFF);
    msg_time.data[6] = gps.month;
    msg_time.data[7] = gps.day;
    twai_transmit(&msg_time, pdMS_TO_TICKS(10));

    // 2. CAN_PACKET_GNSS_LAT (ID 60)
    // int64_t lat scaled by 1e16
    twai_message_t msg_lat = {};
    msg_lat.extd = 1;
    msg_lat.identifier = (60U << 8) | s_primary_vesc_id;
    msg_lat.data_length_code = 8;
    int64_t lat_scaled = (int64_t)(gps.latitude * 1e16);
    for (int i = 0; i < 8; i++) {
        msg_lat.data[i] = (uint8_t)((lat_scaled >> (56 - (i * 8))) & 0xFF);
    }
    twai_transmit(&msg_lat, pdMS_TO_TICKS(10));

    // 3. CAN_PACKET_GNSS_LON (ID 61)
    // int64_t lon scaled by 1e16
    twai_message_t msg_lon = {};
    msg_lon.extd = 1;
    msg_lon.identifier = (61U << 8) | s_primary_vesc_id;
    msg_lon.data_length_code = 8;
    int64_t lon_scaled = (int64_t)(gps.longitude * 1e16);
    for (int i = 0; i < 8; i++) {
        msg_lon.data[i] = (uint8_t)((lon_scaled >> (56 - (i * 8))) & 0xFF);
    }
    twai_transmit(&msg_lon, pdMS_TO_TICKS(10));

    // 4. CAN_PACKET_GNSS_ALT_SPEED_HDOP (ID 62)
    // height (float32_auto), speed (int16 * 100 in m/s), hdop (int16 * 100)
    twai_message_t msg_dyn = {};
    msg_dyn.extd = 1;
    msg_dyn.identifier = (62U << 8) | s_primary_vesc_id;
    msg_dyn.data_length_code = 8;

    int32_t alt_idx = 0;
    buffer_append_float32_auto(msg_dyn.data, gps.altitude_m, &alt_idx);

    // speed in m/s (1 km/h = 1.0 / 3.6 m/s) * 100.0
    float speed_ms = (gps.speed_kmh / 3.6f) * 100.0f;
    int16_t spd_raw = (int16_t)speed_ms;
    msg_dyn.data[4] = (uint8_t)((spd_raw >> 8) & 0xFF);
    msg_dyn.data[5] = (uint8_t)(spd_raw & 0xFF);

    // hdop * 100.0
    int16_t hdop_raw = (int16_t)(gps.hdop * 100.0f);
    msg_dyn.data[6] = (uint8_t)((hdop_raw >> 8) & 0xFF);
    msg_dyn.data[7] = (uint8_t)(hdop_raw & 0xFF);

    twai_transmit(&msg_dyn, pdMS_TO_TICKS(10));
#endif
}



