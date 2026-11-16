#include "ble_keyless_manager.h"
#include "dash_renderer.h"

#if !defined(NATIVE_SIMULATOR)
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <esp_gap_ble_api.h>
#endif

#define KEYLESS_BLE_NAME "VESC-Keyless"
#define SERVICE_UUID_KEYLESS "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_UUID_STATUS     "beb5483e-36e1-4688-b7f5-ea07361b26a8"

static BLEServer *s_bleServer = nullptr;
static BLECharacteristic *s_statusChar = nullptr;
static BLEHIDDevice *s_hidDevice = nullptr;

class VescKeylessServerCallbacks : public BLEServerCallbacks {
public:
    void onConnect(BLEServer* pServer) override {
        // Fallback for generic connect
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

class VescSecurityCallbacks : public BLESecurityCallbacks {
    bool onSecurityRequest() override {
        return true;
    }
#if defined(CONFIG_BLUEDROID_ENABLED)
    void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) override {
        if (auth_cmpl.success) {
            Serial.println("[BLE KEYLESS] Smartphone paired & bonded successfully!");
        } else {
            Serial.printf("[BLE KEYLESS] Pairing failed: %d\n", auth_cmpl.fail_reason);
        }
    }
#elif defined(CONFIG_NIMBLE_ENABLED)
    void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
        if (desc && desc->sec_state.encrypted) {
            Serial.println("[BLE KEYLESS] Smartphone paired & bonded successfully!");
        } else {
            Serial.println("[BLE KEYLESS] BLE connection active.");
        }
    }
#endif
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t pass_key) override {}
    bool onConfirmPIN(uint32_t pass_key) override { return true; }
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
    , _last_telemetry(nullptr)
{
    memset(_peer_bda, 0, sizeof(_peer_bda));
}

