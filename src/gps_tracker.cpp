#include "gps_tracker.h"
#include "vesc_uart.h"
#include <math.h>

GpsTracker::GpsTracker()
    : _timezone_offset(1), // Default: UTC+1 (CET)
      _last_can_broadcast_ms(0),
      _last_sim_tick_ms(0),
      _rx_idx(0) {
    memset(_rx_buf, 0, sizeof(_rx_buf));
}

void GpsTracker::init() {
#if !defined(NATIVE_SIMULATOR)
    // ESP32-S3: Initialize Serial2 on GPIO 6 (RX only, TX not needed for listening)
    Serial2.begin(9600, SERIAL_8N1, 6, -1);
    Serial.println("[GPS] Initialized ATGM336H GPS+BDS on GPIO 6 (RX) @ 9600 baud.");
#else
    Serial.println("[GPS SIM] Running virtual GPS+BDS tracker in simulator mode.");
#endif
}

void GpsTracker::parseRmc(const char* line, DashTelemetry &telemetry) {
    // Example: $GNRMC,091523.00,A,5231.1234,N,01324.5678,E,15.2,180.0,121126,,,A*7A
    // Tokens:
    // 1: Time (HHMMSS.sss)
    // 2: Status (A = valid, V = warning/void)
    // 3: Latitude
    // 4: N/S
    // 5: Longitude
    // 6: E/W
    // 7: Speed in knots
    // 8: Course (deg)
    // 9: Date (DDMMYY)

    const char* p = line;
    int token_idx = 0;
    char token[24];

    char status = 'V';
    float raw_time = 0.0f;
    float speed_knots = 0.0f;
    uint32_t raw_date = 0;
    double raw_lat = 0.0;
    char lat_ns = 'N';
    double raw_lon = 0.0;
    char lon_ew = 'E';

    while (*p) {
        int t_len = 0;
        while (*p && *p != ',' && *p != '*' && t_len < (int)sizeof(token) - 1) {
            token[t_len++] = *p++;
        }
        token[t_len] = '\0';
        if (*p == ',' || *p == '*') p++;

        if (token_idx == 1 && t_len >= 6) {
            raw_time = atof(token);
        } else if (token_idx == 2 && t_len > 0) {
            status = token[0];
        } else if (token_idx == 3 && t_len > 0) {
            raw_lat = atof(token);
        } else if (token_idx == 4 && t_len > 0) {
            lat_ns = token[0];
        } else if (token_idx == 5 && t_len > 0) {
            raw_lon = atof(token);
        } else if (token_idx == 6 && t_len > 0) {
            lon_ew = token[0];
        } else if (token_idx == 7 && t_len > 0) {
            speed_knots = atof(token);
        } else if (token_idx == 9 && t_len >= 6) {
            raw_date = atol(token);
        }

        token_idx++;
        if (token_idx > 12) break;
    }

    if (status == 'A') {
        telemetry.gps.last_fix_ms = millis();
        telemetry.gps.speed_kmh = speed_knots * 1.852f; // 1 knot = 1.852 km/h

        // Parse UTC time
        uint32_t t_int = (uint32_t)raw_time;
        uint8_t utc_h = (t_int / 10000) % 100;
        uint8_t utc_m = (t_int / 100) % 100;
        uint8_t utc_s = t_int % 100;

        int local_h = (int)utc_h + _timezone_offset;
        if (local_h >= 24) local_h -= 24;
        if (local_h < 0)   local_h += 24;

        telemetry.gps.hour = (uint8_t)local_h;
        telemetry.gps.minute = utc_m;
        telemetry.gps.second = utc_s;

        // Parse Date (DDMMYY)
        telemetry.gps.day = (uint8_t)(raw_date / 10000);
        telemetry.gps.month = (uint8_t)((raw_date / 100) % 100);
        telemetry.gps.year = 2000 + (raw_date % 100);

        // Convert NMEA lat/lon (DDMM.mmmm) to decimal degrees
        double lat_deg = floor(raw_lat / 100.0);
        double lat_min = raw_lat - (lat_deg * 100.0);
        telemetry.gps.latitude = lat_deg + (lat_min / 60.0);
        if (lat_ns == 'S') telemetry.gps.latitude = -telemetry.gps.latitude;

        double lon_deg = floor(raw_lon / 100.0);
        double lon_min = raw_lon - (lon_deg * 100.0);
        telemetry.gps.longitude = lon_deg + (lon_min / 60.0);
        if (lon_ew == 'W') telemetry.gps.longitude = -telemetry.gps.longitude;
    }
}

