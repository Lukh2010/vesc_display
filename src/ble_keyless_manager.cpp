#include "ble_keyless_manager.h"
#include "dash_renderer.h"

#if !defined(NATIVE_SIMULATOR)
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>

#define KEYLESS_BLE_NAME "VESC-Keyless"
#define SERVICE_UUID_KEYLESS "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_UUID_STATUS     "beb5483e-36e1-4688-b7f5-ea07361b26a8"

static BLEServer *s_bleServer = nullptr;
static BLECharacteristic *s_statusChar = nullptr;

class VescKeylessServerCallbacks : public BLEServerCallbacks {
public:
    void onConnect(BLEServer* pServer) override {
        // No-op: Detailed onConnect callback below receives connection parameters & MAC address.
    }

#if defined(CONFIG_NIMBLE_ENABLED)
    void onConnect(BLEServer* pServer, struct ble_gap_conn_desc *desc) override {
        if (desc) {
            bool ok = BleKeylessManager::instance().onClientConnect((const uint8_t*)desc->peer_id_addr.val, desc->conn_handle);
            if (!ok && pServer) {
                pServer->disconnect(desc->conn_handle);
                BLEDevice::startAdvertising();
            }
        }
    }
#elif defined(CONFIG_BLUEDROID_ENABLED)
    void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) override {
        if (param) {
            bool ok = BleKeylessManager::instance().onClientConnect(param->connect.remote_bda, param->connect.conn_id);
            if (!ok && pServer) {
                pServer->disconnect(param->connect.conn_id);
                BLEDevice::startAdvertising();
            }
        }
    }
#endif

    void onDisconnect(BLEServer* pServer) override {
        BleKeylessManager::instance().onClientDisconnect();
        BLEDevice::startAdvertising();
    }
};
#endif

BleKeylessManager& BleKeylessManager::instance() {
    static BleKeylessManager inst;
    return inst;
}

BleKeylessManager::BleKeylessManager()
    : _inited(false)
    , _connected(false)
    , _pairing_active(false)
    , _rssi(0)
    , _connect_time_ms(0)
    , _disconnect_time_ms(0)
    , _last_rssi_poll_ms(0)
    , _conn_id(0)
{
    memset(_peer_bda, 0, sizeof(_peer_bda));
}

