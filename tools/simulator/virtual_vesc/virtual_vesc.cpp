#include "virtual_vesc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

VirtualVesc::VirtualVesc()
    : _transport(nullptr),
      _ext_throttle_pct(0.0f),
      _ext_brake_pct(0.0f),
      _ext_target_current_a(0.0f),
      _has_ext_ctrl(false),
      _has_ext_current_ctrl(false),
      _last_ext_ctrl_time(std::chrono::steady_clock::now()),
      _packets_received(0),
      _packets_sent(0),
      _last_cmd_id(0) {
    memset(_cmd_counters, 0, sizeof(_cmd_counters));
}

void VirtualVesc::begin() {
    _parser.reset();
}

void VirtualVesc::reset() {
    _parser.reset();
    _packets_received = 0;
    _packets_sent = 0;
    _last_cmd_id = 0;
    memset(_cmd_counters, 0, sizeof(_cmd_counters));
    _limits = VirtualVescLimits();
    _can1_limits = VirtualVescLimits();
    _telem = VirtualVescTelemetry();
    _last_terminal_cmd.clear();
    _ext_throttle_pct = 0.0f;
    _ext_brake_pct = 0.0f;
    _ext_target_current_a = 0.0f;
    _has_ext_ctrl = false;
    _has_ext_current_ctrl = false;
    _last_ext_ctrl_time = std::chrono::steady_clock::now();
}

void VirtualVesc::setPhysicsTelemetry(const VirtualVescTelemetry &telem) {
    _telem = telem;
}

const VirtualVescLimits& VirtualVesc::getCanLimits(uint8_t can_id) const {
    if (can_id == 1) return _can1_limits;
    return _limits;
}

uint32_t VirtualVesc::getCommandCount(uint8_t cmd_id) const {
    return _cmd_counters[cmd_id];
}

void VirtualVesc::process() {
    if (!_transport) return;

    uint8_t buf[256];
    while (_transport->available() > 0) {
        size_t n = _transport->readBytes(buf, sizeof(buf));
        if (n == 0) break;

        _parser.processBuffer(buf, n, [this](const uint8_t *payload, size_t len) {
            this->handlePacket(payload, len);
        });
    }
}

void VirtualVesc::sendResponse(const uint8_t *payload, size_t len) {
    if (!payload || len == 0) return;

    uint8_t packet[1024];
    size_t enc_len = vesc_encode_packet(payload, len, packet, sizeof(packet));
    if (enc_len > 0) {
        if (_current_resp_cb) {
            _current_resp_cb(packet, enc_len);
        } else if (_transport) {
            _transport->write(packet, enc_len);
        }
        _packets_sent++;
    }
}

void VirtualVesc::handlePacket(const uint8_t *payload, size_t len, const ResponseCallback &resp_cb) {
    if (!payload || len == 0) return;

    _current_resp_cb = resp_cb;
    _packets_received++;
    uint8_t cmd_id = payload[0];
    _last_cmd_id = cmd_id;
    _cmd_counters[cmd_id]++;

    switch (cmd_id) {
        case COMM_FW_VERSION:
            handleFwVersion();
            break;

        case COMM_GET_VALUES:
        case COMM_GET_VALUES_SELECTIVE:
            handleGetValues(cmd_id, payload, len);
            break;

        case COMM_GET_VALUES_SETUP:
        case COMM_GET_VALUES_SETUP_SELECTIVE:
            handleGetValuesSetup(cmd_id, payload, len);
            break;

        case COMM_GET_DECODED_ADC:
            handleGetDecodedAdc();
            break;

        case COMM_GET_DECODED_PPM:
            handleGetDecodedPpm();
            break;

        case COMM_GET_DECODED_CHUK:
            handleGetDecodedChuk();
            break;

        case COMM_GET_MCCONF:
            handleGetMcconf();
            break;

        case COMM_SET_MCCONF:
            handleSetMcconf(payload, len);
            break;

        case COMM_GET_APPCONF:
        case COMM_GET_APPCONF_DEFAULT:
            handleGetAppconf(cmd_id);
            break;

        case COMM_SET_APPCONF: {
            uint8_t ack = COMM_SET_APPCONF;
            sendResponse(&ack, 1);
            break;
        }

        case COMM_SET_MCCONF_TEMP:
        case COMM_SET_MCCONF_TEMP_SETUP:
            handleSetMcconfTemp(payload, len, _limits);
            break;

        case COMM_SET_DUTY:
            handleSetDuty(payload, len);
            break;

        case COMM_SET_CURRENT:
            handleSetCurrent(payload, len);
            break;

        case COMM_SET_CURRENT_BRAKE:
            handleSetCurrentBrake(payload, len);
            break;

        case COMM_SET_RPM:
            handleSetRpm(payload, len);
            break;

        case COMM_SET_HANDBRAKE:
            handleSetHandbrake(payload, len);
            break;

        case COMM_FORWARD_CAN:
            handleForwardCan(payload, len);
            break;

        case COMM_TERMINAL_CMD:
            handleTerminalCmd(payload, len);
            break;

        case COMM_GET_STATS:
            handleGetStats(payload, len);
            break;

        case COMM_ALIVE:
            if (_has_ext_ctrl) {
                _last_ext_ctrl_time = std::chrono::steady_clock::now();
            }
            break;

        default:
            // Unknown or unhandled commands: gracefully ignored without corruption
            break;
    }
    _current_resp_cb = nullptr;
}

