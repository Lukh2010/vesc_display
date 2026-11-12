#pragma once

#include <Arduino.h>
#include "dash_types.h"

// ==============================================================================
// ATGM336H GPS + BDS (BeiDou) Hardware Tracker & NMEA Parser
// ==============================================================================
// Communicates with ATGM336H on ESP32-S3 GPIO 6 (RX) at 9600 baud.
// Parses $GNRMC, $GPRMC, $GNGGA, $GPGGA strings with zero heap allocation.
// Provides live satellite speed, 3D fix status, altitude, and UTC clock.

class GpsTracker {
public:
    static GpsTracker& instance() {
        static GpsTracker inst;
        return inst;
    }

    void init();
    void update(DashTelemetry &telemetry);

    // Set local timezone offset in hours (e.g. +1 for CET Winter, +2 for CEST Summer)
    void setTimezoneOffset(int8_t hours) { _timezone_offset = hours; }
    int8_t getTimezoneOffset() const { return _timezone_offset; }

private:
    GpsTracker();
    void parseNmeaLine(const char* line, DashTelemetry &telemetry);
    void parseRmc(const char* line, DashTelemetry &telemetry);
    void parseGga(const char* line, DashTelemetry &telemetry);

    int8_t _timezone_offset;
    uint32_t _last_can_broadcast_ms;
    uint32_t _last_sim_tick_ms;

    char _rx_buf[128];
    uint8_t _rx_idx;
};
