#include "vesc_packet.h"
#include <string.h>
#include <math.h>

uint16_t vesc_crc16(const uint8_t *buf, uint32_t len) {
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

void buffer_append_int16(uint8_t *buffer, int16_t number, int32_t *index) {
    buffer[(*index)++] = (uint8_t)(number >> 8);
    buffer[(*index)++] = (uint8_t)(number);
}

void buffer_append_uint16(uint8_t *buffer, uint16_t number, int32_t *index) {
    buffer[(*index)++] = (uint8_t)(number >> 8);
    buffer[(*index)++] = (uint8_t)(number);
}

void buffer_append_int32(uint8_t *buffer, int32_t number, int32_t *index) {
    buffer[(*index)++] = (uint8_t)(number >> 24);
    buffer[(*index)++] = (uint8_t)(number >> 16);
    buffer[(*index)++] = (uint8_t)(number >> 8);
    buffer[(*index)++] = (uint8_t)(number);
}

void buffer_append_uint32(uint8_t *buffer, uint32_t number, int32_t *index) {
    buffer[(*index)++] = (uint8_t)(number >> 24);
    buffer[(*index)++] = (uint8_t)(number >> 16);
    buffer[(*index)++] = (uint8_t)(number >> 8);
    buffer[(*index)++] = (uint8_t)(number);
}

void buffer_append_float32_auto(uint8_t *buffer, float number, int32_t *index) {
    union { float f; uint32_t u; } u;
    u.f = number;
    buffer[(*index)++] = (uint8_t)(u.u >> 24);
    buffer[(*index)++] = (uint8_t)(u.u >> 16);
    buffer[(*index)++] = (uint8_t)(u.u >> 8);
    buffer[(*index)++] = (uint8_t)(u.u);
}

int16_t buffer_get_int16(const uint8_t *buffer, int32_t *index) {
    int16_t res = (int16_t)(((uint16_t)buffer[*index] << 8) | ((uint16_t)buffer[*index + 1]));
    *index += 2;
    return res;
}

uint16_t buffer_get_uint16(const uint8_t *buffer, int32_t *index) {
    uint16_t res = ((uint16_t)buffer[*index] << 8) | ((uint16_t)buffer[*index + 1]);
    *index += 2;
    return res;
}

int32_t buffer_get_int32(const uint8_t *buffer, int32_t *index) {
    int32_t res = (int32_t)(((uint32_t)buffer[*index] << 24) |
                           ((uint32_t)buffer[*index + 1] << 16) |
                           ((uint32_t)buffer[*index + 2] << 8) |
                           ((uint32_t)buffer[*index + 3]));
    *index += 4;
    return res;
}

uint32_t buffer_get_uint32(const uint8_t *buffer, int32_t *index) {
    uint32_t res = ((uint32_t)buffer[*index] << 24) |
                    ((uint32_t)buffer[*index + 1] << 16) |
                    ((uint32_t)buffer[*index + 2] << 8) |
                    ((uint32_t)buffer[*index + 3]);
    *index += 4;
    return res;
}

float buffer_get_float32_auto(const uint8_t *buffer, int32_t *index) {
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

size_t vesc_encode_packet(const uint8_t *payload, size_t payload_len, uint8_t *out_buffer, size_t max_out_len) {
    if (!payload || !out_buffer || payload_len == 0) return 0;

    uint16_t crc = vesc_crc16(payload, (uint32_t)payload_len);

    if (payload_len <= 255) {
        // Short packet: [0x02, len, payload..., crc_hi, crc_lo, 0x03]
        size_t total_len = payload_len + 5;
        if (max_out_len < total_len) return 0;

        out_buffer[0] = 0x02;
        out_buffer[1] = (uint8_t)payload_len;
        memcpy(&out_buffer[2], payload, payload_len);
        out_buffer[2 + payload_len] = (uint8_t)(crc >> 8);
        out_buffer[3 + payload_len] = (uint8_t)(crc & 0xFF);
        out_buffer[4 + payload_len] = 0x03;
        return total_len;
    } else {
        // Long packet: [0x03, len_hi, len_lo, payload..., crc_hi, crc_lo, 0x03]
        if (payload_len > 65535) return 0;
        size_t total_len = payload_len + 6;
        if (max_out_len < total_len) return 0;

        out_buffer[0] = 0x03;
        out_buffer[1] = (uint8_t)(payload_len >> 8);
        out_buffer[2] = (uint8_t)(payload_len & 0xFF);
        memcpy(&out_buffer[3], payload, payload_len);
        out_buffer[3 + payload_len] = (uint8_t)(crc >> 8);
        out_buffer[4 + payload_len] = (uint8_t)(crc & 0xFF);
        out_buffer[5 + payload_len] = 0x03;
        return total_len;
    }
}

// -------------------------------------------------------------
// VescPacketParser Implementation
// -------------------------------------------------------------
VescPacketParser::VescPacketParser()
    : _state(STATE_WAIT_START),
      _is_long(false),
      _expected_len(0),
      _payload_index(0),
      _received_crc(0),
      _packets_received(0),
      _crc_errors(0),
      _malformed_errors(0) {
    _payload.reserve(1024);
}

void VescPacketParser::reset() {
    _state = STATE_WAIT_START;
    _is_long = false;
    _expected_len = 0;
    _payload_index = 0;
    _received_crc = 0;
    _payload.clear();
}

bool VescPacketParser::processByte(uint8_t b, const PacketCallback &callback) {
    switch (_state) {
        case STATE_WAIT_START:
            if (b == 0x02) {
                _is_long = false;
                _state = STATE_SHORT_LEN;
            } else if (b == 0x03) {
                _is_long = true;
                _state = STATE_LONG_LEN1;
            }
            break;

        case STATE_SHORT_LEN:
            _expected_len = b;
            if (_expected_len == 0) {
                _malformed_errors++;
                _state = STATE_WAIT_START;
            } else {
                _payload.resize(_expected_len);
                _payload_index = 0;
                _state = STATE_PAYLOAD;
            }
            break;

        case STATE_LONG_LEN1:
            if (b > 0x10) { // Max VESC payload is 4096 bytes (0x1000)
                _malformed_errors++;
                if (b == 0x02) {
                    _is_long = false;
                    _state = STATE_SHORT_LEN;
                } else if (b == 0x03) {
                    _is_long = true;
                    _state = STATE_LONG_LEN1;
                } else {
                    _state = STATE_WAIT_START;
                }
            } else {
                _expected_len = ((size_t)b) << 8;
                _state = STATE_LONG_LEN2;
            }
            break;

        case STATE_LONG_LEN2:
            _expected_len |= (size_t)b;
            if (_expected_len == 0 || _expected_len > 4096) {
                _malformed_errors++;
                if (b == 0x02) {
                    _is_long = false;
                    _state = STATE_SHORT_LEN;
                } else if (b == 0x03) {
                    _is_long = true;
                    _state = STATE_LONG_LEN1;
                } else {
                    _state = STATE_WAIT_START;
                }
            } else {
                _payload.resize(_expected_len);
                _payload_index = 0;
                _state = STATE_PAYLOAD;
            }
            break;

        case STATE_PAYLOAD:
            _payload[_payload_index++] = b;
            if (_payload_index >= _expected_len) {
                _state = STATE_CRC_HI;
            }
            break;

        case STATE_CRC_HI:
            _received_crc = ((uint16_t)b) << 8;
            _state = STATE_CRC_LO;
            break;

        case STATE_CRC_LO:
            _received_crc |= (uint16_t)b;
            _state = STATE_STOP;
            break;

        case STATE_STOP:
            if (b == 0x03) {
                uint16_t calc_crc = vesc_crc16(_payload.data(), (uint32_t)_expected_len);
                if (calc_crc == _received_crc) {
                    _packets_received++;
                    if (callback) {
                        callback(_payload.data(), _expected_len);
                    }
                    _state = STATE_WAIT_START;
                    return true;
                } else {
                    _crc_errors++;
                    _state = STATE_WAIT_START;
                }
            } else {
                _malformed_errors++;
                if (b == 0x02) {
                    _is_long = false;
                    _state = STATE_SHORT_LEN;
                } else if (b == 0x03) {
                    _is_long = true;
                    _state = STATE_LONG_LEN1;
                } else {
                    _state = STATE_WAIT_START;
                }
            }
            break;
    }
    return false;
}

size_t VescPacketParser::processBuffer(const uint8_t *buffer, size_t size, const PacketCallback &callback) {
    if (!buffer || size == 0) return 0;
    size_t count = 0;
    for (size_t i = 0; i < size; i++) {
        if (processByte(buffer[i], callback)) {
            count++;
        }
    }
    return count;
}