void VirtualVesc::handleFwVersion() {
    uint8_t buf[64];
    int32_t idx = 0;

    buf[idx++] = COMM_FW_VERSION;
    buf[idx++] = 6;  // fw_major
    buf[idx++] = 6;  // fw_minor (6.06 to match modern VESC Tool 7.00 without limited mode)

    const char *hw_name = "75_300";
    size_t hw_len = strlen(hw_name) + 1; // null-terminated
    memcpy(&buf[idx], hw_name, hw_len);
    idx += (int32_t)hw_len;

    // Standard Virtual VESC UUID (12 bytes)
    static const uint8_t uuid[12] = {
        'V', 'I', 'R', 'T', 'U', 'A', 'L', 'V', 'E', 'S', 'C', '1'
    };
    memcpy(&buf[idx], uuid, 12);
    idx += 12;

    buf[idx++] = 0; // pairing_done
    buf[idx++] = 0; // is_test_fw
    buf[idx++] = 0; // hw_type (HW_TYPE_VESC)
    buf[idx++] = 0; // custom_config
    buf[idx++] = 0; // phase_filters
    buf[idx++] = 0; // qmlui_hw
    buf[idx++] = 0; // qmlui_app

    sendResponse(buf, idx);
}

void VirtualVesc::handleGetValuesSetup(uint8_t cmd_id, const uint8_t *payload, size_t len) {
    uint8_t buf[128];
    int32_t idx = 0;

    buf[idx++] = cmd_id;

    uint32_t mask = 0xFFFFFFFF;
    if (cmd_id == COMM_GET_VALUES_SETUP_SELECTIVE) {
        if (len >= 5) {
            int32_t ind2 = 1;
            mask = buffer_get_uint32(payload, &ind2);
        }
        buffer_append_uint32(buf, mask, &idx);
    }

    int16_t temp_mos = (int16_t)roundf(_telem.temp_mos * 10.0f);
    int16_t temp_mot = (int16_t)roundf(_telem.temp_mot * 10.0f);
    int32_t current_mot = (int32_t)roundf(_telem.current_mot * 100.0f);
    int32_t current_in  = (int32_t)roundf(_telem.current_in * 100.0f);
    int16_t duty_now    = (int16_t)roundf(_telem.duty_cycle_pct * 10.0f); // 0.1% -> 1e3 float

    int pole_pairs = (_telem.pole_pairs < 2) ? 15 : _telem.pole_pairs;
    int32_t erpm = (int32_t)roundf(_telem.rpm * (float)pole_pairs);

    // Ground speed in mm/s (vesc_uart: speed_kmh = (speed_m_s / 1000.0f) * 3.6f)
    int32_t speed_m_s = (int32_t)roundf((_telem.speed_kmh / 3.6f) * 1000.0f);
    int16_t v_in = (int16_t)roundf(_telem.voltage * 10.0f);
    int16_t batt_level = (int16_t)roundf(_telem.battery_pct * 10.0f); // 0..1000

    int32_t ah_used = (int32_t)roundf(_telem.amphours_used * 10000.0f);
    int32_t ah_charge = (int32_t)roundf(_telem.amphours_charged * 10000.0f);
    int32_t wh_used = (int32_t)roundf(_telem.watthours_used * 10000.0f);
    int32_t wh_charge = (int32_t)roundf(_telem.watthours_charged * 10000.0f);

    // Trip distance in mm (vesc_uart: trip_km = (dist_m / 1000.0f) / 1000.0f)
    int32_t dist_m = (int32_t)roundf(_telem.trip_km * 1000000.0f);
    int32_t dist_tot = (int32_t)roundf(_telem.odo_km * 1000000.0f);

    if (mask & ((uint32_t)1 << 0)) buffer_append_int16(buf, temp_mos, &idx);     // idx 1..2 (temp_mos)
    if (mask & ((uint32_t)1 << 1)) buffer_append_int16(buf, temp_mot, &idx);     // idx 3..4 (temp_mot)
    if (mask & ((uint32_t)1 << 2)) buffer_append_int32(buf, current_mot, &idx);  // idx 5..8 (current_motor)
    if (mask & ((uint32_t)1 << 3)) buffer_append_int32(buf, current_in, &idx);   // idx 9..12 (current_in)
    if (mask & ((uint32_t)1 << 4)) buffer_append_int16(buf, duty_now, &idx);     // idx 13..14 (duty_now)
    if (mask & ((uint32_t)1 << 5)) buffer_append_int32(buf, erpm, &idx);         // idx 15..18 (rpm)
    if (mask & ((uint32_t)1 << 6)) buffer_append_int32(buf, speed_m_s, &idx);    // idx 19..22 (speed)
    if (mask & ((uint32_t)1 << 7)) buffer_append_int16(buf, v_in, &idx);         // idx 23..24 (v_in)
    if (mask & ((uint32_t)1 << 8)) buffer_append_int16(buf, batt_level, &idx);   // idx 25..26 (battery_level)
    if (mask & ((uint32_t)1 << 9)) buffer_append_int32(buf, ah_used, &idx);      // idx 27..30 (amp_hours)
    if (mask & ((uint32_t)1 << 10)) buffer_append_int32(buf, ah_charge, &idx);   // idx 31..34 (amp_hours_charged)
    if (mask & ((uint32_t)1 << 11)) buffer_append_int32(buf, wh_used, &idx);     // idx 35..38 (watt_hours)
    if (mask & ((uint32_t)1 << 12)) buffer_append_int32(buf, wh_charge, &idx);   // idx 39..42 (watt_hours_charged)
    if (mask & ((uint32_t)1 << 13)) buffer_append_int32(buf, dist_m, &idx);      // idx 43..46 (tachometer/distance)
    if (mask & ((uint32_t)1 << 14)) buffer_append_int32(buf, dist_tot, &idx);    // idx 47..50 (tachometer_abs/distance_abs)
    if (mask & ((uint32_t)1 << 15)) buffer_append_int32(buf, 0, &idx);           // idx 51..54 (position)
    if (mask & ((uint32_t)1 << 16)) buf[idx++] = _telem.fault_code;              // idx 55 (fault code)
    if (mask & ((uint32_t)1 << 17)) buf[idx++] = 0;                              // idx 56 (controller_id)
    if (mask & ((uint32_t)1 << 18)) buf[idx++] = 1;                              // idx 57 (num_vescs)
    if (mask & ((uint32_t)1 << 19)) buffer_append_int32(buf, 0, &idx);           // idx 58..61 (wh_batt_left)
    if (mask & ((uint32_t)1 << 20)) buffer_append_uint32(buf, (uint32_t)dist_tot, &idx); // idx 62..65 (odometer)
    if (mask & ((uint32_t)1 << 21)) buffer_append_uint32(buf, 0, &idx);          // system_time_ms

    sendResponse(buf, idx);
}

