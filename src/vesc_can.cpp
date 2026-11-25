#include "vesc_internal.h"


// ==============================================================================
// Live Spintend Ubox / VESC CAN Bus Communication (ESP32-S3 TWAI)
// ==============================================================================
#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
bool s_can_initialized = false;
uint8_t s_primary_vesc_id = VESC_CAN_ID_DEFAULT;
uint8_t s_secondary_vesc_id = 0xFF;
static bool s_has_primary = false;
static bool s_has_76 = false;
bool s_has_secondary = false;
static uint32_t s_sec_last_rx_ms = 0;

static uint8_t s_can_rx_buf[CAN_RX_BUF_SIZE];

void ensureCanBusHealthy(bool force) {
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

void updateDetectedCanIds(uint8_t controller_id, uint8_t cmd_id, uint32_t now) {
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

void twaiTransmitToDetected(twai_message_t &msg, uint8_t cmd_id, TickType_t timeout_ticks) {
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
void sendVescCanBuffer(uint8_t controller_id, const uint8_t *data, size_t len,
                       uint8_t src_id, uint8_t send_flag) {
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

void sendVescCanBufferToDetected(const uint8_t *data, size_t len,
                                 uint8_t src_id, uint8_t send_flag) {
    sendVescCanBuffer(s_primary_vesc_id, data, len, src_id, send_flag);
    if (s_has_secondary && s_secondary_vesc_id != s_primary_vesc_id) {
        sendVescCanBuffer(s_secondary_vesc_id, data, len, src_id, send_flag);
    }
}

// Reassemble VESC CAN buffer replies addressed to ESP_CAN_ID (cmd 5, 6, 7, 8)
bool processCanRxFrameForEsp(const twai_message_t &msg, const uint8_t **out_payload, size_t *out_len) {
    if (msg.data_length_code > 8) {
        return false;
    }
    static uint32_t s_last_rx_frame_ms = 0;
    static bool     s_in_progress = false;

    uint32_t now_ms = millis();
    uint8_t cmd_id = (uint8_t)((msg.identifier >> 8) & 0xFF);

    // Timeout: if no subsequent frame arrived for >500ms, abort incomplete reassembly
    if (s_in_progress && (now_ms - s_last_rx_frame_ms > 500)) {
        s_in_progress = false;
    }

    if (cmd_id == 5 && msg.data_length_code >= 2) {
        // CAN_PACKET_FILL_RX_BUFFER
        size_t offset = msg.data[0];
        if (offset == 0) {
            s_in_progress = true;
        } else if (!s_in_progress) {
            return false; // Orphaned fragment without start frame
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
        } else if (!s_in_progress) {
            return false; // Orphaned fragment without start frame
        }

        s_last_rx_frame_ms = now_ms;
        size_t chunk_len = (size_t)msg.data_length_code - 2;
        if (offset + chunk_len <= CAN_RX_BUF_SIZE) {
            memcpy(s_can_rx_buf + offset, &msg.data[2], chunk_len);
        }
        return false;
    } else if (cmd_id == 7 && msg.data_length_code >= 6) {
        // CAN_PACKET_PROCESS_RX_BUFFER: data[0] is sender controller_id
        if (!s_in_progress) {
            return false; // Not matching current reassembly session
        }

        s_in_progress = false;

        uint16_t rx_len = ((uint16_t)msg.data[2] << 8) | msg.data[3];
        uint16_t rx_crc = ((uint16_t)msg.data[4] << 8) | msg.data[5];
        if (rx_len > 0 && rx_len <= CAN_RX_BUF_SIZE && crc16(s_can_rx_buf, rx_len) == rx_crc) {
            *out_payload = s_can_rx_buf;
            *out_len = rx_len;
            return true;
        }
        return false;
    } else if (cmd_id == 8 && msg.data_length_code >= 3) {
        // CAN_PACKET_PROCESS_SHORT_BUFFER: data[0] is sender controller_id
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
#else
    (void)gps;
#endif
}