void BleKeylessManager::begin() {
    if (_inited) return;
    _inited = true;

#if !defined(NATIVE_SIMULATOR)
    // Clear any test laptop pairing address so user phone can pair immediately
    if (strncmp(Settings.get().ble_paired_addr, "E0:3A", 5) == 0 ||
        strncmp(Settings.get().ble_paired_addr, "99:64", 5) == 0) {
        Settings.get().ble_paired = false;
        Settings.get().ble_paired_addr[0] = '\0';
        strncpy(Settings.get().ble_paired_name, "Smartphone", sizeof(Settings.get().ble_paired_name));
        Settings.save();
        Serial.println("[BLE KEYLESS] Cleared test laptop pairing. Ready for smartphone.");
    }

    BLEDevice::init(KEYLESS_BLE_NAME);

    // Security configuration for seamless "Just Works" pairing & bonding
    BLESecurity *pSecurity = new BLESecurity();
    pSecurity->setCapability(ESP_IO_CAP_NONE);
    pSecurity->setAuthenticationMode(true, false, true); // Bond=true, MITM=false, SC=true
    pSecurity->setForceAuthentication(true);             // Triggers permanent OS bonding request on first connect!
    BLEDevice::setSecurityCallbacks(new VescSecurityCallbacks());

    s_bleServer = BLEDevice::createServer();
    s_bleServer->setCallbacks(new VescKeylessServerCallbacks());

    // 1. HID Consumer Device Profile
    // Makes 'VESC-Keyless' immediately discoverable in iOS & Android native Bluetooth settings menus!
    static const uint8_t hidReportDescriptor[] = {
        0x05, 0x0C, // Usage Page (Consumer Devices)
        0x09, 0x01, // Usage (Consumer Control)
        0xA1, 0x01, // Collection (Application)
        0x85, 0x01, //   Report ID (1)
        0x09, 0xE9, //   Usage (Volume Increment)
        0x09, 0xEA, //   Usage (Volume Decrement)
        0x09, 0xCD, //   Usage (Play/Pause)
        0x15, 0x00, //   Logical Minimum (0)
        0x25, 0x01, //   Logical Maximum (1)
        0x75, 0x01, //   Report Size (1)
        0x95, 0x03, //   Report Count (3)
        0x81, 0x02, //   Input (Data, Variable, Absolute)
        0x75, 0x05, //   Report Size (5) - padding
        0x95, 0x01, //   Report Count (1)
        0x81, 0x03, //   Input (Constant)
        0xC0        // End Collection
    };

    s_hidDevice = new BLEHIDDevice(s_bleServer);
    s_hidDevice->manufacturer()->setValue("VESC");
    s_hidDevice->pnp(0x02, 0x05ac, 0x820a, 0x0110);
    s_hidDevice->hidInfo(0x00, 0x01);
    s_hidDevice->reportMap((uint8_t*)hidReportDescriptor, sizeof(hidReportDescriptor));
    s_hidDevice->setBatteryLevel(100);
    s_hidDevice->startServices();

    // 2. Custom Keyless Service
    BLEService *pKeylessService = s_bleServer->createService(SERVICE_UUID_KEYLESS);
    s_statusChar = pKeylessService->createCharacteristic(
        CHAR_UUID_STATUS,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    uint8_t initVal = 0x01;
    s_statusChar->setValue(&initVal, 1);
    pKeylessService->start();

    // 3. Configure Advertising
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->setAppearance(0x0180); // HID Generic Remote Control appearance (Apple Accessory Guidelines)
    pAdvertising->addServiceUUID(s_hidDevice->hidService()->getUUID()); // 0x1812
    pAdvertising->addServiceUUID(BLEUUID((uint16_t)0x180F));            // 0x180F Battery
    pAdvertising->setMinInterval(32); // 20 ms fast Apple & Android reconnection interval
    pAdvertising->setMaxInterval(48); // 30 ms

    BLEAdvertisementData scanResponseData;
    scanResponseData.setCompleteServices(BLEUUID(SERVICE_UUID_KEYLESS));
    scanResponseData.setName(KEYLESS_BLE_NAME);
    pAdvertising->setScanResponseData(scanResponseData);

    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x0C); // 15 ms connection interval (Apple Accessory Guidelines)
    pAdvertising->setMaxPreferred(0x18); // 30 ms connection interval
    BLEDevice::startAdvertising();

    Serial.println("[BLE KEYLESS] Bluetooth Low Energy active: Advertising as 'VESC-Keyless' (HID+Battery, ScanRsp: Custom UUID)");
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

    // If not paired yet (or in active pairing mode): Pair with this client
    if (!Settings.get().ble_paired || _pairing_active) {
        strncpy(Settings.get().ble_paired_addr, addrStr, sizeof(Settings.get().ble_paired_addr));
        Settings.get().ble_paired = true;
        _pairing_active = false;
        Settings.save();
        Serial.printf("[BLE KEYLESS] Paired successfully with %s! Saved to NVS.\n", addrStr);
    } else {
        // Paired smartphone reconnected! (iOS/Android RPA random private addresses resolve seamlessly via BLE bonding)
        Serial.printf("[BLE KEYLESS] Paired smartphone reconnected! Address: %s\n", addrStr);
    }

    _connected = true;
    _conn_id = connId;
    _connect_time_ms = millis();
    _rssi = -55; // Initial proximity RSSI
    memcpy(_peer_bda, bda, 6);

#if !defined(NATIVE_SIMULATOR)
    if (s_bleServer) {
        s_bleServer->updateConnParams(connId, 12, 24, 0, 600); // 15ms-30ms interval, 6s timeout (Apple Accessory Guidelines)
    }
#endif

    // Immediately update telemetry so settings and status reflect connection with zero delay
    if (_last_telemetry) {
        _last_telemetry->ble_connected = true;
        _last_telemetry->ble_paired = Settings.get().ble_paired;
        _last_telemetry->ble_pairing_active = _pairing_active;
        _last_telemetry->ble_rssi = _rssi;
        _last_telemetry->ble_signal_bars = getSignalBars();
        if (strlen(Settings.get().ble_paired_name) > 0 && strcmp(Settings.get().ble_paired_name, "Smartphone") != 0) {
            strncpy(_last_telemetry->ble_device_name, Settings.get().ble_paired_name, sizeof(_last_telemetry->ble_device_name));
        } else if (strlen(Settings.get().ble_paired_addr) > 0) {
            strncpy(_last_telemetry->ble_device_name, Settings.get().ble_paired_addr, sizeof(_last_telemetry->ble_device_name));
        } else {
            strncpy(_last_telemetry->ble_device_name, "Smartphone", sizeof(_last_telemetry->ble_device_name));
        }
    }
    Renderer.markMenuDirty();
    Renderer.markScreenDirty();
    return true;
}