void VirtualVesc::handleGetValues(uint8_t cmd_id, const uint8_t *payload, size_t len) {
    uint8_t buf[128];
    int32_t idx = 0;

    buf[idx++] = cmd_id;

    uint32_t mask = 0xFFFFFFFF;
    if (cmd_id == COMM_GET_VALUES_SELECTIVE) {
        if (len >= 5) {
            int32_t ind2 = 1;
            mask = buffer_get_uint32(payload, &ind2);
        }
        buffer_append_uint32(buf, mask, &idx);
    }

    int16_t temp_mos = (int16_t)roundf(_telem.temp_mos * 10.0f);
    int16_t temp_mot = (int16_t)roundf(_telem.temp_mot * 10.0f);
    int32_t current_mot = (int32_t)roundf(_telem.current_mot * 100.0f);
    int32_t current_in  = (int32_t)roundf(_telem.current_in * 100.0f);
    int16_t duty_now    = (int16_t)roundf(_telem.duty_cycle_pct * 10.0f);

    int pole_pairs = (_telem.pole_pairs < 2) ? 15 : _telem.pole_pairs;
    int32_t erpm = (int32_t)roundf(_telem.rpm * (float)pole_pairs);
    int16_t v_in = (int16_t)roundf(_telem.voltage * 10.0f);

    int32_t ah_used = (int32_t)roundf(_telem.amphours_used * 10000.0f);
    int32_t ah_charge = (int32_t)roundf(_telem.amphours_charged * 10000.0f);
    int32_t wh_used = (int32_t)roundf(_telem.watthours_used * 10000.0f);
    int32_t wh_charge = (int32_t)roundf(_telem.watthours_charged * 10000.0f);

    if (mask & ((uint32_t)1 << 0)) buffer_append_int16(buf, temp_mos, &idx);     // idx 1..2
    if (mask & ((uint32_t)1 << 1)) buffer_append_int16(buf, temp_mot, &idx);     // idx 3..4
    if (mask & ((uint32_t)1 << 2)) buffer_append_int32(buf, current_mot, &idx);  // idx 5..8
    if (mask & ((uint32_t)1 << 3)) buffer_append_int32(buf, current_in, &idx);   // idx 9..12
    if (mask & ((uint32_t)1 << 4)) buffer_append_int32(buf, 0, &idx);            // idx 13..16 (id)
    if (mask & ((uint32_t)1 << 5)) buffer_append_int32(buf, 0, &idx);            // idx 17..20 (iq)
    if (mask & ((uint32_t)1 << 6)) buffer_append_int16(buf, duty_now, &idx);     // idx 21..22
    if (mask & ((uint32_t)1 << 7)) buffer_append_int32(buf, erpm, &idx);         // idx 23..26
    if (mask & ((uint32_t)1 << 8)) buffer_append_int16(buf, v_in, &idx);         // idx 27..28
    if (mask & ((uint32_t)1 << 9)) buffer_append_int32(buf, ah_used, &idx);      // idx 29..32
    if (mask & ((uint32_t)1 << 10)) buffer_append_int32(buf, ah_charge, &idx);   // idx 33..36
    if (mask & ((uint32_t)1 << 11)) buffer_append_int32(buf, wh_used, &idx);     // idx 37..40
    if (mask & ((uint32_t)1 << 12)) buffer_append_int32(buf, wh_charge, &idx);   // idx 41..44
    if (mask & ((uint32_t)1 << 13)) buffer_append_int32(buf, _telem.tachometer, &idx);     // idx 45..48
    if (mask & ((uint32_t)1 << 14)) buffer_append_int32(buf, _telem.tachometer_abs, &idx); // idx 49..52
    if (mask & ((uint32_t)1 << 15)) buf[idx++] = _telem.fault_code;               // idx 53 (fault code)
    if (mask & ((uint32_t)1 << 16)) buffer_append_int32(buf, 0, &idx);            // idx 54..57 (pid_pos_now)
    if (mask & ((uint32_t)1 << 17)) buf[idx++] = 0;                               // idx 58 (controller_id)
    if (mask & ((uint32_t)1 << 18)) {
        buffer_append_int16(buf, temp_mos, &idx);     // idx 59..60 (temp_mos1)
        buffer_append_int16(buf, temp_mos, &idx);     // idx 61..62 (temp_mos2)
        buffer_append_int16(buf, temp_mos, &idx);     // idx 63..64 (temp_mos3)
    }
    if (mask & ((uint32_t)1 << 19)) buffer_append_int32(buf, 0, &idx);            // idx 65..68 (vd)
    if (mask & ((uint32_t)1 << 20)) buffer_append_int32(buf, 0, &idx);            // idx 69..72 (vq)
    if (mask & ((uint32_t)1 << 21)) buf[idx++] = 0;                               // status

    sendResponse(buf, idx);
}

