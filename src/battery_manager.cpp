#include "battery_manager.h"
#include "mode_manager.h"
#include <string.h>

BatteryManager Battery;

BatteryManager::BatteryManager()
    : _active_profile_id(0),
      _fused_soc_pct(100.0f),
      _remaining_wh(663.0f),
      _est_range_km(30.0f),
      _rest_start_ms(0),
      _is_resting(false),
      _soc_inited(false),
      _cycle_start_v(54.2f),
      _cycle_wh_accum(0.0f),
      _tracking_full_cycle(false),
      _current_sector(0),
      _dist_in_current_sector(0.0f),
      _wh_in_current_sector(0.0f),
      _dynamic_wh_km(22.0f) {
    for (int i = 0; i < ROLLING_SECTORS; i++) {
        _sector_wh[i] = 0.0f;
        _sector_km[i] = 0.0f;
    }
    loadDefaults();
}

void BatteryManager::loadDefaults() {
    // Profile 0: Primary 48V 12.75Ah Pack (13S, 612 Wh nominal)
    strncpy(_profiles[0].name, "12.75Ah 48V", sizeof(_profiles[0].name));
    _profiles[0].capacity_ah         = 12.75f;
    _profiles[0].nominal_wh          = 612.0f;
    _profiles[0].learned_wh          = 612.0f; // 100% Health
    _profiles[0].total_wh_discharged = 0.0f;
    _profiles[0].charge_cycles       = 1;
    _profiles[0].health_soh_pct      = 100.0f;

    // Profile 1: Optional 48V 25Ah Pack (~1200 Wh nominal)
    strncpy(_profiles[1].name, "25.0Ah 48V", sizeof(_profiles[1].name));
    _profiles[1].capacity_ah         = 25.0f;
    _profiles[1].nominal_wh          = 1200.0f;
    _profiles[1].learned_wh          = 1200.0f; // 100% Health
    _profiles[1].total_wh_discharged = 0.0f;
    _profiles[1].charge_cycles       = 1;
    _profiles[1].health_soh_pct      = 100.0f;
}

void BatteryManager::begin() {
    _prefs.begin("ebike_bat", false);

    // Auto-migration to 48V 12.75Ah default profile
    if (!_prefs.isKey("bat_48v_1275ah_v3")) {
        loadDefaults();
        saveProfiles();
        _prefs.putBool("bat_48v_1275ah_v3", true);
        _prefs.putBool("bat_init", true);
        Serial.println("[BATTERY] Initialized/Migrated to 48V 12.75Ah (612 Wh) battery profile.");
    } else {
        _active_profile_id = _prefs.getUChar("active_p", 0);
        if (_active_profile_id > 1) _active_profile_id = 0;

        for (int i = 0; i < 2; i++) {
            char k[16];
            snprintf(k, sizeof(k), "p%d_name", i);
            const char* defName = (i == 0) ? "12.75Ah 48V" : "25.0Ah 48V";
            if (_prefs.getString(k, _profiles[i].name, sizeof(_profiles[i].name)) == 0 || _profiles[i].name[0] == '\0') {
                strncpy(_profiles[i].name, defName, sizeof(_profiles[i].name));
            }

            snprintf(k, sizeof(k), "p%d_ah", i);
            _profiles[i].capacity_ah = _prefs.getFloat(k, (i == 0) ? 12.75f : 25.0f);

            snprintf(k, sizeof(k), "p%d_nom", i);
            _profiles[i].nominal_wh = _prefs.getFloat(k, (i == 0) ? 612.0f : 1200.0f);

            snprintf(k, sizeof(k), "p%d_lrn", i);
            _profiles[i].learned_wh = _prefs.getFloat(k, _profiles[i].nominal_wh);

            snprintf(k, sizeof(k), "p%d_tot", i);
            _profiles[i].total_wh_discharged = _prefs.getFloat(k, 0.0f);

            snprintf(k, sizeof(k), "p%d_cyc", i);
            _profiles[i].charge_cycles = _prefs.getUShort(k, 1);

            _profiles[i].health_soh_pct = (_profiles[i].nominal_wh > 0.0f) ? 
                ((_profiles[i].learned_wh / _profiles[i].nominal_wh) * 100.0f) : 100.0f;
        }
        Serial.printf("[BATTERY] Loaded profile %d (%s) - %.2fAh / %.0fWh - Health: %.1f%%\n",
                      _active_profile_id, _profiles[_active_profile_id].name,
                      _profiles[_active_profile_id].capacity_ah,
                      _profiles[_active_profile_id].learned_wh,
                      _profiles[_active_profile_id].health_soh_pct);
    }
}

