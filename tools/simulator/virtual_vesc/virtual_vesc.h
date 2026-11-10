#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <chrono>
#include "vesc_transport.h"
#include "vesc_packet.h"

// VESC Protocol Command IDs
enum VescCommPacketId : uint8_t {
    COMM_FW_VERSION             = 0,
    COMM_JUMP_TO_BOOTLOADER     = 1,
    COMM_ERASE_NEW_APP          = 2,
    COMM_WRITE_NEW_APP_DATA     = 3,
    COMM_GET_VALUES             = 4,
    COMM_SET_DUTY               = 5,
    COMM_SET_CURRENT            = 6,
    COMM_SET_CURRENT_BRAKE      = 7,
    COMM_SET_RPM                = 8,
    COMM_SET_POS                = 9,
    COMM_SET_HANDBRAKE          = 10,
    COMM_SET_DETECT             = 11,
    COMM_SET_SERVO_POS          = 12,
    COMM_SET_MCCONF             = 13,
    COMM_GET_MCCONF             = 14,
    COMM_GET_MCCONF_DEFAULT     = 15,
    COMM_SET_APPCONF            = 16,
    COMM_GET_APPCONF            = 17,
    COMM_GET_APPCONF_DEFAULT    = 18,
    COMM_SAMPLE_PRINT           = 19,
    COMM_TERMINAL_CMD           = 20,
    COMM_PRINT                  = 21,
    COMM_ROTOR_POSITION         = 22,
    COMM_EXPERIMENT_SAMPLE      = 23,
    COMM_DETECT_MOTOR_PARAM     = 24,
    COMM_DETECT_MOTOR_R_L       = 25,
    COMM_DETECT_MOTOR_FLUX_LINKAGE = 26,
    COMM_DETECT_ENCODER         = 27,
    COMM_DETECT_HALL_FOC        = 28,
    COMM_REBOOT                 = 29,
    COMM_ALIVE                  = 30,
    COMM_GET_DECODED_PPM        = 31,
    COMM_GET_DECODED_ADC        = 32,
    COMM_GET_DECODED_CHUK       = 33,
    COMM_FORWARD_CAN            = 34,
    COMM_SET_CHUCK_DATA         = 35,
    COMM_CUSTOM_APP_DATA        = 36,
    COMM_GET_VALUES_SETUP       = 47,
    COMM_GET_VALUES_SELECTIVE   = 50,
    COMM_GET_VALUES_SETUP_SELECTIVE = 51,
    COMM_SET_MCCONF_TEMP        = 62,
    COMM_SET_MCCONF_TEMP_SETUP  = 63
};

struct VirtualVescLimits {
    float max_phase_amps;
    float min_phase_amps;
    float max_battery_amps;
    float min_battery_amps;
    float max_erpm;
    float min_erpm;
    float max_watt;
    float min_watt;
    float duty_min;
    float duty_max;
    float battery_cut_start;
    float battery_cut_end;

    VirtualVescLimits()
        : max_phase_amps(120.0f),
          min_phase_amps(-120.0f),
          max_battery_amps(60.0f),
          min_battery_amps(-30.0f),
          max_erpm(100000.0f),
          min_erpm(-100000.0f),
          max_watt(15000.0f),
          min_watt(-15000.0f),
          duty_min(0.0f),
          duty_max(0.95f),
          battery_cut_start(46.2f),
          battery_cut_end(42.0f) {}
};

struct VirtualVescTelemetry {
    float temp_mos;          // °C
    float temp_mot;          // °C
    float current_mot;       // Phase Amps (A)
    float current_in;        // Battery pack current (A)
    float duty_cycle_pct;    // Inverter duty (0 - 120%)
    float rpm;               // Mechanical RPM
    int   pole_pairs;        // Motor pole pairs (e.g. 15)
    float speed_kmh;         // Ground speed (km/h)
    float voltage;           // Battery voltage (V)
    float battery_pct;       // SoC percentage (0 - 100%)
    float amphours_used;     // Ah consumed
    float amphours_charged;  // Ah charged
    float watthours_used;    // Wh consumed
    float watthours_charged; // Wh charged
    int32_t tachometer;      // Tachometer step counts
    int32_t tachometer_abs;  // Absolute tachometer steps
    float trip_km;           // Trip distance (km)
    float odo_km;            // Lifetime odometer (km)
    uint8_t fault_code;      // mc_fault_code
    float throttle_pct;      // Throttle (0 - 100%)
    float brake_pct;         // Brake (0 - 100%)

    VirtualVescTelemetry()
        : temp_mos(34.5f),
          temp_mot(38.2f),
          current_mot(0.0f),
          current_in(0.0f),
          duty_cycle_pct(0.0f),
          rpm(0.0f),
          pole_pairs(15),
          speed_kmh(0.0f),
          voltage(53.8f),
          battery_pct(88.0f),
          amphours_used(0.0f),
          amphours_charged(0.0f),
          watthours_used(0.0f),
          watthours_charged(0.0f),
          tachometer(0),
          tachometer_abs(0),
          trip_km(0.0f),
          odo_km(142.0f),
          fault_code(0),
          throttle_pct(0.0f),
          brake_pct(0.0f) {}
};

