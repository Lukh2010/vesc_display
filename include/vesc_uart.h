#pragma once

#include <Arduino.h>
#include "board_config.h"
#include "dash_types.h"
#include "vesc_transport.h"

enum VescSyncStatus {
    SYNC_IDLE = 0,
    SYNC_REQUESTED,
    SYNC_SUCCESS,
    SYNC_FAILED
};

class VescHandler {
public:
    VescHandler();
    void begin();
    void update(DashTelemetry &telemetry);

    // Transport Interface (UART / Loopback / Virtual)
    void setTransport(IVescTransport *transport) { _transport = transport; }
    IVescTransport* getTransport() const { return _transport; }

    // Manual simulation throttle & incline override
    void setSimThrottle(float pct);
    float getSimThrottle() const { return _sim_throttle; }
    void setSimIncline(float deg) { _sim_incline = deg; }
    float getSimIncline() const { return _sim_incline; }
    void triggerSimFault(uint8_t code) { _sim_fault_code = code; }
    uint8_t getSimFault() const { return _sim_fault_code; }

    // ADC Diagnostic Getters
    float getLastAdc1Voltage() const;
    float getLastAdc1Pct() const;
    float getLastAdc2Voltage() const;
    float getLastAdc2Pct() const;

    // Configuration
    void setMotorPolePairs(int poles) { _pole_pairs = poles; }
    void setWheelDiameterMm(float mm) { _wheel_diameter_mm = mm; }
    void setGearRatio(float ratio) { _gear_ratio = ratio; }
    void resetTripOdometer(int32_t current_tacho = 0);

    // Dynamic Live Limit Control (Captive Portal & Ride Modes)
    void sendModeLimits(float max_speed_kmh, float max_power_watts, float max_battery_amps, float max_phase_amps, float cutoff_start = 0.0f, float cutoff_end = 0.0f);
    void sendTerminalCommand(const char *cmd);

    // Transparent USB CDC <-> UART Bridge for VESC Tool
    void enterBridgeMode();
    void exitBridgeMode();
    bool isBridgeActive() const { return _bridge_active || _wifi_bridge_active; }
    bool isUsbBridgeActive() const { return _bridge_active; }
    void updateBridge();
    void updateUsbBridge();
    uint32_t getBridgePcToVescBytes() const { return _bridge_pc_to_vesc; }
    uint32_t getBridgeVescToPcBytes() const { return _bridge_vesc_to_pc; }

    // Transparent Wireless Wi-Fi TCP <-> UART Bridge for VESC Tool
    void enterWifiBridgeMode();
    void exitWifiBridgeMode();
    bool isWifiBridgeActive() const { return _wifi_bridge_active; }
    bool isWifiClientConnected() const { return _wifi_client_connected; }
    const char *getWifiClientIp() const { return _wifi_client_ip; }
    uint32_t getBridgeWifiToVescBytes() const { return _bridge_wifi_to_vesc; }
    uint32_t getBridgeVescToWifiBytes() const { return _bridge_vesc_to_wifi; }
    void updateWifiBridge();

    // On-Demand Settings Pull from VESC Controller
    void requestMcconf();
    VescSyncStatus getSyncStatus() const { return _sync_status; }
    uint32_t getSyncTimeMs() const { return _sync_ms; }
    void resetSyncStatus() { _sync_status = SYNC_IDLE; }
    void runSimulation(DashTelemetry &telemetry);

private:
    void pollRealVesc(DashTelemetry &telemetry);
    void pollCanVesc(DashTelemetry &telemetry);
    void initCanBus();
    void sendVescGetValues();
    void sendVescGetDecodedAdc();
    bool parseVescPacket(uint8_t *buffer, size_t len, DashTelemetry &telemetry);

    IVescTransport *_transport;

    // Telemetry & Ebike Physics Parameters
    int _pole_pairs;            // Standard ebike hub motor: usually 15 or 23 pole pairs
    float _wheel_diameter_mm;   // e.g., 26" wheel ~ 660mm, 20" ~ 500mm
    float _gear_ratio;          // 1.0 for direct-drive hub, or gear reduction ratio
    int32_t _tacho_trip_offset; // Trip odometer tachometer baseline offset
    int32_t _last_tacho;        // Most recent raw tachometer reading from VESC
    int32_t _dist_trip_offset;  // Trip odometer distance baseline offset (for COMM_GET_VALUES_SETUP)
    int32_t _last_dist;         // Most recent raw distance reading from VESC

    // Simulation Engine State
    float _sim_throttle;
    float _sim_incline;
    float _target_speed;
    float _target_rpm;
    uint8_t _sim_fault_code;
    uint32_t _last_poll_ms;
    uint32_t _sim_last_update_ms;

    // USB Bridge State
    bool     _bridge_active;
    uint32_t _bridge_pc_to_vesc;
    uint32_t _bridge_vesc_to_pc;

    // Wi-Fi Bridge State
    bool     _wifi_bridge_active;
    bool     _wifi_client_connected;
    char     _wifi_client_ip[24];
    uint32_t _bridge_wifi_to_vesc;
    uint32_t _bridge_vesc_to_wifi;

    // Configuration Pull State
    VescSyncStatus _sync_status;
    uint32_t       _mcconf_req_ms;
    uint32_t       _sync_ms;
};

extern VescHandler Vesc;