void BatteryManager::saveProfiles() {
    _prefs.putUChar("active_p", _active_profile_id);
    for (int i = 0; i < 2; i++) {
        char k[16];
        snprintf(k, sizeof(k), "p%d_name", i);
        _prefs.putString(k, _profiles[i].name);
        snprintf(k, sizeof(k), "p%d_ah", i);
        _prefs.putFloat(k, _profiles[i].capacity_ah);
        snprintf(k, sizeof(k), "p%d_nom", i);
        _prefs.putFloat(k, _profiles[i].nominal_wh);
        snprintf(k, sizeof(k), "p%d_lrn", i);
        _prefs.putFloat(k, _profiles[i].learned_wh);
        snprintf(k, sizeof(k), "p%d_tot", i);
        _prefs.putFloat(k, _profiles[i].total_wh_discharged);
        snprintf(k, sizeof(k), "p%d_cyc", i);
        _prefs.putUShort(k, _profiles[i].charge_cycles);
    }
}

void BatteryManager::setActiveProfile(uint8_t id) {
    if (id > 1) id = 0;
    _active_profile_id = id;
    saveProfiles();
    _soc_inited = false;
    Serial.printf("[BATTERY] Switched to %s (%.2fAh, Learned Wh: %.0f, Health: %.1f%%)\n",
                  _profiles[_active_profile_id].name,
                  _profiles[_active_profile_id].capacity_ah,
                  _profiles[_active_profile_id].learned_wh,
                  _profiles[_active_profile_id].health_soh_pct);
}

void BatteryManager::resetProfileLearning(uint8_t id) {
    if (id > 1) id = 0;
    _profiles[id].learned_wh = _profiles[id].nominal_wh;
    _profiles[id].health_soh_pct = 100.0f;
    saveProfiles();
}

// Multi-chemistry / Multi-S (10S to 24S) Li-Ion Open-Circuit Voltage calculation
float BatteryManager::getOcvSoc(float v_pack) {
    if (v_pack < 30.0f) return 0.0f;
    uint8_t cfg_cells = ModeManager::instance().getConfig().battery_cells;
    int s_count = (cfg_cells >= 10 && cfg_cells <= 24) ? cfg_cells : (int)roundf(v_pack / 3.75f);
    if (s_count < 10) s_count = 10;
    if (s_count > 24) s_count = 24;

    float cell_v = v_pack / (float)s_count;
    if (cell_v >= 4.15f) return 100.0f;
    if (cell_v >= 4.05f) return 90.0f + (cell_v - 4.05f) * 100.0f;
    if (cell_v >= 3.93f) return 80.0f + (cell_v - 3.93f) * 83.3f;
    if (cell_v >= 3.82f) return 70.0f + (cell_v - 3.82f) * 90.9f;
    if (cell_v >= 3.73f) return 60.0f + (cell_v - 3.73f) * 111.1f;
    if (cell_v >= 3.65f) return 50.0f + (cell_v - 3.65f) * 125.0f;
    if (cell_v >= 3.56f) return 40.0f + (cell_v - 3.56f) * 111.1f;
    if (cell_v >= 3.44f) return 30.0f + (cell_v - 3.44f) * 83.3f;
    if (cell_v >= 3.32f) return 20.0f + (cell_v - 3.32f) * 83.3f;
    if (cell_v >= 3.20f) return 10.0f + (cell_v - 3.20f) * 83.3f;
    if (cell_v >= 3.00f) return (cell_v - 3.00f) * 50.0f;
    return 0.0f;
}