/**
 * @brief Realistic, software-based VESC emulator.
 * Communicates via IVescTransport with standard VESC protocol frames and responds
 * to firmware telemetry requests and configuration updates.
 */
class VirtualVesc {
public:
    VirtualVesc();
    ~VirtualVesc() = default;

    void setTransport(IVescTransport *transport) { _transport = transport; }
    IVescTransport* getTransport() const { return _transport; }

    void begin();
    void reset();

    // Simulation Physics State Interface
    void setPhysicsTelemetry(const VirtualVescTelemetry &telem);
    const VirtualVescTelemetry& getPhysicsTelemetry() const { return _telem; }

    // Active Controller Limits (governed by COMM_SET_MCCONF_TEMP, terminal commands, etc.)
    const VirtualVescLimits& getLimits() const { return _limits; }
    void setLimits(const VirtualVescLimits &limits) { _limits = limits; }

    // CAN Secondary Controller Limits (from COMM_FORWARD_CAN)
    const VirtualVescLimits& getCanLimits(uint8_t can_id) const;

    // Fault Injection
    void setSimFault(uint8_t fault_code) { _telem.fault_code = fault_code; }
    uint8_t getSimFault() const { return _telem.fault_code; }

    // Process incoming packets from transport and dispatch responses
    void process();

    // Response Callback type for routing responses to specific sources (e.g. TCP client)
    using ResponseCallback = std::function<void(const uint8_t *packet, size_t len)>;

    // Directly process a packet payload (for testing, loopback, or TCP bridge)
    void handlePacket(const uint8_t *payload, size_t len, const ResponseCallback &resp_cb = nullptr);

    // External Controls (e.g. from VESC Tool via TCP or CAN)
    float getExternalThrottle() const { return _ext_throttle_pct; }
    float getExternalBrake() const { return _ext_brake_pct; }
    float getExternalTargetCurrent() const { return _ext_target_current_a; }
    bool hasExternalCurrentControl() const { return _has_ext_current_ctrl && hasExternalControl(); }
    bool hasExternalControl() const {
        if (!_has_ext_ctrl) return false;
        auto now = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_ext_ctrl_time).count();
        return (diff < 1500); // 1.5s timeout watchdog
    }
    void resetExternalControl() {
        _ext_throttle_pct = 0.0f;
        _ext_brake_pct = 0.0f;
        _ext_target_current_a = 0.0f;
        _has_ext_ctrl = false;
        _has_ext_current_ctrl = false;
    }

    // Diagnostics & Test Introspection
    uint32_t getPacketsReceived() const { return _packets_received; }
    uint32_t getPacketsSent() const { return _packets_sent; }
    uint8_t getLastCommandId() const { return _last_cmd_id; }
    uint32_t getCommandCount(uint8_t cmd_id) const;
    const std::string& getLastTerminalCmd() const { return _last_terminal_cmd; }

private:
    void sendResponse(const uint8_t *payload, size_t len);

    // Command Handlers
    void handleFwVersion();
    void handleGetValues(uint8_t cmd_id, const uint8_t *payload, size_t len);
    void handleGetValuesSetup(uint8_t cmd_id, const uint8_t *payload, size_t len);
    void handleGetDecodedAdc();
    void handleGetDecodedPpm();
    void handleGetDecodedChuk();
    void handleGetMcconf();
    void handleSetMcconf(const uint8_t *payload, size_t len);
    void handleGetAppconf(uint8_t cmd_id);
    void handleSetMcconfTemp(const uint8_t *payload, size_t len, VirtualVescLimits &target_limits);
    void handleSetDuty(const uint8_t *payload, size_t len);
    void handleSetCurrent(const uint8_t *payload, size_t len);
    void handleSetCurrentBrake(const uint8_t *payload, size_t len);
    void handleSetRpm(const uint8_t *payload, size_t len);
    void handleSetHandbrake(const uint8_t *payload, size_t len);
    void handleForwardCan(const uint8_t *payload, size_t len);
    void handleTerminalCmd(const uint8_t *payload, size_t len);

    IVescTransport *_transport;
    VescPacketParser _parser;
    ResponseCallback _current_resp_cb;

    VirtualVescLimits _limits;
    VirtualVescLimits _can1_limits;
    VirtualVescTelemetry _telem;

    float _ext_throttle_pct;
    float _ext_brake_pct;
    float _ext_target_current_a;
    bool  _has_ext_ctrl;
    bool  _has_ext_current_ctrl;
    std::chrono::steady_clock::time_point _last_ext_ctrl_time;

    uint32_t _packets_received;
    uint32_t _packets_sent;
    uint8_t  _last_cmd_id;
    uint32_t _cmd_counters[256];
    std::string _last_terminal_cmd;
};