void GpsTracker::parseGga(const char* line, DashTelemetry &telemetry) {
    // Example: $GNGGA,091523.00,5231.1234,N,01324.5678,E,1,14,0.9,45.2,M,45.0,M,,*6B
    // Tokens:
    // 6: Fix quality (0=invalid, 1=GPS fix, 2=DGPS)
    // 7: Satellites count
    // 8: HDOP
    // 9: Altitude (meters)

    const char* p = line;
    int token_idx = 0;
    char token[24];

    uint8_t fix_quality = 0;
    uint8_t sats = 0;
    float hdop = 99.0f;
    float alt = 0.0f;

    while (*p) {
        int t_len = 0;
        while (*p && *p != ',' && *p != '*' && t_len < (int)sizeof(token) - 1) {
            token[t_len++] = *p++;
        }
        token[t_len] = '\0';
        if (*p == ',' || *p == '*') p++;

        if (token_idx == 6 && t_len > 0) {
            fix_quality = (uint8_t)atoi(token);
        } else if (token_idx == 7 && t_len > 0) {
            sats = (uint8_t)atoi(token);
        } else if (token_idx == 8 && t_len > 0) {
            hdop = atof(token);
        } else if (token_idx == 9 && t_len > 0) {
            alt = atof(token);
        }

        token_idx++;
        if (token_idx > 12) break;
    }

    telemetry.gps.satellites = sats;
    telemetry.gps.hdop = hdop;
    telemetry.gps.altitude_m = alt;
    telemetry.gps.valid = (fix_quality > 0 && sats >= 4);
}

void GpsTracker::parseNmeaLine(const char* line, DashTelemetry &telemetry) {
    if (strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GPRMC", 6) == 0) {
        parseRmc(line, telemetry);
    } else if (strncmp(line, "$GNGGA", 6) == 0 || strncmp(line, "$GPGGA", 6) == 0) {
        parseGga(line, telemetry);
    }
}

void GpsTracker::update(DashTelemetry &telemetry) {
    uint32_t now = millis();

#if !defined(NATIVE_SIMULATOR)
    // Live ESP32-S3: Read all available NMEA bytes from ATGM336H on Serial2 (GPIO 6)
    while (Serial2.available() > 0) {
        char c = (char)Serial2.read();
        if (c == '\r') continue;
        if (c == '\n') {
            _rx_buf[_rx_idx] = '\0';
            if (_rx_idx > 6 && _rx_buf[0] == '$') {
                parseNmeaLine(_rx_buf, telemetry);
            }
            _rx_idx = 0;
        } else {
            if (_rx_idx < sizeof(_rx_buf) - 1) {
                _rx_buf[_rx_idx++] = c;
            } else {
                _rx_idx = 0; // Buffer overrun safety
            }
        }
    }
#else
    // Simulator Mode: Provide simulated high-accuracy GPS fix with BeiDou satellites
    if (now - _last_sim_tick_ms >= 100) { // 10 Hz virtual GPS
        _last_sim_tick_ms = now;
        telemetry.gps.valid = true;
        telemetry.gps.satellites = 14; // 8 GPS + 6 BDS
        telemetry.gps.hdop = 0.85f;
        telemetry.gps.altitude_m = 48.5f;
        telemetry.gps.latitude = 52.5200;
        telemetry.gps.longitude = 13.4050;
        telemetry.gps.last_fix_ms = now;

        // In simulator, GPS speed matches active test speed
        telemetry.gps.speed_kmh = telemetry.speed_kmh;

        // Clock advances naturally
        uint32_t total_sec = (now / 1000) + (14 * 3600) + (35 * 60);
        telemetry.gps.hour = (total_sec / 3600) % 24;
        telemetry.gps.minute = (total_sec / 60) % 60;
        telemetry.gps.second = total_sec % 60;
    }
#endif

    // Format top status bar clock string ("HH:MM")
    if (telemetry.gps.valid || telemetry.gps.last_fix_ms > 0) {
        snprintf(telemetry.clock_str, sizeof(telemetry.clock_str), "%02d:%02d",
                 telemetry.gps.hour, telemetry.gps.minute);
    } else {
        // Fallback clock when searching for satellites (based on uptime)
        uint32_t up_m = (now / 60000) % 60;
        uint32_t up_h = (12 + (now / 3600000)) % 24;
        snprintf(telemetry.clock_str, sizeof(telemetry.clock_str), "%02d:%02d",
                 (int)up_h, (int)up_m);
    }

    // --------------------------------------------------------------------------
    // SPEED PRIORITY SELECTOR:
    // If GPS has a valid 3D fix and received data in the last 2.5 seconds:
    // -> GPS Ground Speed has 100% PRIORITY!
    // If satellite reception is lost (tunnel, indoors, searching):
    // -> Smoothly fallback to VESC motor-calculated speed!
    // --------------------------------------------------------------------------
    bool has_gps_fix = (telemetry.gps.valid && telemetry.gps.satellites >= 4 &&
                        (now - telemetry.gps.last_fix_ms < 2500));

    if (has_gps_fix) {
        telemetry.use_gps_speed = true;
        telemetry.speed_kmh = telemetry.gps.speed_kmh;
    } else {
        telemetry.use_gps_speed = false;
        // Fallback to VESC motor speed if available, or keep existing speed
        if (telemetry.vesc_motor_speed > 0.0f || telemetry.speed_kmh > 0.0f) {
            telemetry.speed_kmh = telemetry.vesc_motor_speed;
        }
    }

    // --------------------------------------------------------------------------
    // VESC APP & UBOX SYNCHRONIZATION:
    // Transmit official VESC GNSS CAN frames every 200ms (5 Hz) to target Ubox (76)
    // When Ubox receives these, it updates its internal mc_interface_gnss() data,
    // which allows the mobile VESC Tool App to display GPS speed & coordinates!
    // --------------------------------------------------------------------------
    if (has_gps_fix && (now - _last_can_broadcast_ms >= 200)) {
        _last_can_broadcast_ms = now;
        VescHandler::sendGnssData(telemetry.gps);
    }
}