void BleKeylessManager::begin() {
    if (_inited) return;
    _inited = true;

#if !defined(NATIVE_SIMULATOR)
    BLEDevice::init(KEYLESS_BLE_NAME);

    s_bleServer = BLEDevice::createServer();
    s_bleServer->setCallbacks(new VescKeylessServerCallbacks());

    // 1. Custom Keyless Service
    BLEService *pKeylessService = s_bleServer->createService(SERVICE_UUID_KEYLESS);
    s_statusChar = pKeylessService->createCharacteristic(
        CHAR_UUID_STATUS,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    uint8_t initVal = 0x01;
    s_statusChar->setValue(&initVal, 1);
    pKeylessService->start();

    // 2. Battery Service (UUID 0x180F) - standard UUID for instant native iOS/Android Bluetooth discovery!
    BLEService *pBatteryService = s_bleServer->createService(BLEUUID((uint16_t)0x180F));
    BLECharacteristic *pBatChar = pBatteryService->createCharacteristic(
        BLEUUID((uint16_t)0x2A19),
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    uint8_t batLvl = 100;
    pBatChar->setValue(&batLvl, 1);
    pBatteryService->start();

    // 3. Configure Advertising & Scan Response Data
    // In BLE 4.x legacy advertising, advertising packet payload is strictly limited to 31 bytes.
    // Adv packet: Flags (3B) + Battery Service 0x180F (4B) + Complete Name 'VESC-Keyless' (14B) = 21 bytes (<= 31B).
    // Native iOS/Android Bluetooth discovery can immediately see 'VESC-Keyless' and Battery Service.
    // Scan Response packet: Custom 128-bit Keyless Service UUID (18B) + complete local name.
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();

    BLEAdvertisementData advData;
    advData.setFlags(0x06); // ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT
    advData.setCompleteServices(BLEUUID((uint16_t)0x180F));
    advData.setName(KEYLESS_BLE_NAME);
    pAdvertising->setAdvertisementData(advData);

    BLEAdvertisementData scanResponseData;
    scanResponseData.setCompleteServices(BLEUUID(SERVICE_UUID_KEYLESS));
    scanResponseData.setName(KEYLESS_BLE_NAME);
    pAdvertising->setScanResponseData(scanResponseData);

    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06); // for iOS compatibility
    pAdvertising->setMaxPreferred(0x12);
    BLEDevice::startAdvertising();

    Serial.println("[BLE KEYLESS] Bluetooth Low Energy server active: Advertising as 'VESC-Keyless' (Adv: Battery+Name, ScanRsp: Custom UUID)");
#else
    Serial.println("[BLE KEYLESS] Simulator BLE Mock initialized.");
#endif
}

bool BleKeylessManager::onClientConnect(const uint8_t *bda, uint16_t connId) {
    if (!bda) {
        Serial.println("[BLE KEYLESS] Connection attempt with null address rejected.");
        _connected = false;
#if !defined(NATIVE_SIMULATOR)
        if (s_bleServer) {
            s_bleServer->disconnect(connId);
            BLEDevice::startAdvertising();
        }
#endif
        return false;
    }

    char addrStr[24];
    snprintf(addrStr, sizeof(addrStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

    // Whitelist / Exclusive Pairing Lockout:
    // If a phone is already paired, ONLY that paired MAC address is allowed to connect.
    // Any other device is strictly rejected and disconnected immediately so it cannot unlock.
    if (Settings.get().ble_paired && strlen(Settings.get().ble_paired_addr) > 0) {
        if (strcasecmp(addrStr, Settings.get().ble_paired_addr) != 0) {
            Serial.printf("[BLE KEYLESS] Lockout: Unauthorized client %s rejected! Paired device is %s.\n",
                          addrStr, Settings.get().ble_paired_addr);
            _connected = false;
#if !defined(NATIVE_SIMULATOR)
            if (s_bleServer) {
                s_bleServer->disconnect(connId);
                BLEDevice::startAdvertising();
            }
#endif
            return false;
        }
        Serial.printf("[BLE KEYLESS] Paired smartphone reconnected! MAC: %s\n", addrStr);
    } else {
        // Not paired yet (or in active pairing mode): Pair with this client
        strncpy(Settings.get().ble_paired_addr, addrStr, sizeof(Settings.get().ble_paired_addr));
        Settings.get().ble_paired = true;
        _pairing_active = false;
        Settings.save();
        Serial.printf("[BLE KEYLESS] Paired successfully with %s! Saved to NVS.\n", addrStr);
    }

    _connected = true;
    _conn_id = connId;
    _connect_time_ms = millis();
    _rssi = -55; // Initial proximity RSSI
    memcpy(_peer_bda, bda, 6);
    return true;
}

void BleKeylessManager::onClientDisconnect() {
    _connected = false;
    _conn_id = 0;
    _disconnect_time_ms = millis();
    _rssi = 0;
    Serial.println("[BLE KEYLESS] Smartphone disconnected. Waiting for reconnect or walk-away timeout...");
}

void BleKeylessManager::updateRssi(int8_t rssi) {
    if (_connected) {
        _rssi = (int8_t)(0.7f * _rssi + 0.3f * rssi);
    }
}

uint8_t BleKeylessManager::getSignalBars() const {
    if (!_connected) return 0;
    if (_rssi >= -60) return 4;
    if (_rssi >= -72) return 3;
    if (_rssi >= -82) return 2;
    return 1;
}

void BleKeylessManager::startPairing() {
    _pairing_active = true;
#if !defined(NATIVE_SIMULATOR)
    BLEDevice::startAdvertising();
#endif
    Serial.println("[BLE KEYLESS] Pairing mode started. Waiting for smartphone connection to 'VESC-Keyless'...");
}

void BleKeylessManager::cancelPairing() {
    _pairing_active = false;
}

void BleKeylessManager::unpair() {
    Settings.get().ble_paired = false;
    Settings.get().ble_paired_addr[0] = '\0';
    strncpy(Settings.get().ble_paired_name, "Smartphone", sizeof(Settings.get().ble_paired_name));
    Settings.save();
    _pairing_active = false;
    if (_connected) {
        _connected = false;
        _disconnect_time_ms = millis();
        _rssi = 0;
#if !defined(NATIVE_SIMULATOR)
        if (s_bleServer) {
            s_bleServer->disconnect(_conn_id);
            BLEDevice::startAdvertising();
        }
#endif
        _conn_id = 0;
    }
    Serial.println("[BLE KEYLESS] Unpaired smartphone. Settings cleared.");
}

void BleKeylessManager::update(DashTelemetry &telemetry, uint32_t now_ms) {
    // 1. Sync telemetry state
    telemetry.ble_keyless_enabled  = Settings.get().ble_keyless_enabled;
    telemetry.ble_autolock_enabled = Settings.get().ble_autolock_enabled;
    telemetry.ble_paired           = Settings.get().ble_paired;
    telemetry.ble_connected        = _connected;
    telemetry.ble_rssi             = _rssi;
    telemetry.ble_signal_bars      = getSignalBars();
    telemetry.ble_pairing_active   = _pairing_active;

    if (Settings.get().ble_paired) {
        if (strlen(Settings.get().ble_paired_name) > 0 && strcmp(Settings.get().ble_paired_name, "Smartphone") != 0) {
            strncpy(telemetry.ble_device_name, Settings.get().ble_paired_name, sizeof(telemetry.ble_device_name));
        } else if (strlen(Settings.get().ble_paired_addr) > 0) {
            strncpy(telemetry.ble_device_name, Settings.get().ble_paired_addr, sizeof(telemetry.ble_device_name));
        } else {
            strncpy(telemetry.ble_device_name, "Smartphone", sizeof(telemetry.ble_device_name));
        }
    } else {
        telemetry.ble_device_name[0] = '\0';
    }



    // 3. KEYLESS-GO AUTO-UNLOCK LOGIC
    if (telemetry.is_locked && Settings.get().ble_keyless_enabled) {
        if (_connected) {
            if (_connect_time_ms == 0 || (now_ms >= _connect_time_ms && (now_ms - _connect_time_ms >= 300))) {
                telemetry.is_locked = false;
                telemetry.theft_alarm_active = false;
                telemetry.locked_moved_meters = 0.0f;
                telemetry.pin_error = false;
                telemetry.pin_entry[0] = '\0';
                ModeManager::instance().setLocked(false);
                Renderer.markScreenDirty();
                Renderer.invalidateCache();
                Renderer.triggerNeedleSweep();
                Serial.printf("[BLE KEYLESS] Auto-Unlocked via Smartphone Proximity! (RSSI: %d dBm)\n", _rssi);
            }
        }
    }

    // 4. WALK-AWAY AUTO-LOCK LOGIC
    // STRICT SAFETY INVARIANT: ONLY lock when stationary (speed < 0.5 km/h) - NEVER lock while moving!
    if (!telemetry.is_locked && 
        Settings.get().ble_keyless_enabled && 
        Settings.get().ble_autolock_enabled && 
        Settings.get().ble_paired) 
    {
        if (telemetry.speed_kmh < 0.5f) {
            if (!_connected && _disconnect_time_ms > 0 && now_ms >= _disconnect_time_ms) {
                if (now_ms - _disconnect_time_ms >= 10000) {
                    telemetry.is_locked = true;
                    ModeManager::instance().setLocked(true);
                    Renderer.markScreenDirty();
                    Renderer.invalidateCache();
                    Serial.println("[BLE KEYLESS] Walk-Away Auto-Lock triggered (disconnected > 10s)! VESC locked at 0A.");
                }
            }
        }
    }
}