void VirtualVesc::handleGetDecodedAdc() {
    uint8_t buf[32];
    int32_t idx = 0;

    buf[idx++] = COMM_GET_DECODED_ADC;

    // ADC1: Throttle (0..100% -> 0..1,000,000 level, 0..3.3V -> 0..3,300,000 µV)
    float t_pct = _telem.throttle_pct;
    if (t_pct < 0.0f) t_pct = 0.0f;
    if (t_pct > 100.0f) t_pct = 100.0f;
    int32_t raw_level1 = (int32_t)roundf((t_pct / 100.0f) * 1000000.0f);
    int32_t raw_volt1  = (int32_t)roundf((t_pct / 100.0f) * 3.3f * 1000000.0f);

    // ADC2: Brake (0..100% -> 0..1,000,000 level, 0..3.3V -> 0..3,300,000 µV)
    float b_pct = _telem.brake_pct;
    if (b_pct < 0.0f) b_pct = 0.0f;
    if (b_pct > 100.0f) b_pct = 100.0f;
    int32_t raw_level2 = (int32_t)roundf((b_pct / 100.0f) * 1000000.0f);
    int32_t raw_volt2  = (int32_t)roundf((b_pct / 100.0f) * 3.3f * 1000000.0f);

    buffer_append_int32(buf, raw_level1, &idx);   // idx 1..4
    buffer_append_int32(buf, raw_volt1, &idx);    // idx 5..8
    buffer_append_int32(buf, raw_level2, &idx);   // idx 9..12
    buffer_append_int32(buf, raw_volt2, &idx);    // idx 13..16

    sendResponse(buf, idx);
}