void BatteryManager::update(DashTelemetry &telemetry, float dt) {
    uint32_t now = millis();
    BatteryProfile &prof = _profiles[_active_profile_id];

    // Check if battery voltage is within a plausible pack window (30.0V - 105.0V for 10S to 24S)
    // When powered by very low-voltage Lab PSU or disconnected, suppress battery stats
    if (telemetry.voltage < 30.0f || telemetry.voltage > 105.0f) {
        telemetry.battery_pct = -1.0f;
        telemetry.remaining_wh = -1.0f;
        telemetry.est_range_km = -1.0f;
        return;
    }

    // 0. Initial Boot OCV Initialization
    // On the first valid voltage reading at startup, seed SoC & remaining Wh from resting OCV
    if (!_soc_inited) {
        float init_soc = getOcvSoc(telemetry.voltage);
        _fused_soc_pct = init_soc;
        _remaining_wh = (_fused_soc_pct / 100.0f) * prof.learned_wh;
        _soc_inited = true;
        Serial.printf("[BATTERY] Initialized SoC: %.1f%% (%.1f Wh remaining, pack %.1fV)\n",
                      _fused_soc_pct, _remaining_wh, telemetry.voltage);
    }

    // 1. Compute instant power and energy delta
    float power_w = telemetry.voltage * telemetry.current_amps;
    float delta_wh = (power_w * (dt / 3600.0f));

    // 2. Track energy throughput (Strict Coulomb / Wh integration)
    if (delta_wh > 0.0f) {
        telemetry.stats.trip_wh += delta_wh;
        prof.total_wh_discharged += delta_wh;

        // Subtract from remaining Wh in current pack
        _remaining_wh -= delta_wh;
        if (_remaining_wh < 0.0f) _remaining_wh = 0.0f;
    } else if (delta_wh < 0.0f) {
        // Regenerative braking: add energy back capped at learned capacity
        _remaining_wh -= delta_wh;
        if (_remaining_wh > prof.learned_wh) _remaining_wh = prof.learned_wh;
    }

    // 3. Standstill Detection:
    // Voltage sag ONLY dissipates when bike is stopped and motor/load current is near zero!
    bool at_standstill = (telemetry.speed_kmh < 0.5f && fabsf(telemetry.current_amps) < 0.6f);
    if (at_standstill) {
        if (!_is_resting) {
            _is_resting = true;
            _rest_start_ms = now;
        }
    } else {
        _is_resting = false;
    }

    // 4. Sag-Resilient State of Charge & Remaining Wh Filter
    // Live driving SoC from integrated Wh (100% immune to voltage sag):
    float coulomb_soc = (prof.learned_wh > 0.0f) ? ((_remaining_wh / prof.learned_wh) * 100.0f) : 0.0f;

    if (_is_resting && (now - _rest_start_ms > 2000)) {
        // Stationary for > 2 seconds: voltage sag is fully gone, OCV is stable & true
        float ocv_soc = getOcvSoc(telemetry.voltage);
        // Smoothly reconcile Coulomb counter with resting OCV
        _fused_soc_pct += (ocv_soc - _fused_soc_pct) * (dt * 0.12f);
        // Re-synchronize remaining Wh with resting battery capacity
        _remaining_wh = (_fused_soc_pct / 100.0f) * prof.learned_wh;
    } else {
        // Driving, accelerating, or brief stop:
        // Follow integrated energy strictly - zero drop from acceleration voltage sag!
        _fused_soc_pct = coulomb_soc;
    }

    if (_fused_soc_pct > 100.0f) _fused_soc_pct = 100.0f;
    if (_fused_soc_pct < 0.0f)   _fused_soc_pct = 0.0f;

    // 5. Projected Dynamic Remaining Range (km) via rolling distance window
    float dist_delta = (telemetry.speed_kmh / 3600.0f) * dt;
    if (dist_delta > 0.0f) {
        _dist_in_current_sector += dist_delta;
        if (delta_wh > 0.0f) {
            _wh_in_current_sector += delta_wh;
        }

        // When 200m accumulated in current sector, advance bucket
        if (_dist_in_current_sector >= 0.2f) {
            _sector_wh[_current_sector] = _wh_in_current_sector;
            _sector_km[_current_sector] = _dist_in_current_sector;
            _current_sector = (_current_sector + 1) % ROLLING_SECTORS;
            _dist_in_current_sector = 0.0f;
            _wh_in_current_sector = 0.0f;
        }
    }

    // Sum rolling window (up to 2km)
    float rolling_wh_sum = _wh_in_current_sector;
    float rolling_km_sum = _dist_in_current_sector;
    for (int i = 0; i < ROLLING_SECTORS; i++) {
        rolling_wh_sum += _sector_wh[i];
        rolling_km_sum += _sector_km[i];
    }

    // Calculate dynamic Wh/km:
    if (rolling_km_sum >= 0.3f && rolling_wh_sum > 0.5f) {
        float instant_eff = rolling_wh_sum / rolling_km_sum;
        if (instant_eff < 10.0f) instant_eff = 10.0f;
        if (instant_eff > 140.0f) instant_eff = 140.0f;
        // Smooth convergence
        _dynamic_wh_km += (instant_eff - _dynamic_wh_km) * (dt * 0.25f);
    } else if (telemetry.trip_km > 0.4f && telemetry.stats.trip_wh > 1.0f) {
        _dynamic_wh_km = telemetry.stats.trip_wh / telemetry.trip_km;
    } else {
        _dynamic_wh_km = 22.0f;
    }

    if (telemetry.trip_km > 0.4f && telemetry.stats.trip_wh > 1.0f) {
        telemetry.stats.trip_wh_km = telemetry.stats.trip_wh / telemetry.trip_km;
    } else {
        telemetry.stats.trip_wh_km = 22.0f;
    }

    telemetry.stats.dynamic_wh_km = _dynamic_wh_km;
    telemetry.dynamic_wh_km = _dynamic_wh_km;

    float eff = _dynamic_wh_km;
    if (eff < 10.0f) eff = 10.0f;
    _est_range_km = _remaining_wh / eff;
    if (_est_range_km > 180.0f) _est_range_km = 180.0f;
    if (_est_range_km < 0.0f)   _est_range_km = 0.0f;

    // Track min and max voltage under load
    if (telemetry.voltage > 20.0f) {
        if (telemetry.stats.min_voltage == 0.0f || telemetry.voltage < telemetry.stats.min_voltage) {
            telemetry.stats.min_voltage = telemetry.voltage;
        }
        if (telemetry.voltage > telemetry.stats.max_voltage) {
            telemetry.stats.max_voltage = telemetry.voltage;
        }
    }

    // 6. Self-Learning Battery Degradation / Full-Cycle Detection
    if (telemetry.voltage > 57.5f) {
        _tracking_full_cycle = true;
        _cycle_start_v = telemetry.voltage;
        _cycle_wh_accum = 0.0f;
    }

    if (_tracking_full_cycle) {
        if (delta_wh > 0.0f) _cycle_wh_accum += delta_wh;
        // When discharged down past 45.0V (~10% SoC), evaluate learned capacity
        if (telemetry.voltage < 45.0f && _cycle_wh_accum > (prof.learned_wh * 0.65f)) {
            _tracking_full_cycle = false;
            // Extrapolate full 100% capacity from the 100%->10% test
            float measured_full_wh = _cycle_wh_accum / 0.90f;
            // Exponential moving average: 85% previous, 15% new measurement
            prof.learned_wh = (prof.learned_wh * 0.85f) + (measured_full_wh * 0.15f);
            prof.charge_cycles++;
            prof.health_soh_pct = (prof.learned_wh / prof.nominal_wh) * 100.0f;
            saveProfiles();
            Serial.printf("[BATTERY] Updated health! Learned Wh: %.0f, Health: %.1f%%\n",
                          prof.learned_wh, prof.health_soh_pct);
        }
    }

    // 7. Push states into Telemetry struct
    telemetry.battery_profile_id = _active_profile_id;
    telemetry.battery_pct        = _fused_soc_pct;
    telemetry.remaining_wh       = _remaining_wh;
    telemetry.battery_health_soh = prof.health_soh_pct;
    telemetry.est_range_km       = _est_range_km;
}

void BatteryManager::resetTrip() {
    _current_sector = 0;
    _dist_in_current_sector = 0.0f;
    _wh_in_current_sector = 0.0f;
    for (int i = 0; i < ROLLING_SECTORS; i++) {
        _sector_wh[i] = 0.0f;
        _sector_km[i] = 0.0f;
    }
    _dynamic_wh_km = 22.0f;
}
