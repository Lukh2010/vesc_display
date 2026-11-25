#include "vesc_internal.h"

#if !defined(NATIVE_SIMULATOR) && VESC_USE_CAN_BUS
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
#endif

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