void VirtualVesc::handleGetDecodedPpm() {
    uint8_t buf[16];
    int32_t idx = 0;

    buf[idx++] = COMM_GET_DECODED_PPM;
    float t_norm = (_telem.throttle_pct / 100.0f);
    int32_t dec_ppm = (int32_t)roundf(t_norm * 1000000.0f);
    int32_t ppm_len = (int32_t)roundf(1000000.0f + t_norm * 1000000.0f); // 1.0ms .. 2.0ms in ns

    buffer_append_int32(buf, dec_ppm, &idx);
    buffer_append_int32(buf, ppm_len, &idx);

    sendResponse(buf, idx);
}

void VirtualVesc::handleGetDecodedChuk() {
    uint8_t buf[16];
    int32_t idx = 0;

    buf[idx++] = COMM_GET_DECODED_CHUK;
    float t_norm = (_telem.throttle_pct / 100.0f);
    int32_t dec_chuk = (int32_t)roundf(t_norm * 1000000.0f);

    buffer_append_int32(buf, dec_chuk, &idx);
    buf[idx++] = 128; // js_x center
    buf[idx++] = (uint8_t)(128 + (int32_t)(t_norm * 127.0f)); // js_y
    buf[idx++] = 0;   // bt_c
    buf[idx++] = 0;   // bt_z

    sendResponse(buf, idx);
}

void VirtualVesc::handleGetMcconf() {
    uint8_t buf[128];
    int32_t idx = 0;

    buf[idx++] = COMM_GET_MCCONF;
    buffer_append_uint32(buf, 0, &idx);           // idx 1..4 (signature)
    buf[idx++] = 0;                               // idx 5 (pwm_mode)
    buf[idx++] = 0;                               // idx 6 (comm_mode)
    buf[idx++] = 2;                               // idx 7 (motor_type = FOC)
    buf[idx++] = 2;                               // idx 8 (sensor_mode = Hall)

    // Motor current limits
    buffer_append_float32_auto(buf, _limits.max_phase_amps, &idx);    // idx 9..12 (l_current_max)
    buffer_append_float32_auto(buf, _limits.min_phase_amps, &idx);    // idx 13..16 (l_current_min)
    buffer_append_float32_auto(buf, _limits.max_battery_amps, &idx);  // idx 17..20 (l_in_current_max)
    buffer_append_float32_auto(buf, _limits.min_battery_amps, &idx);  // idx 21..24 (l_in_current_min)

    // Additional configuration fields
    buffer_append_float32_auto(buf, _limits.max_erpm, &idx);          // idx 25..28 (l_max_erpm)
    buffer_append_float32_auto(buf, _limits.min_erpm, &idx);          // idx 29..32 (l_min_erpm)
    buffer_append_float32_auto(buf, _limits.max_watt, &idx);          // idx 33..36 (l_watt_max)
    buffer_append_float32_auto(buf, _limits.min_watt, &idx);          // idx 37..40 (l_watt_min)
    buffer_append_float32_auto(buf, _limits.battery_cut_start, &idx); // idx 41..44 (l_battery_cut_start)
    buffer_append_float32_auto(buf, _limits.battery_cut_end, &idx);   // idx 45..48 (l_battery_cut_end)

    sendResponse(buf, idx);
}

