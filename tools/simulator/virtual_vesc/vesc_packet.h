#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <functional>

/**
 * @brief Standard VESC CCITT CRC-16 computation.
 */
uint16_t vesc_crc16(const uint8_t *buf, uint32_t len);

// Serialization primitives (big-endian)
void buffer_append_int16(uint8_t *buffer, int16_t number, int32_t *index);
void buffer_append_uint16(uint8_t *buffer, uint16_t number, int32_t *index);
void buffer_append_int32(uint8_t *buffer, int32_t number, int32_t *index);
void buffer_append_uint32(uint8_t *buffer, uint32_t number, int32_t *index);
void buffer_append_float32_auto(uint8_t *buffer, float number, int32_t *index);

int16_t buffer_get_int16(const uint8_t *buffer, int32_t *index);
uint16_t buffer_get_uint16(const uint8_t *buffer, int32_t *index);
int32_t buffer_get_int32(const uint8_t *buffer, int32_t *index);
uint32_t buffer_get_uint32(const uint8_t *buffer, int32_t *index);
float buffer_get_float32_auto(const uint8_t *buffer, int32_t *index);

/**
 * @brief Wraps a payload in standard VESC framing:
 * - Short packet (payload <= 255): [0x02, len, payload..., crc_hi, crc_lo, 0x03]
 * - Long packet (payload > 255):  [0x03, len_hi, len_lo, payload..., crc_hi, crc_lo, 0x03]
 * @return Total encoded packet size, or 0 if output buffer is too small.
 */
size_t vesc_encode_packet(const uint8_t *payload, size_t payload_len, uint8_t *out_buffer, size_t max_out_len);

/**
 * @brief Streaming VESC packet receiver and parser.
 * Handles framing, short/long packets, CRC verification, and recovery from corrupt streams.
 */
class VescPacketParser {
public:
    using PacketCallback = std::function<void(const uint8_t *payload, size_t len)>;

    VescPacketParser();
    void reset();

    /**
     * @brief Feed a single byte into the parser.
     * @param b Byte received.
     * @param callback Called immediately when a complete, valid packet is framed.
     * @return true if a complete packet was delivered.
     */
    bool processByte(uint8_t b, const PacketCallback &callback);

    /**
     * @brief Feed a buffer into the parser.
     * @return Number of valid packets parsed from this chunk.
     */
    size_t processBuffer(const uint8_t *buffer, size_t size, const PacketCallback &callback);

    // Diagnostics
    uint32_t getPacketsReceived() const { return _packets_received; }
    uint32_t getCrcErrors() const { return _crc_errors; }
    uint32_t getMalformedErrors() const { return _malformed_errors; }

private:
    enum State {
        STATE_WAIT_START = 0,
        STATE_SHORT_LEN,
        STATE_LONG_LEN1,
        STATE_LONG_LEN2,
        STATE_PAYLOAD,
        STATE_CRC_HI,
        STATE_CRC_LO,
        STATE_STOP
    };

    State _state;
    bool _is_long;
    size_t _expected_len;
    size_t _payload_index;
    uint16_t _received_crc;
    std::vector<uint8_t> _payload;

    uint32_t _packets_received;
    uint32_t _crc_errors;
    uint32_t _malformed_errors;
};