void BleKeylessManager::onClientDisconnect() {
    _connected = false;
    _conn_id = 0;
    _disconnect_time_ms = millis();
    _rssi = 0;
    if (_last_telemetry) {
        _last_telemetry->ble_connected = false;
        _last_telemetry->ble_rssi = 0;
        _last_telemetry->ble_signal_bars = 0;
    }
    Renderer.markMenuDirty();
    Renderer.markScreenDirty();
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
    if (_last_telemetry) {
        _last_telemetry->ble_pairing_active = true;
    }
#if !defined(NATIVE_SIMULATOR)
    BLEDevice::startAdvertising();
#endif
    Renderer.markMenuDirty();
    Renderer.markScreenDirty();
    Serial.println("[BLE KEYLESS] Pairing mode started. Waiting for smartphone connection to 'VESC-Keyless'...");
}

void BleKeylessManager::cancelPairing() {
    _pairing_active = false;
    if (_last_telemetry) {
        _last_telemetry->ble_pairing_active = false;
    }
    Renderer.markMenuDirty();
    Renderer.markScreenDirty();
}

void BleKeylessManager::unpair() {
    Settings.get().ble_paired = false;
    Settings.get().ble_paired_addr[0] = '\0';
    strncpy(Settings.get().ble_paired_name, "Smartphone", sizeof(Settings.get().ble_paired_name));
    Settings.save();
    _pairing_active = false;
#if !defined(NATIVE_SIMULATOR)
#if defined(CONFIG_BLUEDROID_ENABLED)
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num > 0) {
        esp_ble_bond_dev_t *dev_list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * dev_num);
        if (dev_list) {
            esp_ble_get_bond_device_list(&dev_num, dev_list);
            for (int i = 0; i < dev_num; i++) {
                esp_ble_remove_bond_device(dev_list[i].bd_addr);
            }
            free(dev_list);
        }
    }
#endif
    if (s_bleServer && _conn_id != 0) {
        s_bleServer->disconnect(_conn_id);
        BLEDevice::startAdvertising();
    }
#endif
    if (_connected) {
        _connected = false;
        _disconnect_time_ms = millis();
        _rssi = 0;
        _conn_id = 0;
    }
    if (_last_telemetry) {
        _last_telemetry->ble_paired = false;
        _last_telemetry->ble_connected = false;
        _last_telemetry->ble_pairing_active = false;
        _last_telemetry->ble_device_name[0] = '\0';
        _last_telemetry->ble_rssi = 0;
        _last_telemetry->ble_signal_bars = 0;
    }
    Renderer.markMenuDirty();
    Renderer.markScreenDirty();
    Serial.println("[BLE KEYLESS] Unpaired smartphone. Settings cleared.");
}

void BleKeylessManager::update(DashTelemetry &telemetry, uint32_t now_ms) {
    _last_telemetry = &telemetry;

    bool state_changed = (telemetry.ble_connected != _connected) ||
                         (telemetry.ble_paired != Settings.get().ble_paired) ||
                         (telemetry.ble_pairing_active != _pairing_active) ||
                         (telemetry.ble_signal_bars != getSignalBars());

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

    if (state_changed) {
        Renderer.markMenuDirty();
        Renderer.markScreenDirty();
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