void VirtualVesc::handleSetMcconf(const uint8_t *payload, size_t len) {
    if (len >= 25) {
        int32_t ind = 9; // Skip cmd_id (1), signature (4), pwm/comm/motor/sensor (4)
        _limits.max_phase_amps   = buffer_get_float32_auto(payload, &ind);
        _limits.min_phase_amps   = buffer_get_float32_auto(payload, &ind);
        _limits.max_battery_amps = buffer_get_float32_auto(payload, &ind);
        _limits.min_battery_amps = buffer_get_float32_auto(payload, &ind);
    }
    // Acknowledge SET_MCCONF
    uint8_t ack = COMM_SET_MCCONF;
    sendResponse(&ack, 1);
}

void VirtualVesc::handleGetAppconf(uint8_t cmd_id) {
    uint8_t buf[128];
    int32_t idx = 0;

    buf[idx++] = cmd_id;
    buffer_append_uint32(buf, 0, &idx);          // signature
    buf[idx++] = 0;                              // controller_id
    buffer_append_uint32(buf, 1000, &idx);       // timeout_msec
    buffer_append_float32_auto(buf, 0.0f, &idx); // timeout_brake_current
    buf[idx++] = 0;                              // send_can_status
    buffer_append_uint16(buf, 50, &idx);         // send_can_status_rate_hz
    buf[idx++] = 0;                              // can_baud_rate
    buf[idx++] = 0;                              // pairing_done
    buf[idx++] = 0;                              // permanent_uart_enabled
    buf[idx++] = 0;                              // shutdown_mode
    buf[idx++] = 0;                              // can_mode
    buf[idx++] = 0;                              // uavcan_esc_index
    buf[idx++] = 0;                              // uavcan_raw_mode
    buf[idx++] = 1;                              // app_to_use (APP_PPM)

    sendResponse(buf, idx);
}

void VirtualVesc::handleSetMcconfTemp(const uint8_t *payload, size_t len, VirtualVescLimits &target_limits) {
    if (len < 37) return;

    int32_t idx = 1;
    target_limits.min_phase_amps   = buffer_get_float32_auto(payload, &idx);
    target_limits.max_phase_amps   = buffer_get_float32_auto(payload, &idx);
    target_limits.min_erpm         = buffer_get_float32_auto(payload, &idx);
    target_limits.max_erpm         = buffer_get_float32_auto(payload, &idx);
    target_limits.min_battery_amps = buffer_get_float32_auto(payload, &idx);
    target_limits.max_battery_amps = buffer_get_float32_auto(payload, &idx);
    target_limits.duty_min         = buffer_get_int16(payload, &idx) / 1000.0f;
    target_limits.duty_max         = buffer_get_int16(payload, &idx) / 1000.0f;
    target_limits.min_watt         = buffer_get_float32_auto(payload, &idx);
    target_limits.max_watt         = buffer_get_float32_auto(payload, &idx);
}

void VirtualVesc::handleSetDuty(const uint8_t *payload, size_t len) {
    if (len < 5) return;
    int32_t idx = 1;
    int32_t duty_raw = buffer_get_int32(payload, &idx);
    float duty = (float)duty_raw / 100000.0f;
    if (duty > 1.0f) duty = 1.0f;
    if (duty < -1.0f) duty = -1.0f;

    _has_ext_current_ctrl = false;
    _ext_target_current_a = 0.0f;

    if (duty >= 0.0f) {
        _ext_throttle_pct = duty * 100.0f;
        _ext_brake_pct = 0.0f;
    } else {
        _ext_throttle_pct = 0.0f;
        _ext_brake_pct = fabsf(duty) * 100.0f;
    }
    printf("[EXT_CTRL] COMM_SET_DUTY: raw=%d duty=%.2f -> Thr=%.1f%%, Brk=%.1f%%\n",
           duty_raw, duty, _ext_throttle_pct, _ext_brake_pct);
    _has_ext_ctrl = true;
    _last_ext_ctrl_time = std::chrono::steady_clock::now();
}

