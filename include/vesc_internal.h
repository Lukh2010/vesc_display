#pragma once

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
extern SemaphoreHandle_t s_uartMutex;
struct UartLock {
    UartLock() { if (s_uartMutex) xSemaphoreTakeRecursive(s_uartMutex, portMAX_DELAY); }
    ~UartLock() { if (s_uartMutex) xSemaphoreGiveRecursive(s_uartMutex); }
};
#else
struct UartLock { UartLock() {} ~UartLock() {} };
#endif

static constexpr size_t CAN_RX_BUF_SIZE = 2048;

// Standard CRC-CCITT for VESC packets
inline uint16_t crc16(const uint8_t *buf, uint32_t len) {
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
inline float buffer_get_float32_auto(const uint8_t *buffer, int32_t *index) {
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
inline void buffer_append_float32_auto(uint8_t *b, float number, int32_t *idx) {
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

// Shared internal CAN bus helpers & state (implemented in src/vesc_can.cpp)
#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
extern bool s_can_initialized;
extern uint8_t s_primary_vesc_id;
extern uint8_t s_secondary_vesc_id;
extern bool s_has_secondary;

void ensureCanBusHealthy(bool force = false);
void updateDetectedCanIds(uint8_t controller_id, uint8_t cmd_id, uint32_t now);
void twaiTransmitToDetected(twai_message_t &msg, uint8_t cmd_id, TickType_t timeout_ticks = pdMS_TO_TICKS(5));
void sendVescCanBuffer(uint8_t controller_id, const uint8_t *data, size_t len,
                       uint8_t src_id = VescHandler::ESP_CAN_ID, uint8_t send_flag = 2);
void sendVescCanBufferToDetected(const uint8_t *data, size_t len,
                                 uint8_t src_id = VescHandler::ESP_CAN_ID, uint8_t send_flag = 2);
bool processCanRxFrameForEsp(const twai_message_t &msg, const uint8_t **out_payload, size_t *out_len);
#endif
