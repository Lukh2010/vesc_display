#include "gps_tracker.h"
#include "vesc_uart.h"
#include "settings_manager.h"
#include <math.h>
#include <stdlib.h>

static bool verifyNmeaChecksum(const char* line) {
    if (!line || line[0] != '$') return false;
    const char* p = line + 1;
    uint8_t csum = 0;
    while (*p && *p != '*') {
        csum ^= (uint8_t)*p;
        p++;
    }
    if (*p != '*') return false;
    p++; // Skip '*'
    if (!p[0] || !p[1]) return false;
    char hex[3] = { p[0], p[1], '\0' };
    char *endptr = nullptr;
    long expected = strtol(hex, &endptr, 16);
    if (endptr != hex + 2) return false;
    return (csum == (uint8_t)expected);
}

GpsTracker::GpsTracker()
    : _timezone_offset(1), // Default: UTC+1 (CET)
      _last_can_broadcast_ms(0),
      _last_sim_tick_ms(0),
      _calib_diam_accum(0.0f),
      _calib_samples_count(0),
      _last_calib_eval_ms(0),
      _rx_idx(0) {
    memset(_rx_buf, 0, sizeof(_rx_buf));
}

void GpsTracker::init() {
#if !defined(NATIVE_SIMULATOR)
    if (PIN_GPS_RX >= 0) {
        Serial2.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
        Serial.printf("[GPS] Initialized ATGM336H GPS+BDS on GPIO %d (RX) @ 9600 baud.\n", PIN_GPS_RX);
    } else {
        Serial.println("[GPS] Physical GPS RX disabled (GPIO 44 reserved for USB Serial RX).");
    }
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
        // Discard any remaining characters in oversized field to prevent tokenizer desync
        while (*p && *p != ',' && *p != '*') p++;
        if (*p == ',' || *p == '*') p++;

        if (token_idx == 1 && t_len >= 6) {
            raw_time = strtof(token, nullptr);
        } else if (token_idx == 2 && t_len > 0) {
            status = token[0];
        } else if (token_idx == 3 && t_len > 0) {
            raw_lat = strtod(token, nullptr);
        } else if (token_idx == 4 && t_len > 0) {
            lat_ns = token[0];
        } else if (token_idx == 5 && t_len > 0) {
            raw_lon = strtod(token, nullptr);
        } else if (token_idx == 6 && t_len > 0) {
            lon_ew = token[0];
        } else if (token_idx == 7 && t_len > 0) {
            speed_knots = strtof(token, nullptr);
        } else if (token_idx == 9 && t_len >= 6) {
            raw_date = atol(token);
        }

        token_idx++;
        if (token_idx > 12) break;
    }

    if (status == 'A') {
        telemetry.gps.last_fix_ms = millis();
        float speed_kmh = speed_knots * 1.852f; // 1 knot = 1.852 km/h
        if (!isfinite(speed_kmh) || speed_kmh < 0.0f) speed_kmh = 0.0f;
        if (speed_kmh > 250.0f) speed_kmh = 250.0f;
        telemetry.gps.speed_kmh = speed_kmh;

        // Parse UTC time
        uint32_t t_int = (uint32_t)raw_time;
        uint8_t utc_h = (t_int / 10000) % 100;
        uint8_t utc_m = (t_int / 100) % 100;
        uint8_t utc_s = t_int % 100;

        int local_h = (int)utc_h + _timezone_offset;
        while (local_h >= 24) local_h -= 24;
        while (local_h < 0)   local_h += 24;

        telemetry.gps.hour = (uint8_t)local_h;
        telemetry.gps.minute = (utc_m < 60) ? utc_m : 0;
        telemetry.gps.second = (utc_s < 60) ? utc_s : 0;

        // Parse Date (DDMMYY)
        uint8_t day = (uint8_t)(raw_date / 10000);
        uint8_t month = (uint8_t)((raw_date / 100) % 100);
        uint16_t year = 2000 + (raw_date % 100);
        if (day >= 1 && day <= 31) telemetry.gps.day = day;
        if (month >= 1 && month <= 12) telemetry.gps.month = month;
        if (year >= 2020 && year <= 2099) telemetry.gps.year = year;

        // Convert NMEA lat/lon (DDMM.mmmm) to decimal degrees
        double lat_deg = floor(raw_lat / 100.0);
        double lat_min = raw_lat - (lat_deg * 100.0);
        double lat = lat_deg + (lat_min / 60.0);
        if (lat_ns == 'S') lat = -lat;
        if (isfinite(lat) && lat >= -90.0 && lat <= 90.0) {
            telemetry.gps.latitude = lat;
        }

        double lon_deg = floor(raw_lon / 100.0);
        double lon_min = raw_lon - (lon_deg * 100.0);
        double lon = lon_deg + (lon_min / 60.0);
        if (lon_ew == 'W') lon = -lon;
        if (isfinite(lon) && lon >= -180.0 && lon <= 180.0) {
            telemetry.gps.longitude = lon;
        }
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
        // Discard any remaining characters in oversized field to prevent tokenizer desync
        while (*p && *p != ',' && *p != '*') p++;
        if (*p == ',' || *p == '*') p++;

        if (token_idx == 6 && t_len > 0) {
            fix_quality = (uint8_t)atoi(token);
        } else if (token_idx == 7 && t_len > 0) {
            sats = (uint8_t)atoi(token);
        } else if (token_idx == 8 && t_len > 0) {
            hdop = strtof(token, nullptr);
        } else if (token_idx == 9 && t_len > 0) {
            alt = strtof(token, nullptr);
        }

        token_idx++;
        if (token_idx > 12) break;
    }

    if (sats > 64) sats = 64;
    if (!isfinite(hdop) || hdop < 0.0f) hdop = 99.0f;
    if (!isfinite(alt) || alt < -500.0f || alt > 10000.0f) alt = 0.0f;

    telemetry.gps.satellites = sats;
    telemetry.gps.hdop = hdop;
    telemetry.gps.altitude_m = alt;
    telemetry.gps.valid = (fix_quality > 0 && sats >= 4);
    if (fix_quality > 0) {
        telemetry.gps.last_fix_ms = millis();
    }
}