void VirtualVesc::handleSetCurrent(const uint8_t *payload, size_t len) {
    if (len < 5) return;
    int32_t idx = 1;
    int32_t current_mA = buffer_get_int32(payload, &idx);
    float current_a = (float)current_mA / 1000.0f;

    float max_a = _limits.max_phase_amps > 1.0f ? _limits.max_phase_amps : 120.0f;
    float min_a = fabsf(_limits.min_phase_amps) > 1.0f ? fabsf(_limits.min_phase_amps) : 120.0f;

    _ext_target_current_a = current_a;
    _has_ext_current_ctrl = true;

    if (current_a >= 0.0f) {
        if (current_a <= 0.05f) {
            _ext_throttle_pct = 0.0f;
        } else {
            // Torque control: even modest test currents (e.g. 3-5A in VESC Tool) produce positive accelerating torque
            float norm = (current_a / max_a);
            if (norm > 1.0f) norm = 1.0f;
            _ext_throttle_pct = 25.0f + norm * 75.0f;
        }
        _ext_brake_pct = 0.0f;
    } else {
        _ext_throttle_pct = 0.0f;
        _ext_brake_pct = (fabsf(current_a) / min_a) * 100.0f;
        if (_ext_brake_pct > 100.0f) _ext_brake_pct = 100.0f;
    }
    printf("[EXT_CTRL] COMM_SET_CURRENT: raw_mA=%d cur=%.2fA (max=%.1fA) -> Thr=%.1f%%, Brk=%.1f%%\n",
           current_mA, current_a, max_a, _ext_throttle_pct, _ext_brake_pct);
    _has_ext_ctrl = true;
    _last_ext_ctrl_time = std::chrono::steady_clock::now();
}

void VirtualVesc::handleSetCurrentBrake(const uint8_t *payload, size_t len) {
    if (len < 5) return;
    int32_t idx = 1;
    int32_t current_mA = buffer_get_int32(payload, &idx);
    float current_a = (float)current_mA / 1000.0f;

    float min_a = fabsf(_limits.min_phase_amps) > 1.0f ? fabsf(_limits.min_phase_amps) : 120.0f;
    _ext_throttle_pct = 0.0f;
    _ext_brake_pct = (fabsf(current_a) / min_a) * 100.0f;
    if (_ext_brake_pct > 100.0f) _ext_brake_pct = 100.0f;
    _has_ext_current_ctrl = false;
    _ext_target_current_a = 0.0f;

    _has_ext_ctrl = true;
    _last_ext_ctrl_time = std::chrono::steady_clock::now();
}

void VirtualVesc::handleSetRpm(const uint8_t *payload, size_t len) {
    if (len < 5) return;
    int32_t idx = 1;
    int32_t erpm = buffer_get_int32(payload, &idx);

    float max_erpm = _limits.max_erpm > 1000.0f ? _limits.max_erpm : 100000.0f;
    if (erpm >= 0) {
        _ext_throttle_pct = ((float)erpm / max_erpm) * 100.0f;
        if (_ext_throttle_pct > 100.0f) _ext_throttle_pct = 100.0f;
        _ext_brake_pct = 0.0f;
    } else {
        _ext_throttle_pct = 0.0f;
        _ext_brake_pct = (fabsf((float)erpm) / max_erpm) * 100.0f;
        if (_ext_brake_pct > 100.0f) _ext_brake_pct = 100.0f;
    }
    _has_ext_ctrl = true;
    _last_ext_ctrl_time = std::chrono::steady_clock::now();
}

void VirtualVesc::handleSetHandbrake(const uint8_t *payload, size_t len) {
    if (len < 5) return;
    int32_t idx = 1;
    int32_t current_mA = buffer_get_int32(payload, &idx);
    float current_a = (float)current_mA / 1000.0f;

    _ext_throttle_pct = 0.0f;
    _ext_brake_pct = current_a > 0.1f ? 100.0f : 0.0f;
    _has_ext_ctrl = true;
    _last_ext_ctrl_time = std::chrono::steady_clock::now();
}

void VirtualVesc::handleForwardCan(const uint8_t *payload, size_t len) {
    if (len < 3) return;
    uint8_t can_id = payload[1];
    const uint8_t *inner = &payload[2];
    size_t inner_len = len - 2;

    if (inner[0] == COMM_SET_MCCONF_TEMP || inner[0] == COMM_SET_MCCONF_TEMP_SETUP) {
        if (can_id == 1) {
            handleSetMcconfTemp(inner, inner_len, _can1_limits);
        } else {
            handleSetMcconfTemp(inner, inner_len, _limits);
        }
    } else {
        handlePacket(inner, inner_len, _current_resp_cb);
    }
}

