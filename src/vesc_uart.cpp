#include "vesc_internal.h"

#if !defined(NATIVE_SIMULATOR)
SemaphoreHandle_t s_uartMutex = NULL;
#endif

VescHandler Vesc;
static IVescTransport *s_lisp_transport = nullptr;


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
        bool has_dist_tot = (payload_len >= 51);
        if (has_dist_tot) {
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

        // Odometer: adopt higher VESC dist_tot (e.g. fresh display), never overwrite persisted odo_km with a lower/zero value
        if (has_dist_tot) {
            float vesc_odo_km = fabsf(((float)dist_tot / 1000.0f) / 1000.0f);
            if (vesc_odo_km > telemetry.odo_km && vesc_odo_km < 500000.0f) {
                telemetry.odo_km = vesc_odo_km;
            }
        }
        static int32_t s_last_setup_dist = 0;
        static bool s_setup_dist_init = false;
        if (!s_setup_dist_init) {
            s_last_setup_dist = dist_m;
            s_setup_dist_init = true;
        }
        int32_t delta_dist = dist_m - s_last_setup_dist;
        if (delta_dist != 0) {
            float delta_km = fabsf((float)delta_dist / 1000000.0f);
            if (delta_km < 1.0f) { // sanity clamp against sudden jumps
                telemetry.odo_km += delta_km;
            }
            s_last_setup_dist = dist_m;
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