void GpsTracker::parseNmeaLine(const char* line, DashTelemetry &telemetry) {
    if (!verifyNmeaChecksum(line)) {
        return;
    }
    if (strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GPRMC", 6) == 0) {
        parseRmc(line, telemetry);
    } else if (strncmp(line, "$GNGGA", 6) == 0 || strncmp(line, "$GPGGA", 6) == 0) {
        parseGga(line, telemetry);
    }
}

void GpsTracker::update(DashTelemetry &telemetry) {
    uint32_t now = millis();

#if !defined(NATIVE_SIMULATOR)
    // Live ESP32-S3: Read up to max 128 bytes per tick to prevent GPS bursts from blocking Core-0 5ms cycle
    if (PIN_GPS_RX >= 0) {
        size_t bytes_processed = 0;
        static constexpr size_t MAX_BYTES_PER_TICK = 128;
        while (Serial2.available() > 0 && bytes_processed < MAX_BYTES_PER_TICK) {
            bytes_processed++;
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

        // Clock advances naturally
        uint32_t total_sec = (now / 1000) + (14 * 3600) + (35 * 60);
        telemetry.gps.hour = (total_sec / 3600) % 24;
        telemetry.gps.minute = (total_sec / 60) % 60;
        telemetry.gps.second = total_sec % 60;
    }

    // In simulator, GPS speed matches active motor/ground speed directly
    telemetry.gps.speed_kmh = telemetry.vesc_motor_speed;
    if (telemetry.vesc_motor_speed < 0.8f || !telemetry.vesc_connected) {
        telemetry.gps.speed_kmh = 0.0f;
    }
#endif

    // Format top status bar clock string ("HH:MM") only when hour or minute changes
    static int8_t s_last_clock_h = -1;
    static int8_t s_last_clock_m = -1;
    int cur_h = -1;
    int cur_m = -1;

    if (telemetry.gps.valid || telemetry.gps.last_fix_ms > 0) {
        cur_h = telemetry.gps.hour;
        cur_m = telemetry.gps.minute;
    } else {
        // Fallback clock when searching for satellites (based on uptime)
        cur_m = (int)((now / 60000) % 60);
        cur_h = (int)((12 + (now / 3600000)) % 24);
    }

    if (cur_h != s_last_clock_h || cur_m != s_last_clock_m) {
        s_last_clock_h = (int8_t)cur_h;
        s_last_clock_m = (int8_t)cur_m;
        snprintf(telemetry.clock_str, sizeof(telemetry.clock_str), "%02d:%02d", cur_h, cur_m);
        telemetry.clock_str[sizeof(telemetry.clock_str) - 1] = '\0';
    }

    // --------------------------------------------------------------------------
    // SPEED PRIORITY SELECTOR (Smart Fusion):
    // 0. VESC Offline: Wenn VESC getrennt ist, steht das Fahrzeug strikt auf 0 km/h
    // 1. Stillstand / Ampel: Steht das Rad (< 0.8 km/h), felsenfest 0 km/h (kein GPS-Drift).
    // 2. Rollenprüfstand / Dyno: Rad dreht sich (> 0.8 km/h), aber GPS steht (GPS < 1.0 km/h):
    //    Tacho zeigt 100% VESC-Wellengeschwindigkeit (11" kalibriert, strikt <= 22 km/h legal).
    // 3. Reale Straßenfahrt: Sobald GPS-Fix aktiv ist und das Fahrzeug real rollt (GPS >= 1.0 km/h):
    //    GPS Ground Speed hat absolute Priorität!
    // 4. Signalverlust / Tunnel: Nahtloser Fallback auf VESC-Wellengeschwindigkeit.
    // --------------------------------------------------------------------------
    static bool s_using_gps = false;
    if (!telemetry.vesc_connected) {
        telemetry.speed_kmh = 0.0f;
        telemetry.gps.speed_kmh = 0.0f;
        telemetry.vesc_motor_speed = 0.0f;
        s_using_gps = false;
        telemetry.use_gps_speed = false;
        return;
    }

    bool has_gps_fix = (telemetry.gps.valid && telemetry.gps.satellites >= 4 &&
                        (now - telemetry.gps.last_fix_ms < 2500));

    if (telemetry.vesc_motor_speed < 0.8f && (!has_gps_fix || telemetry.gps.speed_kmh < 1.2f)) {
        telemetry.speed_kmh = 0.0f;
        s_using_gps = false;
        telemetry.use_gps_speed = false;
    } else if (has_gps_fix) {
        // Hysterese gegen Umschalt-Jitter: Aktivierung bei >= 1.5 km/h, Deaktivierung erst bei < 0.8 km/h
        if (!s_using_gps && telemetry.gps.speed_kmh >= 1.5f) {
            s_using_gps = true;
        } else if (s_using_gps && telemetry.gps.speed_kmh < 0.8f) {
            s_using_gps = false;
        }

        if (s_using_gps) {
            telemetry.speed_kmh = telemetry.gps.speed_kmh;
            telemetry.use_gps_speed = true;
        } else {
            telemetry.speed_kmh = telemetry.vesc_motor_speed;
            telemetry.use_gps_speed = false;
        }
    } else {
        s_using_gps = false;
        // Rollenprüfstand (Rad dreht, GPS = 0) oder Tunnel/Signalverlust: VESC-Drehzahl
        telemetry.speed_kmh = telemetry.vesc_motor_speed;
        telemetry.use_gps_speed = false;
    }

    // --------------------------------------------------------------------------
    // VESC APP & UBOX SYNCHRONIZATION:
    // Transmit official VESC GNSS CAN frames every 200ms (5 Hz) to target Ubox (76)
    // When Ubox receives these, it updates its internal mc_interface_gnss() data,
    // which allows the mobile VESC Tool App to display GPS speed & coordinates!
    // (Autonomous Tire Calibration and Virtual TPMS are handled by TcsTpmsManager)
    // --------------------------------------------------------------------------
    if (has_gps_fix && (now - _last_can_broadcast_ms >= 200)) {
        _last_can_broadcast_ms = now;
        VescHandler::sendGnssData(telemetry.gps);
    }
}