void VirtualVesc::handleTerminalCmd(const uint8_t *payload, size_t len) {
    if (len <= 1) return;

    std::string cmd((const char*)&payload[1], len - 1);
    // Trim trailing newlines / nulls
    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n' || cmd.back() == '\0')) {
        cmd.pop_back();
    }
    _last_terminal_cmd = cmd;

    std::string reply;

    float val = 0.0f;
    if (sscanf(cmd.c_str(), "conf_set l_current_max %f", &val) == 1) {
        _limits.max_phase_amps = val;
        reply = "l_current_max set\n";
    } else if (sscanf(cmd.c_str(), "conf_set l_in_current_max %f", &val) == 1) {
        _limits.max_battery_amps = val;
        reply = "l_in_current_max set\n";
    } else if (sscanf(cmd.c_str(), "conf_set l_watt_max %f", &val) == 1) {
        _limits.max_watt = val;
        reply = "l_watt_max set\n";
    } else if (sscanf(cmd.c_str(), "conf_set l_max_erpm %f", &val) == 1) {
        _limits.max_erpm = val;
        reply = "l_max_erpm set\n";
    } else if (sscanf(cmd.c_str(), "conf_set l_battery_cut_start %f", &val) == 1) {
        _limits.battery_cut_start = val;
        reply = "l_battery_cut_start set\n";
    } else if (sscanf(cmd.c_str(), "conf_set l_battery_cut_end %f", &val) == 1) {
        _limits.battery_cut_end = val;
        reply = "l_battery_cut_end set\n";
    } else if (cmd == "ping") {
        reply = "pong\n";
    } else if (cmd == "fault") {
        char buf[64];
        snprintf(buf, sizeof(buf), "Fault code: %d\n", _telem.fault_code);
        reply = buf;
    } else if (cmd == "help") {
        reply = "Virtual VESC 75_100 Emulator commands: conf_set, ping, fault, help\n";
    }

    if (!reply.empty()) {
        std::vector<uint8_t> resp;
        resp.push_back(COMM_PRINT);
        resp.insert(resp.end(), reply.begin(), reply.end());
        resp.push_back(0); // Null terminator
        sendResponse(resp.data(), resp.size());
    }
}

void VirtualVesc::handleGetStats(const uint8_t *payload, size_t len) {
    uint8_t buf[128];
    int32_t idx = 0;
    buf[idx++] = COMM_GET_STATS;

    uint32_t mask = 0xFFFF;
    if (len >= 3) {
        int32_t p_ind = 1;
        mask = (uint32_t)buffer_get_uint16(payload, &p_ind);
    }
    buffer_append_uint32(buf, mask, &idx);

    int pole_pairs = (_telem.pole_pairs < 2) ? 15 : _telem.pole_pairs;
    float erpm_max = _telem.rpm * (float)pole_pairs;
    float erpm_avg = erpm_max * 0.75f;
    float pwr_avg  = _telem.voltage * _telem.current_in * 0.7f;
    float pwr_max  = _telem.voltage * _telem.current_in;
    float cur_avg  = _telem.current_in * 0.7f;
    float cur_max  = _telem.current_in;

    if (mask & ((uint32_t)1 << 0)) buffer_append_float32_auto(buf, erpm_avg, &idx);
    if (mask & ((uint32_t)1 << 1)) buffer_append_float32_auto(buf, erpm_max, &idx);
    if (mask & ((uint32_t)1 << 2)) buffer_append_float32_auto(buf, pwr_avg, &idx);
    if (mask & ((uint32_t)1 << 3)) buffer_append_float32_auto(buf, pwr_max, &idx);
    if (mask & ((uint32_t)1 << 4)) buffer_append_float32_auto(buf, cur_avg, &idx);
    if (mask & ((uint32_t)1 << 5)) buffer_append_float32_auto(buf, cur_max, &idx);
    if (mask & ((uint32_t)1 << 6)) buffer_append_float32_auto(buf, _telem.temp_mos, &idx);
    if (mask & ((uint32_t)1 << 7)) buffer_append_float32_auto(buf, _telem.temp_mos, &idx);
    if (mask & ((uint32_t)1 << 8)) buffer_append_float32_auto(buf, _telem.temp_mot, &idx);
    if (mask & ((uint32_t)1 << 9)) buffer_append_float32_auto(buf, _telem.temp_mot, &idx);

    sendResponse(buf, idx);
}
