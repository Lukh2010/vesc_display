#pragma once

#include <Arduino.h>
#include "dash_types.h"
#include "settings_manager.h"
#include "mode_manager.h"

class BleKeylessManager {
public:
    static BleKeylessManager& instance();

    void begin();
    void update(DashTelemetry &telemetry, uint32_t now_ms);

    void startPairing();
    void cancelPairing();
    void unpair();

    bool isConnected() const { return _connected; }
    bool isPaired() const { return Settings.get().ble_paired; }
    bool isPairingActive() const { return _pairing_active; }
    int8_t getRssi() const { return _rssi; }
    uint8_t getSignalBars() const;
    const char* getPairedAddr() const { return Settings.get().ble_paired_addr; }
    const char* getPairedName() const { return Settings.get().ble_paired_name; }

    // BLE Stack callbacks
    void onClientConnect(const uint8_t *bda);
    void onClientDisconnect();
    void updateRssi(int8_t rssi);

private:
    BleKeylessManager();
    ~BleKeylessManager() = default;

    bool     _inited;
    bool     _connected;
    bool     _pairing_active;
    int8_t   _rssi;
    uint32_t _connect_time_ms;
    uint32_t _disconnect_time_ms;
    uint32_t _last_rssi_poll_ms;
    uint8_t  _peer_bda[6];
};
